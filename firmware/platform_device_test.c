#include "../../services/deviced/core/devices.c"
#include "../../services/deviced/core/publication.c"
#define DMDevice PendingDriverDevice
#include "../../services/deviced/core/discover.c"
#undef DMDevice
#include "../../services/deviced/core/configparser.c"
#include "../../services/deviced/core/match.c"
#include "../../librt/libds/list.c"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static struct usched_mtx* g_locks[8];
static unsigned int g_depth;
static unsigned int g_deliveries;
static unsigned int g_spawns;
static int g_failSpawn;
static int g_failSend;
static uuid_t g_lastDevice;

void usched_mtx_init(struct usched_mtx* mutex, int type) { (void)type; memset(mutex, 0, sizeof(*mutex)); }
void usched_mtx_lock(struct usched_mtx* mutex)
{
    CHECK(g_depth < 8);
    for (unsigned int i = 0; i < g_depth; i++) { CHECK(g_locks[i] != mutex); }
    g_locks[g_depth++] = mutex;
}
void usched_mtx_unlock(struct usched_mtx* mutex) { CHECK(g_depth && g_locks[g_depth - 1] == mutex); g_depth--; }
void spinlock_init(spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void spinlock_acquire(spinlock_t* lock) { (void)lock; }
void spinlock_release(spinlock_t* lock) { (void)lock; }
int* __errno(void) { static int error; return &error; }
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }
gracht_client_t* GetGrachtClient(void) { return NULL; }
mstring_t* mstr_clone(mstring_t* text) { return (mstring_t*)strdup((char*)text); }
char* mstr_u8(mstring_t* text) { return strdup((char*)text); }
void mstr_delete(mstring_t* text) { free(text); }
int mstr_cmp(mstring_t* a, mstring_t* b) { return strcmp((char*)a, (char*)b); }
oserr_t OSProcessSpawn(const char* path, const char* args, uuid_t* handle)
{
    CHECK(path && args); *handle = 99; g_spawns++;
    if (g_failSpawn) { g_failSpawn = 0; return OS_EUNKNOWN; }
    return OS_EOK;
}
oserr_t OSDeviceIOCtl2(uuid_t device, uuid_t driver, enum OSIOCtlRequest request, void* buffer, size_t length)
{ (void)device; (void)driver; (void)request; (void)buffer; (void)length; return OS_ENOTSUPPORTED; }
int ctt_driver_subscribe(gracht_client_t* client, struct gracht_message_context* context)
{ (void)client; (void)context; return 0; }
int ctt_driver_get_device_protocols(gracht_client_t* client, struct gracht_message_context* context, uuid_t device)
{ (void)client; (void)context; (void)device; return 0; }

static void
__FreeDescription(Device_t* device)
{
    free(device->Identification.Description);
    free(device->Identification.Manufacturer);
    free(device->Identification.Product);
    free(device->Identification.Revision);
    free(device->Identification.Serial);
    free(device);
}

int
ctt_driver_register_device(gracht_client_t* client, struct gracht_message_context* context,
    const struct sys_device* description)
{
    char bytes[8192];
    gracht_buffer_t buffer = { .data = bytes, .limit = sizeof(bytes) };
    struct sys_device decoded;
    PlatformDevice_t* device;

    (void)client; (void)context;
    if (g_failSend) { g_failSend = 0; return -1; }
    // Numeric matching tests use real bus descriptions; platform tests below
    // continue to exercise the full platform message round trip.
    if (description->content_type == SYS_DEVICE_CONTENT_BUS) {
        g_lastDevice = description->content.bus.id;
        g_deliveries++;
        return 0;
    }
    CHECK(description->content_type == SYS_DEVICE_CONTENT_PLATFORM);
    serialize_sys_device(&buffer, description);
    CHECK(!buffer.error);
    buffer.limit = buffer.index;
    buffer.index = 0;
    sys_device_init(&decoded);
    deserialize_sys_device(&buffer, &decoded);
    CHECK(!buffer.error && buffer.index == buffer.limit);
    device = (PlatformDevice_t*)from_sys_device(&decoded);
    CHECK(device && PlatformDeviceValidate(device));
    CHECK(device->Pending == (PLATFORM_DEVICE_PENDING_INTERRUPTS | PLATFORM_DEVICE_PENDING_DMA));
    CHECK(device->Base.ParentId != UUID_INVALID);
    CHECK(device->Registers[0].Base == 0x1f00200000ULL);
    CHECK(device->Interrupts[0].Number == 31 && device->Interrupts[0].Controller == 42);
    g_lastDevice = device->Base.Id;
    g_deliveries++;
    __FreeDescription(&device->Base);
    sys_device_destroy(&decoded);
    return 0;
}

