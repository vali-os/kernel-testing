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

// Included after the wire fixture builders. No interrupt hardware is present.
static uint32_t
__ItsGroup(uint32_t first, uint32_t count)
{
    uint32_t offset = __AddNode(0, 1, 20 + 4 * count, 0);

    ((ACPI_IORT_ITS_GROUP*)__Body(offset))->ItsCount = count;
    for (uint32_t i = 0; i < count; i++) {
        memcpy(g_table + offset + 20 + 4 * i, &first, sizeof(first));
        first += 7;
    }
    return offset;
}

static uint32_t
__Mmu500WithMapping(void)
{
    uint32_t offset = __Mmu500();

    __Node(offset)->MappingOffset = __Node(offset)->Length;
    __Node(offset)->MappingCount = 1;
    __Node(offset)->Length += 20;
    g_length += 20;
    return offset;
}

static void
__MsiCheck(const ArmIortCatalogue_t* catalogue, uint32_t segment, uint16_t rid,
           uint32_t groupOffset, uint32_t deviceId, uint32_t firstIts, uint32_t count)
{
    ArmIortMsiRoute_t route;

    assert(ArmIortResolvePciMsi(catalogue, segment, rid, &route) == OS_EOK);
    assert(route.DeviceId == deviceId && route.ItsGroup->Offset == groupOffset);
    assert(route.ItsGroup->Type == ArmIortItsGroup);
    assert(route.ItsGroup->ItsCount == count);
    for (uint32_t i = 0; i < count; i++) {
        assert(route.ItsGroup->ItsIdentifiers[i] == firstIts + 7 * i);
    }
}

static void
__MsiError(const ArmIortCatalogue_t* catalogue, uint32_t segment, uint16_t rid, oserr_t expected)
{
    ArmIortMsiRoute_t route, before;

    memset(&route, 0xa5, sizeof(route));
    before = route;
    assert(ArmIortResolvePciMsi(catalogue, segment, rid, &route) == expected);
    assert(!memcmp(&route, &before, sizeof(route)));
}

static void
__MsiDirect(void)
{
    ArmIortCatalogue_t* catalogue;
    uint32_t group, other, root;

    __Reset();
    group = __ItsGroup(17, 3);
    __Node(group)->Identifier = 0xfeed1234;
    other = __ItsGroup(51, 1);
    __Root(group, 7, 0x100, 0xff, 0xffffff00);
    __Root(other, 7, 0x200, 0x7f, 0x10000);
    __Root(other, 8, 0x100, 0xff, 0x20000);
    catalogue = __Catalogue();
    // ACPICA released its exact-sized table allocation. Poison the builder too.
    memset(g_table, 0xa5, g_length);
    __MsiCheck(catalogue, 7, 0x100, group, 0xffffff00, 17, 3);
    __MsiCheck(catalogue, 7, 0x1ff, group, UINT32_MAX, 17, 3);
    __MsiCheck(catalogue, 7, 0x200, other, 0x10000, 51, 1);
    __MsiCheck(catalogue, 8, 0x100, other, 0x20000, 51, 1);
    __MsiError(catalogue, 7, 0xff, OS_ENOENT);
    __MsiError(catalogue, 7, 0x280, OS_ENOENT);
    __MsiError(catalogue, 9, 0x100, OS_ENOENT);
    __MsiError(NULL, 0, 0, OS_EINVALPARAMS);
    assert(ArmIortResolvePciMsi(catalogue, 7, 0x100, NULL) == OS_EINVALPARAMS);
    assert(ArmIortGetNode(catalogue, 0)->Identifier == 0xfeed1234);
    assert(ArmIortGetNode(catalogue, ArmIortGetNodeCount(catalogue)) == NULL);
    assert(ArmIortGetNode(NULL, 0) == NULL && ArmIortGetNodeCount(NULL) == 0);
    __DirectLimitError(catalogue, 7, OS_ENOTSUPPORTED);
    ArmIortDestroy(&catalogue);

    __Reset();
    group = __ItsGroup(12, 1);
    root = __Root(group, 0, 0, 65535, 0);
    __Map(root, 0)->Flags = 1;
    __Map(root, 0)->InputBase = UINT32_MAX;
    __Map(root, 0)->IdCount = UINT32_MAX;
    __Map(root, 0)->OutputBase = 321;
    catalogue = __Catalogue();
    __MsiCheck(catalogue, 0, 0, group, 321, 12, 1);
    __MsiCheck(catalogue, 0, UINT16_MAX, group, 321, 12, 1);
    ArmIortDestroy(&catalogue);

    __Reset();
    group = __ItsGroup(0, 0);
    __Root(group, 0, 0, 65535, 0);
    catalogue = __Catalogue();
    __MsiError(catalogue, 0, 5, OS_ENOTSUPPORTED);
    ArmIortDestroy(&catalogue);
}

