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
 */

// Exercise copied discovery through the real ACPICA table-validation boundary.
// Every lookup receives an exactly sized allocation, released before queries.
// Fixture builders use wire offsets directly so a wrong production structure
// size cannot silently produce matching test input. Sizes are bytes.
#include "../../kernel/arch/aarch64/smmu/discovery.h"
#include "../../kernel/arch/aarch64/acpi/tables.h"
#include "../../kernel/arch/aarch64/acpi/iort.h"
#include <ddk/types/interrupt.h>
#include <log.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

struct Arm64Platform g_arm64Platform;

static uint8_t  g_table[65536];
static size_t   g_length;
static unsigned g_tables;
static unsigned g_allocations;
static int      g_failAfter = -1;
static unsigned g_getCalls;
static ACPI_STATUS g_tableStatus = AE_OK;

void*
kmalloc(
    size_t size)
{
    void* pointer;

    if (g_failAfter == 0) {
        return NULL;
    }
    if (g_failAfter > 0) {
        g_failAfter--;
    }
    pointer = malloc(size);
    assert(pointer != NULL);
    g_allocations++;
    return pointer;
}

void
kfree(
    void* pointer)
{
    if (pointer != NULL) {
        assert(g_allocations != 0);
        g_allocations--;
        free(pointer);
    }
}

void
LogAppendMessage(
    enum OSSysLogLevel level,
    const char* format,
    ...)
{
    (void)level;
    (void)format;
}

ACPI_STATUS
AcpiGetTable(
    char* signature,
    UINT32 instance,
    ACPI_TABLE_HEADER** result)
{
    ACPI_TABLE_HEADER* header = (void*)g_table;
    size_t size = header->Length < sizeof(*header) ? sizeof(*header) : header->Length;

    assert(!memcmp(signature, "IORT", 4) && instance == 1);
    g_getCalls++;
    if (ACPI_FAILURE(g_tableStatus)) {
        return g_tableStatus;
    }
    assert(size <= sizeof(g_table));
    *result = malloc(size);
    assert(*result != NULL);
    memcpy(*result, g_table, size);
    g_tables++;
    return AE_OK;
}

void
AcpiPutTable(
    ACPI_TABLE_HEADER* table)
{
    assert(g_tables == 1);
    g_tables--;
    free(table);
}

static ACPI_IORT_NODE*
__Node(
    uint32_t offset)
{
    return (void*)(g_table + offset);
}

static void*
__Body(
    uint32_t offset)
{
    return g_table + offset + 16;
}

static ACPI_IORT_ID_MAPPING*
__Map(
    uint32_t offset,
    uint32_t index)
{
    return (void*)(g_table + offset + __Node(offset)->MappingOffset + index * 20);
}

static void
__Reset(void)
{
    ACPI_TABLE_IORT* table = (void*)g_table;

    assert(g_allocations == 0 && g_tables == 0);
    memset(g_table, 0, sizeof(g_table));
    memcpy(table->Header.Signature, "IORT", 4);
    table->Header.Revision = 5;
    table->NodeOffset = 48;
    g_length = 48;
    g_failAfter = -1;
    g_tableStatus = AE_OK;
}

static uint32_t
__AddNode(
    uint8_t type,
    uint8_t revision,
    uint16_t fixed,
    uint32_t maps)
{
    ACPI_TABLE_IORT* table = (void*)g_table;
    uint32_t offset = (uint32_t)g_length;
    ACPI_IORT_NODE* node = __Node(offset);

    g_length += fixed + maps * 20;
    assert(g_length <= sizeof(g_table));
    node->Type = type;
    node->Revision = revision;
    node->Length = fixed + maps * 20;
    node->Identifier = table->NodeCount++;
    node->MappingCount = maps;
    node->MappingOffset = maps == 0 ? 0 : fixed;
    return offset;
}

static uint32_t
__V3(
    uint32_t maps)
{
    uint32_t offset = __AddNode(4, 4, 68, maps);
    ACPI_IORT_SMMU_V3* smmu = __Body(offset);

    smmu->BaseAddress = 0x100000000ULL;
    smmu->Flags = 1;
    smmu->EventGsiv = 74;
    smmu->GerrGsiv = 75;
    smmu->PriGsiv = 76;
    smmu->SyncGsiv = 77;
    return offset;
}

static uint32_t
__Mmu500(void)
{
    uint32_t offset = __AddNode(3, 3, 92, 0);
    ACPI_IORT_SMMU* smmu = __Body(offset);
    ACPI_IORT_SMMU_GSI* global = (void*)(g_table + offset + 60);
    uint32_t contexts[] = {100, 1, 100, 1};

    smmu->BaseAddress = 0x200000000ULL;
    smmu->Span = 0x40000;
    smmu->Model = 3;
    smmu->Flags = 2;
    smmu->GlobalInterruptOffset = 60;
    smmu->ContextInterruptCount = 2;
    smmu->ContextInterruptOffset = 76;
    global->NSgIrpt = 99;
    memcpy(g_table + offset + 76, contexts, sizeof(contexts));
    return offset;
}