static struct DmDriver*
__Driver(const char* yaml, const char* path, int ready)
{
    struct DriverConfiguration* configuration = calloc(1, sizeof(*configuration));
    struct DmDriver* driver;

    CHECK(configuration != NULL);
    CHECK(DmDriverConfigParseYaml((const uint8_t*)yaml, strlen(yaml), configuration) == OS_EOK);
    CHECK(DmDiscoverAddDriver((mstring_t*)path, configuration) == OS_EOK);
    driver = g_drivers.tail->value;
    if (ready) { driver->state = DmDriverState_AVAILABLE; driver->handle = 77 + driver->id; }
    return driver;
}

static PlatformDevice_t*
__Platform(uuid_t parent, const char* compatibles, unsigned int length)
{
    PlatformDevice_t* device = calloc(1, sizeof(*device));

    CHECK(device != NULL);
    device->Base.Length = sizeof(*device);
    device->Base.ParentId = parent;
    device->Base.Identification.Description = strdup("firmware child");
    device->Version = 1;
    device->Pending = 3;
    device->FirmwareNode = 0x1234;
    memcpy(device->Compatibles, compatibles, length);
    device->CompatibleLength = length;
    device->RegisterCount = 1;
    device->Registers[0] = (struct PlatformRegister) { 0x1f00200000ULL, 0x100000 };
    device->InterruptCount = 1;
    device->Interrupts[0] = (struct PlatformInterrupt) { 42, 31, 1 };
    return device;
}

static void
__WireValidation(uuid_t parent)
{
    PlatformDevice_t* native = __Platform(parent, "snps,dwc3", sizeof("snps,dwc3"));
    struct sys_device wire;
    Device_t* copy;
    unsigned int count;

    CHECK(to_sys_device(&native->Base, &wire) == OS_EOK);
    copy = from_sys_device(&wire);
    CHECK(copy && ((PlatformDevice_t*)copy)->FirmwareNode == 0x1234);
    __FreeDescription(copy);
    count = wire.content.platform.compatibles_count;
    wire.content.platform.compatibles_count = PLATFORM_DEVICE_MAX_COMPATIBLES + 1;
    CHECK(from_sys_device(&wire) == NULL);
    wire.content.platform.compatibles_count = count - 1;
    CHECK(from_sys_device(&wire) == NULL);
    wire.content.platform.compatibles_count = count;
    wire.content.platform.registers_count = 9;
    CHECK(from_sys_device(&wire) == NULL);
    wire.content.platform.registers_count = 1;
    wire.content.platform.interrupts_count = 9;
    CHECK(from_sys_device(&wire) == NULL);
    wire.content.platform.interrupts_count = 1;
    wire.content.platform.registers[0].base = UINT64_MAX;
    CHECK(from_sys_device(&wire) == NULL);
    wire.content.platform.registers[0].base = 0x1f00200000ULL;
    wire.content.platform.version = 2;
    CHECK(from_sys_device(&wire) == NULL);
    sys_device_destroy(&wire);
    __FreeDescription(&native->Base);
}

static void
__RegisterMatchingBus(
    _InOut_ struct DmPublicationGroup* group,
    _In_    uint32_t                   productId,
    _InOut_ uuid_t*                    id)
{
    BusDevice_t* device = calloc(1, sizeof(*device));
    struct DmDeviceRegistration registration = { .Kind = DmDeviceDescriptionBus };

    CHECK(device != NULL);
    device->Base.Length = sizeof(*device);
    device->Base.VendorId = 0x1234;
    device->Base.ProductId = productId;
    device->Base.Class = 0x120003;
    device->Base.Subclass = 0x340000;
    registration.Description = &device->Base;
    CHECK(DmPublicationAdd(group, &registration, 1, id) == OS_EOK);
}

