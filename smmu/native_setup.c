/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Native SMMUv3 self-test: setup and connections.
 * Compiled only with VALI_ARM64_SMMU_SELFTEST on generic AArch64.
 *
 * ArmSMMUSelfTestSetup is the first stage of every run. It checks the system
 * clock and prints the IORT. It then prepares both endpoints, resolves their
 * stream IDs and creates and starts the SMMUv3 unit. Last, it allocates the
 * test pages, creates one address space per domain, installs the four
 * mappings and connects the domains that the case asks for. Most steps print
 * a STEP record with what they found or set up.
 *
 * The connection calls at the end of this file are used again later in the
 * run. The positive case reconnects domain A, after-disconnect disconnects
 * it, and cleanup disconnects and destroys every connection that is left.
 */

#define __MODULE "SMMUTEST"

#include "native_coordinator.h"
#include "../../kernel/arch/aarch64/acpi/iort.h"
#include "../../kernel/arch/aarch64/acpi/tables.h"
#include <component/timer.h>
#include <debug.h>
#include <string.h>

#define __PAGE_SIZE         ARM_SMMU_MEMORY_PAGE_SIZE

// Separate 64 KiB arena for the guard and data pages, apart from the unit's
// own control arena.
#define __TEST_ARENA_SIZE   (64U * 1024U)

// Largest IORT this fixture prints. QEMU's table is a few hundred bytes.
#define __IORT_MAX_LENGTH   (64U * 1024U)

// Table bytes per IORT record. 32 bytes give 64 hex digits, which keeps the
// whole record well below the 127-byte kernel log line.
#define __IORT_CHUNK_SIZE   32U

// An SMMUv3 level-2 stream table page holds 64 stream table entries, so
// stream IDs that only differ in their low 6 bits share one page.
#define __STREAM_LEAF_SHIFT 6U

// SMMU_IDR0 bit 1 (S1P). The stage=2 QEMU profile must clear it.
#define __IDR0_STAGE1       (1U << 1)

/*******************************************************************************
 * Setup
 ******************************************************************************/

// The SMMU backend waits on the system clock. A zero frequency means the
// boot timer setup did not select a clock, so no wait could end.
static oserr_t
__CheckClock(void)
{
    UInteger64_t frequency;

    __ArmSMMUSelfTestStep("clock");
    SystemTimerGetClockFrequency(&frequency);
    if (frequency.QuadPart == 0) {
        ERROR("SMMU-SELFTEST: no system clock is running");
        return OS_ENOTSUPPORTED;
    }
    return OS_EOK;
}

// Prints one chunk of table bytes as hex.
static void
__PrintIortChunk(
    _In_ const uint8_t* bytes,
    _In_ uint32_t       offset,
    _In_ uint32_t       length)
{
    static const char digits[] = "0123456789abcdef";
    char              text[(__IORT_CHUNK_SIZE * 2U) + 1U];

    for (uint32_t i = 0; i < length; i++) {
        text[i * 2U] = digits[bytes[offset + i] >> 4];
        text[(i * 2U) + 1U] = digits[bytes[offset + i] & 0xFU];
    }
    text[length * 2U] = '\0';
    DEBUG("SMMU-SELFTEST IORT offset=0x%05x data=%s", offset, &text[0]);
}

// Prints the guest's real IORT before any SMMU register is written, so the
// host can rebuild it and check it against the decoded routes. This reads
// the table through ACPICA. It does not touch the shared catalogue.
static oserr_t
__CaptureIort(void)
{
    ACPI_TABLE_HEADER* header = NULL;
    uint32_t           length;
    oserr_t            status;

    __ArmSMMUSelfTestStep("iort");
    status = Arm64AcpiGetTable(ACPI_SIG_IORT, sizeof(ACPI_TABLE_HEADER), &header);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: reading the IORT failed: %u", status);
        return status;
    }

    length = header->Length;
    if (length > __IORT_MAX_LENGTH) {
        ERROR("SMMU-SELFTEST: IORT length %u is above the %u byte limit", length, __IORT_MAX_LENGTH);
        AcpiPutTable(header);
        return OS_ENOTSUPPORTED;
    }

    DEBUG("SMMU-SELFTEST IORT length=%u chunk=%u", length, __IORT_CHUNK_SIZE);
    for (uint32_t offset = 0; offset < length; offset += __IORT_CHUNK_SIZE) {
        uint32_t chunk = length - offset < __IORT_CHUNK_SIZE ? length - offset : __IORT_CHUNK_SIZE;

        __PrintIortChunk((const uint8_t*)header, offset, chunk);
    }
    AcpiPutTable(header);
    return OS_EOK;
}

