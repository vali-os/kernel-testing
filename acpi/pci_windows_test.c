#include <ddk/pci/mcfg.h>
#include <ddk/pci/windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static struct { ACPI_TABLE_MCFG Table; ACPI_MCFG_ALLOCATION Entries[2]; } mcfg;
static void checksum(void)
{
    uint8_t sum = 0;
    mcfg.Table.Header.Checksum = 0;
    for (size_t i = 0; i < mcfg.Table.Header.Length; i++) sum += ((uint8_t*)&mcfg)[i];
    mcfg.Table.Header.Checksum = (uint8_t)-sum;
}
static void reset(void)
{
    memset(&mcfg, 0, sizeof(mcfg));
    memcpy(mcfg.Table.Header.Signature, "MCFG", 4); mcfg.Table.Header.Length = sizeof(mcfg);
    mcfg.Entries[0] = (ACPI_MCFG_ALLOCATION) { .Address = 0x80000000, .PciSegment = 2, .StartBusNumber = 32, .EndBusNumber = 63 };
    mcfg.Entries[1] = (ACPI_MCFG_ALLOCATION) { .Address = 0xa0000000, .PciSegment = 7, .StartBusNumber = 32, .EndBusNumber = 63 };
    checksum();
}
int main(void)
{
    struct PciAddressWindow windows[2] = {{.Space = PciAddressMemory32,
        .BusBase = 0x80000000, .PhysicalBase = 0x180000000ULL, .Length = 0x100000}};
    uint64_t translated = 0;
    reset(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) == OS_EOK);
    assert(__PciFindMcfg(&mcfg.Table.Header, 2, 32, 47) == &mcfg.Entries[0]);
    assert(__PciFindMcfg(&mcfg.Table.Header, 2, 48, 63) == &mcfg.Entries[0]);
    assert(__PciFindMcfg(&mcfg.Table.Header, 7, 32, 63) == &mcfg.Entries[1]);
    assert(!__PciFindMcfg(&mcfg.Table.Header, 2, 31, 63));
    assert(!__PciFindMcfg(&mcfg.Table.Header, 0, 32, 63));
    mcfg.Table.Header.Checksum++; assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) != OS_EOK);
    reset(); mcfg.Table.Header.Length--; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)-1) != OS_EOK);
    reset(); mcfg.Entries[1].PciSegment = 2; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) != OS_EOK);
    reset(); mcfg.Entries[1].Address = mcfg.Entries[0].Address; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) != OS_EOK);
    reset(); mcfg.Entries[0].Address++; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) != OS_EOK);
    reset(); mcfg.Entries[0].Address = UINT64_MAX & ~0xfffffULL; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) == OS_EOVERFLOW);
    reset(); mcfg.Entries[0].StartBusNumber = 64; checksum(); assert(__PciValidateMcfg(&mcfg.Table.Header, sizeof(mcfg)) != OS_EOK);
    assert(__PciTranslateWindow(windows, 1, PciAddressMemory64, 0, 0x800ff000, 0x1000, &translated) == OS_EOK);
    assert(translated == 0x1800ff000ULL);
    assert(__PciTranslateWindow(windows, 1, PciAddressMemory32, 0, 0x800ff000, 0x1001, &translated) == OS_ENOENT);
    assert(__PciTranslateWindow(windows, 1, PciAddressIo, 0, 0x800ff000, 1, &translated) == OS_ENOENT);
    assert(__PciTranslateWindow(windows, 1, PciAddressMemory64, 0, UINT64_MAX, 2, &translated) == OS_EOVERFLOW);
    windows[0].Attributes = PCI_WINDOW_PREFETCHABLE;
    assert(__PciTranslateWindow(windows, 1, PciAddressMemory64, 0, 0x800ff000, 1, &translated) == OS_ENOTSUPPORTED);
    assert(__PciTranslateWindow(windows, 1, PciAddressMemory64, 8, 0x800ff000, 1, &translated) == OS_EOK);
    windows[0].Attributes = 0;
    windows[1] = windows[0]; assert(__PciTranslateWindow(windows, 2, PciAddressMemory64, 0, 0x800ff000, 1, &translated) == OS_EINVALPARAMS);
    windows[0].PhysicalBase = UINT64_MAX; assert(!__PciWindowValid(&windows[0]));
    puts("PCI MCFG and forwarding windows: PASS"); return 0;
}
