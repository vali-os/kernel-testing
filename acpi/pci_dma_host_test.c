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

// Verify root-policy loading and the common ACPI ECAM DMA callback.
// Any configuration-space access during a DMA query is a test failure.
#ifndef PCI_DMA_HOST_SOURCE
#define PCI_DMA_HOST_SOURCE "../../services/deviced/bus/pci/dma.c"
#endif
#include PCI_DMA_HOST_SOURCE
#include "../../services/deviced/bus/pci/hosts/ecam.c"
#ifndef PCI_LEGACY_DMA_SOURCE
#define PCI_LEGACY_DMA_SOURCE "../../services/deviced/bus/pci/hosts/legacy.c"
#endif
#include PCI_LEGACY_DMA_SOURCE
#ifndef PCI_ROOT_LOAD_SOURCE
#define PCI_ROOT_LOAD_SOURCE "../../services/deviced/bus/pci/host-windows.c"
#endif
#include PCI_ROOT_LOAD_SOURCE
#include <assert.h>
#include <stdio.h>

static oserr_t g_status;
static struct PciAddressWindow g_window = {.Space = PciAddressMemory32, .Length = 4096};
static unsigned g_queries;

void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

const struct PciHostIdentification*
PciHostGetIdentification(const PciHost_t* host)
{
    return &host->Identification;
}

oserr_t
AcpiQueryPci(const struct AcpiPciQuery* query, union AcpiPciResult* result)
{
    assert(query->Type == AcpiPciQueryWindow && query->RootId == 7 && query->Index == 0);
    g_queries++;
    if (g_status != OS_EOK) return g_status;
    result->Window = g_window;
    return OS_EOK;
}

size_t
DeviceIoRead(DeviceIo_t* io, size_t offset, size_t width)
{
    assert(false && "DMA policy query read a device register");
    return 0;
}

oserr_t
DeviceIoWrite(DeviceIo_t* io, size_t offset, size_t value, size_t width)
{
    assert(false && "DMA policy query wrote a device register");
    return OS_EUNKNOWN;
}

oserr_t
FdtResolvePciInterrupt(const struct FdtPciHost* host, unsigned int bus, unsigned int slot,
                       unsigned int function, unsigned int pin, int* line, unsigned int* flags)
{
    assert(false && "DMA policy query resolved an interrupt");
    return OS_EUNKNOWN;
}

static void
__Failure(PciHost_t* host, oserr_t expected)
{
    struct PciDmaDescription result, before;

    memset(&result, 0xa5, sizeof(result));
    before = result;
    assert(PciHostGetAcpiDma(host, &result) == expected);
    assert(!memcmp(&result, &before, sizeof(result)));
}

static void
__LoadRoot(void)
{
    struct AcpiPciRoot root = {.Id = 7, .WindowCount = 1, .DmaStatus = OS_ENOTSUPPORTED};
    PciHost_t host = {.AcpiRootId = 99, .AcpiDmaStatus = OS_EUNKNOWN};
    PciHost_t before = host;

    g_status = OS_EOOM;
    assert(PciHostLoadAcpiRoot(&host, &root) == OS_EOOM);
    assert(!memcmp(&host, &before, sizeof(host)));
    g_status = OS_EOK;
    g_window.Length = 0;
    assert(PciHostLoadAcpiRoot(&host, &root) == OS_EINVALPARAMS);
    assert(!memcmp(&host, &before, sizeof(host)));
    g_window.Length = 4096;
    assert(PciHostLoadAcpiRoot(&host, &root) == OS_EOK);
    assert(host.AcpiRootId == 7 && host.AcpiDmaStatus == OS_ENOTSUPPORTED);
    assert(host.Resources.Count == 1 && host.Resources.Windows[0].Length == 4096);
    __Failure(&host, OS_ENOTSUPPORTED);
    free(host.Resources.Windows);
}

