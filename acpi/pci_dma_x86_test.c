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
 */

// The x86 policy keeps coherent identity addressing without reading Arm firmware.
#include "../../kernel/arch/x86/components/acpi-pci.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    const struct AcpiPciRoot root = {.Id = 1, .Segment = 7};
    AcpiPciDma_t dma, before;

    memset(&dma, 0xa5, sizeof(dma));
    before = dma;
    assert(ArchAcpiGetPciDma(NULL, &root, &dma) == OS_EINVALPARAMS);
    assert(!memcmp(&dma, &before, sizeof(dma)));
    assert(ArchAcpiGetPciDma((void*)1, NULL, &dma) == OS_EINVALPARAMS);
    assert(!memcmp(&dma, &before, sizeof(dma)));
    assert(ArchAcpiGetPciDma((void*)1, &root, NULL) == OS_EINVALPARAMS);
    assert(ArchAcpiGetPciDma((void*)1, &root, &dma) == OS_EOK);
    assert(dma.AddressLimit == UINT64_MAX && dma.CachePolicy == SHMDeviceCacheCoherent);
    assert(dma.Flags == ACPI_PCI_DMA_DIRECT);
    puts("ACPI PCI x86 DMA policy: PASS");
    return 0;
}
