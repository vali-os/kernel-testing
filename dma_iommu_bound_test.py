"""Exercise translated domains through native bindings, grants and SHM operations."""
import argparse
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
from dma_bound_test import ROOT, build_host_fixture


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mutation-check", action="store_true")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="vali-dma-iommu-bound-") as directory:
        output = Path(directory)
        flags, sources, environment = build_host_fixture(output)
        sources[0] = "testing/dma_iommu_bound_test.c"
        sources += ["testing/dma_iommu_sim.c", "kernel/memory/shm/shm.c"]
        flags += ["-DDMA_TEST_REAL_DEVICE_BUFFER", "-Wno-sign-compare", "-Wl,--wrap=MemorySpaceMap",
                  "-Wl,--wrap=MemorySpaceUnmap", "-Wl,--wrap=GetCurrentMemorySpace", "-Wl,--wrap=kfree", "-Wl,--wrap=DMAIOMMUUnitCreate",
                  "-Wl,--wrap=DMAAddressSpaceAcquire", "-Wl,--wrap=DMAAddressSpaceCreateTranslated", "-Wl,--wrap=DMAAddressMappingSegments",
                  "-Wl,--wrap=DMARecoveryGroupAcquire", "-Wl,--wrap=DMAConnectionPrepare",
                  "-Wl,--wrap=DMAAddressSpaceCheck"]
        binary = output / "iommu-bound"
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=60)
        if args.mutation_check:
            run_mutations(output, flags, sources, environment)


