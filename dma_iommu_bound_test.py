"""Exercise translated domains through native bindings, grants and SHM operations."""
import argparse
from pathlib import Path
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
        sources += ["testing/dma_iommu_sim.c", "testing/dma_iommu_routing.c", "kernel/memory/shm/shm.c"]
        flags += ["-DDMA_TEST_REAL_DEVICE_BUFFER", "-Wno-sign-compare", "-Wl,--wrap=MemorySpaceMap",
                  "-Wl,--wrap=MemorySpaceUnmap", "-Wl,--wrap=GetCurrentMemorySpace", "-Wl,--wrap=kfree", "-Wl,--wrap=DMAIOMMUUnitCreate",
                  "-Wl,--wrap=DMAAddressSpaceAcquire", "-Wl,--wrap=DMAAddressSpaceCreateTranslated", "-Wl,--wrap=DMAAddressMappingSegments",
                  "-Wl,--wrap=DMARecoveryGroupAcquire"]
        binary = output / "iommu-bound"
        subprocess.run(flags + [str(ROOT / path) for path in sources] + ["-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True, env=environment, timeout=60)
        if args.mutation_check:
            run_mutations(output, flags, sources, environment)


def run_mutations(output, flags, sources, environment):
    """Compile each mutant separately and require an assertion failure."""
    mutations = (
        ("accept partial domain membership", "kernel/memory/dma/binding.c",
         "domain->Members != domain->DeviceCount", "false"),
        ("forget shared domain principal", "kernel/memory/dma/binding.c",
         "prepared->Attachment->Domain->Principal = binding->Principal;",
         "prepared->Attachment->Domain->Principal = 0;"),
        ("release identities on busy destruction", "kernel/memory/dma/domain.c",
         "if (released->Members != 0) {", "if (false) {"),
        ("allow another domain principal", "kernel/memory/dma/binding.c",
         "domain->Principal != 0 && domain->Principal != principal", "false"),
        ("forget the context space reference", "kernel/memory/dma/binding.c",
         "status = DMAAddressSpaceAcquire(configuration.AddressSpace);", "status = OS_EOK;"),
        ("reopen a failed reset peer", "kernel/memory/dma/binding.c",
         "member->RecoveryGroup == attachment->RecoveryGroup && member->Domain != NULL", "false"),
        ("lose destruction retry reference", "kernel/memory/dma/address.c",
         "atomic_store(&released->References, 1);", "atomic_store(&released->References, 0);"),
        ("reuse closing requester identities", "kernel/memory/dma/domain.c",
         "if (domain->Unit != added->Unit) {",
         "if (domain->State == DMATranslatedDomainClosing || domain->Unit != added->Unit) {"),
        ("publish a failed domain", "kernel/memory/dma/domain.c",
         "status = DMAAddressSpaceCheck(domain->AddressSpace);", "status = OS_EOK;"),
        ("skip domain callback propagation", "kernel/memory/dma/domain.c",
         "DMARecoveryCheckDomains(manager)", "OS_EOK"),
        ("leak unpublished manager reference", "kernel/memory/dma/domain.c",
         "kfree(domain);\n        DMAManagerRelease(&manager);", "kfree(domain);"),
        ("skip final context setup health", "kernel/memory/dma/binding.c",
         "status = DMABindingCheckOperation(binding);", "status = OS_EOK;"),
        ("skip Running cancellation propagation", "kernel/memory/dma/binding.c",
         "DMARecoveryCancel(binding->Group);\n    __PropagateBindingFailures(binding->Manager);",
         "DMARecoveryCancel(binding->Group);"),
        ("publish failed attachment", "kernel/memory/dma/manager.c",
         "status = __CheckAttachmentPublicationLocked(prepared);", "status = OS_EOK;"),
        ("select another domain space", "kernel/memory/dma/binding.c",
         "configuration.AddressSpace = binding->Attachment->Domain->AddressSpace;",
         "configuration.AddressSpace = ((DMATranslatedDomain_t*)binding->Manager->Domains.head->value)->AddressSpace;"),
        ("ignore restricted view offset", "kernel/memory/shm/device/device.c",
         "offset += source.Offset;", "offset += 0;"),
        ("ignore backing buffer offset", "kernel/memory/shm/device/layout.c",
         "__GatherExtents(buffer->Pages, buffer->Offset + offset, length, pageSize, extents, &extentCount)",
         "__GatherExtents(buffer->Pages, offset, length, pageSize, extents, &extentCount)"),
        ("bounce translated placement failures", "kernel/memory/shm/device/layout.c",
         "granule != 0 || kind != SHMDeviceMappingStreamingBounce", "kind != SHMDeviceMappingStreamingBounce"),
        ("apply device boundary to physical pool", "kernel/memory/shm/device/device.c",
         "physicalBoundary = 0;", "physicalBoundary = (size_t)requirements->Boundary;"),
        ("omit IOVA boundary alignment", "kernel/memory/shm/device/device.c",
         "mapRequirements.AddressAlignment = requirements->Boundary;", "mapRequirements.AddressAlignment = 0;"),
        ("expose unzeroed rounded storage", "kernel/memory/shm/shm.c",
         "MAPPING_USERSPACE | MAPPING_PERSISTENT | MAPPING_COMMIT | MAPPING_CLEAN;",
         "MAPPING_USERSPACE | MAPPING_PERSISTENT | MAPPING_COMMIT;"),
        ("round shared allocation only to CPU pages", "kernel/memory/shm/device/device.c",
         "allocationUnit = granule > pageSize ? (size_t)granule : pageSize;", "allocationUnit = pageSize;"),
        ("ignore shared length overflow", "kernel/memory/shm/device/device.c",
         "requirements->Length > SIZE_MAX - (allocationUnit - 1)", "false"),
        ("skip final segment health check", "kernel/memory/shm/device/device.c",
         "if (SHMDeviceOperationCheck(operation) != OS_EOK) {", "if (false) {"),
        ("cancel only one failed-unit domain", "kernel/memory/dma/recovery.c",
         "attachment->Domain == NULL || attachment->Domain->Unit != domain->Unit",
         "attachment->Domain != domain || domain != manager->Domains.head->value"),
        ("drop failure events with snapshot error", "kernel/memory/dma/recovery.c",
         "DMARecoveryBlockLocked(manager, group);\n            groups[count++] = group;",
         "if (DMARecoveryGroupAcquire(group) != OS_EOK) { continue; }\n"
         "            DMARecoveryGroupRelease(&group);\n            group = attachment->RecoveryGroup;\n"
         "            DMARecoveryBlockLocked(manager, group);\n            groups[count++] = group;"),
        ("skip recovery at operation end", "kernel/memory/dma/recovery.c",
         "status = DMARecoveryCheckDomains(binding->Manager);", "status = OS_EOK;"),
        ("skip recovery after stop cleanup", "kernel/memory/dma/recovery.c",
         "oserr_t propagationStatus = DMARecoveryCheckDomains(manager);", "oserr_t propagationStatus = OS_EOK;"),
        ("skip failure cancellation retry on event read", "kernel/memory/dma/events.c",
         "DMARecoveryCheckDomains(manager)", "OS_EOK"),
        ("free source pages before invalidation", "kernel/memory/shm/device/cleanup.c",
         "status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (status != OS_EOK) {",
         "status = DMAAddressMappingRelease(&mapping->AddressMapping);\n    if (false) {"),

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
        if result.returncode == 0 or b"Assertion" not in result.stderr or sanitizer_fault:
            raise AssertionError(f"Mutation did not fail at an assertion: {name}: {result.stderr.decode()}")
        print("Translated binding mutation rejected:", name, flush=True)


if __name__ == "__main__":
    main()