static void
__MsiControllers(void)
{
    Arm64ItsDescription_t controllers[] = {
        {51, 0x8400000, 0x20000},
        {24, 0x8200000, 0x20000},
        {17, 0x8300000, 0x20000}
    };
    Arm64ItsDescription_t selected;
    Arm64ItsDescription_t before;
    ArmIortCatalogue_t*   catalogue;
    ArmIortMsiRoute_t     route;
    ArmIortMsiRoute_t     otherRoute;
    uint32_t              group;
    uint32_t              other;

    __Reset();
    group = __ItsGroup(17, 3);
    other = __ItsGroup(51, 1);
    __Root(group, 7, 0x100, 0xff, 0x10000);
    __Root(other, 8, 0x100, 0xff, 0x20000);
    catalogue = __Catalogue();
    memset(g_table, 0xa5, g_length);
    assert(ArmIortResolvePciMsi(catalogue, 7, 0x123, &route) == OS_EOK);
    assert(ArmIortResolvePciMsi(catalogue, 8, 0x123, &otherRoute) == OS_EOK);
    g_arm64Platform.Gic.ItsCount = 3;
    g_arm64Platform.Gic.ItsControllers = controllers;
    assert(ArmIortGetItsController(route.ItsGroup, 17, &selected) == OS_EOK);
    assert(selected.Identifier == 17 && selected.Base == 0x8300000 && selected.Length == 0x20000);
    assert(ArmIortGetItsController(route.ItsGroup, 24, &selected) == OS_EOK);
    assert(selected.Identifier == 24 && selected.Base == 0x8200000);
    assert(route.DeviceId == 0x10023 && route.ItsGroup->ItsCount == 3);
    assert(ArmIortGetItsController(otherRoute.ItsGroup, 51, &selected) == OS_EOK);
    assert(selected.Identifier == 51 && selected.Base == 0x8400000);

    memset(&selected, 0xa5, sizeof(selected));
    before = selected;
    assert(ArmIortGetItsController(route.ItsGroup, 31, &selected) == OS_ENOENT);
    assert(!memcmp(&before, &selected, sizeof(selected))); // Member missing from MADT.
    assert(ArmIortGetItsController(route.ItsGroup, 51, &selected) == OS_ENOENT);
    assert(!memcmp(&before, &selected, sizeof(selected))); // Hardware belongs to another group.
    assert(ArmIortGetItsController(route.ItsGroup, 999, &selected) == OS_ENOENT);
    assert(!memcmp(&before, &selected, sizeof(selected)));
    assert(ArmIortGetItsController(NULL, 17, &selected) == OS_EINVALPARAMS);
    assert(ArmIortGetItsController(route.ItsGroup, 17, NULL) == OS_EINVALPARAMS);
    assert(ArmIortGetItsController(ArmIortGetNode(catalogue, 2), 17, &selected) == OS_EINVALPARAMS);
    assert(!memcmp(&before, &selected, sizeof(selected)));
    g_arm64Platform.Gic.ItsCount = 0;
    g_arm64Platform.Gic.ItsControllers = NULL;
    assert(ArmIortGetItsController(route.ItsGroup, 17, &selected) == OS_ENOENT);
    assert(!memcmp(&before, &selected, sizeof(selected)));
    ArmIortDestroy(&catalogue);
}

