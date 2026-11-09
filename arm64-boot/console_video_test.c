// Exercise real console and GOP code with firmware protocols on the host.
#include <platform/arm64.h>
#include <console.h>
#include <video.h>
#include <Protocol/GraphicsOutput.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

EFI_BOOT_SERVICES* gBS;
EFI_SYSTEM_TABLE* gST;
EFI_HANDLE gImageHandle;
static EFI_BOOT_SERVICES gServices;
static EFI_SYSTEM_TABLE gSystemTable;
static EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL gConsole;
static EFI_GRAPHICS_OUTPUT_PROTOCOL gGop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE gMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION gInfo[40];
static EFI_STATUS gOutputStatus;
static BOOLEAN gAbsent;
static unsigned gOutputs, gQueries, gSets, gBuffers;
static UINT32 gFailedMode;
static UINT32 gFailedQuery;

VOID* EFIAPI
SetMem(OUT VOID* Buffer, IN UINTN Length, IN UINT8 Value)
{
    return memset(Buffer, Value, Length);
}

UINTN EFIAPI
UnicodeVSPrint(OUT CHAR16* Buffer, IN UINTN Size, IN CONST CHAR16* Format, IN VA_LIST Args)
{
    (void)Format;
    (void)Args;
    assert(Size >= 4);
    Buffer[0] = 'X';
    Buffer[1] = 0;
    return 1;
}

static EFI_STATUS EFIAPI
__Output(IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, IN CHAR16* Text)
{
    (void)This;
    assert(Text[0] == 'X' && !Text[1]);
    ++gOutputs;
    return gOutputStatus;
}

