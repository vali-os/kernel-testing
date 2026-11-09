#include <vboot/vboot.h>
#include <vboot/uefi.h>
#include "layout.h"

extern void kentry(void);
// This stored absolute address requires a DIR64 fixup when the image moves.
// A PC-relative reference alone would let a broken relocator pass the gate.
static void (*volatile gEntryPointer)(void) = kentry;

static void
__Print(
    const char* text)
{
    volatile unsigned* uart = (volatile unsigned*)0x9000000;

    while (*text) {
        while (uart[6] & 32) {}
        uart[0] = *text++;
    }
}

static uint64_t
__Read(
    const uint8_t* bytes,
    unsigned       count)
{
    uint64_t value = 0;
    unsigned i;

    for (i = 0; i < count; i++) {
        value |= (uint64_t)bytes[i] << (8 * i);
    }
    return value;
}

static int
__Reserved(
    const struct VBoot* boot,
    uint64_t            base,
    uint64_t            length)
{
    const struct VBootMemoryEntry* entries = (void*)(uintptr_t)boot->Memory.Entries;
    uint64_t end = base + length;
    uint64_t next;
    unsigned i;

    if (!length || end < base) {
        return 0;
    }
    while (base < end) {
        next = base;
        for (i = 0; i < boot->Memory.NumberOfEntries; i++) {
            if (base >= entries[i].PhysicalBase &&
                base - entries[i].PhysicalBase < entries[i].Length &&
                entries[i].Type != VBootMemoryType_Available) {
                next = entries[i].PhysicalBase + entries[i].Length;
                break;
            }
        }
        if (next <= base) {
            return 0;
        }
        base = next;
    }
    return 1;
}

static unsigned
__PsciConduit(
    const struct VBoot* boot)
{
    __EFI_GUID acpi = EFI_ACPI_20_TABLE_GUID;
    uint8_t* rsdp;
    uint8_t* xsdt;
    uint8_t* table;
    unsigned length;
    unsigned offset;

    rsdp = __LocateGuidInConfigurationTable((void*)(uintptr_t)boot->ConfigurationTable,
        boot->ConfigurationTableCount, acpi);
    if (!rsdp || !__Reserved(boot, (uintptr_t)rsdp, 36)) {
        return 0;
    }
    xsdt = (void*)(uintptr_t)__Read(rsdp + 24, 8);
    if (!__Reserved(boot, (uintptr_t)xsdt, 36)) {
        return 0;
    }
    length = __Read(xsdt + 4, 4);
    if (length < 36 || length > 0x100000 || (length - 36) % 8 ||
        !__Reserved(boot, (uintptr_t)xsdt, length)) {
        return 0;
    }
    for (offset = 36; offset < length; offset += 8) {
        table = (void*)(uintptr_t)__Read(xsdt + offset, 8);
        if (!__Reserved(boot, (uintptr_t)table, 36)) {
            return 0;
        }
        if (__Read(table, 4) == 0x50434146 && __Read(table + 4, 4) >= 132 &&
            __Reserved(boot, (uintptr_t)table, 132) && (table[129] & 1)) {
            return table[129] & 2 ? VBOOT_PSCI_HVC : VBOOT_PSCI_SMC;
        }
    }
    return 0;
}

void
FixtureMain(
    struct VBoot*   boot,
    const uint64_t* entry)
{
    uint64_t el, flags, control, frequency, counter;
    unsigned conduit;
    unsigned i;
    int ok;
    register uint64_t function __asm__("x0");

    __asm__ volatile("mrs %0, CurrentEL; mrs %1, daif; mrs %2, sctlr_el1; mrs %3, cntfrq_el0; mrs %4, cntpct_el0"
        : "=r"(el), "=r"(flags), "=r"(control), "=r"(frequency), "=r"(counter));
    ok = el == 4 && (flags & 0x3c0) == 0x3c0 && !(control & 0x1005);
    ok &= boot->Magic == VBOOT_MAGIC && boot->Version == VBOOT_VERSION;
    ok &= boot->Firmware == VBootFirmware_UEFI && boot->Memory.EntrySize == 36;
    ok &= boot->ConfigurationEntrySize == 24 && boot->ConfigurationTableCount != 0;
    ok &= !boot->DeviceTree.PhysicalBase && !boot->DeviceTree.Length;
    ok &= !boot->Video.FrameBuffer && frequency != 0 && counter != 0;
    ok &= boot->Kernel.EntryPoint == (uintptr_t)kentry && gEntryPointer == kentry;
    for (i = 0; i < 5; i++) {
        ok &= entry[i] == 0;
    }
    ok &= entry[6] == boot->Stack.Base + boot->Stack.Length && !(entry[6] & 15);
    ok &= !(entry[8] & (3ULL << 20)) && entry[9] == 1;
    ok &= __Reserved(boot, entry[5], 4) && __Reserved(boot, entry[7], 2048);
    ok &= __Reserved(boot, (uintptr_t)boot, sizeof(*boot));
    ok &= __Reserved(boot, boot->Memory.Entries, boot->Memory.NumberOfEntries * 36ULL);
    ok &= __Reserved(boot, boot->Kernel.Base, boot->Kernel.Length);
    ok &= __Reserved(boot, boot->Stack.Base, boot->Stack.Length);
    ok &= __Reserved(boot, boot->Ramdisk.Data, boot->Ramdisk.Length);
    ok &= __Reserved(boot, boot->Phoenix.Base, boot->Phoenix.Length);
    ok &= __Reserved(boot, boot->ConfigurationTable, boot->ConfigurationTableCount * 24ULL);
    conduit = __PsciConduit(boot);
    function = 0x84000000;
    if (conduit == VBOOT_PSCI_HVC) {
        __asm__ volatile("hvc #0" : "+r"(function) :: "x1", "x2", "x3", "memory");
    } else if (conduit == VBOOT_PSCI_SMC) {
        __asm__ volatile("smc #0" : "+r"(function) :: "x1", "x2", "x3", "memory");
    } else {
        ok = 0;
    }
    ok &= (unsigned)function != 0xffffffff;
    __Print(ok ? "ARM64 HANDOFF PASS\n" : "ARM64 HANDOFF FAIL\n");
    if (ok && boot->Kernel.Base >= 0x100000000ULL) {
        __Print("ARM64 HIGH HANDOFF PASS\n");
    }
    for (;;) {
        __asm__ volatile("wfe");
    }
}
