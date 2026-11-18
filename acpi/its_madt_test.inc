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

static ACPI_MADT_GENERIC_TRANSLATOR*
__AppendIts(
    _In_ uint32_t identifier,
    _In_ uint64_t base)
{
    ACPI_TABLE_HEADER*            header = (void*)g_madt;
    ACPI_MADT_GENERIC_TRANSLATOR* its = (void*)(g_madt + header->Length);

    memset(its, 0, sizeof(*its));
    its->Header.Type = ACPI_MADT_TYPE_GENERIC_TRANSLATOR;
    its->Header.Length = sizeof(*its);
    its->TranslationId = identifier;
    its->BaseAddress = base;
    header->Length += sizeof(*its);
    return its;
}

static void
__ItsMadtFixture(
    _In_ int regions)
{
    __Reset();
    __Gic()->Version = 3;
    for (unsigned int i = 0; i < 3; i++) {
        __Cpu(i)->GicrBaseAddress = 0x80a0000 + i * 0x20000;
    }
    if (regions) {
        ACPI_TABLE_HEADER*              header = (void*)g_madt;
        ACPI_MADT_GENERIC_REDISTRIBUTOR region = {0};

        region.Header.Type = ACPI_MADT_TYPE_GENERIC_REDISTRIBUTOR;
        region.Header.Length = sizeof(region);
        region.BaseAddress = 0x80a0000;
        region.Length = 0x60000;
        memcpy(g_madt + header->Length, &region, sizeof(region));
        header->Length += sizeof(region);
        // These GICC addresses must be ignored in favor of the GICR range.
        __Cpu(0)->GicrBaseAddress = 0x8200000;
    }
}

static void
__ItsMadtTests(void)
{
    ACPI_TABLE_HEADER*            header = (void*)g_madt;
    ACPI_MADT_GENERIC_TRANSLATOR* entry;

    for (unsigned int regions = 0; regions < 2; regions++) {
        ACPI_MADT_GENERIC_TRANSLATOR first;

        __ItsMadtFixture(regions);
        first = *__AppendIts(0, 0x8200000);
        // Put the ITS before GICD, GICC and GICR to check order independence.
        memmove(g_madt + sizeof(ACPI_TABLE_MADT) + sizeof(first),
            g_madt + sizeof(ACPI_TABLE_MADT),
            header->Length - sizeof(ACPI_TABLE_MADT) - sizeof(first));
        memcpy(g_madt + sizeof(ACPI_TABLE_MADT), &first, sizeof(first));
        __AppendIts(UINT32_MAX, (1ULL << 48) - 0x20000);
        __Checksum(header);
        assert(ArchAcpiInitialize() == OS_EOK);
        assert(!g_liveTables && g_arm64Platform.Gic.ItsCount == 2);
        memset(g_madt, 0xa5, sizeof(g_madt));
        assert(g_arm64Platform.Gic.ItsControllers[0].Identifier == 0);
        assert(g_arm64Platform.Gic.ItsControllers[0].Base == 0x8200000);
        assert(g_arm64Platform.Gic.ItsControllers[0].Length == 0x20000);
        assert(g_arm64Platform.Gic.ItsControllers[1].Identifier == UINT32_MAX);
        assert(g_arm64Platform.Gic.ItsControllers[1].Base == (1ULL << 48) - 0x20000);

        __ItsMadtFixture(regions);
        __AppendIts(4, 0x80b0000);
        __Checksum(header);
        __Reject(); // Overlaps a GICR range or a per-CPU redistributor.

        for (unsigned int allocation = 0; allocation < 2 + regions; allocation++) {
            __ItsMadtFixture(regions);
            __AppendIts(4, 0x8200000);
            __Checksum(header);
            g_allocationsBeforeFailure = (int)allocation;
            __Reject();
        }
    }

    for (unsigned int length = 2; length < 20; length++) {
        __ItsMadtFixture(0);
        entry = __AppendIts(4, 0x8200000);
        entry->Header.Length = length;
        header->Length -= sizeof(*entry) - length;
        __Checksum(header);
        __Reject();
    }

    for (unsigned int invalid = 0; invalid < 9; invalid++) {
        __ItsMadtFixture(0);
        __AppendIts(4, 0x8200000);
        entry = __AppendIts(9, 0x8230000); // A 64 KiB aligned base is sufficient.
        switch (invalid) {
            case 0: entry->TranslationId = 4; break;
            case 1: entry->BaseAddress = 0x8210000; break;
            case 2: entry->BaseAddress = 0x8000000; break;
            case 3: entry->BaseAddress = 0; break;
            case 4: entry->BaseAddress++; break;
            case 5: entry->BaseAddress = (1ULL << 48) - 0x10000; break;
            case 6: entry->Reserved = 1; break;
            case 7: entry->Reserved2 = 1; break;
            case 8: __Gic()->Version = 2; break;
        }
        __Checksum(header);
        __Reject();
    }

    __ItsMadtFixture(0);
    __AppendIts(4, 0x8210000);
    __Checksum(header);
    assert(ArchAcpiInitialize() == OS_EOK);
    assert(g_arm64Platform.Gic.ItsControllers[0].Base == 0x8210000);

    __ItsMadtFixture(1);
    __AppendIts(4, 0x8200000);
    __Checksum(header);
    g_gtdt.Header.Checksum++;
    __Reject(); // A later required-table failure frees the copied ITS records.

    __ItsMadtFixture(1);
    __AppendIts(4, 0x8200000);
    __Checksum(header);
    g_controllerStatus = OS_EOOM;
    assert(ArchAcpiInitialize() == OS_EOOM);
    assert(!g_allocations && !g_liveTables && !g_arm64Platform.Gic.ItsCount);

    for (unsigned int extra = 0; extra < 2; extra++) {
        __ItsMadtFixture(0);
        for (unsigned int i = 0; i < ARM64_ACPI_MAX_ITS + extra; i++) {
            __AppendIts(i, 0x8200000 + i * 0x20000);
        }
        __Checksum(header);
        if (extra) {
            __Reject();
        } else {
            assert(ArchAcpiInitialize() == OS_EOK);
            assert(g_arm64Platform.Gic.ItsCount == ARM64_ACPI_MAX_ITS);
        }
    }
    __Reset();
}