static EFI_STATUS EFIAPI
__Clear(IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This)
{
    (void)This;
    return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI
__Cursor(IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, IN BOOLEAN Enabled)
{
    (void)This;
    (void)Enabled;
    return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI
__Attribute(IN EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL* This, IN UINTN Attribute)
{
    (void)This;
    (void)Attribute;
    return EFI_DEVICE_ERROR;
}

static EFI_STATUS EFIAPI
__Locate(IN EFI_GUID* Guid, IN VOID* Registration, OUT VOID** Interface)
{
    (void)Guid;
    (void)Registration;
    *Interface = &gGop;
    return gAbsent ? EFI_NOT_FOUND : EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__Query(IN EFI_GRAPHICS_OUTPUT_PROTOCOL* This, IN UINT32 Number,
    OUT UINTN* Size, OUT EFI_GRAPHICS_OUTPUT_MODE_INFORMATION** Info)
{
    (void)This;
    assert(Number < gMode.MaxMode);
    assert(++gQueries <= 5 * gMode.MaxMode);
    if (Number == gFailedQuery) {
        return EFI_DEVICE_ERROR;
    }
    *Size = sizeof(**Info);
    *Info = malloc(*Size);
    assert(*Info);
    **Info = gInfo[Number];
    ++gBuffers;
    return EFI_SUCCESS;
}

EFI_STATUS
LibraryFreeMemory(IN VOID* Buffer)
{
    assert(gBuffers);
    --gBuffers;
    free(Buffer);
    return EFI_SUCCESS;
}

static EFI_STATUS EFIAPI
__SetMode(IN EFI_GRAPHICS_OUTPUT_PROTOCOL* This, IN UINT32 Number)
{
    (void)This;
    ++gSets;
    if (Number == gFailedMode) {
        return EFI_DEVICE_ERROR;
    }
    gMode.Mode = Number;
    gMode.Info = &gInfo[Number];
    return EFI_SUCCESS;
}

static void
__Reset(void)
{
    unsigned i;

    gAbsent = FALSE;
    gQueries = gSets = gBuffers = 0;
    gFailedMode = gFailedQuery = MAX_UINT32;
    gMode = (EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE){0};
    gMode.MaxMode = 3;
    gMode.Info = &gInfo[0];
    gMode.SizeOfInfo = sizeof(gInfo[0]);
    gMode.FrameBufferBase = 0x80000000;
    gMode.FrameBufferSize = 64 * 1024 * 1024;
    for (i = 0; i < 40; i++) {
        gInfo[i] = (EFI_GRAPHICS_OUTPUT_MODE_INFORMATION){0};
        gInfo[i].HorizontalResolution = 1920;
        gInfo[i].VerticalResolution = 1080;
        gInfo[i].PixelsPerScanLine = 2048;
        gInfo[i].PixelFormat = PixelRedGreenBlueReserved8BitPerColor;
    }
}

static struct VBootVideo
__Video(void)
{
    struct VBoot boot;

    gQueries = 0;
    memset(&boot, 0xa5, sizeof(boot));
    assert(VideoInitialize(&boot) == EFI_SUCCESS);
    assert(!gBuffers);
    return boot.Video;
}

int
main(void)
{
    struct VBootVideo video;
    struct VBootVideo empty = {0};
    unsigned i;

    gBS = &gServices;
    gST = &gSystemTable;
    gServices.LocateProtocol = __Locate;
    gGop.Mode = &gMode;
    gGop.QueryMode = __Query;
    gGop.SetMode = __SetMode;
    gConsole.ClearScreen = __Clear;
    gConsole.EnableCursor = __Cursor;
    gConsole.SetAttribute = __Attribute;
    gConsole.OutputString = __Output;
    assert(ConsoleInitialize() == EFI_SUCCESS);
    ConsoleWrite(L"Absent");
    assert(!gOutputs);
    gSystemTable.ConOut = &gConsole;
    assert(ConsoleInitialize() == EFI_SUCCESS);
    ConsoleWrite(L"Present despite cosmetic failures");
    assert(gOutputs == 1);
    gOutputStatus = EFI_DEVICE_ERROR;
    ConsoleWrite(L"Failure");
    ConsoleWrite(L"Disabled after failure");
    assert(gOutputs == 2);
    gOutputStatus = EFI_SUCCESS;
    assert(ConsoleInitialize() == EFI_SUCCESS);
    ConsoleDisable();
    ConsoleWrite(L"After exit boundary");
    assert(gOutputs == 2);

    __Reset();
    gAbsent = TRUE;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    video = __Video();
    assert(video.Width == 1920 && !gSets);
    assert(video.RedPosition == 0 && video.BluePosition == 16 && video.Pitch == 8192);
    __Reset();
    gInfo[1].HorizontalResolution = 1280;
    gInfo[1].VerticalResolution = 1024;
    gInfo[1].PixelFormat = PixelBlueGreenRedReserved8BitPerColor;
    video = __Video();
    assert(gMode.Mode == 1 && video.RedPosition == 16 && video.BluePosition == 0);
    __Reset();
    gInfo[1].HorizontalResolution = 1280;
    gInfo[1].VerticalResolution = 1024;
    gFailedMode = 1;
    video = __Video();
    assert(video.Width == 1920 && gSets == 1);
    __Reset();
    gInfo[0].PixelFormat = PixelBltOnly;
    video = __Video();
    assert(gMode.Mode == 1 && video.FrameBuffer);
    __Reset();
    for (i = 0; i < 3; i++) {
        gInfo[i].PixelFormat = PixelBltOnly;
    }
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    gMode.MaxMode = 40;
    gInfo[39].HorizontalResolution = 1280;
    gInfo[39].VerticalResolution = 1024;
    video = __Video();
    assert(gMode.Mode == 39 && video.FrameBuffer);
    __Reset();
    gMode.MaxMode = 1;
    gInfo[0].PixelFormat = PixelBitMask;
    gInfo[0].PixelInformation = (EFI_PIXEL_BITMASK){0xf800, 0x7e0, 0x1f, 0};
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    assert(!gSets);
    gInfo[0].PixelInformation = (EFI_PIXEL_BITMASK){0xff0000, 0xff00, 0xff, 0};
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    assert(!gSets);
    gInfo[0].PixelInformation.ReservedMask = 0xff000000;
    video = __Video();
    assert(video.BitsPerPixel == 32 && video.RedMask == 255 && video.GreenMask == 255);
    assert(video.RedPosition == 16 && video.Pitch == 8192);
    gInfo[0].PixelInformation.RedMask = 0xff0001;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    gInfo[1].HorizontalResolution = 1280;
    gInfo[1].VerticalResolution = 1024;
    gInfo[1].PixelFormat = PixelBitMask;
    gInfo[1].PixelInformation = (EFI_PIXEL_BITMASK){0xf800, 0x7e0, 0x1f, 0};
    video = __Video();
    assert(gMode.Mode == 0 && !gSets && video.BitsPerPixel == 32);
    __Reset();
    gInfo[0].PixelFormat = PixelBitMask;
    gInfo[0].PixelInformation = (EFI_PIXEL_BITMASK){0xf800, 0x7e0, 0x1f, 0};
    video = __Video();
    assert(gMode.Mode == 1 && video.BitsPerPixel == 32);
    __Reset();
    gMode.FrameBufferSize = 1;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    gMode.MaxMode = 1;
    gInfo[0].PixelsPerScanLine = 1;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    gInfo[0].PixelsPerScanLine = MAX_UINT32;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    gInfo[0].PixelFormat = PixelBltOnly;
    gFailedQuery = 1;
    video = __Video();
    assert(gMode.Mode == 2 && video.FrameBuffer);
    __Reset();
    gMode.MaxMode = 2;
    gInfo[0].PixelFormat = PixelBltOnly;
    gFailedMode = 1;
    video = __Video();
    assert(!memcmp(&video, &empty, sizeof(video)));
    __Reset();
    assert(ConsoleInitialize() == EFI_SUCCESS);
    video = __Video();
    ConsoleWrite(L"Firmware console remains available after GOP selection");
    assert(video.FrameBuffer && gOutputs == 3);
    ConsoleDisable();
    ConsoleWrite(L"Disabled after graphics setup");
    assert(gOutputs == 3);
    puts("UEFI console/GOP tests PASS");
}
