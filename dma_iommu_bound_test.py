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
        flags += ["-DDMA_TEST_REAL_DEVICE_BUFFER", "-DDMA_WARNING_LOG_TEST", "-Wno-sign-compare", "-Wl,--wrap=MemorySpaceMap",
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
        # before the test can check its incorrect bounce fallback. Translated
        # bounce is chosen before allocation, so this mutant only retries a
        # failed translated placement through the direct bounce path.
        ("retry a failed translated placement with direct bounce", "kernel/memory/shm/device/layout.c",
         "granularity != 0 || kind != SHMDeviceMappingStreamingBounce",
         "(granularity != 0 && false) || kind != SHMDeviceMappingStreamingBounce"),
        ("leave a direct-only request above the physical limit to the address core",
         "kernel/memory/shm/device/layout.c",
         "if (kind != SHMDeviceMappingStreamingDirect) {\n            return OS_EOK;",
         "if (kind != SHMDeviceMappingStreamingDirect || true) {\n            return OS_EOK;"),
        # Translated bounce. The numbers refer to the mutation list in
        # docs/dma-translated-bounce.md.
        ("1: map rounded source pages instead of private storage", "kernel/memory/shm/device/layout.c",
         "    status = __UseBounceExtents(layout);\n    if (status != OS_EOK) {\n        return status;\n    }\n\n    // The address mapping logs",
         "    layout->ExtentCount = 1;\n    layout->Extents[0].Address &= ~(uintptr_t)(layout->BouncePlan.UnitLength - 1);\n"
         "    layout->Extents[0].Length = layout->BouncePlan.MappedLength;\n"
         "    layout->Bounce.MappedLength = layout->BouncePlan.MappedLength;\n\n    // The address mapping logs"),
        ("2: publish the padded native segment", "kernel/memory/shm/device/device.c",
         "if (__SHMDeviceMappingIsTranslatedBounce(mapping)) {\n        segments = &mapping->Stream.PayloadSegment;",
         "if (false) {\n        segments = &mapping->Stream.PayloadSegment;"),
        ("2: accept transfer ranges inside the padding", "kernel/memory/shm/device/device.c",
         "if (!__SHMRangeContains(0, mapping->Length, offset, length)) {",
         "if (!__SHMRangeContains(0, __SHMDeviceMappingIsTranslatedBounce(mapping) ? "
         "mapping->Stream.Bounce.MappedLength : mapping->Length, offset, length)) {"),
        ("3: drop the backing buffer offset from the source view", "kernel/memory/shm/device/layout.c",
         "    size_t       start = buffer->Offset + offset;", "    size_t       start = offset;"),
        ("4: round translated bounce units only to CPU pages", "kernel/memory/shm/device/layout.c",
         "    unit = granule > pageSize ? granule : pageSize;", "    unit = pageSize;"),
        ("4: size the bounce page list from the source pages", "kernel/memory/shm/device/layout.c",
         "    size_t       pageCount = plan->AllocationLength / pageSize;",
         "    size_t       pageCount = bounce->SourceMappingLength / pageSize;"),
        ("5: map bounce storage without zeroing it", "kernel/memory/shm/device/layout.c",
         "    unsigned int flags = MAPPING_COMMIT | MAPPING_PERSISTENT | MAPPING_CLEAN;\n    oserr_t      status;\n\n    if (pageCount",
         "    unsigned int flags = MAPPING_COMMIT | MAPPING_PERSISTENT;\n    oserr_t      status;\n\n    if (pageCount"),
        ("6: copy the source during preparation", "kernel/memory/shm/device/layout.c",
         "    status = __UseBounceExtents(layout);\n    if (status != OS_EOK) {\n        return status;\n    }\n\n    // The address mapping logs",
         "    memcpy((void*)layout->Bounce.BounceMapping, (const void*)(layout->Bounce.SourceMapping + layout->Bounce.SourceOffset), length);\n"
         "    status = __UseBounceExtents(layout);\n    if (status != OS_EOK) {\n        return status;\n    }\n\n    // The address mapping logs"),
        ("6: read the source for a device write", "kernel/memory/shm/device/device.c",
         "mapping->Stream.Bounce.BounceMapping != 0 && mapping->Direction != DMAFromDevice",
         "mapping->Stream.Bounce.BounceMapping != 0"),
        ("7: check LengthAlignment only on the padded length", "kernel/memory/shm/device/device.c",
         "    return requirements->LengthAlignment == 0 ||\n        (length & (requirements->LengthAlignment - 1)) == 0;",
         "    return true;"),
        ("13: skip the translated bounce size limit", "kernel/memory/shm/device/layout.c",
         "if (plan->MappedLength > SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT) {", "if (false) {"),
        ("13: compare the size limit with the logical length", "kernel/memory/shm/device/layout.c",
         "if (plan->MappedLength > SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT) {", "if (length > SHM_DEVICE_TRANSLATED_BOUNCE_LIMIT) {"),
        ("14: ignore the physical limit when selecting bounce", "kernel/memory/shm/device/layout.c",
         "    if (__ReachesAbove(extents,", "    if (false && __ReachesAbove(extents,"),
        ("15: leave units taken before a pool failure allocated", "kernel/memory/shm/device/layout.c",
         "            __ReportPoolFailure(context, plan, bounce, taken, status);\n            break;",
         "            __ReportPoolFailure(context, plan, bounce, taken, status);\n            bounce->UnitCount = 0;\n            break;"),
        ("5: skip the cache work before installation", "kernel/memory/shm/device/layout.c",
         "            CpuDataCacheCleanInvalidate(bounce->Units[i].PhysicalBase, bounce->Units[i].Length);",
         "            (void)bounce->Units[i];"),
        ("8: keep the logical range for translated bounce cache work", "kernel/memory/shm/device/device.c",
         "        *offset = 0;\n        *length = mapping->Stream.Bounce.MappedLength;",
         "        (void)offset;\n        (void)length;"),
        ("8: skip full-span cache work at transfer end", "kernel/memory/shm/device/device.c",
         "        __CacheRange(mapping, &cacheOffset, &cacheLength);\n        __SyncCacheRange(mapping, cacheOffset, cacheLength, CpuDataCacheInvalidate);",
         "        __SyncCacheRange(mapping, cacheOffset, cacheLength, CpuDataCacheInvalidate);"),
        ("9: copy back the submitted length instead of the completed prefix", "kernel/memory/shm/device/device.c",
         "        __CopyFromBounce(mapping, offset, length);", "        __CopyFromBounce(mapping, offset, mapping->Stream.Length);"),
        ("9: copy uncertain bounce bytes back after a stop", "kernel/memory/shm/device/cleanup.c",
         "        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);",
         "        SHMDeviceCompleteForCpu(mapping, 0, mapping->Length);\n"
         "        if (__SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Bounce.BounceMapping != 0) {\n"
         "            __builtin_memcpy((void*)(mapping->Stream.Bounce.SourceMapping + mapping->Stream.Bounce.SourceOffset),\n"
         "                (const void*)mapping->Stream.Bounce.BounceMapping, mapping->Length);\n        }"),
        ("10: return bounce units before the stop cache work", "kernel/memory/shm/device/cleanup.c",
         "    if (mapping->AddressMapping != NULL) {\n        status = __RemoveDeviceAccess(mapping, stopped);",
         "    if (__SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Bounce.BounceMapping != 0) {\n"
         "        (void)MemorySpaceUnmap(GetDomainMemorySpace(), mapping->Stream.Bounce.BounceMapping, mapping->Stream.Bounce.BounceMappingLength);\n"
         "        mapping->Stream.Bounce.BounceMapping = 0;\n"
         "        for (size_t i = 0; i < mapping->Stream.Bounce.UnitCount; i++) {\n"
         "            (void)SystemMemoryReleaseAllocation(&mapping->Stream.Bounce.Units[i]);\n        }\n    }\n"
         "    if (mapping->AddressMapping != NULL) {\n        status = __RemoveDeviceAccess(mapping, stopped);"),
        ("16: return quarantined units with a second release", "kernel/memory/shm/device/layout.c",
         "    status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    if (status != OS_EOK) {",
         "    status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    if (status != OS_EOK) {\n"
         "        status = SHMDeviceReleaseBounce(context, &layout->Bounce);\n    }\n    if (status != OS_EOK) {"),
        ("16: revoke the binding after a failed release before installation", "kernel/memory/shm/device/layout.c",
         "              context, __BounceBytesAllocated(&layout->Bounce), status, originalStatus);",
         "              context, __BounceBytesAllocated(&layout->Bounce), status, originalStatus);\n        DMABindingRevoke(context->Binding);"),
        ("17: stop returning units after the first failure", "kernel/memory/shm/device/cleanup.c",
         "                firstError = status;\n            }\n            continue;",
         "                firstError = status;\n            }\n            break;"),
        ("17: return a unit that is already back in the pool", "kernel/memory/shm/device/cleanup.c",
         "        if (unit->Owner == NULL) {\n            continue;\n        }\n", ""),
        ("18: report every translated bounce mapping as a warning", "kernel/memory/shm/device/layout.c",
         "    if (SHMDeviceContextCountBounceMapping(context)) {",
         "    if (SHMDeviceContextCountBounceMapping(context) || true) {"),
        ("18: never count a translated bounce mapping", "kernel/memory/shm/device/layout.c",
         "    if (layout.Bounce.Translated) {\n        __ReportTranslatedBounce(", "    if (false) {\n        __ReportTranslatedBounce("),
        # Size limit refusals and pool shortages are normal results. Each is a
        # warning once per context, and closing the context gives the totals.
        ("diagnostics: report every size limit refusal as a warning", "kernel/memory/shm/device/layout.c",
         "    if (SHMDeviceContextCountBounceLimitRefusal(context)) {",
         "    if (SHMDeviceContextCountBounceLimitRefusal(context) || true) {"),
        ("diagnostics: never report a size limit refusal", "kernel/memory/shm/device/layout.c",
         "        if (status == OS_EBUFFER) {\n            __ReportLimitRefusal(",
         "        if (false) {\n            __ReportLimitRefusal("),
        ("diagnostics: report every pool shortage as a warning", "kernel/memory/shm/device/layout.c",
         "    if (SHMDeviceContextCountBouncePoolShortage(context)) {",
         "    if (SHMDeviceContextCountBouncePoolShortage(context) || true) {"),
        ("diagnostics: log a translated pool shortage as an error", "kernel/memory/shm/device/layout.c",
         "    if (status != OS_EOOM || !bounce->Translated) {", "    if (true) {"),
        ("diagnostics: hide refusal totals when a context is freed", "kernel/memory/shm/device/context.c",
         "    if (use->LimitRefusals != 0 || use->PoolShortages != 0) {", "    if (false) {"),
        ("diagnostics: hide lost bounce storage when a context is freed", "kernel/memory/shm/device/context.c",
         "    if (use->HeldBytes != 0) {", "    if (false) {"),
        ("abandon: release bounce storage before the stop pass", "kernel/memory/shm/device/cleanup.c",
         "        TRACE(\"SHMDeviceAbandon: mapping %p handed to context %p\", current, context);",
         "        TRACE(\"SHMDeviceAbandon: mapping %p handed to context %p\", current, context);\n"
         "        if (__SHMDeviceMappingIsStreaming(current)) {\n"
         "            (void)SHMDeviceReleaseBounce(context, &current->Stream.Bounce);\n        }"),
        ("abandon: drop the private source view", "kernel/memory/shm/device/cleanup.c",
         "        TRACE(\"SHMDeviceAbandon: mapping %p handed to context %p\", current, context);",
         "        TRACE(\"SHMDeviceAbandon: mapping %p handed to context %p\", current, context);\n"
         "        if (__SHMDeviceMappingIsStreaming(current) && current->Stream.Bounce.SourceMapping != 0 &&\n"
         "            MemorySpaceUnmap(GetDomainMemorySpace(), current->Stream.Bounce.SourceMapping,\n"
         "                current->Stream.Bounce.SourceMappingLength) == OS_EOK) {\n"
         "            current->Stream.Bounce.SourceMapping = 0;\n        }"),
        ("admission: copy into bounce storage before admission", "kernel/memory/shm/device/device.c",
         "    status = SHMDeviceOperationBeginAccess(\n        operation,\n        &mapping->Access,\n        offset,",
         "    if (mapping->Stream.Bounce.BounceMapping != 0 && mapping->Direction != DMAFromDevice) {\n"
         "        __CopyToBounce(mapping, offset, length);\n    }\n"
         "    status = SHMDeviceOperationBeginAccess(\n        operation,\n        &mapping->Access,\n        offset,"),
        ("terminal failure: copy received bytes back when removal fails", "kernel/memory/shm/device/cleanup.c",
         "        // Keep the whole buffer range locked and all backing pages allocated.",
         "        if (__SHMDeviceMappingIsStreaming(mapping) && mapping->Stream.Bounce.BounceMapping != 0) {\n"
         "            __builtin_memcpy((void*)(mapping->Stream.Bounce.SourceMapping + mapping->Stream.Bounce.SourceOffset),\n"
         "                (const void*)mapping->Stream.Bounce.BounceMapping, mapping->Length);\n        }\n"
         "        // Keep the whole buffer range locked and all backing pages allocated."),
        ("held bytes: forget units that failed to go back", "kernel/memory/shm/device/cleanup.c",
         "            continue;\n        }\n        returned += length;", "        }\n        returned += length;"),
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
        # Device faults are reported only. See docs/iommu-fault-containment.md.
        ("fail the unit on a device fault", "kernel/memory/dma/domain.c",
         "queued = __QueueFaultEventsLocked(manager, domain);",
         "DMAIOMMUUnitFail(unit, OS_EDEVFAULT);\n    queued = __QueueFaultEventsLocked(manager, domain);"),
        ("block the recovery group on a device fault", "kernel/memory/dma/domain.c",
         "            DMA_ATTACHMENT_DEVICE_FAULT\n        );\n        queued++;",
         "            DMA_ATTACHMENT_DEVICE_FAULT\n        );\n        queued++;\n"
         "        DMARecoveryBlockLocked(manager, attachment->RecoveryGroup);"),
        ("close the binding on a device fault", "kernel/memory/dma/domain.c",
         "            DMA_ATTACHMENT_DEVICE_FAULT\n        );\n        queued++;",
         "            DMA_ATTACHMENT_DEVICE_FAULT\n        );\n        queued++;\n"
         "        if (attachment->Binding.Current != NULL) {\n"
         "            attachment->Binding.Current->State = DMABindingClosed;\n        }"),
        ("send the fault event outside the faulting domain", "kernel/memory/dma/domain.c",
         "if (attachment->Domain != domain) {", "if (attachment->Domain == NULL) {"),
        ("queue an event for every fault", "kernel/memory/dma/domain.c",
         "return (count & (count - 1)) == 0;", "return count != 0;"),
        ("use stop round 0 for a fault on a stopped group", "kernel/memory/dma/domain.c",
         "group->Access.State == DMARecoveryLive ? 0 : group->Stop.Round", "0"),
        ("use the group round for a fault on a live group", "kernel/memory/dma/domain.c",
         "group->Access.State == DMARecoveryLive ? 0 : group->Stop.Round", "group->Stop.Round"),
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
