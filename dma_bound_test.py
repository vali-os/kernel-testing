"""Exercise real DMA binding, SHM and syscall integration with host memory hooks."""
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
    with tempfile.TemporaryDirectory(prefix="vali-dma-bound-") as directory:
        output = Path(directory)
        shims = {name: source for name, source in SHIMS.items() if name != "memoryspace.h"}
        shims["threading.h"] = "#pragma once\ntypedef struct Thread Thread_t;\n" + shims["threading.h"]
        shims["ioset.h"] = (ROOT / "librt/libc/include/ioset.h").read_text()
        shims["debug.h"] = "#pragma once\n" + shims["debug.h"] + """
#ifdef DMA_ATTACHMENT_LOG_TEST
void TestDmaError(const char*, ...) __attribute__((format(printf, 1, 2)));
#undef ERROR
#define ERROR(...) TestDmaError(__VA_ARGS__)
#endif
"""
        shims["mutex.h"] = """#pragma once
#include <assert.h>
#include <pthread.h>
typedef struct { pthread_mutex_t Native; unsigned int Flags; int held; } Mutex_t;
#define MUTEX_FLAG_PLAIN 0
#define OS_MUTEX_INIT(flags) {PTHREAD_MUTEX_INITIALIZER, flags, 0}
static inline void MutexConstruct(Mutex_t* m, unsigned int flags)
{ assert(pthread_mutex_init(&m->Native, NULL) == 0); m->Flags=flags; m->held=0; }
static inline void MutexDestruct(Mutex_t* m)
{ assert(!m->held); assert(pthread_mutex_destroy(&m->Native) == 0); }
static inline void MutexLock(Mutex_t* m)
{ assert(pthread_mutex_lock(&m->Native) == 0); assert(!m->held); m->held=1; }
static inline void MutexUnlock(Mutex_t* m)
{ assert(m->held); m->held=0; assert(pthread_mutex_unlock(&m->Native) == 0); }
"""
        for name, source in shims.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-DDMA_ATTACHMENT_LOG_TEST", "-D__LIBDS_KERNEL__", "-fms-extensions", "-fasm-blocks",
                 "-DKERNELAPI=", "-DKERNELABI=", "-DSERVICEAPI=static inline", "-DSERVICEABI=",
                 "-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-parameter",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-pthread",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-Wl,--wrap=DMAAddressMappingRelease", "-Wl,--wrap=kmalloc",
                 "-Wl,--wrap=SHMDeviceMappingLink", "-Wl,--wrap=SHMDeviceContextRevoke", "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in
                  ("testing/include", "kernel", "kernel/include", "librt/libos/include")]
        flags += ["-isystem" + str(ROOT / path) for path in
                  ("librt/libds/include", "librt/libddk/include", "boot/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        sources = ["testing/dma_bound_test.c", "kernel/memory/dma/manager.c",
                   "kernel/memory/dma/binding.c", "kernel/memory/dma/recovery.c",
                   "kernel/memory/dma/groups.c", "kernel/memory/dma/events.c",
                   "kernel/memory/dma/direct.c", "kernel/memory/shm/device_context.c",
                   "kernel/memory/shm/device.c", "kernel/memory/shm/device_cleanup.c",
                   "kernel/memory/shm/device_attachment.c", "kernel/memory/shm/access.c",
                   "kernel/memory/shm/view.c", "kernel/memory/space/mapping.c",
                   "kernel/memory/dma/address.c", "testing/shm_access_futex.c",
                   "kernel/components/memory.c", "kernel/utils/memory_stack.c",
                   "librt/libds/hashtable.c", "librt/libds/queue.c", "librt/libds/list.c",
                   "librt/libds/rbtree.c"]
        binary = output / "bound"
        subprocess.run(flags + [str(ROOT / source) for source in sources] + ["-o", str(binary)], check=True)
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)
        if not args.mutation_check:
            return
        mutations = (
            ("skip operation context release", "kernel/memory/shm/device_attachment.c", None,
             "current->Setup >= SHMDeviceOperationSetupContext",
             "current->Setup > SHMDeviceOperationSetupContext"),
            ("skip operation mapping release", "kernel/memory/shm/device_attachment.c", None,
             "current->Setup >= SHMDeviceOperationSetupMapping",
             "current->Setup > SHMDeviceOperationSetupMapping"),
            ("forget operation admission", "kernel/memory/shm/device_attachment.c", None,
             "operation->Setup = SHMDeviceOperationSetupCounted;",
             "operation->Setup = SHMDeviceOperationSetupMapping;"),
            ("drop cancellation requested during a pass", "kernel/memory/dma/recovery.c", None,
             "        group->Stop.Phase = DMARecoveryStopCancelAgain;", "        // Mutant drops the rerun request."),
            ("treat repeated cancellation as idle", "kernel/memory/dma/recovery.c", None,
             "if (group->Stop.Phase != DMARecoveryStopIdle) {",
             "if (group->Stop.Phase == DMARecoveryStopCancelling) {"),
            ("allow overlapping stop passes", "kernel/memory/dma/recovery.c", None,
             "group->Gate.Operations != 0 || group->Stop.Phase != DMARecoveryStopIdle",
             "group->Gate.Operations != 0"),
            ("allow driver release during stop cleanup", "kernel/memory/dma/binding.c", None,
             "group->Stop.Phase == DMARecoveryStopCleaning ? OS_EBUSY : OS_EOK",
             "false ? OS_EBUSY : OS_EOK"),
            ("admit ordinary work after closure", "kernel/memory/dma/binding.c", None,
             "binding->State != DMABindingLive || binding->Group->Gate.State != DMARecoveryLive",
             "false"),
            ("ignore bound generation", "kernel/memory/dma/binding.c", None,
             "binding->Generation != parameters->Generation", "false"),
            ("ignore bound principal", "kernel/memory/dma/binding.c", None,
             "binding->Principal != (uint64_t)caller->GlobalHandleID", "false"),
            ("end operation in forked parent", "kernel/api/dma.c", "DMA_API_SOURCE",
             "if (status != OS_EFORKED) {", "if (true) {"),
            ("accept old stop ticket", "kernel/memory/dma/recovery.c", None,
             "group->Stop.Epoch != ticket->Epoch ||", ""),
            ("acknowledge promoted event using old sequence", "kernel/memory/dma/events.c", None,
             "eventState->Sequence != batch->Sequences[eventIndex]", "false"),
            ("copy uncertain bounce bytes back", "kernel/memory/shm/device_cleanup.c", None,
             "        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);",
             """        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);
        if (__SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Bounce.BounceMapping != 0) {
            __builtin_memcpy((void*)(mapping->Stream.Bounce.SourceMapping + mapping->Stream.Bounce.SourceOffset),
                (const void*)mapping->Stream.Bounce.BounceMapping, mapping->Length);
        }"""),
            ("complete cache for only submitted prefix", "kernel/memory/shm/device_cleanup.c", None,
             "SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);",
             "SHMDeviceCompleteForCpu(mapping, mapping->Stream.Offset, mapping->Stream.Length);"),
            ("skip idle mappings during confirmed cleanup", "kernel/memory/shm/device_attachment.c", None,
             "    status = SHMDeviceMappingCleanup(mapping, true);",
             "    status = __SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Length == 0 ? OS_EOK : SHMDeviceMappingCleanup(mapping, true);"),
            ("omit admitted work count", "kernel/memory/dma/binding.c", None,
             "            group->Gate.Operations++;", "            // Mutant omits admission count."),
            ("confirm after incomplete cancellation", "kernel/memory/dma/recovery.c", None,
             "} else if (group->Stop.CancellationError != OS_EOK) {", "} else if (false) {"),
            ("skip preparing-open drain", "kernel/memory/dma/binding.c", None,
             "        DMABindingEndOperation(current);", "        // Mutant leaves open counted."),
            ("omit idle peers from group stop", "kernel/memory/dma/recovery.c", None,
             "if (attachment->RecoveryGroup != group) {",
             "if (attachment->RecoveryGroup != group || attachment->Binding.Current == NULL) {"),
            ("repeat stopped cache work", "kernel/memory/shm/device_cleanup.c", None,
             "if (stopped && mapping->State != SHMDeviceMappingReleasingAfterStop) {", "if (stopped) {"),
            ("forget stopped cache progress", "kernel/memory/shm/device_cleanup.c", None,
             "if (mapping->State == SHMDeviceMappingReady) {",
             "if (true) {"),
            ("allow cleanup after manager loss", "kernel/memory/dma/binding.c", None,
             "if (group->Gate.ManagerClosed) {", "if (false) {"),
            ("lose confirmed release permission", "kernel/memory/shm/device_cleanup.c", None,
             "if (operation->Stopped || current->State == SHMDeviceMappingReleasingAfterStop) {",
             "if (current->State == SHMDeviceMappingReleasingAfterStop) {"),
            ("publish map after closure", "kernel/api/dma.c", "DMA_API_SOURCE",
             """    status = SHMDeviceOperationCheck(operation);
    if (status != OS_EOK) {
        return status;
    }

    // Trace before we publish the mapping.""",
             "    // Mutant publishes after closure.\n    // Trace before we publish the mapping."),
        )
        for name, path, macro, before, after in mutations:
            original = (ROOT / path).read_text()
            assert original.count(before) == (2 if name == "accept old stop ticket" else 1), name
            mutant = output / "mutant.c"
            mutant.write_text(original.replace(before, after))
            extra = ["-I" + str((ROOT / path).parent)]
            compiled_sources = [str(ROOT / source) for source in sources]
            if macro is not None:
                extra += [f'-D{macro}="{mutant}"']
            else:
                compiled_sources = [str(mutant) if source == str(ROOT / path) else source
                                    for source in compiled_sources]
            subprocess.run(flags + extra + compiled_sources + ["-o", str(binary)], check=True)
            completed = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=30)
            if completed.returncode == 0 or b"Assertion" not in completed.stderr:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("Bound DMA mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