// Resolves the stream ID of one endpoint and checks that it has exactly one
// ID on a coherent route.
static oserr_t
__ResolveDomain(
    _In_    const ArmSMMUDiscovery_t* discovery,
    _InOut_ ArmSMMUTestDomain_t*      domain,
    _Out_   ArmSMMUDeviceRoute_t*     routeOut)
{
    const ArmSMMUTestDevice_t* device = domain->Device;
    oserr_t                    status;

    status = ArmSMMUResolvePci(discovery, device->Segment, device->RequesterId, routeOut);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: no SMMU route for %u:%04x: %u", device->Segment, device->RequesterId, status);
        return status;
    }

    if (routeOut->RequesterCount != 1 || routeOut->CachePolicy != SHMDeviceCacheCoherent) {
        ERROR("SMMU-SELFTEST: route of %04x has %u stream IDs and cache policy %u, need 1 coherent ID",
              device->RequesterId, routeOut->RequesterCount, routeOut->CachePolicy);
        return OS_ENOTSUPPORTED;
    }

    domain->HardwareID = routeOut->Requesters[0];
    DEBUG("SMMU-SELFTEST STEP route %s rid=%04x sid=0x%x unit=%u limit=0x%llx",
          domain->Name, device->RequesterId, domain->HardwareID,
          routeOut->UnitIndex, routeOut->AddressLimit);
    return OS_EOK;
}

// Both stream IDs must be distinct, on the same SMMUv3 unit, and share one
// level-2 stream table page.
static oserr_t
__CheckRoutes(
    _In_ const ArmSMMUDiscovery_t*   discovery,
    _In_ const ArmSMMUDeviceRoute_t  routes[2])
{
    uint32_t first = g_armSMMUSelfTest.Domains[0].HardwareID;
    uint32_t second = g_armSMMUSelfTest.Domains[1].HardwareID;
    oserr_t  status;

    if (routes[0].UnitIndex != routes[1].UnitIndex || first == second ||
        (first >> __STREAM_LEAF_SHIFT) != (second >> __STREAM_LEAF_SHIFT)) {
        ERROR("SMMU-SELFTEST: need distinct stream IDs in one table page of one unit, got 0x%x/%u and 0x%x/%u",
              first, routes[0].UnitIndex, second, routes[1].UnitIndex);
        return OS_ENOTSUPPORTED;
    }

    status = ArmSMMUGetUnitDescription(discovery, routes[0].UnitIndex, &g_armSMMUSelfTest.Description);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: reading unit %u failed: %u", routes[0].UnitIndex, status);
        return status;
    }

    if (g_armSMMUSelfTest.Description.Kind != ArmSMMUv3 || !g_armSMMUSelfTest.Description.TableCoherent) {
        ERROR("SMMU-SELFTEST: unit %u is kind %u, coherent %u, need a coherent SMMUv3",
              routes[0].UnitIndex, g_armSMMUSelfTest.Description.Kind, g_armSMMUSelfTest.Description.TableCoherent);
        return OS_ENOTSUPPORTED;
    }

    g_armSMMUSelfTest.UnitIndex = routes[0].UnitIndex;
    g_armSMMUSelfTest.AddressLimit = routes[0].AddressLimit < routes[1].AddressLimit ?
        routes[0].AddressLimit : routes[1].AddressLimit;
    return OS_EOK;
}

