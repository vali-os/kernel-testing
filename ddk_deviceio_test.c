// Tests the device I/O accessors in libddk. Registers of every supported width
// must be read and written with exactly that width, a width or I/O space type
// that is not supported must be reported, and the create calls must describe
// the I/O space they register. Built with NDEBUG, so the argument checks are
// tested instead of the asserts in front of them.
#ifndef DDK_DEVICEIO_SOURCE
#define DDK_DEVICEIO_SOURCE "../librt/libddk/io/deviceio.c"
#endif
#include DDK_DEVICEIO_SOURCE
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static DeviceIo_t g_registered;
static int        g_registers;
static int        g_acquires;
static int        g_releases;
static int        g_destroys;
static int        g_errors;

// The last access made through the port stubs.
static uint16_t g_port;
static uint32_t g_portValue;
static int      g_portWidth;

oserr_t Syscall_IoSpaceRegister(DeviceIo_t* ioSpace) { g_registered = *ioSpace; g_registers++; ioSpace->Id = 9; return OS_EOK; }
oserr_t Syscall_IoSpaceAcquire(DeviceIo_t* ioSpace)  { (void)ioSpace; g_acquires++; return OS_EOK; }
oserr_t Syscall_IoSpaceRelease(DeviceIo_t* ioSpace)  { (void)ioSpace; g_releases++; return OS_EOK; }
oserr_t Syscall_IoSpaceDestroy(DeviceIo_t* ioSpace)  { (void)ioSpace; g_destroys++; return OS_EOK; }

oserr_t
Syscall_IoSpaceMap(
    _In_  uuid_t id,
    _In_  size_t offset,
    _In_  size_t length,
    _Out_ void** mappingOut)
{
    CHECK(id == 9 && offset == 16 && length == 32);
    *mappingOut = (void*)(uintptr_t)0x4000;
    return OS_EOK;
}

uint8_t  __readbyte(uint16_t port) { g_port = port; g_portWidth = 1; return 0x5A; }
uint16_t __readword(uint16_t port) { g_port = port; g_portWidth = 2; return 0x5A5B; }
uint32_t __readlong(uint16_t port) { g_port = port; g_portWidth = 4; return 0x5A5B5C5D; }
void __writebyte(uint16_t port, uint8_t value)  { g_port = port; g_portWidth = 1; g_portValue = value; }
void __writeword(uint16_t port, uint16_t value) { g_port = port; g_portWidth = 2; g_portValue = value; }
void __writelong(uint16_t port, uint32_t value) { g_port = port; g_portWidth = 4; g_portValue = value; }

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
}

static void
__TestCreate(void)
{
    DeviceIo_t ioSpace = { 0 };
    void*      mapping = NULL;

    ioSpace.Access.Memory.VirtualBase = 0x1234;
    CHECK(DeviceIoCreateMemory(&ioSpace, 0xFE000000, 0x1000) == OS_EOK && ioSpace.Id == 9);
    CHECK(g_registered.Type == DeviceIoMemoryBased);
    CHECK(g_registered.Access.Memory.PhysicalBase == 0xFE000000 && g_registered.Access.Memory.Length == 0x1000);
    CHECK(g_registered.Access.Memory.VirtualBase == 0);

    CHECK(DeviceIoCreatePort(&ioSpace, 0x3F8, 8) == OS_EOK);
    CHECK(g_registered.Type == DeviceIoPortBased);
    CHECK(g_registered.Access.Port.Base == 0x3F8 && g_registered.Access.Port.Length == 8);

    CHECK(DeviceIoCreatePin(&ioSpace, 3, 5) == OS_EOK);
    CHECK(g_registered.Type == DeviceIoPinBased);
    CHECK(g_registered.Access.Pin.Port == 3 && g_registered.Access.Pin.Pin == 5);
    CHECK(g_registers == 3);

    CHECK(DeviceIoAcquire(&ioSpace) == OS_EOK && g_acquires == 1);
    CHECK(DeviceIoRelease(&ioSpace) == OS_EOK && g_releases == 1);
    CHECK(DeviceIoDestroy(&ioSpace) == OS_EOK && g_destroys == 1);
    CHECK(DeviceIoMap(9, 16, 32, &mapping) == OS_EOK && mapping == (void*)(uintptr_t)0x4000);

    // A missing I/O space is refused before the kernel is asked.
    CHECK(DeviceIoCreateMemory(NULL, 0x1000, 16) == OS_EINVALPARAMS);
    CHECK(DeviceIoCreatePort(NULL, 0x60, 1) == OS_EINVALPARAMS);
    CHECK(DeviceIoCreatePin(NULL, 1, 1) == OS_EINVALPARAMS);
    CHECK(DeviceIoAcquire(NULL) == OS_EINVALPARAMS);
    CHECK(DeviceIoRelease(NULL) == OS_EINVALPARAMS);
    CHECK(DeviceIoDestroy(NULL) == OS_EINVALPARAMS);
    CHECK(g_registers == 3 && g_acquires == 1 && g_releases == 1 && g_destroys == 1);
}