static uint32_t
__Root(
    uint32_t target,
    uint32_t segment,
    uint32_t first,
    uint32_t count,
    uint32_t output)
{
    uint32_t offset = __AddNode(2, 3, 36, 1);
    ACPI_IORT_ROOT_COMPLEX* root = __Body(offset);

    root->PciSegmentNumber = segment;
    root->MemoryProperties = 0x0300000000000001ULL;
    root->MemoryAddressLimit = 64;
    *__Map(offset, 0) = (ACPI_IORT_ID_MAPPING){first, count, output, target, 0};
    return offset;
}

static uint32_t
__Named(
    uint32_t target,
    uint32_t count)
{
    uint32_t offset = __AddNode(1, 4, 40, count);
    ACPI_IORT_NAMED_COMPONENT* named = __Body(offset);

    named->MemoryAddressLimit = 40;
    memcpy(named->DeviceName, "\\_SB_.DEV0", 11);
    for (uint32_t i = 0; i < count; i++) {
        // The ignored fields deliberately overflow if incorrectly added.
        *__Map(offset, i) = (ACPI_IORT_ID_MAPPING){UINT32_MAX, UINT32_MAX, i, target, 1};
    }
    return offset;
}

static uint32_t
__Rmr(
    uint32_t target,
    uint32_t first,
    uint32_t count)
{
    uint32_t offset = __AddNode(6, 3, 48, 1);
    ACPI_IORT_RMR* rmr = __Body(offset);
    ACPI_IORT_RMR_DESC descriptor = {0x100000, 0x10000, 0};

    rmr->RmrCount = 1;
    rmr->RmrOffset = 28;
    memcpy(g_table + offset + 28, &descriptor, sizeof(descriptor));
    *__Map(offset, 0) = (ACPI_IORT_ID_MAPPING){UINT32_MAX, count, first, target, 0};
    return offset;
}

static void
__Checksum(void)
{
    ACPI_TABLE_HEADER* header = (void*)g_table;
    uint8_t checksum = 0;

    header->Checksum = 0;
    for (uint32_t i = 0; i < header->Length; i++) {
        checksum += g_table[i];
    }
    header->Checksum = (uint8_t)-checksum;
}

static void
__Finish(void)
{
    ((ACPI_TABLE_HEADER*)g_table)->Length = (uint32_t)g_length;
    __Checksum();
}

static oserr_t
__DiscoverAcpi(ArmSMMUDiscovery_t** discoveryOut)
{
    ArmIortCatalogue_t* catalogue = NULL;
    oserr_t status;

    if (discoveryOut == NULL) return OS_EINVALPARAMS;
    if (*discoveryOut != NULL) return OS_EBUSY;
    status = ArmIortCreate(&catalogue);
    if (status != OS_EOK) return status;
    status = ArmSMMUDiscoverIort(catalogue, discoveryOut);
    ArmIortDestroy(&catalogue);
    return status;
}

static ArmSMMUDiscovery_t*
__Discover(void)
{
    ArmSMMUDiscovery_t* discovery = NULL;

    __Finish();
    assert(__DiscoverAcpi(&discovery) == OS_EOK);
    assert(discovery != NULL && g_tables == 0);
    return discovery;
}

static void
__Reject(
    oserr_t expected)
{
    ArmSMMUDiscovery_t* discovery = NULL;

    __Finish();
    assert(__DiscoverAcpi(&discovery) == expected);
    assert(discovery == NULL && g_tables == 0 && g_allocations == 0);
}

static void
__LookupError(
    ArmSMMUDiscovery_t* discovery,
    uint32_t segment,
    uint16_t rid,
    oserr_t expected)
{
    ArmSMMUDeviceRoute_t result;
    ArmSMMUDeviceRoute_t before;

    memset(&result, 0xa5, sizeof(result));
    before = result;
    assert(ArmSMMUResolvePci(discovery, segment, rid, &result) == expected);
    assert(!memcmp(&result, &before, sizeof(result)));
}

static void
__NamedError(
    ArmSMMUDiscovery_t* discovery,
    oserr_t expected)
{
    ArmSMMUDeviceRoute_t result;
    ArmSMMUDeviceRoute_t before;

    memset(&result, 0xa5, sizeof(result));
    before = result;
    assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.DEV0", &result) == expected);
    assert(!memcmp(&result, &before, sizeof(result)));
}