// Prepares both endpoints and resolves their stream IDs through the shared
// IORT catalogue. The discovery table is only needed for these lookups.
// The adapter prints what it verified for each endpoint ("STEP endpoint").
static oserr_t
__DiscoverRoutes(void)
{
    const ArmIortCatalogue_t* catalogue = NULL;
    ArmSMMUDiscovery_t*       discovery = NULL;
    ArmSMMUDeviceRoute_t      routes[ARM_SMMU_TEST_DEVICE_COUNT];
    oserr_t                   status;

    __ArmSMMUSelfTestStep("devices");
    status = ArmSMMUTestDevicesPrepare(g_armSMMUSelfTest.Devices);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: preparing the test devices failed: %u", status);
        return status;
    }

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        DEBUG("SMMU-SELFTEST STEP device rid=%04x bar=0x%llx pa=0x%llx",
              g_armSMMUSelfTest.Devices[i].RequesterId, g_armSMMUSelfTest.Devices[i].BarBusAddress,
              g_armSMMUSelfTest.Devices[i].BarPhysicalAddress);
    }

    __ArmSMMUSelfTestStep("discovery");
    status = ArmIortGetCatalogue(&catalogue);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: the boot IORT catalogue is unavailable: %u", status);
        return status;
    }

    status = ArmSMMUDiscoverIort(catalogue, &discovery);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: SMMU discovery failed: %u", status);
        return status;
    }

    __ArmSMMUSelfTestStep("routes");
    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        status = __ResolveDomain(discovery, &g_armSMMUSelfTest.Domains[i], &routes[i]);
        if (status != OS_EOK) {
            ArmSMMUDiscoveryDestroy(&discovery);
            return status;
        }
    }

    status = __CheckRoutes(discovery, routes);
    ArmSMMUDiscoveryDestroy(&discovery);
    return status;
}

// Logs the identification registers, so the result shows the hardware
// profile and not only the requested command line. QEMU's stage=2 profile
// must report stage 2 without stage 1.
static oserr_t
__LogUnitProfile(void)
{
    uintptr_t registers;
    uint32_t  id0;
    uint32_t  id1;
    uint32_t  id5;
    uint32_t  revision;

    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    registers = g_armSMMUSelfTest.Unit->Common.Registers;
    id0 = __SMMUv3Read32(registers, SMMUV3_IDR0);
    id1 = __SMMUv3Read32(registers, SMMUV3_IDR1);
    id5 = __SMMUv3Read32(registers, SMMUV3_IDR5);
    revision = __SMMUv3Read32(registers, SMMUV3_AIDR);
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);

    DEBUG("SMMU-SELFTEST STEP unit index=%u base=0x%llx idr0=0x%x idr1=0x%x idr5=0x%x aidr=0x%x",
          g_armSMMUSelfTest.UnitIndex, g_armSMMUSelfTest.Description.RegisterBase, id0, id1, id5, revision);
    if (!(id0 & SMMUV3_IDR0_STAGE2) || (id0 & __IDR0_STAGE1)) {
        ERROR("SMMU-SELFTEST: unit reports IDR0 0x%x, the fixture needs stage 2 only", id0);
        return OS_ENOTSUPPORTED;
    }
    return OS_EOK;
}

// Probes and starts the SMMUv3 unit. Both endpoints are disarmed with bus
// mastering off, as Start requires.
static oserr_t
__StartUnit(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("unit-create");
    status = SMMUv3UnitCreate(&g_armSMMUSelfTest.Description, &g_armSMMUSelfTest.Unit);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: SMMUv3UnitCreate failed: %u", status);
        return status;
    }

    g_armSMMUSelfTest.Backend = SMMUv3UnitGetBackend(g_armSMMUSelfTest.Unit);
    if (g_armSMMUSelfTest.Backend == NULL) {
        ERROR("SMMU-SELFTEST: the unit has no DMA backend");
        return OS_EUNKNOWN;
    }

    status = __LogUnitProfile();
    if (status != OS_EOK) {
        return status;
    }

    __ArmSMMUSelfTestStep("unit-start");
    status = SMMUv3UnitStart(g_armSMMUSelfTest.Unit);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: SMMUv3UnitStart failed: %u", status);
        return status;
    }

    status = ArmSMMUSelfTestCheckNoFaults();
    if (status != OS_EOK) {
        return status;
    }
    DEBUG("SMMU-SELFTEST STEP unit-start ok");
    return OS_EOK;
}

