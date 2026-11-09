#define _GNU_SOURCE
#include <Uefi.h>
#include <Library/BaseMemoryLib.h>
#include <Library/CacheMaintenanceLib.h>
#include <Library/PeCoffLib.h>
#include <Library/SafeIntLib.h>
#include <assert.h>
#include <platform/arm64-image.h>
#include <platform/arm64-memory.h>
#include <platform/arm64.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

EFI_BOOT_SERVICES* gBS;
EFI_SYSTEM_TABLE* gST;
EFI_HANDLE gImageHandle;
static EFI_BOOT_SERVICES gServices;
static UINT64 gAllocation;
static UINTN gPages;
static BOOLEAN gRejectPreferred;
static BOOLEAN gRejectAll;
static BOOLEAN gIdentity = TRUE;
static unsigned gFreed;
static unsigned gRequests;

VOID* EFIAPI
CopyMem(
    VOID* To,
    const VOID* From,
    UINTN Size)
{
    return memcpy(To, From, Size);
}
VOID* EFIAPI
ZeroMem(
    VOID* To,
    UINTN Size)
{
    return memset(To, 0, Size);
}
VOID* EFIAPI
SetMem(
    VOID* To,
    UINTN Size,
    UINT8 Value)
{
    return memset(To, Value, Size);
}
VOID* EFIAPI
InvalidateInstructionCacheRange(
    VOID* Base,
    UINTN Size)
{
    (void)Size;
    return Base;
}
RETURN_STATUS EFIAPI
SafeUint32Add(
    UINT32 A,
    UINT32 B,
    UINT32* Result)
{
    if (A > MAX_UINT32 - B) {
        return RETURN_BUFFER_TOO_SMALL;
    }
    *Result = A + B;
    return RETURN_SUCCESS;
}
VOID EFIAPI
PeCoffLoaderRelocateImageExtraAction(
    PE_COFF_LOADER_IMAGE_CONTEXT* Context)
{
    (void)Context;
}
VOID EFIAPI
PeCoffLoaderUnloadImageExtraAction(
    PE_COFF_LOADER_IMAGE_CONTEXT* Context)
{
    (void)Context;
}
BOOLEAN
Arm64Identity(
    UINT64 Base,
    UINT64 Length)
{
    return Base && Length && gIdentity;
}
EFI_STATUS
LibraryAllocateMemory(
    UINTN Size,
    VOID** Out)
{
    *Out = malloc(Size);
    return *Out ? EFI_SUCCESS : EFI_OUT_OF_RESOURCES;
}
EFI_STATUS
LibraryFreeMemory(
    VOID* Buffer)
{
    free(Buffer);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__AllocatePages(
    EFI_ALLOCATE_TYPE Type,
    EFI_MEMORY_TYPE MemoryType,
    UINTN Pages,
    EFI_PHYSICAL_ADDRESS* Base)
{
    VOID* memory;

    ++gRequests;
    assert(MemoryType == EfiLoaderCode && !gAllocation);
    if (gRejectAll || (Type == AllocateAddress && gRejectPreferred)) {
        return EFI_OUT_OF_RESOURCES;
    }
    if (Type == AllocateMaxAddress) {
        assert(*Base == ARM64_KERNEL_LIMIT - 1);
        // Deliberately page-aligned but not 64 KiB-aligned, above 4 GiB.
        *Base = 0x140001000ULL;
    }
    memory = mmap((void*)(uintptr_t)*Base, Pages * EFI_PAGE_SIZE, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(memory != MAP_FAILED);
    gAllocation = *Base;
    gPages = Pages;
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__FreePages(
    EFI_PHYSICAL_ADDRESS Base,
    UINTN Pages)
{
    assert(Base == gAllocation && Pages == gPages);
    assert(munmap((void*)(uintptr_t)Base, Pages * EFI_PAGE_SIZE) == 0);
    gAllocation = 0;
    ++gFreed;
    return EFI_SUCCESS;
}

static void
__Failure(
    UINT8* File,
    UINTN Size)
{
    struct VBootModule module = { 1, 2, 3 };

    assert(EFI_ERROR(Arm64LoadKernelImage(File, Size, &module)));
    assert(!module.Base && !module.EntryPoint && !module.Length && !gAllocation);
}

static void
__MapTests(
    void)
{
    // Padded descriptors prove callers use EFI's stride rather than sizeof.
    struct __Record {
        EFI_MEMORY_DESCRIPTOR Descriptor;
        UINT64 Padding[2];
    } records[3] = { 0 };
    struct Arm64FirmwareMap map = { (void*)records, sizeof(records), sizeof(records[0]) };
    unsigned i;

    for (i = 0; i < 3; i++) {
        records[i].Descriptor.Type = EfiACPIReclaimMemory;
        records[i].Descriptor.PhysicalStart = 0x100000000ULL + i * EFI_PAGE_SIZE;
        records[i].Descriptor.NumberOfPages = 1;
    }
    records[1].Descriptor.Type = EfiACPIMemoryNVS;
    assert(Arm64AcpiRange(&map, 0x100000800ULL, 0x2000));
    assert(!Arm64AcpiRange(&map, 0x100000800ULL, 0x3000));
    records[1].Descriptor.Type = EfiConventionalMemory;
    assert(!Arm64AcpiRange(&map, 0x100000800ULL, 0x2000));
    records[1].Descriptor.Type = EfiMemoryMappedIO;
    assert(!Arm64AcpiRange(&map, 0x100000800ULL, 0x2000));
    records[1].Descriptor.Type = EfiACPIMemoryNVS;
    records[1].Descriptor.Attribute = EFI_MEMORY_RP;
    assert(!Arm64AcpiRange(&map, 0x100000800ULL, 0x2000));
    records[1].Descriptor.Attribute = 0;
    gIdentity = FALSE;
    assert(!Arm64AcpiRange(&map, 0x100000000ULL, 32));
    gIdentity = TRUE;
    assert(!Arm64AcpiRange(&map, MAX_UINT64 - 8, 32));
    records[0].Descriptor.NumberOfPages = MAX_UINT64;
    assert(!Arm64AcpiRange(&map, 0x100000000ULL, 32));
}

int
main(
    int Argc,
    char** Argv)
{
    FILE* input;
    long size;
    UINT8* file;
    UINT8* changed;
    EFI_IMAGE_DOS_HEADER* dos;
    EFI_IMAGE_NT_HEADERS64* header;
    EFI_IMAGE_NT_HEADERS64* modified;
    EFI_IMAGE_SECTION_HEADER* sections;
    EFI_IMAGE_DATA_DIRECTORY reloc;
    EFI_IMAGE_BASE_RELOCATION* block;
    struct VBootModule module;
    UINTN i;
    UINTN relocationOffset = 0;
    UINT32 target;
    UINT16* fixup;
    UINT64 pointer;
    unsigned requests;
    EFI_IMAGE_SECTION_HEADER* modifiedSections;

    assert(Argc == 2);
    input = fopen(Argv[1], "rb");
    assert(input);
    assert(!fseek(input, 0, SEEK_END));
    size = ftell(input);
    rewind(input);
    file = malloc(size);
    changed = malloc(size);
    assert(file && changed && fread(file, 1, size, input) == (UINTN)size);
    fclose(input);
    dos = (void*)file;
    header = (void*)(file + dos->e_lfanew);
    sections = (void*)((UINT8*)&header->OptionalHeader + header->FileHeader.SizeOfOptionalHeader);
    reloc = header->OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC];
    assert(reloc.Size);
    for (i = 0; i < header->FileHeader.NumberOfSections; i++) {
        if (reloc.VirtualAddress >= sections[i].VirtualAddress
            && reloc.VirtualAddress - sections[i].VirtualAddress < sections[i].SizeOfRawData) {
            relocationOffset
                = sections[i].PointerToRawData + reloc.VirtualAddress - sections[i].VirtualAddress;
        }
    }
    assert(relocationOffset);
    block = (void*)(file + relocationOffset);
    fixup = (void*)(block + 1);
    assert((*fixup >> 12) == EFI_IMAGE_REL_BASED_DIR64);
    target = block->VirtualAddress + (*fixup & 0xfff);
    gServices.AllocatePages = __AllocatePages;
    gServices.FreePages = __FreePages;
    gBS = &gServices;
    assert(Arm64LoadKernelImage(file, size, &module) == EFI_SUCCESS);
    assert(module.Base == header->OptionalHeader.ImageBase);
    __FreePages(gAllocation, gPages);
    gRejectPreferred = TRUE;
    assert(Arm64LoadKernelImage(file, size, &module) == EFI_SUCCESS);
    assert(module.Base == 0x140010000ULL && !(module.Base & 0xffff));
    memcpy(&pointer, (void*)(uintptr_t)(module.Base + target), sizeof(pointer));
    assert(pointer == module.EntryPoint);
    __FreePages(gAllocation, gPages);
    gRejectAll = TRUE;
    __Failure(file, size);
    gRejectAll = FALSE;
    __Failure(file, 32);
    __Failure(file, size / 2);
    memcpy(changed, file, size);
    modified = (void*)(changed + dos->e_lfanew);
    modified->OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC].Size = 0;
    __Failure(changed, size);
    gRejectPreferred = FALSE;
    assert(Arm64LoadKernelImage(changed, size, &module) == EFI_SUCCESS);
    __FreePages(gAllocation, gPages);
    gRejectPreferred = TRUE;
    modified->OptionalHeader.DataDirectory[EFI_IMAGE_DIRECTORY_ENTRY_BASERELOC] = reloc;
    modified->OptionalHeader.AddressOfEntryPoint = modified->OptionalHeader.SizeOfImage;
    __Failure(changed, size);
    memcpy(changed, file, size);
    block = (void*)(changed + relocationOffset);
    block->SizeOfBlock = 0;
    __Failure(changed, size);
    memcpy(changed, file, size);
    block = (void*)(changed + relocationOffset);
    fixup = (void*)(block + 1);
    *fixup = (EFI_IMAGE_REL_BASED_HIGHLOW << 12) | (*fixup & 0xfff);
    __Failure(changed, size);
    *fixup = (EFI_IMAGE_REL_BASED_DIR64 << 12) | (reloc.VirtualAddress & 0xfff);
    block->VirtualAddress = reloc.VirtualAddress & ~0xfffU;
    __Failure(changed, size);
    assert(gFreed >= 7);
    // A raw copy with zero VirtualSize must be rejected before EDK2 writes it.
    // Merely checking the loaded image would be too late to prevent corruption.
    memcpy(changed, file, size);
    modifiedSections = (void*)((UINT8*)&modified->OptionalHeader + modified->FileHeader.SizeOfOptionalHeader);
    modifiedSections[0].VirtualAddress = modified->OptionalHeader.SizeOfImage - 0x1000;
    modifiedSections[0].Misc.VirtualSize = 0;
    modifiedSections[0].SizeOfRawData = 0x2000;
    requests = gRequests;
    __Failure(changed, size);
    assert(gRequests == requests);
    memcpy(changed, file, size);
    modified->OptionalHeader.SizeOfImage = ARM64_KERNEL_MAX_SIZE + 0x1000;
    __Failure(changed, size);
    memcpy(changed, file, size);
    modified->FileHeader.Machine = EFI_IMAGE_MACHINE_X64;
    __Failure(changed, size);
    memcpy(changed, file, size);
    modified->OptionalHeader.SectionAlignment = 3;
    __Failure(changed, size);
    __MapTests();
    free(changed);
    free(file);
    puts("ARM64 image placement/relocation and ACPI ranges PASS");
}