static void
__MsiThroughSmmu(void)
{
    for (unsigned model = 0; model < 3; model++) {
        ArmIortCatalogue_t* catalogue;
        ArmSMMUDiscovery_t* dma = NULL;
        uint32_t group, control, unit, root;

        __Reset();
        group = __ItsGroup(5, 2);
        control = __ItsGroup(90, 1);
        unit = model == 0 ? __Mmu500WithMapping() : __V3(2);
        root = __Root(unit, 3, 0x100, 0xff, 0x10000);
        *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0x10000, 0x7f, 0xffffff80, group, 0};
        if (model != 0) {
            ACPI_IORT_SMMU_V3* smmu = __Body(unit);

            smmu->EventGsiv = 0;
            smmu->IdMappingIndex = 1;
            // An unflagged control mapping still ignores input base and count.
            *__Map(unit, 1) = (ACPI_IORT_ID_MAPPING){UINT32_MAX, UINT32_MAX, 42, control, model - 1};
            if (model == 2) smmu->Model = 0xff;
        }
        // Exercise RMR, root attributes and unsupported models independently.
        if (model == 1) ((ACPI_IORT_ROOT_COMPLEX*)__Body(root))->MemoryAddressLimit = 0;
        __Rmr(unit, 0x10000, 0);
        catalogue = __Catalogue();
        assert(ArmSMMUDiscoverIort(catalogue, &dma) == OS_EOK);
        __LookupError(dma, 3, 0x100, OS_ENOTSUPPORTED);
        __DirectLimitError(catalogue, 3, OS_ENOTSUPPORTED);
        __MsiCheck(catalogue, 3, 0x100, group, 0xffffff80, 5, 2);
        __MsiCheck(catalogue, 3, 0x17f, group, UINT32_MAX, 5, 2);
        __MsiError(catalogue, 3, 0x180, OS_ENOENT);
        __MsiError(catalogue, 3, 0x200, OS_ENOENT);
        if (model != 0) {
            const ArmIortNode_t* node = ArmIortGetNode(catalogue, 2);

            assert(node->Mappings[1].IsOwnInterruptMapping && node->Mappings[1].MapsAllInputsToOne);
            assert(!node->Mappings[0].IsOwnInterruptMapping);
            assert(ArmIortGetNode(catalogue, node->Mappings[1].TargetIndex)->Offset == control);
        }
        ArmSMMUDiscoveryDestroy(&dma);
        ArmIortDestroy(&catalogue);
    }
}

static void
__MsiControlAndAmbiguity(void)
{
    ArmIortCatalogue_t* catalogue;
    uint32_t group, unit;

    __Reset();
    group = __ItsGroup(9, 1);
    unit = __V3(2);
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->EventGsiv = 0;
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 0, 42, group, 1};
    *__Map(unit, 1) = (ACPI_IORT_ID_MAPPING){10, 9, 100, group, 0};
    __Root(unit, 0, 0, 65535, 0);
    catalogue = __Catalogue();
    __MsiCheck(catalogue, 0, 10, group, 100, 9, 1);
    __MsiError(catalogue, 0, 9, OS_ENOENT);
    ArmIortDestroy(&catalogue);

    // All four wired interrupts make even an out-of-range control index inert.
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->EventGsiv = 48;
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->IdMappingIndex = UINT32_MAX;
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 9, 200, group, 0};
    catalogue = __Catalogue();
    __MsiCheck(catalogue, 0, 9, group, 209, 9, 1);
    __MsiCheck(catalogue, 0, 10, group, 100, 9, 1);
    ArmIortDestroy(&catalogue);

    // Overlapping SMMU output mappings cannot silently select the first ITS.
    __Map(unit, 0)->IdCount = 10;
    catalogue = __Catalogue();
    __MsiError(catalogue, 0, 10, OS_EINVALPARAMS);
    ArmIortDestroy(&catalogue);

    // A control-only SMMU has no endpoint MSI route.
    __Reset();
    group = __ItsGroup(9, 1);
    unit = __V3(1);
    ((ACPI_IORT_SMMU_V3*)__Body(unit))->EventGsiv = 0;
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 0, 42, group, 1};
    __Root(unit, 0, 0, 65535, 0);
    catalogue = __Catalogue();
    __MsiError(catalogue, 0, 0, OS_ENOENT);
    __MsiError(catalogue, 0, UINT16_MAX, OS_ENOENT);
    ArmIortDestroy(&catalogue);
}