static void
__MixedCatalogue(void)
{
    uint32_t v3, mmu500, its, named;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDescription_t unit;
    ArmSMMUDeviceRoute_t route;

    __Reset();
    v3 = __V3(1);
    mmu500 = __Mmu500();
    its = __AddNode(0, 1, 24, 0);
    ((ACPI_IORT_ITS_GROUP*)__Body(its))->ItsCount = 1;
    *__Map(v3, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0x10000, its, 0};
    __Root(v3, 0, 0, 65535, 0);
    __Root(mmu500, 1, 0x100, 255, 0x20);
    named = __Named(v3, 3);
    __Map(named, 0)->OutputBase = 9;
    __Map(named, 1)->OutputBase = 3;
    __Map(named, 2)->OutputBase = 9;
    discovery = __Discover();
    // Poison firmware bytes after release. Queries must use copied records.
    memset(g_table, 0xe5, g_length);
    assert(ArmSMMUGetUnitCount(discovery) == 2);
    assert(ArmSMMUGetUnitDescription(discovery, 0, &unit) == OS_EOK);
    assert(unit.Kind == ArmSMMUv3 && unit.RegisterBase == 0x100000000ULL);
    assert(unit.RegisterLength == 0x20000 && unit.TableCoherent);
    assert(unit.V3.EventQueue.Line == 74 && unit.V3.GlobalError.Line == 75);
    assert(unit.V3.EventQueue.Flags == INTERRUPT_ACPICONFORM_PRESENT);
    assert(unit.V3.GlobalError.Flags == INTERRUPT_ACPICONFORM_PRESENT);
    unit.RegisterBase = 0;
    assert(ArmSMMUGetUnitDescription(discovery, 0, &unit) == OS_EOK && unit.RegisterBase != 0);
    assert(ArmSMMUGetUnitDescription(discovery, 1, &unit) == OS_EOK);
    assert(unit.Kind == ArmSMMU500 && unit.Mmu500.ContextInterruptCount == 2);
    assert(unit.Mmu500.GlobalFault.Line == 99);
    assert(unit.Mmu500.ConfigurationFault.Line == UINT32_MAX && unit.Mmu500.ConfigurationFault.Flags == 0);
    assert(unit.Mmu500.ContextInterrupts[0].Line == 100 && unit.Mmu500.ContextInterrupts[1].Line == 100);
    assert(unit.Mmu500.ContextInterrupts[0].Flags == INTERRUPT_ACPICONFORM_PRESENT);
    assert(ArmSMMUResolvePci(discovery, 0, 0, &route) == OS_EOK && route.Requesters[0] == 0);
    assert(ArmSMMUResolvePci(discovery, 0, 65535, &route) == OS_EOK && route.Requesters[0] == 65535);
    assert(route.UnitIndex == 0 && route.CachePolicy == SHMDeviceCacheCoherent && route.AddressLimit == UINT64_MAX);
    assert(ArmSMMUResolvePci(discovery, 1, 0x1ff, &route) == OS_EOK && route.Requesters[0] == 0x11f);
    assert(route.UnitIndex == 1);
    __LookupError(discovery, 1, 0xff, OS_ENOENT);
    __LookupError(discovery, 1, 0x200, OS_ENOENT);
    __LookupError(discovery, 2, 0, OS_ENOENT);
    assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.DEV0", &route) == OS_EOK);
    assert(route.RequesterCount == 2 && route.Requesters[0] == 3 && route.Requesters[1] == 9);
    assert(route.CachePolicy == SHMDeviceCacheNonCoherent && route.AddressLimit == 0xffffffffffULL);
    assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.NONE", &route) == OS_ENOENT);
    ArmSMMUDiscoveryDestroy(&discovery);
    assert(discovery == NULL && g_allocations == 0);
}

static void
__Boundaries(void)
{
    uint32_t unit, root, named;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDeviceRoute_t route;

    __Reset();
    unit = __V3(0);
    __Root(unit, 0, 0, 0x7fff, 0);
    __Root(unit, 0, 0x8000, 0x7fff, 0x8000);
    discovery = __Discover();
    assert(ArmSMMUResolvePci(discovery, 0, 0x8000, &route) == OS_EOK && route.Requesters[0] == 0x8000);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    unit = __V3(0);
    root = __Root(unit, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX);
    __Map(root, 0)->Flags = 1;
    discovery = __Discover();
    assert(ArmSMMUResolvePci(discovery, UINT32_MAX, 0, &route) == OS_EOK && route.Requesters[0] == UINT32_MAX);
    assert(ArmSMMUResolvePci(discovery, UINT32_MAX, 65535, &route) == OS_EOK && route.Requesters[0] == UINT32_MAX);
    ArmSMMUDiscoveryDestroy(&discovery);
    for (uint32_t count = 255; count <= 256; count++) {
        __Reset();
        unit = __V3(0);
        named = __Named(unit, 1);
        *__Map(named, 0) = (ACPI_IORT_ID_MAPPING){0, count, UINT32_MAX - count, unit, 0};
        discovery = __Discover();
        if (count == 255) {
            assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.DEV0", &route) == OS_EOK);
            assert(route.RequesterCount == 256 && route.Requesters[255] == UINT32_MAX);
        } else {
            __NamedError(discovery, OS_ENOTSUPPORTED);
        }
        ArmSMMUDiscoveryDestroy(&discovery);
    }
}

