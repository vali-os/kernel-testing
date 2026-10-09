#include <vboot/vboot.h>
static void puts(const char* s)
{
    volatile unsigned* uart = (volatile unsigned*)0x9000000;
    while (*s) {
        while (uart[6] & 32) {}
        uart[0] = *s++;
    }
}
void kentry(struct VBoot2* b)
{
    unsigned long long el, flags, control, sp, frequency, counter;
    __asm__ volatile("mrs %0, CurrentEL; mrs %1, daif; mrs %2, sctlr_el1; mov %3, sp; mrs %4, cntfrq_el0; mrs %5, cntpct_el0"
        : "=r"(el), "=r"(flags), "=r"(control), "=r"(sp), "=r"(frequency), "=r"(counter));
    int ok = el == 4 && (flags & 0x3c0) == 0x3c0 && !(control & 0x1005) && !(sp & 15) &&
        sp >= b->V1.Stack.Base && sp < b->V1.Stack.Base + b->V1.Stack.Length &&
        b->V1.Magic == VBOOT_MAGIC && b->V1.Version == VBOOT_VERSION_2 &&
        b->DescriptorSize == sizeof(*b) && b->Architecture == VBOOT_ARCH_AARCH64 &&
        b->MemoryEntrySize == sizeof(struct VBootMemoryEntry) && b->ConfigurationEntrySize == 24 &&
        b->Features == (VBOOT_FEATURE_PHYSICAL | VBOOT_FEATURE_MMU_OFF | VBOOT_FEATURE_RESERVED_FIRMWARE) &&
        !b->V1.Video.FrameBuffer && b->ConsoleKind == VBOOT_CONSOLE_PL011 && b->AcpiRsdp &&
        b->KernelPhysicalEntry == (unsigned long long)kentry && b->V1.Kernel.Base == 0x48000000 &&
        b->V1.Ramdisk.Length && b->V1.Phoenix.Length && b->CounterFrequency == frequency && counter;
    struct VBootMemoryEntry* entries = (void*)b->V1.Memory.Entries;
    unsigned long long retained[] = {(unsigned long long)b, b->V1.Memory.Entries,
        b->V1.Kernel.Base, b->V1.Stack.Base, b->V1.Ramdisk.Data, b->V1.Phoenix.Base,
        b->V1.ConfigurationTable, b->LoaderBase};
    for (unsigned p = 0; p < sizeof(retained)/sizeof(retained[0]); ++p) {
        int found = 0;
        for (unsigned i = 0; i < b->V1.Memory.NumberOfEntries; ++i)
            if (retained[p] >= entries[i].PhysicalBase && retained[p] - entries[i].PhysicalBase < entries[i].Length &&
                entries[i].Type != VBootMemoryType_Available) found = 1;
        if (!found) ok = 0;
    }
    register unsigned long long function __asm__("x0") = 0x84000000;
    if (b->PsciConduit == VBOOT_PSCI_HVC)
        __asm__ volatile("hvc #0" : "+r"(function) :: "x1", "x2", "x3", "memory");
    else if (b->PsciConduit == VBOOT_PSCI_SMC)
        __asm__ volatile("smc #0" : "+r"(function) :: "x1", "x2", "x3", "memory");
    else ok = 0;
    if ((unsigned)function == 0xffffffff) ok = 0;
    puts(ok ? "ARM64 HANDOFF PASS\n" : "ARM64 HANDOFF FAIL\n");
    for (;;) __asm__ volatile("wfe");
}