static void
__MsiDmaLimits(void)
{
    ArmIortCatalogue_t* catalogue;
    ArmSMMUDiscovery_t* dma = NULL;
    uint32_t group, unit = 0, next;

    __Reset();
    group = __ItsGroup(3, 1);
    for (uint32_t i = 0; i < 9; i++) {
        unit = __V3(1);
        ((ACPI_IORT_SMMU_V3*)__Body(unit))->BaseAddress += 0x20000 * i;
        *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, UINT32_MAX, 0, group, 0};
    }
    __Root(unit, 0, 0, 65535, 0xffff0000);
    catalogue = __Catalogue();
    assert(ArmSMMUDiscoverIort(catalogue, &dma) == OS_ENOTSUPPORTED && dma == NULL);
    __MsiCheck(catalogue, 0, UINT16_MAX, group, UINT32_MAX, 3, 1);
    ArmIortDestroy(&catalogue);

    __Reset();
    group = __ItsGroup(3, 1);
    unit = __V3(1);
    next = __Mmu500WithMapping();
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0, next, 0};
    *__Map(next, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0, group, 0};
    __Root(unit, 0, 0, 65535, 0);
    catalogue = __Catalogue();
    __MsiError(catalogue, 0, 0, OS_ENOTSUPPORTED);
    ArmIortDestroy(&catalogue);
}

static void
__CatalogueFailures(void)
{
    ArmIortCatalogue_t* catalogue = NULL;
    ArmSMMUDiscovery_t* dma = NULL;
    uint32_t group, unit;
    int failures = 0;

    __Reset();
    group = __ItsGroup(1, 2);
    unit = __V3(1);
    *__Map(unit, 0) = (ACPI_IORT_ID_MAPPING){0, 65535, 0, group, 0};
    __Named(unit, 1);
    __Finish();
    // Fail every allocation in both the decoder and DMA projection.
    for (int failure = 0; failure < 16; failure++) {
        oserr_t status;

        g_failAfter = failure;
        status = ArmIortCreate(&catalogue);
        if (status == OS_EOK) {
            status = ArmSMMUDiscoverIort(catalogue, &dma);
            if (status == OS_EOK) ArmSMMUDiscoveryDestroy(&dma);
            ArmIortDestroy(&catalogue);
        }
        assert(g_allocations == 0 && g_tables == 0 && catalogue == NULL && dma == NULL);
        if (status == OS_EOK) break;
        assert(status == OS_EOOM);
        failures++;
    }
    assert(failures == 7);
    g_failAfter = -1;
    catalogue = __Catalogue();
    assert(ArmIortCreate(&catalogue) == OS_EBUSY);
    assert(ArmIortCreate(NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUDiscoverIort(NULL, &dma) == OS_EINVALPARAMS);
    assert(ArmSMMUDiscoverIort(catalogue, NULL) == OS_EINVALPARAMS);
    assert(ArmSMMUDiscoverIort(catalogue, &dma) == OS_EOK);
    assert(ArmSMMUDiscoverIort(catalogue, &dma) == OS_EBUSY);
    ArmSMMUDiscoveryDestroy(&dma);
    ArmIortDestroy(&catalogue);
    ArmIortDestroy(&catalogue);
    ArmIortDestroy(NULL);
}

static void
__SharedCatalogue(void)
{
    // Isolate boot lifetimes without adding a production reset or shutdown API.
    for (unsigned missing = 0; missing < 2; missing++) {
        pid_t child = fork();
        int status;

        assert(child >= 0);
        if (child == 0) {
            const ArmIortCatalogue_t* first = NULL;
            const ArmIortCatalogue_t* second = NULL;
            unsigned before = g_getCalls;
            uint32_t group;

            __Reset();
            group = __ItsGroup(4, 1);
            __Root(group, 0, 0, 65535, 0);
            __Finish();
            assert(ArmIortGetCatalogue(&first) == OS_ENOENT && first == NULL);
            if (missing) g_tableStatus = AE_NOT_FOUND;
            assert(ArmIortInitialize() == (missing ? OS_ENOENT : OS_EOK));
            g_tableStatus = AE_ERROR;
            assert(ArmIortInitialize() == (missing ? OS_ENOENT : OS_EOK));
            assert(g_getCalls == before + 1 && g_tables == 0);
            assert(ArmIortGetCatalogue(NULL) == OS_EINVALPARAMS);
            if (missing) {
                assert(ArmIortGetCatalogue(&first) == OS_ENOENT && first == NULL);
                assert(g_allocations == 0);
            } else {
                assert(ArmIortGetCatalogue(&first) == OS_EOK);
                assert(ArmIortGetCatalogue(&second) == OS_EOK && first == second);
                __MsiCheck(first, 0, 10, group, 10, 4, 1);
            }
            _exit(0);
        }
        assert(waitpid(child, &status, 0) == child);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
}
