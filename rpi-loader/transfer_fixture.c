// This PE is an entry-contract probe, not a kernel. It deliberately includes
// an absolute pointer and BSS so the transfer depends on correct PE staging.
#include <vboot/vboot.h>

extern void kentry(void);
void (*volatile relocatedEntry)(void) = kentry;
volatile unsigned char zeroFill[8192];

static int
__Reserved(
    const struct VBoot* boot,
    uint64_t            address)
{
    const struct VBootMemoryEntry* entries = (const void*)(uintptr_t)boot->Memory.Entries;
    for (unsigned int i = 0; i < boot->Memory.NumberOfEntries; i++) {
        if (entries[i].Type == VBootMemoryType_Reserved && address >= entries[i].PhysicalBase &&
            address - entries[i].PhysicalBase < entries[i].Length) {
            return 1;
        }
    }
    return 0;
}

uint64_t
VerifyHandoff(
    struct VBoot*   boot,
    const uint64_t* state)
{
    if (!boot || boot->Magic != VBOOT_MAGIC || boot->Version != VBOOT_VERSION ||
        boot->Firmware != VBootFirmware_Native || boot->ConfigurationTable ||
        boot->Phoenix.Length || boot->Ramdisk.Length || boot->DeviceTree.Length < 40) {
        return 1;
    }
    if (state[0] != (uintptr_t)boot || state[1] || state[2] || state[3] || state[5] || state[7] ||
        state[4] != boot->Stack.Base + boot->Stack.Length || (state[4] & 15) ||
        state[8] != 4 || state[9] != 0x3c0 || state[10] != 1 ||
        (state[11] & 0x3001005) || !state[12] || (state[12] & 2047)) {
        return 2;
    }
    if (state[13] || state[14] || (state[15] & 3) || (state[16] & 3) ||
        state[17] || state[18] || state[19] || !state[20] || !state[21] || !state[22] ||
        state[23] || state[24] || state[25] || state[26]) {
        return 3;
    }
    if (state[27] != boot->Kernel.EntryPoint || (uintptr_t)relocatedEntry != state[27]) {
        return 4;
    }
    for (unsigned int i = 0; i < sizeof(zeroFill); i++) {
        if (zeroFill[i]) {
            return 5;
        }
    }
    if (!__Reserved(boot, (uintptr_t)boot) || !__Reserved(boot, boot->Memory.Entries) ||
        !__Reserved(boot, boot->Kernel.Base) || !__Reserved(boot, boot->Stack.Base) ||
        !__Reserved(boot, boot->DeviceTree.PhysicalBase) ||
        !__Reserved(boot, state[6]) || !__Reserved(boot, state[12])) {
        return 6;
    }
    return 0x600d;
}
