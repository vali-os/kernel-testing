/* Native tests execute the real ARM64 ExitBootServices implementation. */
#include <library.h>
#include <arm64.h>
#include <assert.h>
#include <stdlib.h>
#include <setjmp.h>
#include <stdio.h>
static EFI_BOOT_SERVICES services;
static EFI_SYSTEM_TABLE systemTable;
static unsigned maps, exits, allocations, diagnostics, disabled, scenario;
static jmp_buf halt;
static EFI_STATUS EFIAPI allocate(EFI_MEMORY_TYPE type, UINTN size, VOID** out)
{
    assert(type == EfiLoaderData && exits == 0);
    *out = calloc(1, size); assert(*out); ++allocations; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI getmap(UINTN* size, EFI_MEMORY_DESCRIPTOR* buffer,
    UINTN* key, UINTN* stride, UINT32* version)
{
    ++maps; *stride = sizeof(*buffer) + 16; *version = 1;
    if (!buffer) { *size = *stride * 2; return EFI_BUFFER_TOO_SMALL; }
    if (scenario == 2 && exits) { *size += *stride; return EFI_BUFFER_TOO_SMALL; }
    assert(*size >= 3 * *stride);
    *size = 3 * *stride; *key = maps;
    for (unsigned i = 0; i < 3; ++i) {
        EFI_MEMORY_DESCRIPTOR* d = (void*)((char*)buffer + i * *stride);
        d->Type = i == 0 ? EfiConventionalMemory : i == 1 ? EfiLoaderData : EfiBootServicesData;
        d->PhysicalStart = 0x40000000 + i * 4096; d->NumberOfPages = 1;
    }
    return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI exitboot(EFI_HANDLE image, UINTN key)
{
    (void)image; assert(key == maps && disabled); ++exits;
    if (scenario == 3) return EFI_DEVICE_ERROR;
    if (scenario == 4 || exits == 1) return EFI_INVALID_PARAMETER;
    return EFI_SUCCESS;
}
BOOLEAN Arm64Identity(UINT64 base, UINT64 length) { return base && length; }
void ConsoleDisable(void) { ++disabled; }
void ConsoleWrite(CHAR16* format, ...) { (void)format; ++diagnostics; }
int main(void)
{
    services.AllocatePool = allocate; services.GetMemoryMap = getmap; services.ExitBootServices = exitboot;
    systemTable.BootServices = &services;
    for (scenario = 1; scenario <= 4; ++scenario) {
        struct VBoot boot = {0};
        maps = exits = allocations = diagnostics = disabled = 0;
        LibraryInitialize(NULL, &systemTable);
        if (!setjmp(halt)) {
            assert(LibraryCleanup(&boot) == EFI_SUCCESS);
            assert(scenario == 1 && exits == 2 && maps == 3 && !gBootServices && !diagnostics);
            assert(boot.Memory.NumberOfEntries == 3);
            struct VBootMemoryEntry* e = (void*)boot.Memory.Entries;
            assert(e[0].Type == VBootMemoryType_Available);
            assert(e[1].Type == VBootMemoryType_Reserved && e[2].Type == VBootMemoryType_Reserved);
        } else {
            assert(scenario != 1 && diagnostics == 1);
            assert(exits == (scenario == 4 ? 8 : 1));
        }
        assert(allocations == 2);
    }
    puts("ARM64 ExitBootServices retry tests PASS");
}