static void
__UnsupportedRoutes(void)
{
    uint32_t v3, mmu500, named, root, its;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDeviceRoute_t route;

    __Reset();
    v3 = __V3(0);
    mmu500 = __Mmu500();
    __Root(v3, 0, 0, 65535, 0);
    __Root(mmu500, 1, 0, 65535, 0);
    named = __Named(v3, 2);
    __Map(named, 1)->OutputBase = 9;
    __Rmr(v3, 9, 0);
    discovery = __Discover();
    __LookupError(discovery, 0, 9, OS_ENOTSUPPORTED);
    assert(ArmSMMUResolvePci(discovery, 0, 8, &route) == OS_EOK);
    assert(ArmSMMUResolvePci(discovery, 0, 10, &route) == OS_EOK);
    assert(ArmSMMUResolvePci(discovery, 1, 9, &route) == OS_EOK && route.UnitIndex == 1);
    __NamedError(discovery, OS_ENOTSUPPORTED);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    v3 = __V3(0);
    mmu500 = __Mmu500();
    ((ACPI_IORT_SMMU*)__Body(mmu500))->Model = 0x80;
    __Root(mmu500, 0, 0, 65535, 0);
    discovery = __Discover();
    assert(ArmSMMUGetUnitCount(discovery) == 1);
    __LookupError(discovery, 0, 0, OS_ENOTSUPPORTED);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    v3 = __V3(0);
    mmu500 = __Mmu500();
    named = __Named(v3, 2);
    __Map(named, 1)->OutputReference = mmu500;
    discovery = __Discover();
    __NamedError(discovery, OS_ENOTSUPPORTED);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    its = __AddNode(0, 1, 24, 0);
    ((ACPI_IORT_ITS_GROUP*)__Body(its))->ItsCount = 1;
    __Root(its, 0, 0, 65535, 0);
    __Named(its, 1);
    discovery = __Discover();
    __LookupError(discovery, 0, 0, OS_ENOENT);
    __NamedError(discovery, OS_ENOENT);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    v3 = __V3(1);
    mmu500 = __Mmu500();
    *__Map(v3, 0) = (ACPI_IORT_ID_MAPPING){0, 0, 0, mmu500, 0};
    root = __Root(v3, 0, 0, 0, 0);
    discovery = __Discover();
    __LookupError(discovery, 0, 0, OS_ENOTSUPPORTED);
    ArmSMMUDiscoveryDestroy(&discovery);
    ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryAddressLimit = 0;
    discovery = __Discover();
    __LookupError(discovery, 0, 0, OS_ENOTSUPPORTED);
    ArmSMMUDiscoveryDestroy(&discovery);
}

