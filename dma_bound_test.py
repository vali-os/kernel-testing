"""Exercise real DMA binding, SHM and syscall integration with host memory hooks."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile
from handle_lifetime_test import SHIMS

ROOT = Path(__file__).resolve().parents[1]


def build_host_fixture(output):
    """Share the checked handle/SHM fixture and production sources with IOMMU tests."""
    shims = {name: source for name, source in SHIMS.items() if name != "memoryspace.h"}
    shims["threading.h"] = "#pragma once\ntypedef struct Thread Thread_t;\n" + shims["threading.h"]
    shims["ioset.h"] = (ROOT / "librt/libc/include/ioset.h").read_text()
    shims["debug.h"] = "#pragma once\n" + shims["debug.h"] + """
#ifdef DMA_ATTACHMENT_LOG_TEST
void TestDmaError(const char*, ...) __attribute__((format(printf, 1, 2)));
#undef ERROR
#define ERROR(...) TestDmaError(__VA_ARGS__)
#endif
#ifdef DMA_WARNING_LOG_TEST
void TestDmaWarning(const char*, ...) __attribute__((format(printf, 1, 2)));
#undef WARNING
#define WARNING(...) TestDmaWarning(__VA_ARGS__)
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
    shims["spinlock.h"] = """#pragma once
#include <assert.h>
#include <stdatomic.h>
typedef struct { atomic_int held; } Spinlock_t;
#define OS_SPINLOCK_INIT {0}
static inline void SpinlockConstruct(Spinlock_t* l) { atomic_init(&l->held, 0); }
static inline void SpinlockAcquireIrq(Spinlock_t* l) {
    int expected = 0;
    while (!atomic_compare_exchange_weak(&l->held, &expected, 1)) { expected = 0; }
}
static inline void SpinlockReleaseIrq(Spinlock_t* l) { assert(atomic_exchange(&l->held, 0) == 1); }
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
             "-Wl,--wrap=DMAAddressMappingInstall", "-Wl,--wrap=SystemMemoryReleaseAllocation",
             "-Wl,--wrap=SHMDeviceMappingLink", "-Wl,--wrap=SHMDeviceContextRevoke", "-I" + str(output)]
    flags += ["-I" + str(ROOT / path) for path in
              ("testing/include", "kernel", "kernel/include", "librt/libos/include")]
    flags += ["-isystem" + str(ROOT / path) for path in
              ("librt/libds/include", "librt/libddk/include", "boot/include")]
    flags += ["-idirafter", str(ROOT / "librt/libc/include")]
    sources = ["testing/dma_bound_test.c", "kernel/memory/dma/manager.c",
               "kernel/memory/dma/binding.c", "kernel/memory/dma/recovery.c",
               "kernel/memory/dma/region_operation.c",
               "kernel/memory/shm/region.c", "kernel/memory/shm/region_usage.c",
               "kernel/memory/shm/device/region_mapping.c",
               "kernel/memory/shm/device/region_session.c", "kernel/memory/shm/device/region_use.c",
               "kernel/memory/dma/domain.c", "kernel/memory/dma/connection.c",
               "kernel/memory/dma/groups.c", "kernel/memory/dma/events.c",
               "kernel/memory/dma/direct.c", "kernel/memory/shm/device/context.c",
               "kernel/memory/shm/device/device.c", "kernel/memory/shm/device/layout.c",
               "kernel/memory/shm/device/cleanup.c",
               "kernel/memory/shm/device/attachment.c", "kernel/memory/shm/access.c",
               "kernel/memory/shm/view.c", "kernel/memory/space/mapping.c",
               "kernel/memory/dma/address.c", "testing/shm_access_futex.c",
               "kernel/memory/dma/unit.c", "kernel/memory/dma/translated.c",
               "kernel/memory/dma/iova.c",
               "kernel/components/memory.c", "kernel/utils/memory_stack.c",
               "librt/libds/hashtable.c", "librt/libds/queue.c", "librt/libds/list.c",
               "librt/libds/rbtree.c"]
    environment = dict(os.environ)
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    return flags, sources, environment


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-bound-") as directory:
        output = Path(directory)
        flags, sources, environment = build_host_fixture(output)
        binary = output / "bound"
        subprocess.run(flags + [str(ROOT / source) for source in sources] + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=30)
        if not args.mutation_check:
            return
        mutations = (
            ("make prepared SHM mappings usable", "kernel/memory/shm/device/layout.c", None,
             "mapping->State = SHMDeviceMappingPreparing;",
             "mapping->State = SHMDeviceMappingReady;"),
            ("skip address installation", "kernel/memory/shm/device/device.c", None,
             "status = DMAAddressMappingInstall(mapping->AddressMapping);",
             "status = OS_EOK;"),
            ("install before linking SHM mapping", "kernel/memory/shm/device/device.c", None,
             "    SHMDeviceMappingLink(mapping);",
             "    status = DMAAddressMappingInstall(mapping->AddressMapping);\n    SHMDeviceMappingLink(mapping);"),
            ("ignore allocation layout constraints", "kernel/memory/shm/device/layout.c", None,
             "if (allocationRequirements != NULL) {", "if (false) {"),
            ("ignore installation failure", "kernel/memory/shm/device/device.c", None,
             'if (status != OS_EOK) {\n        ERROR("__MapBuffer: DMAAddressMappingInstall',
             'if (false) {\n        ERROR("__MapBuffer: DMAAddressMappingInstall'),
            ("install after pre-install cancellation", "kernel/memory/shm/device/device.c", None,
             """    SHMDeviceMappingLink(mapping);
    status = SHMDeviceOperationCheck(operation);
    if (status != OS_EOK) {
        goto abandon;
    }""",
             "    SHMDeviceMappingLink(mapping);"),
            ("publish SHM mapping after installation cancellation", "kernel/memory/shm/device/device.c", None,
             """              source.GlobalHandleID, status);
        goto abandon;
    }

    status = SHMDeviceOperationCheck(operation);
    if (status != OS_EOK) {
        goto abandon;
    }