static void
__ExactMatchPrecedence(void)
{
    struct DmPublicationGroup group = { 0 };
    struct DmDriver* general;
    struct DmDriver* exact;
    uuid_t exactId = UUID_INVALID;
    uuid_t fallbackId = UUID_INVALID;
    unsigned int deliveries = g_deliveries;
    unsigned int order;

    general = __Driver("driver:\n  type:\n    - class: 0x120003\n    - subclass: 0x340000\n", "general", 1);
    exact = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x5678\n", "exact", 1);
    for (order = 0; order < 2; order++) {
        __RegisterMatchingBus(&group, 0x5678, &exactId);
        __RegisterMatchingBus(&group, 0x9999, &fallbackId);
        DmDeviceRefreshDrivers();
        CHECK(g_deliveries == deliveries);
        CHECK(!__GetDeviceUnsafe(exactId)->has_driver);
        CHECK(!__GetDeviceUnsafe(fallbackId)->has_driver);
        CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
        CHECK(DmPublicationFinish(&group) == OS_EOK);
        CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
        CHECK(__GetDeviceUnsafe(exactId)->driver_id == exact->handle);
        CHECK(__GetDeviceUnsafe(fallbackId)->driver_id == general->handle);
        CHECK(g_deliveries == deliveries + 2);
        CHECK(DmPublicationRemove(&group) == OS_EOK);
        CHECK(exactId == UUID_INVALID && fallbackId == UUID_INVALID);
        CHECK(!exact->devices.count && !general->devices.count);
        CHECK(DmPublicationReset(&group) == OS_EOK);
        deliveries = g_deliveries;

        // Repeat with the exact driver listed first to rule out list-order luck.
        list_remove(&g_drivers, &general->list_header);
        list_append(&g_drivers, &general->list_header);
    }
    CHECK(g_devices.count == 0);
    puts("Driver matching: exact identity wins in either order; no binding before finish passed");
}

