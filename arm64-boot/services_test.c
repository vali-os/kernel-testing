// Native tests execute the ARM64 implementation using EDK2's host ABI.
#include <platform/arm64.h>
#include <memory.h>
#include <Library/BaseLib.h>
#include <assert.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdio.h>
#include "layout.h"

EFI_BOOT_SERVICES* gBS;
EFI_SYSTEM_TABLE* gST;
EFI_HANDLE gImageHandle;
static EFI_BOOT_SERVICES gServices;
static EFI_SYSTEM_TABLE gSystemTable;
static unsigned gMaps, gExits, gAllocations, gDiagnostics, gDisabled, gScenario;
static void* gBuffers[2];
static jmp_buf gHalt;
static EFI_STATUS gWatchdogStatus;
static unsigned gWatchdogCalls;

static EFI_STATUS EFIAPI
__Watchdog(
    IN UINTN Timeout,
    IN UINT64 Code,
    IN UINTN Size,
    IN CHAR16* Data)
{
    assert(!Timeout && !Code && !Size && !Data && !gExits);
    ++gWatchdogCalls;
    return gWatchdogStatus;
}


static EFI_STATUS EFIAPI
__Allocate(
    IN  EFI_MEMORY_TYPE Type,
    IN  UINTN           Size,
    OUT VOID**          Out)
{
    assert(Type == EfiLoaderData && gExits == 0 && gAllocations < 2);
    *Out = calloc(1, Size);
    assert(*Out);
    gBuffers[gAllocations++] = *Out;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__GetMap(
    IN OUT UINTN*                  Size,
    OUT    EFI_MEMORY_DESCRIPTOR* Buffer,
    OUT    UINTN*                 Key,
    OUT    UINTN*                 Stride,
    OUT    UINT32*                Version)
{
    unsigned i;
    EFI_MEMORY_DESCRIPTOR* descriptor;

    ++gMaps;
    *Stride = sizeof(*Buffer) + 16;
    *Version = 1;
    if (!Buffer) {
        *Size = *Stride * 2;
        return EFI_BUFFER_TOO_SMALL;
    }
    if (gScenario == 2 && gExits) {
        *Size += *Stride;
        return EFI_BUFFER_TOO_SMALL;
    }
    assert(*Size >= 3 * *Stride);
    *Size = 3 * *Stride;
    *Key = gMaps;
    for (i = 0; i < 3; i++) {
        descriptor = (void*)((char*)Buffer + i * *Stride);
        descriptor->Type = i == 0 ? EfiConventionalMemory :
            i == 1 ? EfiLoaderData : EfiBootServicesData;
        descriptor->PhysicalStart = 0x40000000 + i * 4096;
        descriptor->NumberOfPages = 1;
        descriptor->Attribute = EFI_MEMORY_WB;
    }
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__ExitBoot(
    IN EFI_HANDLE Image,
    IN UINTN      Key)
{
    (void)Image;
    assert(Key == gMaps && gDisabled);
    ++gExits;
    if (gScenario == 3) {
        return EFI_DEVICE_ERROR;
    }
    if (gScenario == 4 || gExits == 1) {
        return EFI_INVALID_PARAMETER;
    }
    return EFI_SUCCESS;
}

BOOLEAN
Arm64Identity(
    IN UINT64 Base,
    IN UINT64 Length)
{
    return Base && Length;
}

void
ConsoleDisable(void)
{
    ++gDisabled;
}

void
ConsoleWrite(
    IN CHAR16* Format,
    ...)
{
    (void)Format;
    assert(gDisabled);
    ++gDiagnostics;
}

VOID EFIAPI
CpuDeadLoop(VOID)
{
    longjmp(gHalt, 1);
}

int
main(void)
{
    struct VBoot boot;
    struct VBootMemoryEntry* entries;
    unsigned i;

    // Both UEFI architectures use this conversion. Reserved must include MMIO,
    // even when its descriptor advertises cacheability capabilities.
    assert(__ToVBootMemoryType(EfiMemoryMappedIO) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(EfiMemoryMappedIOPortSpace) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(EfiReservedMemoryType) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(EfiUnusableMemory) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(EfiPersistentMemory) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(0x80000000) == VBootMemoryType_Reserved);
    assert(__ToVBootMemoryType(EfiACPIReclaimMemory) == VBootMemoryType_ACPI);
    assert(__ToVBootMemoryType(EfiACPIMemoryNVS) == VBootMemoryType_NVS);
    for (i = EfiLoaderCode; i <= EfiRuntimeServicesData; i++) {
        assert(__ToVBootMemoryType(i) == VBootMemoryType_Firmware);
    }

    gServices.SetWatchdogTimer = __Watchdog;
    gServices.AllocatePool = __Allocate;
    gServices.GetMemoryMap = __GetMap;
    gServices.ExitBootServices = __ExitBoot;
    gSystemTable.BootServices = &gServices;
    gWatchdogStatus = EFI_UNSUPPORTED;
    assert(LibraryInitialize(NULL, &gSystemTable) == EFI_SUCCESS);
    gWatchdogStatus = EFI_DEVICE_ERROR;
    assert(LibraryInitialize(NULL, &gSystemTable) == EFI_DEVICE_ERROR);
    gWatchdogStatus = EFI_SUCCESS;
    for (gScenario = 1; gScenario <= 4; gScenario++) {
        boot = (struct VBoot){0};
        gMaps = gExits = gAllocations = gDiagnostics = gDisabled = 0;
        assert(LibraryInitialize(NULL, &gSystemTable) == EFI_SUCCESS);
        if (!setjmp(gHalt)) {
            assert(LibraryCleanup(&boot) == EFI_SUCCESS);
            assert(gScenario == 1 && gExits == 2 && gMaps == 3);
            assert(!gBS && !gDiagnostics);
            assert(boot.Memory.NumberOfEntries == 3);
            entries = (void*)(uintptr_t)boot.Memory.Entries;
            assert(entries[0].Type == VBootMemoryType_Available);
            assert(entries[1].Type == VBootMemoryType_Firmware);
            assert(entries[2].Type == VBootMemoryType_Firmware);
            for (i = 0; i < 3; i++) {
                assert(entries[i].PhysicalBase == 0x40000000 + i * 4096);
                assert(entries[i].Length == 4096 && entries[i].VirtualBase == 0);
                assert(entries[i].Attributes == EFI_MEMORY_WB);
            }
        } else {
            assert(gScenario != 1 && gDiagnostics == 0);
            assert(gExits == (gScenario == 4 ? 8 : 1));
        }
        assert(gAllocations == 2);
        for (i = 0; i < gAllocations; i++) {
            free(gBuffers[i]);
        }
    }
    assert(gWatchdogCalls == 6);
    puts("ARM64 ExitBootServices retry tests PASS");
}