""",
             """              source.GlobalHandleID, status);
        goto abandon;
    }
"""),
            ("release a partially published mapping normally", "kernel/api/dma.c", "DMA_API_SOURCE",
             "cleanupStatus = SHMRecordCleanup(record);",
             "cleanupStatus = SHMRecordRelease(record);"),
            ("defer publication cleanup to handle destruction", "kernel/api/dma.c", "DMA_API_SOURCE",
             "cleanupStatus = SHMRecordCleanup(record);",
             "cleanupStatus = OS_EOK;"),
            ("end parent operation before publication cleanup", "kernel/api/dma.c", "DMA_API_SOURCE",
             "cleanupStatus = SHMRecordCleanup(record);",
             "SHMDeviceOperationEnd(&operation);\n    cleanupStatus = SHMRecordCleanup(record);"),
            ("hide synchronous record cleanup failure", "kernel/api/shm.c", "SHM_API_SOURCE",
             "return __ReleaseObject(record, record->Ops->Cleanup);",
             "__ReleaseObject(record, record->Ops->Cleanup);\n    return OS_EOK;"),
            ("accept missing operation token", "kernel/memory/shm/device/attachment.c", None,
             "if (operation == NULL) {\n        return false;",
             "if (operation == NULL) {\n        return true;"),
            ("skip operation context release", "kernel/memory/shm/device/attachment.c", None,
             "current->Setup >= SHMDeviceOperationSetupContext",
             "current->Setup > SHMDeviceOperationSetupContext"),
            ("skip operation mapping release", "kernel/memory/shm/device/attachment.c", None,
             "current->Setup >= SHMDeviceOperationSetupMapping",
             "current->Setup > SHMDeviceOperationSetupMapping"),
            ("forget operation admission", "kernel/memory/shm/device/attachment.c", None,
             "operation->Setup = SHMDeviceOperationSetupCounted;",
             "operation->Setup = SHMDeviceOperationSetupMapping;"),
            ("drop cancellation requested during a pass", "kernel/memory/dma/recovery.c", None,
             "        group->Stop.Phase = DMARecoveryStopCancelAgain;", "        // Mutant drops the rerun request."),
            ("treat repeated cancellation as idle", "kernel/memory/dma/recovery.c", None,
             "if (group->Stop.Phase != DMARecoveryStopIdle) {",
             "if (group->Stop.Phase == DMARecoveryStopCancelling) {"),
            ("allow overlapping stop passes", "kernel/memory/dma/recovery.c", None,
             "group->Access.Operations != 0 || group->Stop.Phase != DMARecoveryStopIdle",
             "group->Access.Operations != 0"),
            ("allow driver release during stop cleanup", "kernel/memory/dma/binding.c", None,
             "group->Access.Cleaning ? OS_EBUSY : OS_EOK",
             "false ? OS_EBUSY : OS_EOK"),
            ("admit ordinary work after closure", "kernel/memory/dma/binding.c", None,
             "binding->State != DMABindingLive || binding->Group->Access.State != DMARecoveryLive",
             "false"),
            ("ignore bound generation", "kernel/memory/dma/binding.c", None,
             "binding->Generation != parameters->Generation", "false"),
            ("ignore bound principal", "kernel/memory/dma/binding.c", None,
             "binding->OwnerSecurityID != (uint64_t)caller->GlobalHandleID", "false"),
            ("end operation in forked parent", "kernel/api/dma.c", "DMA_API_SOURCE",
             "if (status != OS_EFORKED) {", "if (true) {"),
            ("accept old stop ticket", "kernel/memory/dma/recovery.c", None,
             "group->Stop.Round != ticket->Epoch ||", ""),
            ("acknowledge promoted event using old sequence", "kernel/memory/dma/events.c", None,
             "eventState->Sequence != batch->Sequences[eventIndex]", "false"),
            ("copy uncertain bounce bytes back", "kernel/memory/shm/device/cleanup.c", None,
             "        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);",
             """        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);
        if (__SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Bounce.BounceMapping != 0) {
            __builtin_memcpy((void*)(mapping->Stream.Bounce.SourceMapping + mapping->Stream.Bounce.SourceOffset),
                (const void*)mapping->Stream.Bounce.BounceMapping, mapping->Length);
        }"""),
            ("complete cache for only submitted prefix", "kernel/memory/shm/device/cleanup.c", None,
             "SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);",
             "SHMDeviceCompleteForCpu(mapping, mapping->Stream.Offset, mapping->Stream.Length);"),
            ("skip idle mappings during confirmed cleanup", "kernel/memory/shm/device/attachment.c", None,
             "    status = SHMDeviceMappingCleanup(mapping, true);",
             "    status = __SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Length == 0 ? OS_EOK : SHMDeviceMappingCleanup(mapping, true);"),
            ("omit admitted work count", "kernel/memory/dma/binding.c", None,
             "        status = DMARecoveryAddOperationLocked(&group->Access);", "        status = OS_EOK; // Mutant omits admission count."),
            ("confirm after incomplete cancellation", "kernel/memory/dma/recovery.c", None,
             "if (group->Stop.CancellationError != OS_EOK) {", "if (false) {"),
            ("skip preparing-open drain", "kernel/memory/dma/binding.c", None,
             "        DMABindingEndOperation(current);", "        // Mutant leaves open counted."),
            ("omit idle peers from group stop", "kernel/memory/dma/recovery.c", None,
             "if (attachment->RecoveryGroup != group) {",
             "if (attachment->RecoveryGroup != group || attachment->Binding.Current == NULL) {"),
            ("repeat stopped cache work", "kernel/memory/shm/device/cleanup.c", None,
             "if (stopped && mapping->State != SHMDeviceMappingReleasingAfterStop) {", "if (stopped) {"),
            ("forget stopped cache progress", "kernel/memory/shm/device/cleanup.c", None,
             "if (mapping->State == SHMDeviceMappingReady) {",
             "if (true) {"),
            ("allow cleanup after manager loss", "kernel/memory/dma/binding.c", None,
             "if (group->Access.ManagerClosed) {", "if (false) {"),
            ("lose confirmed release permission", "kernel/memory/shm/device/cleanup.c", None,
             "if (operation->Stopped || current->State == SHMDeviceMappingReleasingAfterStop) {",
             "if (current->State == SHMDeviceMappingReleasingAfterStop) {"),
            ("return bounce units while their view is mapped", "kernel/memory/shm/device/cleanup.c", None,
             "        \"bounce\"\n    );\n    if (status != OS_EOK) {\n        return status;\n    }",
             "        \"bounce\"\n    );\n    if (status != OS_EOK) {\n        (void)__ReturnBounceUnits(context, bounce);\n        return status;\n    }"),
            ("clear a view address after a failed unmap", "kernel/memory/shm/device/cleanup.c", None,
             "        return status;\n    }\n    *view = 0;",
             "        *view = 0;\n        return status;\n    }\n    *view = 0;"),
            ("free unit records while units are allocated", "kernel/memory/shm/device/cleanup.c", None,
             "    status = __ReturnBounceUnits(context, bounce);\n    if (status != OS_EOK) {\n        return status;\n    }",
             "    status = __ReturnBounceUnits(context, bounce);\n    if (status != OS_EOK) {\n        kfree(bounce->Units);\n"
             "        bounce->Units = NULL;\n        bounce->UnitCount = 0;\n        return status;\n    }"),
            ("log only the first direct pool shortage as an error", "kernel/memory/shm/device/layout.c", None,
             "    if (status != OS_EOOM || !bounce->Translated) {", "    if (status != OS_EOOM) {"),
            ("log a pool shortage as a syscall error", "kernel/api/dma.c", "DMA_API_SOURCE",
             "        if (status == OS_EBUFFER || status == OS_EOOM) {", "        if (false) {"),
            ("hide every failed map call in the syscall layer", "kernel/api/dma.c", "DMA_API_SOURCE",
             "        if (status == OS_EBUFFER || status == OS_EOOM) {", "        if (true) {"),
            ("log a refused direct-only map as a syscall error", "kernel/api/dma.c", "DMA_API_SOURCE",
             "        } else if (kind == SHMDeviceMappingStreamingDirect &&",
             "        } else if (false &&"),
            ("log a refused page layout as a prepare error", "kernel/memory/dma/address.c", None,
             "        if (status == OS_ENOENT || status == OS_ENOTSUPPORTED) {",
             "        if (false) {"),
            ("skip the first direct bounce report", "kernel/memory/shm/device/layout.c", None,
             "    } else if (layout.UsesBounce) {\n        __ReportDirectBounce(",
             "    } else if (false) {\n        __ReportDirectBounce("),
            ("finish mapping cleanup after a failed bounce release", "kernel/memory/shm/device/cleanup.c", None,
             "failed: %u\", mapping, status);\n            return status;", "failed: %u\", mapping, status);"),
            ("repeat device access removal on a cleanup retry", "kernel/memory/shm/device/cleanup.c", None,
             "if (mapping->AddressMapping != NULL) {\n        status = __RemoveDeviceAccess(",
             "if (true) {\n        status = __RemoveDeviceAccess("),
            ("return quarantined units with a second release", "kernel/memory/shm/device/layout.c", None,
             "    status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    if (status != OS_EOK) {",
             "    status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    if (status != OS_EOK) {\n"
             "        status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    }\n    if (status != OS_EOK) {"),
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
            # A sanitizer crash or an arbitrary nonzero exit is not evidence
            # that the regression assertion detected the changed behavior.
            sanitizer_fault = b"AddressSanitizer" in completed.stderr or b"runtime error:" in completed.stderr
            if completed.returncode != -signal.SIGABRT or b"Assertion" not in completed.stderr or sanitizer_fault:
                raise AssertionError(f"Mutation did not fail an assertion: {name}: {completed.stderr.decode()}")
            print("Bound DMA mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