int
main(void)
{
    const char ethernet[] = "raspberrypi,rp1-gem\0cdns,macb";
    const char* bad[] = {
        "driver:\n  compatibles: snps,dwc3\n", "driver:\n  compatibles: []\n",
        "driver:\n  compatibles: [\"\"]\n", "driver:\n  compatibles: [ok]\n  compatibles: [again]\n",
        "driver:\n  compatibles: [\"bad\\0tail\"]\n", "driver:\n  compatibles: [broken\n"
    };
    struct DriverConfiguration invalid;
    struct DriverIdentification identification = { 0 };
    struct DmDriver* numeric;
    struct DmDriver* fallback;
    struct DmDriver* specific;
    struct DmDriver* usb;
    struct DmDriver* queued;
    PlatformDevice_t* platform;
    Device_t* parent = calloc(1, sizeof(*parent));
    uuid_t parentId, ethId, usbId, lateId, cancelledId;
    unsigned int deliveries;
    size_t value = 0;
    struct OSIOCtlBusControl control = { 0 };

    DmDevicesInitialize();
    usched_mtx_init(&g_driversLock, USCHED_MUTEX_PLAIN);
    parent->Length = sizeof(*parent);
    parent->VendorId = 0x1de4;
    parent->Identification.Description = strdup("RP1 parent");
    CHECK(DmDeviceCreate(parent, 0, &parentId) == OS_EOK);
    __WireValidation(parentId);
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        if (DmDriverConfigParseYaml((const uint8_t*)bad[i], strlen(bad[i]), &invalid) == OS_EOK) {
            printf("invalid YAML accepted at case %u\n", i);
            CHECK(0);
        }
    }
    numeric = __Driver("driver:\n  type:\n    - class: 0xC0003\n    - subclass: 0x300000\n", "pci-usb", 1);
    fallback = __Driver("driver:\n  compatibles: [\"cdns,macb\"]\n", "fallback", 1);
    specific = __Driver("driver:\n  compatibles: [\"raspberrypi,rp1-gem\"]\n", "specific", 1);
    identification.Class = 0xC0003; identification.Subclass = 0x300000;
    CHECK(DmDriverMatchScore(numeric->configuration, &identification) == 2);
    identification.IsPlatform = 1; identification.Compatibles = ethernet;
    identification.CompatibleLength = sizeof(ethernet);
    CHECK(DmDriverMatchScore(numeric->configuration, &identification) == 0);
    CHECK(DmDriverMatchScore(fallback->configuration, &identification) == 2);
    CHECK(DmDriverMatchScore(specific->configuration, &identification) == 1);
    identification.CompatibleLength--;
    CHECK(DmDriverMatchScore(specific->configuration, &identification) == 0);

    platform = __Platform(parentId, ethernet, sizeof(ethernet));
    CHECK(DmDeviceCreate(&platform->Base, 0, &ethId) == OS_EOK);
    DmDeviceRefreshDrivers();
    CHECK(!g_deliveries && !DmDeviceIsBindable(parentId));
    CHECK(DmHandleIoctl(ethId, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(ethId, 1, 4, 0, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(DmDeviceEnableDriverBinding(ethId) == OS_EOK);
    CHECK(g_deliveries == 1 && g_lastDevice == ethId);
    CHECK(__GetDeviceUnsafe(ethId)->driver_id == specific->handle);
    CHECK(!fallback->devices.count && specific->devices.count == 1);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 1);

    platform = __Platform(parentId, "snps,dwc3", sizeof("snps,dwc3"));
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &usbId) == OS_EOK);
    CHECK(!__GetDeviceUnsafe(usbId)->has_driver);
    usb = __Driver("driver:\n  compatibles: [\"snps,dwc3\"]\n", "dwc3", 0);
    g_failSpawn = 1;
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 1 && !usb->devices.count && !__GetDeviceUnsafe(usbId)->has_driver);
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 2 && usb->devices.count == 1 && usb->state == DmDriverState_LOADING);
    DmHandleNotify(usb->id, 500);
    CHECK(g_deliveries == 2 && g_lastDevice == usbId && usb->state == DmDriverState_AVAILABLE);
    platform = __Platform(parentId, "snps,dwc3", sizeof("snps,dwc3"));
    g_failSend = 1;
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &lateId) == OS_EOK);
    CHECK(!__GetDeviceUnsafe(lateId)->has_driver && usb->devices.count == 1);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 3 && g_lastDevice == lateId && usb->devices.count == 2);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 3);

    queued = __Driver("driver:\n  compatibles: [\"test,pending\"]\n", "pending", 0);
    platform = __Platform(parentId, "test,pending", sizeof("test,pending"));
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &cancelledId) == OS_EOK);
    CHECK(queued->devices.count == 1);
    CHECK(DmDeviceDestroy(cancelledId) == OS_EOK && !queued->devices.count);
    deliveries = g_deliveries;
    DmHandleNotify(queued->id, 600);
    CHECK(g_deliveries == deliveries);
    CHECK(DmDeviceDestroy(parentId) == OS_EBUSY);
    CHECK(DmDeviceDestroy(ethId) == OS_EOK && !specific->devices.count);
    CHECK(DmDeviceDestroy(usbId) == OS_EOK);
    CHECK(DmDeviceDestroy(lateId) == OS_EOK && !usb->devices.count);
    CHECK(DmDeviceDestroy(parentId) == OS_EOK && !g_devices.count);
    CHECK(!DmDeviceIsBindable(ethId) && DmDevicesRegister(500, ethId) == OS_ENOENT);
    __ExactMatchPrecedence();
    CHECK(!g_depth);
    while (g_drivers.head) {
        struct DmDriver* driver = g_drivers.head->value;
        CHECK(!driver->devices.count);
        list_remove(&g_drivers, &driver->list_header);
        __DestroyDriver(driver);
    }
    puts("Platform devices: real registry/discovery, YAML matching, codec, gates, late binding and removal passed");
    return 0;
}
