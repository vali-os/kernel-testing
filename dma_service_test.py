# Copyright 2026, Philip Meulengracht
#
# This program is free software : you can redistribute it and / or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation ? , either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program. If not, see <http://www.gnu.org/licenses/>.
#
#
# Exercise deviced registration, binding and recovery against the native kernel.

"""Exercise deviced DMA lifecycle against the real kernel manager and binding code."""
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
    with tempfile.TemporaryDirectory(prefix="vali-dma-service-") as directory:
        output = Path(directory)
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols/device.gr"), "--out", directory,
                        "--lang-c", "--server"], check=True)
        shims = dict(SHIMS)
        shims["debug.h"] = "#pragma once\n" + shims["debug.h"] + """
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
        shims["ddk/ddkdefs.h"] = '#pragma once\n#include <os/osdefs.h>\n#define DDKDECL(ty, decl) ty decl\n'
        shims["internal/_tls.h"] = "#pragma once\n#include <os/types/async.h>\nstruct ServiceTestTls { OSAsyncContext_t* async_context; };\nstatic struct ServiceTestTls g_serviceTls;\nstatic inline struct ServiceTestTls* __tls_current(void) { return &g_serviceTls; }\n"
        shims["internal/_syscalls.h"] = "#pragma once\n#include <os/osdefs.h>\noserr_t Syscall_HandleDestroy(uuid_t);\n"
        # Non-DMA registration codecs are outside this combined service/kernel fixture.
        shims["ddk/convert.h"] = "#pragma once\n#include <ddk/devices/device.h>\nstruct sys_device;\nDevice_t* from_sys_device(const struct sys_device*);\n"
        shims["ddk/utils.h"] = "#pragma once\n#include <debug.h>\n"
        for name, source in shims.items():
            path = output / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(source)
        flags = [os.environ.get("CC", "clang"), "-std=gnu11", "-D_POSIX_C_SOURCE=200809L",
                 "-DVALI", "-DMOLLENOS", "-D__LIBDS_KERNEL__", "-DKERNELAPI=", "-DKERNELABI=",
                 "-DSERVICEAPI=static inline", "-DSERVICEABI=", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unused-parameter", "-Wno-sign-compare", "-Wno-unused-function",
                 "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                 "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-g", "-I" + directory]
        flags += ["-I" + str(ROOT / path) for path in (
            "testing/include", "kernel", "kernel/include", "librt/libos/include",
            "librt/libds/include", "librt/libddk/include", "services/deviced", "services/deviced/include", "librt/libgracht/include",
            "librt/libfdt/include")]
        flags += ["-idirafter", str(ROOT / "librt/libc/include")]
        sources = [str(ROOT / path) for path in (
            "testing/dma_service_test.c", "librt/libds/hashtable.c",
            "librt/libds/queue.c", "librt/libds/list.c", "librt/libds/rbtree.c")]
        binary = output / "native"
        environment = dict(os.environ)
        environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
        subprocess.run(flags + sources + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=20)
        if args.mutation_check:
            mutations = [
                ("truncate control principal", "dma.c", "DMA_SERVICE_SOURCE",
                 "registered->DriverPrincipal != principal", "(uint32_t)registered->DriverPrincipal != (uint32_t)principal"),
                ("skip live-binding admission", "dma.c", "DMA_SERVICE_SOURCE",
                 "if (!enableDma) {", "if (true) {"),
                ("forget retired request", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "entry->Binding.Idle = false;", "entry->Binding.Idle = false; entry->Binding.RequestId = 0;"),
                ("release idle before stop", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "entry->Host->State != DmDmaHostReady && entry->Host->State != DmDmaHostStopped", "false"),
                ("skip removal admission", "dma.c", "DMA_SERVICE_SOURCE",
                 "registered == NULL || registered->State != DmDeviceRemoving", "registered == NULL"),
                ("split host grouping", "dma.c", "DMA_SERVICE_SOURCE",
                 "parameters.RequesterGroup = description->HostId;\n    parameters.ResetGroup = description->HostId;",
                 "parameters.RequesterGroup = entry->Device;\n    parameters.ResetGroup = entry->Device;"),
                ("drop trusted route limit", "dma.c", "DMA_SERVICE_SOURCE",
                 "parameters.AddressLimit = description->AddressLimit;", "parameters.AddressLimit = UINT64_MAX;"),
                ("skip repeated-event sleep", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "if (eventsRead) {", "if (eventsRead && false) {"),
                ("accept stale idle", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "events[i].Generation == entry->Binding.Result.Generation", "true"),
                ("confirm before physical stop", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "if (host->State == DmDmaHostStopPrepared) {", "if (false) {"),
                ("repeat successful physical stop", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "if (host->State == DmDmaHostStopPrepared) {", "if (true) {"),
                ("restart stop on duplicate event", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "if (entry->Host->State == DmDmaHostReady) {", "if (true) {"),
                ("retry unsupported physical stop", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "host->State = DmDmaHostStopUnsupported;", "host->State = DmDmaHostStopPrepared;"),
                ("reopen stopped host", "dma-recovery.c", "DMA_SERVICE_RECOVERY_SOURCE",
                 "host->State = DmDmaHostStopped;", "host->State = DmDmaHostReady;"),
            ]
            for name, filename, define, before, after in mutations:
                source = (ROOT / "services/deviced/core" / filename).read_text()
                if source.count(before) != 1:
                    raise RuntimeError(f"mutation anchor changed: {name}")
                mutant = output / "dma-mutant.c"
                mutant.write_text(source.replace(before, after))
                mutation_flags = ["-I" + str(ROOT / "services/deviced/core"), f'-D{define}="{mutant}"']
                subprocess.run(flags + mutation_flags + sources + ["-o", str(binary)], check=True)
                result = subprocess.run([str(binary)], env=environment, capture_output=True, text=True, timeout=20)
                if result.returncode == 0 or "Assertion" not in result.stderr:
                    raise RuntimeError(f"mutation was not rejected by an assertion: {name}\n{result.stderr}")
                print(f"DMA service mutation rejected: {name}")



if __name__ == "__main__":
    main()