// Allocates the test arena and all observed blocks under the unit mutex, as
// the arena requires. The arena limit is the lowest device address limit, so
// the guard range is also a valid IOVA for both endpoints.
static oserr_t
__AllocatePagesLocked(void)
{
    oserr_t status;

    status = ArmSMMUMemoryInitialize(&g_armSMMUSelfTest.Memory, __TEST_ARENA_SIZE, g_armSMMUSelfTest.AddressLimit);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: test arena setup failed: %u", status);
        return status;
    }

    for (uint32_t i = 0; i < ArmSMMUTestBlockCount; i++) {
        ArmSMMUTestBlock_t* block = &g_armSMMUSelfTest.Blocks[i];

        status = ArmSMMUMemoryAllocate(
            &g_armSMMUSelfTest.Memory,
            (size_t)block->PageCount * __PAGE_SIZE,
            0,
            &block->Memory
        );
        if (status != OS_EOK) {
            ERROR("SMMU-SELFTEST: allocating %s failed: %u", block->Name, status);
            return status;
        }
        DEBUG("SMMU-SELFTEST STEP block %s pa=0x%llx pages=%u",
              block->Name, (uint64_t)block->Memory.PhysicalAddress, block->PageCount);
    }
    return OS_EOK;
}

static oserr_t
__AllocatePages(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("pages");
    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    status = __AllocatePagesLocked();
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    return status;
}

// Creates one translated address space per domain. Both IOVA allocators are
// limited to exactly the guard range, so the lowest-first allocator returns
// predictable IOVAs. Physical guard address and IOVA are the same number on
// purpose. Data pages must still be reachable as translation output.
static oserr_t
__CreateDomains(void)
{
    const ArmSMMUTestBlock_t* guard = &g_armSMMUSelfTest.Blocks[ArmSMMUTestBlockGuard];
    DMAIOVAConfig_t           config;
    oserr_t                   status;

    __ArmSMMUSelfTestStep("spaces");
    memset(&config, 0, sizeof(config));
    config.Bounds.First = guard->Memory.PhysicalAddress;
    config.Bounds.Last = guard->Memory.PhysicalAddress + (guard->Memory.Length - 1U);
    config.Granularity = __PAGE_SIZE;

    for (uint32_t i = 0; i < ARM_SMMU_TEST_DEVICE_COUNT; i++) {
        ArmSMMUTestDomain_t* domain = &g_armSMMUSelfTest.Domains[i];

        status = DMAAddressSpaceCreateTranslated(g_armSMMUSelfTest.Backend, &config, &domain->Space);
        if (status != OS_EOK) {
            ERROR("SMMU-SELFTEST: creating %s failed: %u", domain->Name, status);
            return status;
        }
    }

    for (uint32_t i = 0; i < ArmSMMUTestBlockCount; i++) {
        const ArmSMMUMemoryBlock_t* memory = &g_armSMMUSelfTest.Blocks[i].Memory;

        if (memory->PhysicalAddress + (memory->Length - 1U) >
            DMAAddressSpaceGetPhysicalLimit(g_armSMMUSelfTest.Domains[0].Space)) {
            ERROR("SMMU-SELFTEST: %s is above the unit physical limit", g_armSMMUSelfTest.Blocks[i].Name);
            return OS_ENOTSUPPORTED;
        }
    }
    DEBUG("SMMU-SELFTEST STEP spaces iova=0x%llx-0x%llx", config.Bounds.First, config.Bounds.Last);
    return OS_EOK;
}

