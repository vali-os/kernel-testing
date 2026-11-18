// Checks the order in which the USB mass storage driver lists a new disk and
// registers it with filed (modules/storage/msd/main.c and msd.c).
//
// filed answers the register request and then sends a stat query back to the
// driver at once. The driver handles that query in its own job, and the job
// can run before the registration job has read filed's answer. The doubles
// below run the stat handler while the registration job waits for filed's
// answer, which is the earliest point the query can be handled.
//
// The production sources are compiled into this file. Doubles replace the
// transport functions from functions.c, the USB library, the DMA pool and
// the IPC calls.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#ifndef MSD_MAIN_SOURCE
#define MSD_MAIN_SOURCE "../modules/storage/msd/main.c"
#endif
#include MSD_MAIN_SOURCE
#include "../modules/storage/msd/msd.c"
#include "../librt/libds/list.c"

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

#define TEST_DEVICE_ID      7
#define TEST_FILE_SERVICE   10
#define TEST_SERVER_IOD     11
#define TEST_DRIVER_HANDLE  12
#define TEST_SECTOR_COUNT   4096

// Number of WARNING and ERROR lines printed by the driver.
static int g_logCount;

// The register request sent to filed.
static int    g_registerCount;
static uuid_t g_registeredDriver;
static uuid_t g_registeredDevice;
static int    g_registeredFlags;

// The stat answer the driver sent while it waited for filed.
static int                        g_statCount;
static oserr_t                    g_statResult;
static struct sys_disk_descriptor g_statDescriptor;

static int g_unregisterCount;
static int g_abandonCount;

// USB configuration with one SCSI bulk-only interface and two bulk endpoints.
static usb_endpoint_descriptor_t      g_endpoints[2];
static usb_device_interface_setting_t g_setting;
static usb_device_interface_t         g_interface;

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
    va_list arguments;

    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    putchar('\n');
    if (level == OSSYSLOGLEVEL_WARNING || level == OSSYSLOGLEVEL_ERROR) {
        g_logCount++;
    }
}

struct dma_pool*
UsbRetrievePool(void)
{
    return (struct dma_pool*)1;
}

oserr_t
dma_pool_allocate(
    _In_  struct dma_pool* pool,
    _In_  size_t           length,
    _Out_ void**           addressOut)
{
    CHECK(pool == (struct dma_pool*)1);
    *addressOut = calloc(1, length);
    return *addressOut != NULL ? OS_EOK : OS_EOOM;
}

oserr_t
dma_pool_free(
    _In_ struct dma_pool* pool,
    _In_ void*            address)
{
    CHECK(pool == (struct dma_pool*)1);
    free(address);
    return OS_EOK;
}

enum USBTransferCode
UsbGetActiveConfigDescriptor(
    _In_ usb_device_context_t*       deviceContext,
    _In_ usb_device_configuration_t* configuration)
{
    (void)deviceContext;
    memset(&g_endpoints, 0, sizeof(g_endpoints));
    g_endpoints[0].Address = USB_ENDPOINT_ADDRESS_IN | 1;
    g_endpoints[0].Attributes = USB_ENDPOINT_BULK;
    g_endpoints[1].Address = 2;
    g_endpoints[1].Attributes = USB_ENDPOINT_BULK;

    memset(&g_setting, 0, sizeof(g_setting));
    g_setting.base.Class = USB_CLASS_MSD;
    g_setting.base.Subclass = MSD_SUBCLASS_SCSI;
    g_setting.base.Protocol = MSD_PROTOCOL_BULK_ONLY;
    g_setting.base.NumEndpoints = 2;
    g_setting.endpoints = &g_endpoints[0];
    g_interface.settings = &g_setting;
    g_interface.settings_count = 1;

    memset(configuration, 0, sizeof(*configuration));
    configuration->base.NumInterfaces = 1;
    configuration->interfaces = &g_interface;
    return USBTRANSFERCODE_SUCCESS;
}

void
UsbFreeConfigDescriptor(
    _In_ usb_device_configuration_t* configuration)
{
    (void)configuration;
}

// Replaces the transport setup in functions.c.
oserr_t
MSDDeviceInitialize(
    _In_ MSDDevice_t* device)
{
    CHECK(device->Protocol == ProtocolBulk);
    CHECK(device->In != NULL && device->Out != NULL);
    return OS_EOK;
}

// Replaces the disk start in functions.c. The sector size keeps the default
// that __GetDeviceProtocol sets.
oserr_t
MSDDeviceStart(
    _In_ MSDDevice_t* device)
{
    device->Descriptor.SectorCount = TEST_SECTOR_COUNT;
    device->IsReady = 1;
    return OS_EOK;
}

oserr_t
WaitForFileService(
    _In_ size_t timeout)
{
    (void)timeout;
    return OS_EOK;
}