static void
__Malformed(void)
{
    uint32_t unit, root, other;

    // Each case starts from fresh valid bytes. These are parser contract tests,
    // not replicas of its private helpers or internal catalogue layout.
    for (unsigned test = 0; test < 20; test++) {
        __Reset();
        unit = __V3(0);
        root = __Root(unit, 0, 0, 65535, 0);
        switch (test) {
            case 0: __Node(root)->MappingOffset = 16; break;
            case 1: __Node(root)->MappingOffset = UINT32_MAX; break;
            case 2: __Node(root)->MappingCount = UINT32_MAX; break;
            case 3: __Node(root)->Length = 0; break;
            case 4: __Map(root, 0)->OutputReference++; break;
            case 5: __Map(root, 0)->OutputReference = root; break;
            case 6: __Map(root, 0)->OutputBase = UINT32_MAX; break;
            case 7: __Map(root, 0)->InputBase = 1; break;
            case 8: __Map(root, 0)->Flags = 2; break;
            case 9: __Root(unit, 0, 65535, 0, 0); break;
            case 10: other = __V3(0); (void)other; break;
            case 11: ((ACPI_IORT_SMMU_V3*)__Body(unit))->BaseAddress = UINT64_MAX - 4095; break;
            case 12: ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryAddressLimit = 65; break;
            case 13: __Node(root)->Identifier = __Node(unit)->Identifier; break;
            case 14: ((ACPI_TABLE_IORT*)g_table)->NodeOffset = 0; break;
            case 15: ((ACPI_TABLE_IORT*)g_table)->NodeCount++; break;
            case 16: ((ACPI_TABLE_IORT*)g_table)->NodeCount--; break;
            case 17: ((ACPI_TABLE_IORT*)g_table)->Reserved = 1; break;
            case 18: ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryProperties = 2; break;
            case 19: ((ACPI_IORT_SMMU_V3*)__Body(unit))->Reserved = 1; break;
        }
        __Reject(OS_EINVALPARAMS);
    }
    for (unsigned test = 0; test < 8; test++) {
        __Reset();
        unit = __Mmu500();
        other = __Named(unit, 1);
        switch (test) {
            case 0: ((ACPI_IORT_SMMU*)__Body(unit))->ContextInterruptOffset = 60; break;
            case 1: ((ACPI_IORT_SMMU*)__Body(unit))->GlobalInterruptOffset = 44; break;
            case 2: ((ACPI_IORT_SMMU*)__Body(unit))->PmuInterruptCount = UINT32_MAX; break;
            case 3: memset(g_table + other + 29, 'A', 11); break;
            case 4: g_table[other + 29] = 'A'; break;
            case 5: g_table[other + 30] = '0'; break;
            case 6: __Named(unit, 1); break;
            case 7: ((ACPI_IORT_SMMU_GSI*)(g_table + unit + 60))->NSgIrptFlags = 2; break;
        }
        __Reject(OS_EINVALPARAMS);
    }
    __Reset();
    unit = __V3(1);
    other = __Mmu500();
    // Two-node cycle uses the v3 map and a root in the opposite direction.
    root = __Root(unit, 0, 0, 0, 0);
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 0, 0, root, 0};
    __Reject(OS_EINVALPARAMS);
    __Reset();
    unit = __V3(0);
    other = __Rmr(unit, UINT32_MAX, 1);
    __Reject(OS_EINVALPARAMS);
    __Map(other, 0)->IdCount = 0;
    ((ACPI_IORT_RMR*)__Body(other))->RmrOffset = 16;
    __Reject(OS_EINVALPARAMS);
}

static void
__LimitsAndRevisions(void)
{
    uint32_t unit, root;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDeviceRoute_t route;

    __Reset();
    for (uint32_t i = 0; i < 9; i++) {
        unit = __V3(0);
        ((ACPI_IORT_SMMU_V3*)__Body(unit))->BaseAddress += i * 0x20000;
        if (i == 7) {
            discovery = __Discover();
            assert(ArmSMMUGetUnitCount(discovery) == 8);
            ArmSMMUDiscoveryDestroy(&discovery);
        }
    }
    __Reject(OS_ENOTSUPPORTED);
    __Reset();
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t its = __AddNode(0, 1, 24, 0);

        ((ACPI_IORT_ITS_GROUP*)__Body(its))->ItsCount = 1;
    }
    discovery = __Discover();
    assert(ArmSMMUGetUnitCount(discovery) == 0);
    ArmSMMUDiscoveryDestroy(&discovery);
    ((ACPI_TABLE_IORT*)g_table)->NodeCount = 257;
    __Reject(OS_ENOTSUPPORTED);
    __Reset();
    unit = __V3(0);
    __Named(unit, 256);
    discovery = __Discover();
    assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.DEV0", &route) == OS_EOK);
    assert(route.RequesterCount == 256);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Reset();
    unit = __V3(0);
    __Named(unit, 257);
    __Reject(OS_ENOTSUPPORTED);
    __Reset();
    unit = __V3(0);
    root = __Root(unit, 0, 0, 0, 0);
    __Node(unit)->Revision = 99;
    __Reject(OS_ENOTSUPPORTED);
    __Node(unit)->Revision = 4;
    ((ACPI_TABLE_HEADER*)g_table)->Revision = 6;
    __Reject(OS_ENOTSUPPORTED);
    ((ACPI_TABLE_HEADER*)g_table)->Revision = 0;
    discovery = __Discover();
    assert(ArmSMMUResolvePci(discovery, 0, 0, &route) == OS_EOK);
    ArmSMMUDiscoveryDestroy(&discovery);
    // Root revision 4 has four additional fixed bytes before the mapping.
    ((ACPI_TABLE_HEADER*)g_table)->Revision = 5;
    memmove(g_table + root + 40, g_table + root + 36, 20);
    memset(g_table + root + 36, 0, 4);
    __Node(root)->Revision = 4;
    __Node(root)->Length += 4;
    __Node(root)->MappingOffset = 40;
    g_length += 4;
    discovery = __Discover();
    assert(ArmSMMUResolvePci(discovery, 0, 0, &route) == OS_EOK);
    ArmSMMUDiscoveryDestroy(&discovery);
    __Node(root)->MappingOffset = 36;
    __Reject(OS_EINVALPARAMS);
}