// Prepares and installs one mapping, then checks that the allocator returned
// the expected IOVA as one segment of the block's length.
static oserr_t
__InstallMapping(
    _InOut_ ArmSMMUTestMapping_t* mapping)
{
    const ArmSMMUTestBlock_t* block = &g_armSMMUSelfTest.Blocks[mapping->Block];
    uint64_t                  expectedAddress = __ArmSMMUSelfTestGuardBase() + mapping->IovaOffset;
    const struct DMASegment*  segments;
    SHMSG_t                   extent;
    uint32_t                  count = 0;
    oserr_t                   status;

    extent.Address = (uintptr_t)block->Memory.PhysicalAddress;
    extent.Length = block->Memory.Length;
    status = DMAAddressMappingPrepare(
        g_armSMMUSelfTest.Domains[mapping->Domain].Space,
        &extent,
        1,
        g_armSMMUSelfTest.AddressLimit,
        mapping->Direction,
        NULL,
        &mapping->Mapping
    );
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: preparing mapping %s failed: %u", mapping->Name, status);
        return status;
    }

    segments = DMAAddressMappingSegments(mapping->Mapping, &count);
    if (segments == NULL || count != 1 || segments[0].Address != expectedAddress ||
        segments[0].Length != block->Memory.Length) {
        ERROR("SMMU-SELFTEST: mapping %s has %u segments at 0x%llx, expected one at 0x%llx",
              mapping->Name, count, segments != NULL && count != 0 ? segments[0].Address : 0,
              expectedAddress);
        return OS_EUNKNOWN;
    }

    status = DMAAddressMappingInstall(mapping->Mapping);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: installing mapping %s failed: %u", mapping->Name, status);
        return status;
    }

    mapping->DeviceAddress = expectedAddress;
    DEBUG("SMMU-SELFTEST STEP mapping %s iova=0x%llx pa=0x%llx length=0x%llx direction=%u",
          mapping->Name, expectedAddress, (uint64_t)block->Memory.PhysicalAddress,
          (uint64_t)block->Memory.Length, mapping->Direction);
    return OS_EOK;
}

// Installs the four mappings in table order, which gives the IOVA layout
// shown at ArmSMMUTestMappingIndex.
static oserr_t
__InstallMappings(void)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("mappings");
    for (uint32_t i = 0; i < ArmSMMUTestMappingCount; i++) {
        status = __InstallMapping(&g_armSMMUSelfTest.Mappings[i]);
        if (status != OS_EOK) {
            return status;
        }
    }
    return OS_EOK;
}

// Reserves the domain's stream ID with a new connection record.
static oserr_t
__PrepareConnection(
    _InOut_ ArmSMMUTestDomain_t* domain)
{
    oserr_t status;

    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    status = SMMUv3HardwareIDsPrepare(
        domain->Space->Translated.BackendSpace,
        &domain->HardwareID,
        1,
        &domain->Connection
    );
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: reserving sid 0x%x for %s failed: %u", domain->HardwareID, domain->Name, status);
        return status;
    }
    domain->ConnectionState = ArmSMMUTestConnectionPrepared;
    return OS_EOK;
}

// Routes the reserved stream ID to the domain's stage-2 table. The endpoint
// is idle: it is disarmed and bus mastering is off.
static oserr_t
__Connect(
    _InOut_ ArmSMMUTestDomain_t* domain)
{
    oserr_t status;

    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    status = SMMUv3HardwareIDsConnect(domain->Space->Translated.BackendSpace, domain->Connection, ARM_SMMU_TIMEOUT_NS);
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: connecting sid 0x%x to %s failed: %u", domain->HardwareID, domain->Name, status);
        return status;
    }
    domain->ConnectionState = ArmSMMUTestConnectionConnected;
    DEBUG("SMMU-SELFTEST STEP connect %s sid=0x%x", domain->Name, domain->HardwareID);
    return OS_EOK;
}