def run_mutations(output, flags, sources, environment):
    """Compile each mutant separately and require an assertion failure."""
    mutations = (
        ("hide IRQ failure from healthy reset peer", "kernel/memory/dma/unit.c",
         "!atomic_compare_exchange_weak(&g_failureSequence, &sequence, sequence + 1)", "false"),
        ("accept partial domain membership", "kernel/memory/dma/binding.c",
         "domain->Members != domain->DeviceCount", "false"),
        ("forget shared domain principal", "kernel/memory/dma/binding.c",
         "prepared->Attachment->Domain->OwnerSecurityID = binding->OwnerSecurityID;",
         "prepared->Attachment->Domain->OwnerSecurityID = 0;"),
        ("release identities on busy destruction", "kernel/memory/dma/domain.c",
         "if (released->Members != 0) {", "if (false) {"),
        ("allow another domain principal", "kernel/memory/dma/binding.c",
         "domain->OwnerSecurityID != 0 && domain->OwnerSecurityID != ownerSecurityID", "false"),
        ("forget the context space reference", "kernel/memory/dma/binding.c",
         "status = DMAAddressSpaceAcquire(configuration->AddressSpace);", "status = OS_EOK;"),
        ("reopen a failed reset peer", "kernel/memory/dma/binding.c",
         "member->RecoveryGroup == attachment->RecoveryGroup && member->Domain != NULL", "false"),
        ("lose destruction retry reference", "kernel/memory/dma/address.c",
         "atomic_store(&released->References, 1);", "atomic_store(&released->References, 0);"),
        ("reuse closing hardware ID identities", "kernel/memory/dma/domain.c",
         "domain->Unit == added->Unit && __SharesHardwareID(domain, added)",
         "domain->State != DMATranslatedDomainClosing && domain->Unit == added->Unit && __SharesHardwareID(domain, added)"),
        ("publish a failed domain", "kernel/memory/dma/domain.c",
         "status = DMAAddressSpaceCheck(domain->AddressSpace);", "status = OS_EOK;"),
        ("skip domain callback propagation", "kernel/memory/dma/domain.c",
         "DMARecoveryBlockFailedUnits(manager)", "OS_EOK"),
        ("leak unpublished manager reference", "kernel/memory/dma/domain.c",
         "kfree(domain);\n        DMAManagerRelease(&manager);", "kfree(domain);"),
        ("skip final context setup health", "kernel/memory/dma/binding.c",
         "status = DMABindingCheckOperation(binding);", "status = OS_EOK;"),
        ("skip Running cancellation propagation", "kernel/memory/dma/binding.c",
         "DMARecoveryCancel(binding->Group);\n    __BlockFailedUnitGroupsForBinding(binding->Manager);",
         "DMARecoveryCancel(binding->Group);"),
        ("publish failed attachment", "kernel/memory/dma/manager.c",
         "status = __CheckAttachmentPublicationLocked(prepared);", "status = OS_EOK;"),
        ("select another domain space", "kernel/memory/dma/binding.c",
         "configuration->AddressSpace = binding->Attachment->Domain->AddressSpace;",
         "configuration->AddressSpace = ((DMATranslatedDomain_t*)binding->Manager->Domains.head->value)->AddressSpace;"),
        ("ignore restricted view offset", "kernel/memory/shm/device/device.c",
         "offset += source.Offset;", "offset += 0;"),
        ("ignore backing buffer offset", "kernel/memory/shm/device/layout.c",
         """        buffer->Pages,
        buffer->Offset + offset,""",
         """        buffer->Pages,
        offset,"""),
        # Keep the local referenced so -Werror does not reject the mutant
        # before the test can check its incorrect bounce fallback.
        ("bounce translated placement failures", "kernel/memory/shm/device/layout.c",
         "granularity != 0 || kind != SHMDeviceMappingStreamingBounce",
         "(granularity != 0 && false) || kind != SHMDeviceMappingStreamingBounce"),
        ("apply device boundary to physical pool", "kernel/memory/shm/device/device.c",
         "plan->PhysicalBoundary = 0;", "plan->PhysicalBoundary = (size_t)requirements->Boundary;"),
        ("omit IOVA boundary alignment", "kernel/memory/shm/device/device.c",
         "plan->MapRequirements.AddressAlignment = requirements->Boundary;", "plan->MapRequirements.AddressAlignment = 0;"),
        ("expose unzeroed rounded storage", "kernel/memory/shm/shm.c",
         "MAPPING_USERSPACE | MAPPING_PERSISTENT | MAPPING_COMMIT | MAPPING_CLEAN;",
         "MAPPING_USERSPACE | MAPPING_PERSISTENT | MAPPING_COMMIT;"),
        ("round shared allocation only to CPU pages", "kernel/memory/shm/device/device.c",
         "allocationUnit = granularity > pageSize ? (size_t)granularity : pageSize;", "allocationUnit = pageSize;"),
        ("ignore shared length overflow", "kernel/memory/shm/device/device.c",
         "requirements->Length > SIZE_MAX - (allocationUnit - 1)", "false"),
        ("skip final segment health check", "kernel/memory/shm/device/device.c",
         "if (SHMDeviceOperationCheck(operation) != OS_EOK) {", "if (false) {"),
        ("cancel only one failed-unit domain", "kernel/memory/dma/recovery.c",
         "attachment->Domain == NULL || attachment->Domain->Unit != unit",
         "attachment->Domain == NULL || attachment->Domain->Unit != unit || attachment->Domain != manager->Domains.head->value"),
        ("drop failure events with snapshot error", "kernel/memory/dma/recovery.c",
         "DMARecoveryBlockLocked(manager, group);\n        groups[(*count)++] = group;",
         "if (DMARecoveryGroupAcquire(group) != OS_EOK) { continue; }\n"
         "        DMARecoveryGroupRelease(&group);\n        group = attachment->RecoveryGroup;\n"
         "        DMARecoveryBlockLocked(manager, group);\n        groups[(*count)++] = group;"),
        ("skip recovery at operation end", "kernel/memory/dma/recovery.c",
         "status = DMARecoveryCheckFailures(binding->Manager);", "status = OS_EOK;"),
        ("skip recovery after stop cleanup", "kernel/memory/dma/recovery.c",
         "propagationStatus = DMARecoveryBlockFailedUnits(manager);", "propagationStatus = OS_EOK;"),
        ("skip failure cancellation retry on event read", "kernel/memory/dma/events.c",
         "DMARecoveryBlockFailedUnits(manager)", "OS_EOK"),
        ("free source pages before invalidation", "kernel/memory/shm/device/cleanup.c",
         "status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (status != OS_EOK) {",
         "status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (false) {"),

        ("admit an unconnected attachment", "kernel/memory/dma/manager.c",
         "selected->ConnectionState != DMAConnectionConnected", "false"),
        ("publish an unconnected binding", "kernel/memory/dma/binding.c",
         "domain->ConnectionState != DMAConnectionConnected", "false"),
        ("omit connection space reference", "kernel/memory/dma/connection.c",
         "status = DMAAddressSpaceAcquire(space);", "status = OS_EOK;"),
        ("retry a failed connect", "kernel/memory/dma/connection.c",
         "domain->ConnectionState != DMAConnectionPrepared ||", "false ||"),
        ("reconnect an already connected domain", "kernel/memory/dma/connection.c",
         "if (domain->ConnectionState == DMAConnectionConnected)", "if (false)"),
        ("publish before connect completes", "kernel/memory/dma/connection.c",
         "domain->ConnectionState = DMAConnectionConnecting;",
         "domain->ConnectionState = DMAConnectionConnected;"),
        ("publish after manager close", "kernel/memory/dma/connection.c",
         "manager->State != DMAManagerLive || domain->State != DMATranslatedDomainReady", "false"),
        ("skip final connection health", "kernel/memory/dma/connection.c",
         """    } else {
        status = DMAAddressSpaceCheck(domain->ConnectionSpace);""",
         """    } else {
        status = OS_EOK;"""),
        ("skip connection failure propagation", "kernel/memory/dma/connection.c",
         "DMARecoveryBlockFailedUnits(manager)", "OS_EOK"),
        ("refuse prepared cleanup on failed unit", "kernel/memory/dma/connection.c",
         "domain->ConnectionState != DMAConnectionPrepared &&", "true &&"),
        ("omit connection reference release", "kernel/memory/dma/connection.c",
         """    domain->Connection = NULL;
    MutexUnlock(&unit->Lock);

    DMAAddressSpaceRelease(&domain->ConnectionSpace);""",
         """    domain->Connection = NULL;
    MutexUnlock(&unit->Lock);

"""),
        ("ignore unit hardware ID reservations", "testing/dma_iommu_sim.c",
         "__FindHardwareID(sim, hardwareIDs[i], &index) != NULL",
         "__FindHardwareID(sim, hardwareIDs[i], &index) != NULL && false"),
        ("ignore cached hardware ID routing", "testing/dma_iommu_sim.c",
         "if (!route->Cached)", "if (true)"),
        ("keep hardware ID cache after completion", "testing/dma_iommu_sim.c",
         "hardwareID->Cached = false;", "// Mutant leaves the old root cached."),
        ("clear hardware ID cache before completion", "testing/dma_iommu_sim.c",
         "connection->HardwareIDs[connection->Removed].Installed = false;",
         """connection->HardwareIDs[connection->Removed].Installed = false;
        if (connection->HardwareIDs[connection->Removed].Cached) {
            connection->HardwareIDs[connection->Removed].Cached = false;
            sim->Stats.CachedHardwareIDs--;
        }"""),
        ("restart disconnect deadline", "testing/dma_iommu_sim.c",
         "status = __Submit(sim, &connection->Disconnect);",
         "deadline = sim->Now + timeoutNs; status = __Submit(sim, &connection->Disconnect);"),
    )
    binary = output / "mutant"
    for name, path, before, after in mutations:
        original = (ROOT / path).read_text()
        assert original.count(before) == (3 if name == "skip domain callback propagation" else 1), name
        mutant = output / "mutant.c"
        mutant.write_text(original.replace(before, after))
        compiled = [str(mutant) if source == path else str(ROOT / source) for source in sources]
        subprocess.run(flags + ["-I" + str((ROOT / path).parent)] + compiled + ["-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, env=environment, timeout=60)
        sanitizer_fault = b"AddressSanitizer" in result.stderr or b"runtime error:" in result.stderr
        # Terminal failure cases run in children. The fixture reports the raw
        # wait status and exits with 1, preserving the child's diagnostic.
        child = re.search(rb"^Translated DMA child \d+ failed \(wait status (\d+)\)$", result.stderr, re.M)
        child_aborted = (child is not None and os.WIFSIGNALED(int(child[1])) and
                         os.WTERMSIG(int(child[1])) == signal.SIGABRT)
        aborted = result.returncode == -signal.SIGABRT or (result.returncode == 1 and child_aborted)
        if not aborted or b"Assertion" not in result.stderr or sanitizer_fault:
            raise AssertionError(f"Mutation did not fail at an assertion: {name}: {result.stderr.decode()}")
        print("Translated binding mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