static void
__TestMemory(void)
{
    static const uint8_t pattern[16] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F
    };
    static const size_t widths[] = { 1, 2, 4, 8 };
    _Alignas(8) uint8_t registers[16];
    uint8_t             expected[16];
    DeviceIo_t          ioSpace = {
        .Id = 9,
        .Type = DeviceIoMemoryBased,
        .Access.Memory = { .VirtualBase = (uintptr_t)&registers[0], .Length = sizeof(registers) }
    };
    uint64_t value = 0;
    size_t   i;

    // Each width reads its own bytes and nothing more. The test runs on a
    // little-endian host, as the kernel does on every supported architecture.
    memcpy(registers, pattern, sizeof(registers));
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        memcpy(&value, &pattern[8], widths[i]);
        CHECK(DeviceIoRead(&ioSpace, 8, widths[i]) == value);
        value = 0;
    }

    // Each width writes its own bytes and leaves its neighbours alone, also
    // when the value has bits above the width.
    for (i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        memcpy(registers, pattern, sizeof(registers));
        memcpy(expected, pattern, sizeof(expected));
        value = 0xA1A2A3A4A5A6A7A8ULL;
        memcpy(&expected[4], &value, widths[i]);
        CHECK(DeviceIoWrite(&ioSpace, 4, (size_t)value, widths[i]) == OS_EOK);
        CHECK(memcmp(registers, expected, sizeof(registers)) == 0);
    }
    CHECK(g_errors == 0);

    // A width that is not a register size changes nothing and is logged.
    memcpy(registers, pattern, sizeof(registers));
    CHECK(DeviceIoWrite(&ioSpace, 0, 0xFFFFFF, 3) == OS_EINVALPARAMS && g_errors == 1);
    CHECK(memcmp(registers, pattern, sizeof(registers)) == 0);
    CHECK(DeviceIoRead(&ioSpace, 0, 3) == 0 && g_errors == 2);
    CHECK(DeviceIoRead(&ioSpace, 0, 0) == 0 && g_errors == 3);
}

static void
__TestPorts(void)
{
    DeviceIo_t ioSpace = {
        .Id = 9,
        .Type = DeviceIoPortBased,
        .Access.Port = { .Base = 0x3F8, .Length = 8 }
    };

    g_errors = 0;
    CHECK(DeviceIoRead(&ioSpace, 5, 1) == 0x5A && g_port == 0x3FD && g_portWidth == 1);
    CHECK(DeviceIoRead(&ioSpace, 2, 2) == 0x5A5B && g_port == 0x3FA && g_portWidth == 2);
    CHECK(DeviceIoRead(&ioSpace, 4, 4) == 0x5A5B5C5D && g_port == 0x3FC && g_portWidth == 4);

    CHECK(DeviceIoWrite(&ioSpace, 1, 0x1234, 1) == OS_EOK);
    CHECK(g_port == 0x3F9 && g_portWidth == 1 && g_portValue == 0x34);
    CHECK(DeviceIoWrite(&ioSpace, 2, 0x123456, 2) == OS_EOK);
    CHECK(g_port == 0x3FA && g_portWidth == 2 && g_portValue == 0x3456);
    CHECK(DeviceIoWrite(&ioSpace, 4, 0x12345678, 4) == OS_EOK);
    CHECK(g_port == 0x3FC && g_portWidth == 4 && g_portValue == 0x12345678);
    CHECK(g_errors == 0);

    // Ports have no 8 byte access.
    g_portWidth = 0;
    CHECK(DeviceIoWrite(&ioSpace, 0, 1, 8) == OS_EINVALPARAMS && g_portWidth == 0 && g_errors == 1);
    CHECK(DeviceIoRead(&ioSpace, 0, 8) == 0 && g_portWidth == 0 && g_errors == 2);
}

static void
__TestUnsupported(void)
{
    DeviceIo_t pin = { .Id = 9, .Type = DeviceIoPinBased, .Access.Pin = { .Port = 1, .Pin = 2 } };
    DeviceIo_t invalid = { .Id = 9, .Type = DeviceIoInvalid };

    g_errors = 0;
    g_portWidth = 0;
    CHECK(DeviceIoWrite(&pin, 0, 1, 1) == OS_ENOTSUPPORTED && g_errors == 1);
    CHECK(DeviceIoRead(&pin, 0, 1) == 0 && g_errors == 2);
    CHECK(DeviceIoWrite(&invalid, 0, 1, 1) == OS_ENOTSUPPORTED && g_errors == 3);
    CHECK(DeviceIoRead(&invalid, 0, 1) == 0 && g_errors == 4);
    CHECK(g_portWidth == 0);

    // A missing I/O space is not an access at all, so nothing is logged.
    CHECK(DeviceIoWrite(NULL, 0, 1, 1) == OS_EINVALPARAMS);
    CHECK(DeviceIoRead(NULL, 0, 1) == 0);
    CHECK(g_errors == 4);
}

int
main(void)
{
    __TestCreate();
    __TestMemory();
    __TestPorts();
    __TestUnsupported();
    puts("DDK device I/O: registration, register widths, port access and unsupported requests passed");
    return 0;
}