// Connects the first count domains, starting with domain A. Preparing domain
// A's connection also fills the shared stream table page with valid abort
// entries, so a domain left unconnected here has a stream ID that the SMMU
// rejects without reading any table of its own.
static oserr_t
__ConnectDomains(
    _In_ uint32_t count)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("connect");
    for (uint32_t i = 0; i < count; i++) {
        status = __PrepareConnection(&g_armSMMUSelfTest.Domains[i]);
        if (status != OS_EOK) {
            return status;
        }

        status = __Connect(&g_armSMMUSelfTest.Domains[i]);
        if (status != OS_EOK) {
            return status;
        }
    }
    return ArmSMMUSelfTestCheckNoFaults();
}

// Brings the test from boot to connected domains with all mappings.
// The order matters: the endpoints are stopped before the unit starts, the
// pages exist before the spaces (their bounds come from the guard), and
// mappings are installed before any stream ID is routed.
oserr_t
ArmSMMUSelfTestSetup(
    _In_ uint32_t connectedDomains)
{
    oserr_t status;

    status = __CheckClock();
    if (status != OS_EOK) {
        return status;
    }

    status = __CaptureIort();
    if (status != OS_EOK) {
        return status;
    }

    status = __DiscoverRoutes();
    if (status != OS_EOK) {
        return status;
    }

    status = __StartUnit();
    if (status != OS_EOK) {
        return status;
    }

    status = __AllocatePages();
    if (status != OS_EOK) {
        return status;
    }

    status = __CreateDomains();
    if (status != OS_EOK) {
        return status;
    }

    status = __InstallMappings();
    if (status != OS_EOK) {
        return status;
    }
    return __ConnectDomains(connectedDomains);
}

/*******************************************************************************
 * Connections used by the cases and by cleanup
 ******************************************************************************/

// Completes a disconnect, retrying only a timeout, which Disconnect
// documents as resumable from its saved progress.
oserr_t
ArmSMMUSelfTestDisconnect(
    _InOut_ ArmSMMUTestDomain_t* domain)
{
    oserr_t status = OS_ETIMEOUT;

    for (uint32_t attempt = 0; attempt < ARM_SMMU_SELFTEST_MAX_ATTEMPTS && status == OS_ETIMEOUT; attempt++) {
        DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
        status = SMMUv3HardwareIDsDisconnect(
            domain->Space->Translated.BackendSpace,
            domain->Connection,
            ARM_SMMU_TIMEOUT_NS
        );
        DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    }

    if (status != OS_EOK) {
        ERROR("SMMU-SELFTEST: disconnecting sid 0x%x from %s failed: %u", domain->HardwareID, domain->Name, status);
        return status;
    }
    domain->ConnectionState = ArmSMMUTestConnectionDisconnected;
    DEBUG("SMMU-SELFTEST STEP disconnect %s sid=0x%x", domain->Name, domain->HardwareID);
    return OS_EOK;
}

// Releases the stream ID reservation and the connection record. Only valid
// after Prepare alone or after a completed Disconnect.
void
ArmSMMUSelfTestDestroyConnection(
    _InOut_ ArmSMMUTestDomain_t* domain)
{
    DMAIOMMUUnitLock(g_armSMMUSelfTest.Backend);
    SMMUv3HardwareIDsDestroy(domain->Space->Translated.BackendSpace, domain->Connection);
    DMAIOMMUUnitUnlock(g_armSMMUSelfTest.Backend);
    domain->Connection = NULL;
    domain->ConnectionState = ArmSMMUTestConnectionNone;
}

// Disconnects the domain's endpoint and connects it again with a fresh
// connection record. A disconnected record is never connected again. The
// endpoint is idle, since each completed Write disarms it.
oserr_t
ArmSMMUSelfTestReconnect(
    _InOut_ ArmSMMUTestDomain_t* domain)
{
    oserr_t status;

    __ArmSMMUSelfTestStep("reconnect");
    status = ArmSMMUSelfTestDisconnect(domain);
    if (status != OS_EOK) {
        return status;
    }

    ArmSMMUSelfTestDestroyConnection(domain);
    status = __PrepareConnection(domain);
    if (status != OS_EOK) {
        return status;
    }
    return __Connect(domain);
}
