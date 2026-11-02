// Exercise the persistent DMA identification buffer through the real manager.
#include <stdio.h>
#include <stdlib.h>
#include "../modules/storage/ahci/manager.c"
#include "../librt/libds/list.c"

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static ATAIdentify_t g_identify;
static uuid_t g_registeredDevice;

void
spinlock_init(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    (void)level;
    (void)format;
    abort();
}

size_t
MemoryPageSize(void)
{
    return 4096;
}

oserr_t
AhciTransactionControlCreate(
    _In_ AhciDevice_t* device,
    _In_ AtaCommand_t command,
    _In_ size_t length,
    _In_ int direction)
{
    (void)device;
    (void)command;
    (void)length;
    (void)direction;
    abort();
}

int
sys_storage_unregister(
    _In_ gracht_client_t* client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t device,
    _In_ const uint8_t forced)
{
    (void)client;
    (void)context;
    (void)device;
    (void)forced;
    abort();
}

int
ctt_storage_stat_response(
    _In_ struct gracht_message* message,
    _In_ const oserr_t status,
    _In_ struct sys_disk_descriptor* descriptor)
{
    (void)message;
    (void)status;
    (void)descriptor;
    abort();
}

void*
DMABufferData(
    _In_ const struct DMABuffer* buffer)
{
    CHECK(buffer == (void*)1);
    return &g_identify;
}

gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

uuid_t
GetFileService(void)
{
    return 10;
}

uuid_t
GetNativeHandle(
    _In_ int descriptor)
{
    CHECK(descriptor == 11);
    return 12;
}

int
__crt_get_server_iod(void)
{
    return 11;
}

int
sys_storage_register(
    _In_ gracht_client_t* client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t driver,
    _In_ const uuid_t device,
    _In_ const enum sys_storage_flags flags)
{
    (void)client;
    (void)context;
    (void)flags;
    CHECK(driver == 12);
    g_registeredDevice = device;
    return 0;
}

static void
__Identify(
    _In_ uint16_t geometry,
    _In_ uint32_t words,
    _In_ size_t expected)
{
    AhciDevice_t device = { 0 };
    AhciPort_t port = { 0 };

    memset(&g_identify, 0, sizeof(g_identify));
    g_identify.Capabilities0 = 3;
    g_identify.CommandSetSupport1 = 1 << 10;
    g_identify.SectorCountLBA48 = UINT64_C(0x12345678);
    g_identify.SectorSize = geometry;
    g_identify.WordsPerLogicalSector = words;
    ELEMENT_INIT(&device.header, (void*)(uintptr_t)7, &device);
    device.Port = &port;
    port.Dma.InternalBuffer = (void*)1;
    HandleIdentifyCommand(&device);
    CHECK(device.SectorSize == expected && device.Descriptor.SectorSize == expected);
    CHECK(device.SectorCount == g_identify.SectorCountLBA48);
    CHECK(device.HasDMAEngine && device.AddressingMode == AHCI_DEVICE_MODE_LBA48);
    CHECK(device.Descriptor.DeviceID == 7 && g_registeredDevice == 7);
}

int
main(void)
{
    // Physical-sector ratios do not alter the units used by ATA commands.
    __Identify(0x6000, 0, 512);
    __Identify(0x6003, 0, 512);
    __Identify(0x5000, 2048, 4096);
    __Identify(0x7003, 2048, 4096);
    __Identify(0, 0, 512);
    __Identify(0xD000, 2048, 512);
    __Identify(0x5000, 0, 512);
    __Identify(0x5000, 256, 512);
    __Identify(0x5000, UINT32_MAX, 512);
    puts("AHCI identification: logical sectors, physical ratios and device identity passed");
    return 0;
}