static void
__LegacyLayouts(void)
{
    uint32_t unit, root;
    ACPI_IORT_SMMU_V3* smmu;
    ACPI_IORT_ROOT_COMPLEX* pci;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDeviceRoute_t route;

    __Reset();
    ((ACPI_TABLE_HEADER*)g_table)->Revision = 0;
    // Revision-zero v3 stops before PXM and DeviceID index. Revision-zero
    // roots stop before the address limit, so their DMA limit is unknown.
    unit = __AddNode(4, 0, 60, 0);
    smmu = __Body(unit);
    smmu->BaseAddress = 0x8000000;
    smmu->Flags = 1;
    smmu->EventGsiv = 74;
    smmu->GerrGsiv = 75;
    root = __AddNode(2, 0, 32, 1);
    pci = __Body(root);
    pci->MemoryProperties = 0x0300000000000001ULL;
    *__Map(root, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0, unit, 0};
    __Named(unit, 1);
    discovery = __Discover();
    assert(ArmSMMUGetUnitCount(discovery) == 1);
    __LookupError(discovery, 0, 0, OS_ENOTSUPPORTED);
    assert(ArmSMMUResolveAcpiDevice(discovery, "\\_SB_.DEV0", &route) == OS_EOK);
    ArmSMMUDiscoveryDestroy(&discovery);
}

static void
__AuxiliaryCoverage(void)
{
    uint32_t unit, its, root, named, monitor, rmrNode;
    ArmSMMUDiscovery_t* discovery;
    ArmSMMUDescription_t description;

    __Reset();
    unit = __V3(1);
    its = __AddNode(0, 1, 24, 0);
    ((ACPI_IORT_ITS_GROUP*)__Body(its))->ItsCount = 1;
    // The selected control-MSI mapping ignores both input fields even though
    // its flag is clear. Event IRQ zero is copied as an absent wired line.
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->EventGsiv = 0;
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){UINT32_MAX, UINT32_MAX, 42, its, 0};
    root = __Root(unit, 0, 0, 0, 0);
    named = __Named(unit, 1);
    for (unsigned i = 0; i < 3; i++) {
        ACPI_IORT_PMCG* pmcg;

        monitor = __AddNode(5, 2, 40, 1);
        pmcg = __Body(monitor);
        pmcg->NodeReference = i == 0 ? unit : (i == 1 ? root : named);
        *__Map(monitor, 0) = (ACPI_IORT_ID_MAPPING){UINT32_MAX, UINT32_MAX, 43, its, 0};
    }
    discovery = __Discover();
    assert(ArmSMMUGetUnitDescription(discovery, 0, &description) == OS_EOK);
    assert(description.V3.EventQueue.Line == UINT32_MAX && description.V3.EventQueue.Flags == 0);
    ArmSMMUDiscoveryDestroy(&discovery);
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->IdMappingIndex = 1;
    __Reject(OS_EINVALPARAMS);
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->IdMappingIndex = 0;
    ((ACPI_IORT_PMCG*)__Body(monitor))->NodeReference++;
    __Reject(OS_EINVALPARAMS);

    // Individually bounded RMR nodes can still exceed the catalogue-wide
    // descriptor budget. The boundary case succeeds before one more is added.
    __Reset();
    unit = __V3(0);
    rmrNode = __AddNode(6, 3, 28 + 256 * 20, 1);
    ((ACPI_IORT_RMR*)__Body(rmrNode))->RmrCount = 256;
    ((ACPI_IORT_RMR*)__Body(rmrNode))->RmrOffset = 28;
    for (uint32_t i = 0; i < 256; i++) {
        ACPI_IORT_RMR_DESC range = {0x10000ULL * i, 0x10000, 0};

        memcpy(g_table + rmrNode + 28 + i * 20, &range, sizeof(range));
    }
    *__Map(rmrNode, 0) = (ACPI_IORT_ID_MAPPING){0, 0, 0, unit, 0};
    discovery = __Discover();
    ArmSMMUDiscoveryDestroy(&discovery);
    __Rmr(unit, 1, 0);
    __Reject(OS_ENOTSUPPORTED);
}

