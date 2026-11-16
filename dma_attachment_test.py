"""Exercise DMA attachment administration with real handle/grant/notification code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-attachment-") as directory:
        output = Path(directory)
        shims = dict(SHIMS)
        shims["debug.h"] = "#pragma once\n" + shims["debug.h"] + """
#include <stdio.h>
#undef ERROR
#define ERROR(...) ((void)fprintf(stderr, __VA_ARGS__))
#ifdef DMA_ATTACHMENT_LOG_TEST
void TestDmaError(const char*, ...) __attribute__((format(printf, 1, 2)));
#undef ERROR
#define ERROR(...) TestDmaError(__VA_ARGS__)
#endif
#ifdef DMA_ATTACHMENT_WARNING_TEST
void TestDmaWarning(const char*, ...) __attribute__((format(printf, 1, 2)));
#undef WARNING
#define WARNING(...) TestDmaWarning(__VA_ARGS__)
#endif
"""
        shims["memoryspace.h"] += """
#define MEMORY_SPACE_APPLICATION 2
#define MEMORY_SPACE_INHERIT 1
typedef struct MemorySpace {
    uuid_t GroupGlobalHandleID;
    unsigned int Flags;
} MemorySpace_t;
oserr_t MSContextAcquireSecurity(uint64_t, MSContextSecurity_t**);
"""
        shims["ioset.h"] = (ROOT / "librt/libc/include/ioset.h").read_text()
        shims["mutex.h"] = """#pragma once