uuid_t
GetFileService(void)
{
    return TEST_FILE_SERVICE;
}

gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

int
__crt_get_server_iod(void)
{
    return TEST_SERVER_IOD;
}

uuid_t
GetNativeHandle(
    _In_ int descriptor)
{
    CHECK(descriptor == TEST_SERVER_IOD);
    return TEST_DRIVER_HANDLE;
}

int
sys_storage_register(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t                   driverId,
    _In_ const uuid_t                   deviceId,
    _In_ const enum sys_storage_flags   flags)
{
    (void)client;
    (void)context;
    g_registerCount++;
    g_registeredDriver = driverId;
    g_registeredDevice = deviceId;
    g_registeredFlags = (int)flags;
    return 0;
}

// filed has answered the register request and sent its stat query. The stat
// query is handled here, before the registration job reads the answer.
int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                   flags)
{
    (void)client;
    (void)context;
    (void)flags;
    CHECK(g_registerCount == 1);
    ctt_storage_stat_invocation(NULL, g_registeredDevice);
    return 0;
}

int
gracht_client_get_status(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    return GRACHT_MESSAGE_COMPLETED;
}

int
sys_storage_register_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                       resultOut)
{
    (void)client;
    (void)context;
    *resultOut = OS_EOK;
    return 0;
}

int
sys_storage_unregister(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ const uuid_t                   driverId,
    _In_ const uuid_t                   deviceId,
    _In_ const uint8_t                  forced)
{
    (void)client;
    (void)context;
    CHECK(driverId == TEST_DRIVER_HANDLE && deviceId == TEST_DEVICE_ID && forced == 1);
    g_unregisterCount++;
    return 0;
}

int
gracht_client_abandon(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    g_abandonCount++;
    return 0;
}

int
ctt_storage_stat_response(
    _In_ struct gracht_message*      message,
    _In_ oserr_t                     result,
    _In_ struct sys_disk_descriptor* descriptor)
{
    (void)message;
    g_statCount++;
    g_statResult = result;
    g_statDescriptor = *descriptor;
    return 0;
}

// A new disk must be listed before filed is asked to register it, so the stat
// query that filed sends right after its answer finds the disk.
static void
__TestStatDuringRegister(void)
{
    struct sys_device sysDevice;
    MSDDevice_t*      msdDevice;

    memset(&sysDevice, 0, sizeof(sysDevice));
    sysDevice.content_type = SYS_DEVICE_CONTENT_USB;
    sysDevice.content.usb.id = TEST_DEVICE_ID;
    ctt_driver_register_device_invocation(NULL, &sysDevice);

    CHECK(g_registerCount == 1);
    CHECK(g_registeredDriver == TEST_DRIVER_HANDLE && g_registeredDevice == TEST_DEVICE_ID);
    CHECK(g_registeredFlags == SYS_STORAGE_FLAGS_REMOVABLE);
    CHECK(g_statCount == 1 && g_statResult == OS_EOK);
    CHECK(g_statDescriptor.device_id == TEST_DEVICE_ID);
    CHECK(g_statDescriptor.driver_id == TEST_DRIVER_HANDLE);
    CHECK(g_statDescriptor.geometry.sectors_total == TEST_SECTOR_COUNT);
    CHECK(g_statDescriptor.geometry.sector_size == 512);
    CHECK(g_logCount == 0);

    msdDevice = MsdDeviceGet(TEST_DEVICE_ID);
    CHECK(msdDevice != NULL);

    // MSDDeviceDestroy does not free the endpoint copies.
    free(msdDevice->In);
    free(msdDevice->Out);
    CHECK(list_remove(&g_devices, &msdDevice->Header) == 0);
    CHECK(MSDDeviceDestroy(msdDevice) == OS_EOK);
    CHECK(g_unregisterCount == 1 && g_abandonCount == 1);
    CHECK(MsdDeviceGet(TEST_DEVICE_ID) == NULL);
}

// A device description that cannot be read is rejected and logged, and
// nothing is created or registered for it.
static void
__TestUnreadableDevice(void)
{
    struct sys_device sysDevice;
    int               registerCount = g_registerCount;
    int               logCount = g_logCount;

    // A platform description without compatible strings is invalid.
    memset(&sysDevice, 0, sizeof(sysDevice));
    sysDevice.content_type = SYS_DEVICE_CONTENT_PLATFORM;
    sysDevice.content.platform.id = TEST_DEVICE_ID;
    ctt_driver_register_device_invocation(NULL, &sysDevice);

    CHECK(g_registerCount == registerCount);
    CHECK(g_logCount == logCount + 1);
    CHECK(MsdDeviceGet(TEST_DEVICE_ID) == NULL);
}

int
main(void)
{
    __TestStatDuringRegister();
    __TestUnreadableDevice();
    puts("MSD registration: disk is listed before filed's stat query, unreadable device rejected");
    return 0;
}