static void
__FailureCleanup(void)
{
    uint32_t unit;
    unsigned before;
    ArmSMMUDiscovery_t* discovery = NULL;
    ArmSMMUDescription_t description, saved;

    __Reset();
    unit = __V3(0);
    __Named(unit, 1);
    __Finish();
    for (int failure = 0; failure < 3; failure++) {
        g_failAfter = failure;
        assert(__DiscoverAcpi(&discovery) == OS_EOOM);
        assert(discovery == NULL && g_allocations == 0 && g_tables == 0);
    }
    g_failAfter = -1;
    g_tableStatus = AE_NOT_FOUND;
    assert(__DiscoverAcpi(&discovery) == OS_ENOENT);
    g_tableStatus = AE_ERROR;
    assert(__DiscoverAcpi(&discovery) == OS_EUNKNOWN);
    g_tableStatus = AE_OK;
    g_table[10] ^= 1;
    assert(__DiscoverAcpi(&discovery) == OS_EINVALPARAMS);
    assert(g_tables == 0 && g_allocations == 0);
    g_table[10] ^= 1;
    discovery = __Discover();
    before = g_getCalls;
    assert(__DiscoverAcpi(&discovery) == OS_EBUSY);
    assert(__DiscoverAcpi(NULL) == OS_EINVALPARAMS);
    assert(before == g_getCalls);
    memset(&description, 0xa5, sizeof(description));
    saved = description;
    assert(ArmSMMUGetUnitDescription(discovery, 1, &description) == OS_ENOENT);
    assert(!memcmp(&description, &saved, sizeof(description)));
    assert(ArmSMMUGetUnitDescription(NULL, 0, &description) == OS_EINVALPARAMS);
    assert(ArmSMMUGetUnitDescription(discovery, 0, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUGetUnitCount(NULL) == 0);
    __LookupError(NULL, 0, 0, OS_EINVALPARAMS);
    assert(ArmSMMUResolvePci(discovery, 0, 0, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUResolveAcpiDevice(discovery, NULL, NULL) == OS_EINVALPARAMS);
    ArmSMMUDiscoveryDestroy(&discovery);
    ArmSMMUDiscoveryDestroy(&discovery);
    ArmSMMUDiscoveryDestroy(NULL);
}

static void
__TruncationAndByteMutations(void)
{
    uint8_t original[512];
    uint32_t unit;
    size_t length;

    __Reset();
    unit = __V3(0);
    __Root(unit, 0, 0, 65535, 0);
    __Finish();
    length = g_length;
    assert(length < sizeof(original));
    memcpy(original, g_table, length);
    for (size_t prefix = 0; prefix < length; prefix++) {
        ArmSMMUDiscovery_t* discovery = NULL;

        memcpy(g_table, original, length);
        ((ACPI_TABLE_HEADER*)g_table)->Length = (uint32_t)prefix;
        __Checksum();
        assert(__DiscoverAcpi(&discovery) != OS_EOK);
        assert(discovery == NULL && g_allocations == 0 && g_tables == 0);
    }
    // Deterministic byte mutations check memory safety and balanced cleanup.
    // They complement the semantic assertions above, not replace them.
    for (size_t byte = 48; byte < length; byte++) {
        ArmSMMUDiscovery_t* discovery = NULL;
        oserr_t status;

        memcpy(g_table, original, length);
        g_table[byte] ^= 0xff;
        __Checksum();
        status = __DiscoverAcpi(&discovery);
        if (status == OS_EOK) {
            assert(ArmSMMUGetUnitCount(discovery) <= 8);
            ArmSMMUDiscoveryDestroy(&discovery);
        } else {
            assert(discovery == NULL);
        }
        assert(g_allocations == 0 && g_tables == 0);
    }
}

/** Build a root with either no output or a complete, split ITS mapping. */
static uint32_t
__DirectRoot(
    uint32_t maps)
{
    uint32_t its = __AddNode(0, 1, 24, 0);
    uint32_t offset = __AddNode(2, 3, 36, maps);
    ACPI_IORT_ROOT_COMPLEX* root = __Body(offset);

    ((ACPI_IORT_ITS_GROUP*)__Body(its))->ItsCount = 1;
    root->MemoryProperties = 0x0300000000000001ULL;
    root->MemoryAddressLimit = 64;
    if (maps == 1) {
        *__Map(offset, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0, its, 0};
    } else if (maps == 2) {
        // Reverse firmware order to exercise coverage independently of sorting.
        *__Map(offset, 0) = (ACPI_IORT_ID_MAPPING){0x8000, 0x7fff, 0x8000, its, 0};
        *__Map(offset, 1) = (ACPI_IORT_ID_MAPPING){0, 0x7fff, 0, its, 0};
    }
    return offset;
}

static ArmIortCatalogue_t*
__Catalogue(void)
{
    ArmIortCatalogue_t* catalogue = NULL;

    __Finish();
    assert(ArmIortCreate(&catalogue) == OS_EOK);
    assert(catalogue != NULL && g_tables == 0);
    return catalogue;
}

static void
__DirectLimitError(
    ArmIortCatalogue_t* discovery,
    uint32_t segment,
    oserr_t expected)
{
    uint64_t limit = 0x9876543210ULL;

    assert(ArmIortGetPciDirectLimit(discovery, segment, &limit) == expected);
    assert(limit == 0x9876543210ULL);
}

static void
__DirectSegments(void)
{
    ArmIortCatalogue_t* discovery;
    uint64_t limit;
    uint32_t root, unit;

    for (uint32_t maps = 0; maps <= 2; maps++) {
        __Reset();
        root = __DirectRoot(maps);
        ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryAddressLimit = 40;
        // An unrelated translated segment does not change this root's policy.
        unit = __V3(0);
        __Root(unit, 1, 0, 65535, 0);
        discovery = __Catalogue();
        memset(g_table, 0xa5, g_length);
        assert(ArmIortGetPciDirectLimit(discovery, 0, &limit) == OS_EOK);
        assert(limit == 0xffffffffffULL);
        __DirectLimitError(discovery, 1, OS_ENOTSUPPORTED);
        __DirectLimitError(discovery, 2, OS_ENOTSUPPORTED);
        __DirectLimitError(NULL, 0, OS_EINVALPARAMS);
        assert(ArmIortGetPciDirectLimit(discovery, 0, NULL) == OS_EINVALPARAMS);
        ArmIortDestroy(&discovery);
    }

    __Reset();
    __DirectRoot(1);
    discovery = __Catalogue();
    assert(ArmIortGetPciDirectLimit(discovery, 0, &limit) == OS_EOK && limit == UINT64_MAX);
    // The existing translated lookup still reports no translated route.
    {
        ArmSMMUDiscovery_t* dma = NULL;

        assert(ArmSMMUDiscoverIort(discovery, &dma) == OS_EOK);
        __LookupError(dma, 0, 0, OS_ENOENT);
        ArmSMMUDiscoveryDestroy(&dma);
    }
    ArmIortDestroy(&discovery);

    __Reset();
    root = __DirectRoot(1);
    // SINGLE_MAPPING covers every RID and ignores both input fields.
    __Map(root, 0)->Flags = 1;
    __Map(root, 0)->InputBase = UINT32_MAX;
    __Map(root, 0)->IdCount = UINT32_MAX;
    discovery = __Catalogue();
    assert(ArmIortGetPciDirectLimit(discovery, 0, &limit) == OS_EOK && limit == UINT64_MAX);
    ArmIortDestroy(&discovery);

    // Every coherency bit is required even when the root has no output map.
    for (uint32_t maps = 0; maps <= 1; maps++) {
        const uint64_t properties[] = {
            0x0300000000000000ULL, 0x0200000000000001ULL, 0x0100000000000001ULL
        };

        for (size_t i = 0; i < SIZEOF_ARRAY(properties); i++) {
            __Reset();
            root = __DirectRoot(maps);
            ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryProperties = properties[i];
            discovery = __Catalogue();
            __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
            ArmIortDestroy(&discovery);
        }
        __Reset();
        root = __DirectRoot(maps);
        ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryAddressLimit = 0;
        discovery = __Catalogue();
        __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
        ArmIortDestroy(&discovery);
    }

    __Reset();
    __DirectRoot(0);
    __DirectRoot(0);
    discovery = __Catalogue();
    __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
    ArmIortDestroy(&discovery);

    __Reset();
    root = __DirectRoot(2);
    __Map(root, 1)->IdCount--;
    discovery = __Catalogue();
    __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
    ArmIortDestroy(&discovery);
    __Map(root, 1)->IdCount += 2;
    __Reject(OS_EINVALPARAMS);

    __Reset();
    root = __DirectRoot(1);
    __Map(root, 0)->InputBase = 1;
    __Reject(OS_EINVALPARAMS);

    __Reset();
    root = __DirectRoot(2);
    unit = __V3(0);
    __Map(root, 1)->OutputReference = unit;
    discovery = __Catalogue();
    __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
    ArmIortDestroy(&discovery);
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->Model = 0xff;
    discovery = __Catalogue();
    __DirectLimitError(discovery, 0, OS_ENOTSUPPORTED);
    ArmIortDestroy(&discovery);

    __Reset();
    root = __DirectRoot(1);
    __Map(root, 0)->OutputReference = __AddNode(0xff, 0, 16, 0);
    __Reject(OS_ENOTSUPPORTED);
}

#include "iort_msi_test.inc"

int
main(void)
{
    __MsiDirect();
    __MsiControllers();
    __MsiThroughSmmu();
    __MsiControlAndAmbiguity();
    __MsiDmaLimits();
    __CatalogueFailures();
    __SharedCatalogue();
    __DirectSegments();
    __MixedCatalogue();
    __Boundaries();
    __UnsupportedRoutes();
    __Malformed();
    __LimitsAndRevisions();
    __LegacyLayouts();
    __AuxiliaryCoverage();
    __FailureCleanup();
    __TruncationAndByteMutations();
    assert(g_allocations == 0 && g_tables == 0);
    puts("Arm SMMU discovery: all cases passed");
    return 0;
}