#include <assert.h>
typedef struct { int held; } Mutex_t;
#define MUTEX_FLAG_PLAIN 0
#define OS_MUTEX_INIT(flags) {0}
static inline void MutexConstruct(Mutex_t* m, unsigned int flags) { (void)flags; m->held=0; }
static inline void MutexDestruct(Mutex_t* m) { assert(!m->held); }
static inline void MutexLock(Mutex_t* m) { assert(!m->held); m->held=1; }
static inline void MutexUnlock(Mutex_t* m) { assert(m->held); m->held=0; }
"""
        for name, source in shims.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-unused-function",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in (
            "testing/include", "kernel", "kernel/include", "librt/libos/include",
            "librt/libds/include", "librt/libddk/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        sources = [str(ROOT / path) for path in (
            "testing/dma_attachment_test.c", "kernel/memory/dma/domain.c",
            "kernel/memory/dma/address.c", "kernel/memory/dma/unit.c",
            "kernel/memory/dma/translated.c", "kernel/memory/dma/iova.c",
            "librt/libds/hashtable.c",
            "librt/libds/queue.c", "librt/libds/list.c", "librt/libds/rbtree.c")]
        binary = output / "native"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run(flags + sources + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=20)
        syscall_sources = [str(ROOT / "testing/dma_attachment_syscall_test.c")] + sources[1:]
        subprocess.run(flags + syscall_sources + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=20)
        # Client boundary uses real DDK code with only its syscall transport replaced.
        (output / "internal").mkdir(exist_ok=True)
        (output / "internal/_syscalls.h").write_text("#pragma once\n")
        (output / "ddk/ddkdefs.h").write_text(
            '#pragma once\n#include <os/osdefs.h>\n#define DDKDECL(type, decl) extern type decl\n'
            '#define _CODE_BEGIN\n#define _CODE_END\n')
        subprocess.run(flags + [str(ROOT / "testing/dma_attachment_client_test.c"),
                               "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=20)
        if args.mutation_check:
            mutations = (
                ("wrap exhausted event sequence", "kernel/memory/dma/events.c", "DMA_EVENTS_SOURCE",
                 "if (eventState->Sequence != UINT64_MAX) {", "if (true) {"),
                ("acknowledge exhausted event", "kernel/memory/dma/events.c", "DMA_EVENTS_SOURCE",
                 "eventState == NULL || eventState->Sequence == UINT64_MAX", "eventState == NULL"),
                ("warn before sequence exhaustion", "kernel/memory/dma/events.c", "DMA_EVENTS_SOURCE",
                 "if (eventState->Sequence == UINT64_MAX) {",
                 "if (eventState->Sequence == UINT64_MAX - 1) {"),
                ("truncate the receiver principal", "kernel/memory/dma/binding.c", "DMA_BINDING_SOURCE",
                 "MSContextAcquireSecurity(parameters->Principal, &receiver)",
                 "MSContextAcquireSecurity((uint32_t)parameters->Principal, &receiver)"),
                ("accept another binding principal", "kernel/memory/dma/binding.c", "DMA_BINDING_SOURCE",
                 "bindingState->Principal != 0 && bindingState->Principal != parameters->Principal", "false"),
                ("remove the existing grant on failed retry", "kernel/memory/dma/binding.c", "DMA_BINDING_SOURCE",
                 "if (cancel && prepared->CreatesBinding) {", "if (cancel) {"),
                ("publish after receiver close", "kernel/memory/dma/binding.c", "DMA_BINDING_SOURCE",
                 "binding->State == DMABindingClosed ||", "false ||"),
                ("create another binding for a retry", "kernel/memory/dma/binding.c", "DMA_BINDING_SOURCE",
                 """return AcquireHandleReferenceInContext(
            registration->Receiver,
            bindingState->Current->ID,
            HandleTypeDMABinding,
            (void**)&registration->Binding
        );""",
                 "registration->CreatesBinding = true; *newGenerationOut = manager->Registry.NextBindingGeneration++; return OS_EOK;"),
                ("acknowledge a failed event copy", "kernel/memory/dma/events.c", "DMA_EVENTS_SOURCE",
                 "if (outputsCopied) {", "if (true) {"),
                ("ignore full manager principal", "kernel/memory/dma/manager.c", "DMA_MANAGER_SOURCE",
                 "manager->Principal != (uint64_t)caller->GlobalHandleID", "false"),
                ("invert the registry reference error check", "kernel/memory/dma/manager.c", "DMA_MANAGER_SOURCE",
                 """if (status != OS_EOK) {
        ERROR("DMAManagerPrepareCreate: AcquireGlobalHandle registry reference""",
                 """if (status == OS_EOK) {
        ERROR("DMAManagerPrepareCreate: AcquireGlobalHandle registry reference"""),
                ("grant a manager across groups", "kernel/handles/handle.c", "HANDLES_SOURCE",
                 "        case HandleTypeDMAManager:\n", ""),
            )
            for name, path, macro, before, after in mutations:
                original = (ROOT / path).read_text()
                assert original.count(before) == 1, name
                mutant = output / "mutant.c"
                mutant.write_text(original.replace(before, after))
                extra = [f'-D{macro}="{mutant}"', "-I" + str((ROOT / path).parent)]
                subprocess.run(flags + extra + sources + ["-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=20)
                if result.returncode == 0 or b"Assertion" not in result.stderr:
                    raise AssertionError(f"Mutation was not caught by an assertion: {name}: {result}")
                print("mutation rejected:", name)

        # Use production headers and the Vali compiler for all supported ABIs.
        compiler = os.environ.get("VALI_CLANG", "/usr/local/valicc/bin/clang")
        includes = ["-I" + str(ROOT / path) for path in (
            "kernel/include", "boot/include", "librt/libc/include", "librt/libos/include",
            "librt/libddk/include", "librt/libds/include", "librt/libgracht/include",
            "librt/libfdt/include", "librt/libacpi/source/include")]
        for arch in ("i386", "amd64", "aarch64"):
            target_flags = [compiler, "--target=" + arch + "-uml-vali", "-std=gnu11",
                            "-fms-extensions", "-ffreestanding", "-nostdlibinc", "-O2",
                            "-Wall", "-Werror", "-Wno-unused-function", "-Wno-self-assign",
                            "-Wno-unused-but-set-variable", "-Wno-address-of-packed-member",
                            "-D__OSCONFIG_ENABLE_MULTIPROCESSORS"]
            if arch == "aarch64":
                target_flags += ["-march=armv8-a", "-mno-outline-atomics",
                                 "-D__OSCONFIG_HAS_MMIO", "-D__OSCONFIG_DEVICETREE_SUPPORT"]
            else:
                target_flags += ["-D__OSCONFIG_ACPI_SUPPORT", "-D__OSCONFIG_HAS_LEGACY_PCI"]
            for source in ("kernel/memory/dma/domain.c", "kernel/memory/dma/address.c",
                           "kernel/memory/shm/device/attachment.c", "kernel/memory/shm/device/layout.c",
                           "kernel/memory/shm/device/device.c", "kernel/memory/shm/device/context.c",
                           "kernel/memory/dma/binding.c", "kernel/memory/dma/manager.c", "kernel/memory/dma/groups.c",
                           "kernel/memory/dma/events.c", "kernel/memory/dma/recovery.c", "kernel/api/dma_attachment.c",
                           "kernel/api/entry.c", "librt/libddk/dma/attachment.c",
                           "testing/dma_attachment_abi_test.c"):
                definitions = ["-DLIBC_KERNEL", "-D__LIBDS_KERNEL__"] if source.startswith("kernel/") else []
                subprocess.run(target_flags + includes + definitions + ["-c", str(ROOT / source),
                               "-o", str(output / "abi.o")], check=True)
            print("DMA attachment ABI and production compilation:", arch, "passed")


if __name__ == "__main__":
    main()
