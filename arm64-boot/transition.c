#include <platform/arm64.h>

extern UINT32 gArm64GicVersion;

extern void SeedStart(void);
extern char StackTop[];

// Exercise the same read-only CPU and translation checks used before firmware
// exit. Checking both a translated alias and a missing mapping proves that the
// VHE case does not silently take Arm64Identity's MMU-off shortcut.
UINT64
CheckFirmwareState(
    VOID)
{
    if (Arm64CheckCpu() != EFI_SUCCESS) {
        return 1;
    }
    if (!Arm64Identity((UINTN)SeedStart, (UINTN)StackTop - (UINTN)SeedStart)) {
        return 2;
    }
    if (Arm64Identity(0x80000000, EFI_PAGE_SIZE)) {
        return 3;
    }
    if (Arm64Identity(0xc0000000, EFI_PAGE_SIZE)) {
        return 4;
    }
    gArm64GicVersion = 3;
    return 0;
}