int
main(void)
{
    PciHost_t host = {.Identification = {.HostId = 42, .Segment = 3}};
    struct AcpiPciRoot root = {.Id = 7, .DmaStatus = OS_EOK,
        .Dma = {.AddressLimit = 0xffffffff, .CachePolicy = SHMDeviceCacheCoherent,
                .Flags = ACPI_PCI_DMA_DIRECT}};
    struct PciDmaDescription result;
    const uint64_t limits[] = {0, 0xffffffff, 0xffffffffffULL, UINT64_MAX - 1, UINT64_MAX};
    const oserr_t failures[] = {OS_ENOENT, OS_ENOTSUPPORTED, OS_EOOM, OS_EINVALPARAMS, OS_EUNKNOWN};

    __LoadRoot();
    g_queries = 0;
    assert(PciHostLoadAcpiRoot(&host, &root) == OS_EOK);
    assert(host.AcpiRootId == 7 && host.AcpiDmaStatus == OS_EOK);
    assert(!memcmp(&host.AcpiDma, &root.Dma, sizeof(root.Dma)));
    memset(&root, 0, sizeof(root));
    // The source snapshot may go away after loading. DMA must use the host copy.
    assert(PciHostGetAcpiDma(&host, &result) == OS_EOK);
    assert(result.Map.Ranges[0].Length == 0x100000000ULL);
    for (size_t i = 0; i < SIZEOF_ARRAY(limits); i++) {
        host.AcpiDma.AddressLimit = limits[i];
        memset(&result, 0xa5, sizeof(result));
        assert(PciHostGetAcpiDma(&host, &result) == OS_EOK);
        assert(result.Map.Count == 1 && result.CachePolicy == DmDmaCacheCoherent);
        assert(result.Host.HostId == 0);
        assert(result.Map.Ranges[0].DeviceBase == 0 && result.Map.Ranges[0].PhysicalBase == 0);
        assert(result.Map.Ranges[0].Length == (limits[i] == UINT64_MAX ? UINT64_MAX : limits[i] + 1));
    }
    host.Operations = &g_pciAcpiEcamOperations;
    assert(host.Operations->GetDmaDescription == PciHostGetAcpiDma);
    assert(PciHostGetDmaDescription(&host, &result) == OS_EOK);
    assert(result.Host.HostId == 42 && result.Host.Segment == 3);
    host.Operations = &g_pciDtEcamOperations;
    assert(PciHostGetDmaDescription(&host, &result) == OS_ENOTSUPPORTED);

    for (size_t i = 0; i < SIZEOF_ARRAY(failures); i++) {
        host.AcpiDmaStatus = failures[i];
        __Failure(&host, failures[i]);
    }
    host.AcpiDmaStatus = UINT32_MAX;
    __Failure(&host, OS_EINVALPARAMS);
    host.AcpiDmaStatus = OS_EOK;
    host.AcpiDma.Flags = ACPI_PCI_DMA_DIRECT | 2;
    __Failure(&host, OS_EINVALPARAMS);
    host.AcpiDma.Flags = 0;
    __Failure(&host, OS_ENOTSUPPORTED);
    host.AcpiDma.Flags = ACPI_PCI_DMA_DIRECT;
    host.AcpiDma.CachePolicy = SHMDeviceCacheNonCoherent;
    __Failure(&host, OS_ENOTSUPPORTED);
    host.AcpiDma.CachePolicy = UINT32_MAX;
    __Failure(&host, OS_ENOTSUPPORTED);
    // Older kernels return only the identity prefix and zero the rest of the union.
    memset(&host.AcpiDma, 0, sizeof(host.AcpiDma));
    __Failure(&host, OS_ENOTSUPPORTED);
    host.AcpiRootId = 0;
    __Failure(&host, OS_ENOTSUPPORTED);
    __Failure(NULL, OS_EINVALPARAMS);
    assert(PciHostGetAcpiDma(&host, NULL) == OS_EINVALPARAMS);
    assert(g_queries == 0);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    host.Operations = &g_pciLegacyOperations;
    // Rootless legacy discovery retains its x86 identity policy.
    assert(PciHostGetDmaDescription(&host, &result) == OS_EOK);
    assert(result.Map.Ranges[0].Length == UINT64_MAX);
    host.AcpiRootId = 7;
    assert(PciHostGetDmaDescription(&host, &result) == OS_ENOTSUPPORTED);
    host.AcpiDma.Flags = ACPI_PCI_DMA_DIRECT;
    host.AcpiDma.AddressLimit = 0xffffffff;
    host.AcpiDma.CachePolicy = SHMDeviceCacheCoherent;
    assert(PciHostGetDmaDescription(&host, &result) == OS_EOK);
    assert(result.Map.Ranges[0].Length == 0x100000000ULL);
    host.AcpiDmaStatus = OS_EOOM;
    assert(PciHostGetDmaDescription(&host, &result) == OS_EOOM);
#endif
    puts("ACPI PCI DMA host: PASS");
    return 0;
}
