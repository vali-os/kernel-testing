#ifndef DEVICES_SOURCE
#define DEVICES_SOURCE "../../services/deviced/core/devices.c"
#endif
#include DEVICES_SOURCE
#include "../../services/deviced/core/dma-device.c"
#include "../../services/deviced/core/dma.c"
// Kernel registration is exercised by dma_service_test. This fixture keeps
// publication/registry failure injection independent of the manager transport.
static oserr_t __TestPublicationDma(uuid_t device) { (void)device; return OS_EOK; }
#define DmDmaRegisterDevice __TestPublicationDma
#include "../../services/deviced/core/publication.c"
#undef DmDmaRegisterDevice
#ifndef DISCOVER_SOURCE
#define DISCOVER_SOURCE "../../services/deviced/core/drivers.c"
#endif
#include DISCOVER_SOURCE
#include "../../services/deviced/core/configparser.c"
#include "../../services/deviced/core/match.c"
#include "../../librt/libds/list.c"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static struct usched_mtx* g_locks[8];
static unsigned int g_depth;
static unsigned int g_deliveries;
static unsigned int g_spawns;
static unsigned int g_subscriptions;
static void (*g_discoveryUnlockHook)(void);
static int g_failSpawn;
static int g_failSend;
// A device whose removal starts while its driver is being started.
static uuid_t g_removeDuringSpawn = UUID_INVALID;
static uuid_t g_lastDevice;

// The process API keeps options opaque; this fixture records only fields it uses.
struct OSProcessOptions {
    const char* Arguments;
    Capabilities_t Capabilities;
};

OSProcessOptions_t*
OSProcessOptionsNew(void)
{
    return calloc(1, sizeof(OSProcessOptions_t));
}

void
OSProcessOptionsDelete(
    _In_ OSProcessOptions_t* options)
{
    free(options);
}

void
OSProcessOptionsSetArguments(
    _In_ OSProcessOptions_t* options,
    _In_ const char* arguments)
{
    options->Arguments = arguments;
}

void
OSProcessOptionsSetCapabilities(
    _In_ OSProcessOptions_t* options,
    _In_ Capabilities_t capabilities)
{
    options->Capabilities = capabilities;
}

// Keep failure diagnostics alongside the existing delivery and ownership checks.
static struct {
    enum OSSysLogLevel Level;
    char Text[256];
} g_diagnostics[32];
static unsigned int g_diagnosticCount;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    va_list arguments;

    if (level != OSSYSLOGLEVEL_WARNING && level != OSSYSLOGLEVEL_ERROR) {
        return;
    }
    CHECK(g_diagnosticCount < sizeof(g_diagnostics) / sizeof(g_diagnostics[0]));
    g_diagnostics[g_diagnosticCount].Level = level;
    va_start(arguments, format);
    vsnprintf(g_diagnostics[g_diagnosticCount].Text,
        sizeof(g_diagnostics[g_diagnosticCount].Text), format, arguments);
    va_end(arguments);
    g_diagnosticCount++;
}

static int
__HasDiagnostic(
    _In_ enum OSSysLogLevel level,
    _In_ const char* text)
{
    unsigned int index;

    for (index = 0; index < g_diagnosticCount; index++) {
        if (g_diagnostics[index].Level == level &&
            strstr(g_diagnostics[index].Text, text) != NULL) {
            return 1;
        }
    }
    return 0;
}

void usched_mtx_init(struct usched_mtx* mutex, int type) { (void)type; memset(mutex, 0, sizeof(*mutex)); }
void usched_mtx_lock(struct usched_mtx* mutex)
{
    CHECK(g_depth < 8);
    for (unsigned int i = 0; i < g_depth; i++) { CHECK(g_locks[i] != mutex); }
    g_locks[g_depth++] = mutex;
}
void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    CHECK(g_depth && g_locks[g_depth - 1] == mutex);
    g_depth--;
    if (mutex == &g_driversLock && g_discoveryUnlockHook != NULL) {
        g_discoveryUnlockHook();
    }
}
void spinlock_init(spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void spinlock_acquire(spinlock_t* lock) { (void)lock; }
void spinlock_release(spinlock_t* lock) { (void)lock; }
int* __errno(void) { static int error; return &error; }
gracht_client_t* GetGrachtClient(void) { return NULL; }
mstring_t* mstr_clone(mstring_t* text) { return (mstring_t*)strdup((char*)text); }
char* mstr_u8(mstring_t* text) { return strdup((char*)text); }
void mstr_delete(mstring_t* text) { free(text); }
int mstr_cmp(mstring_t* a, mstring_t* b) { return strcmp((char*)a, (char*)b); }
oserr_t
OSProcessSpawn(
    _In_     const char*          path,
    _In_Opt_ OSProcessOptions_t*  options,
    _Out_    OSProcessIdentity_t* identityOut)
{
    // Discovery must exclude readiness until it has stored the spawn result.
    CHECK(g_depth == 1 && g_locks[0] == &g_driversLock);
    CHECK(options != NULL && options->Capabilities == 0);
    CHECK(path && options->Arguments);
    g_spawns++;
    if (g_removeDuringSpawn != UUID_INVALID) {
        // The lookup runs without the registry lock, so a removal can start
        // here. A request that is still running keeps the record in place.
        CHECK(DmDeviceDestroy(g_removeDuringSpawn) == OS_EBUSY);
        g_removeDuringSpawn = UUID_INVALID;
    }
    if (g_failSpawn) {
        g_failSpawn = 0;
        return OS_EUNKNOWN;
    }

    *identityOut = (OSProcessIdentity_t) {
        .Process = 99,
        .Principal = 0x123456780000002aULL
    };
    return OS_EOK;
}

Capabilities_t
CapabilityParse(
    _In_ const char* name)
{
    // These matching fixtures do not request process capabilities.
    (void)name;
    CHECK(0);
    return 0;
}

oserr_t OSDeviceIOCtl2(uuid_t device, uuid_t driver, enum OSIOCtlRequest request, void* buffer, size_t length)
{ (void)device; (void)driver; (void)request; (void)buffer; (void)length; return OS_ENOTSUPPORTED; }
int ctt_driver_subscribe(gracht_client_t* client, struct gracht_message_context* context)
{ (void)client; (void)context; g_subscriptions++; return 0; }
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
    DmDevice_t* assigned;
    uuid_t deviceId;

    (void)client; (void)context;
    if (description->content_type == SYS_DEVICE_CONTENT_BUS) {
        deviceId = description->content.bus.id;
    } else {
        CHECK(description->content_type == SYS_DEVICE_CONTENT_PLATFORM);
        deviceId = description->content.platform.id;
    }
    // A driver can use the device as soon as this send completes. Its verified
    // assignment must already be visible, even when the send later fails.
    assigned = DmDeviceFindLocked(deviceId);
    CHECK(assigned != NULL && assigned->DriverId != UUID_INVALID);
    CHECK(assigned->DriverPrincipal == 0x123456780000002aULL);
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

static struct __DmDriver*
__Driver(const char* yaml, const char* path, int ready)
{
    struct DriverConfiguration* configuration = calloc(1, sizeof(*configuration));
    struct __DmDriver* driver;

    CHECK(configuration != NULL);
    CHECK(DmDriverConfigParseYaml((const uint8_t*)yaml, strlen(yaml), configuration) == OS_EOK);
    CHECK(DmDriverAdd((mstring_t*)path, configuration) == OS_EOK);
    driver = g_drivers.tail->value;
    if (ready) {
        driver->State = DmDriverState_AVAILABLE;
        driver->Handle = 77 + driver->Id;
        driver->Process = 99;
        driver->Principal = 0x123456780000002aULL;
    }
    return driver;
}

// Frees a driver that __Driver added. Discovery never frees its drivers, so
// the tests do it themselves after taking the driver off g_drivers.
static void
__DestroyTestDriver(struct __DmDriver* driver)
{
    DmDriverConfigDestroy(driver->Configuration);
    mstr_delete(driver->Path);
    free(driver);
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
    struct __DmDriver* general;
    struct __DmDriver* exact;
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
        CHECK(DmDeviceFindLocked(exactId)->State == DmDeviceNoDriver);
        CHECK(DmDeviceFindLocked(fallbackId)->State == DmDeviceNoDriver);
        CHECK(DmPublicationEnableBinding(&group) == OS_EBUSY);
        CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
        CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
        CHECK(DmDeviceFindLocked(exactId)->DriverId == exact->Handle);
        CHECK(DmDeviceFindLocked(fallbackId)->DriverId == general->Handle);
        CHECK(g_deliveries == deliveries + 2);
        CHECK(DmPublicationRemove(&group) == OS_EOK);
        CHECK(exactId == UUID_INVALID && fallbackId == UUID_INVALID);
        CHECK(!exact->Devices.count && !general->Devices.count);
        CHECK(group.State == DmPublicationCollecting);
        deliveries = g_deliveries;

        // Repeat with the exact driver listed first to rule out list-order luck.
        list_remove(&g_drivers, &general->ListHeader);
        list_append(&g_drivers, &general->ListHeader);
    }
    CHECK(g_devices.count == 0);
    puts("Driver matching: exact identity wins in either order; no binding before finish passed");
}

// The manifest keys "controller-compatibles" and "clock-routes": every
// accepted and refused form, and the lookup by driver endpoint that deviced
// uses when a driver enables its controller. Only a driver that reported its
// endpoint is found.
static void
__TestControllerManifest(void)
{
    static const char manifest[] =
        "driver:\n"
        "  compatibles: [\"brcm,bcm2835-mbox\"]\n"
        "  controller-compatibles:\n"
        "    - raspberrypi,firmware-gpio\n"
        "    - raspberrypi,firmware-clocks\n"
        "  clock-routes:\n"
        "    - compatible: brcm,bcm2711-cprman\n"
        "      clock: 51\n"
        "      controller-clock: 12\n"
        "    - compatible: test,other\n"
        "      clock: 0x10\n"
        "      controller-clock: 0\n";
    const char* bad[] = {
        // A key missing, repeated or unknown.
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: 1\n",
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: 1\n      clock: 2\n      controller-clock: 3\n",
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: 1\n      controller-clock: 3\n      rate: 4\n",
        // Numbers that do not parse in full, are negative or do not fit.
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: 5x\n      controller-clock: 3\n",
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: -1\n      controller-clock: 3\n",
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: 1\n      controller-clock: 0x100000000\n",
        "driver:\n  clock-routes:\n    - compatible: a\n      clock: \"\"\n      controller-clock: 3\n",
        // Empty, embedded NUL or too long compatible.
        "driver:\n  clock-routes:\n    - compatible: \"\"\n      clock: 1\n      controller-clock: 3\n",
        "driver:\n  clock-routes:\n    - compatible: \"a\\0b\"\n      clock: 1\n      controller-clock: 3\n",
        "driver:\n  clock-routes:\n    - compatible: "
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
            "      clock: 1\n      controller-clock: 3\n",
        // Wrong shapes, a second list, and more routes than allowed.
        "driver:\n  clock-routes:\n    - compatible: [a]\n      clock: 1\n      controller-clock: 3\n",
        "driver:\n  clock-routes: a\n",
        "driver:\n  clock-routes: []\n  clock-routes: []\n",
        "driver:\n  clock-routes:\n"
            "    - {compatible: a, clock: 1, controller-clock: 1}\n"
            "    - {compatible: a, clock: 2, controller-clock: 2}\n"
            "    - {compatible: a, clock: 3, controller-clock: 3}\n"
            "    - {compatible: a, clock: 4, controller-clock: 4}\n"
            "    - {compatible: a, clock: 5, controller-clock: 5}\n",
        // Controller compatibles: empty list, wrong shapes, a second list,
        // a bad string and more entries than allowed.
        "driver:\n  controller-compatibles: []\n",
        "driver:\n  controller-compatibles: a\n",
        "driver:\n  controller-compatibles: [[a]]\n",
        "driver:\n  controller-compatibles: [a]\n  controller-compatibles: [b]\n",
        "driver:\n  controller-compatibles: [\"\"]\n",
        "driver:\n  controller-compatibles: [a, b, c, d, e]\n",
    };
    struct DriverConfiguration      invalid;
    struct DriverControllerManifest copy;
    struct DriverControllerManifest saved;
    struct __DmDriver*              driver;
    struct __DmDriver*              plain;

    // Each refused manifest logs. Only the lines of the last cases matter.
    for (unsigned int i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        g_diagnosticCount = 0;
        if (DmDriverConfigParseYaml((const uint8_t*)bad[i], strlen(bad[i]), &invalid) == OS_EOK) {
            printf("invalid clock route accepted at case %u\n", i);
            CHECK(0);
        }
    }
    g_diagnosticCount = 0;

    driver = __Driver(manifest, "routes", 0);
    CHECK(driver->Configuration->Controller.CompatibleCount == 2);
    CHECK(!strcmp(driver->Configuration->Controller.Compatibles[0], "raspberrypi,firmware-gpio"));
    CHECK(!strcmp(driver->Configuration->Controller.Compatibles[1], "raspberrypi,firmware-clocks"));
    CHECK(driver->Configuration->Controller.ClockRouteCount == 2);
    CHECK(!strcmp(driver->Configuration->Controller.ClockRoutes[0].Compatible, "brcm,bcm2711-cprman"));
    CHECK(driver->Configuration->Controller.ClockRoutes[0].Clock == 51);
    CHECK(driver->Configuration->Controller.ClockRoutes[0].ControllerClock == 12);
    CHECK(!strcmp(driver->Configuration->Controller.ClockRoutes[1].Compatible, "test,other"));
    CHECK(driver->Configuration->Controller.ClockRoutes[1].Clock == 16);
    CHECK(driver->Configuration->Controller.ClockRoutes[1].ControllerClock == 0);

    // Before the driver reported its endpoint, nothing is found, and the
    // output stays unchanged.
    memset(&copy, 0xa5, sizeof(copy));
    saved = copy;
    CHECK(DmDriverControllerManifest(700, &copy) == OS_ENOENT);
    CHECK(DmDriverControllerManifest(UUID_INVALID, &copy) == OS_ENOENT);
    CHECK(DmDriverControllerManifest(700, NULL) == OS_EINVALPARAMS);
    CHECK(!memcmp(&copy, &saved, sizeof(copy)));

    // Readiness makes the manifest reachable by the endpoint.
    driver->State = DmDriverState_LOADING;
    driver->Process = 98;
    driver->Principal = 0x1234567800000077ULL;
    DmHandleNotify(driver->Principal, driver->Id, 700);
    CHECK(driver->State == DmDriverState_AVAILABLE);
    CHECK(DmDriverControllerManifest(700, &copy) == OS_EOK);
    CHECK(!memcmp(&copy, &driver->Configuration->Controller, sizeof(copy)));
    CHECK(DmDriverControllerManifest(701, &copy) == OS_ENOENT);

    // A driver without the key has no routes.
    plain = __Driver("driver:\n  compatibles: [\"test,plain\"]\n", "plain", 0);
    plain->State = DmDriverState_LOADING;
    plain->Process = 97;
    plain->Principal = 0x1234567800000078ULL;
    DmHandleNotify(plain->Principal, plain->Id, 702);
    CHECK(DmDriverControllerManifest(702, &copy) == OS_EOK);
    CHECK(copy.CompatibleCount == 0 && copy.ClockRouteCount == 0);
    CHECK(!g_depth);
}

static void
__TestDriverDiagnostics(void)
{
    struct DmPublicationGroup group = { 0 };
    struct __DmDriver* driver;
    uuid_t deviceId = UUID_INVALID;
    char diagnostic[256];

    DmDevicesInitialize();
    usched_mtx_init(&g_driversLock, USCHED_MUTEX_PLAIN);
    usched_mtx_init(&g_startedDriversLock, USCHED_MUTEX_PLAIN);
    __RegisterMatchingBus(&group, 0x5678, &deviceId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    g_diagnosticCount = 0;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(DmDeviceFindLocked(deviceId)->State == DmDeviceNoDriver && g_diagnosticCount == 0);
    driver = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x5678\n", "retry-driver", 0);

    g_failSpawn = 1;
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 1 && !driver->Devices.count && DmDeviceFindLocked(deviceId)->State == DmDeviceNoDriver);
    CHECK(driver->State == DmDriverState_LOADING && driver->Principal == 0);
    snprintf(diagnostic, sizeof(diagnostic),
        "DmDriverFind: starting driver %u for device %u failed: %u",
        driver->Id, deviceId, OS_EUNKNOWN);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
    g_diagnosticCount = 0;
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 1 && driver->Devices.count == 1 && driver->State == DmDriverState_LOADING);
    CHECK(driver->Process == UUID_INVALID && driver->Principal == 0);
    DmHandleNotify(0x123456780000002aULL, driver->Id, 500);
    CHECK(driver->Handle == UUID_INVALID && g_deliveries == 0);

    // The fixture explicitly removes the failed attempt before testing send retry.
    // Production discovery must never turn a lost spawn reply into another spawn.
    __ForgetDriverDevice(driver, deviceId);
    list_remove(&g_drivers, &driver->ListHeader);
    __DestroyTestDriver(driver);
    DmDeviceFindLocked(deviceId)->State = DmDeviceNoDriver;
    driver = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x5678\n", "send-retry-driver", 0);
    g_diagnosticCount = 0;
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 2 && driver->Principal == 0x123456780000002aULL);
    CHECK(g_diagnosticCount == 0);

    g_failSend = 1;
    DmHandleNotify(driver->Principal, driver->Id, 500);
    CHECK(g_deliveries == 0 && DmDeviceFindLocked(deviceId)->State == DmDeviceDriverRequested);
    CHECK(DmDeviceFindLocked(deviceId)->DriverId == driver->Handle);
    CHECK(DmDeviceFindLocked(deviceId)->DriverPrincipal == driver->Principal);
    CHECK(driver->Devices.count == 1 && driver->State == DmDriverState_AVAILABLE);
    snprintf(diagnostic, sizeof(diagnostic),
        "DmDevicesRegister: sending device %u to driver %u failed: -1", deviceId, driver->Handle);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
    g_diagnosticCount = 0;
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 0 && driver->Devices.count == 1);
    CHECK(DmDeviceFindLocked(deviceId)->DriverId == driver->Handle && g_diagnosticCount == 0);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 0);

    CHECK(DmPublicationRemove(&group) == OS_EOK && deviceId == UUID_INVALID);
    CHECK(g_devices.count == 0 && driver->Devices.count == 0 && g_depth == 0);
    list_remove(&g_drivers, &driver->ListHeader);
    __DestroyTestDriver(driver);
    puts("Driver diagnostics: normal misses, failed spawn/send context and retry state passed");
}

static void
__TestRemovalDuringLookup(void)
{
    struct DmPublicationGroup group = { 0 };
    struct __DmDriver*        driver;
    uuid_t                    deviceId = UUID_INVALID;
    unsigned int              spawns;

    DmDevicesInitialize();
    usched_mtx_init(&g_driversLock, USCHED_MUTEX_PLAIN);
    usched_mtx_init(&g_startedDriversLock, USCHED_MUTEX_PLAIN);
    __RegisterMatchingBus(&group, 0x5678, &deviceId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(DmDeviceFindLocked(deviceId)->State == DmDeviceNoDriver);
    driver = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x5678\n", "removal-driver", 0);

    // The driver fails to start, and the removal of its device begins before
    // the lookup hears of the failure. The failed lookup must leave the
    // removal standing. Otherwise the next refresh hands a device that is
    // going away to a driver.
    DmDeviceFindLocked(deviceId)->ActiveRequests = 1;
    spawns = g_spawns;
    g_failSpawn = 1;
    g_removeDuringSpawn = deviceId;
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == spawns + 1 && g_removeDuringSpawn == UUID_INVALID);
    CHECK(DmDeviceFindLocked(deviceId)->State == DmDeviceRemoving);
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == spawns + 1 && DmDeviceFindLocked(deviceId)->State == DmDeviceRemoving);

    __ForgetDriverDevice(driver, deviceId);
    DmDeviceFindLocked(deviceId)->ActiveRequests = 0;
    CHECK(DmPublicationRemove(&group) == OS_EOK && deviceId == UUID_INVALID);
    CHECK(g_devices.count == 0 && g_depth == 0);
    list_remove(&g_drivers, &driver->ListHeader);
    __DestroyTestDriver(driver);
    // The tests after this one count driver starts from zero.
    g_spawns = spawns;
    puts("Driver lookup: a removal that starts during a failed lookup stays in place passed");
}

int
main(
    _In_ int argc,
    _In_ char** argv)
{
    const char ethernet[] = "raspberrypi,rp1-gem\0cdns,macb";
    const char* bad[] = {
        "driver:\n  compatibles: snps,dwc3\n", "driver:\n  compatibles: []\n",
        "driver:\n  compatibles: [\"\"]\n", "driver:\n  compatibles: [ok]\n  compatibles: [again]\n",
        "driver:\n  compatibles: [\"bad\\0tail\"]\n", "driver:\n  compatibles: [broken\n"
    };
    struct DriverConfiguration invalid;
    struct DriverIdentification identification = { 0 };
    struct __DmDriver* numeric;
    struct __DmDriver* fallback;
    struct __DmDriver* specific;
    struct __DmDriver* usb;
    struct __DmDriver* queued;
    PlatformDevice_t* platform;
    Device_t* parent;
    uuid_t parentId, ethId, usbId, lateId, cancelledId;
    unsigned int deliveries;
    size_t value = 0;
    struct OSIOCtlBusControl control = { 0 };

    if (argc == 2 && strcmp(argv[1], "--diagnostics-only") == 0) {
        __TestDriverDiagnostics();
        return 0;
    }
    __TestRemovalDuringLookup();

    DmDevicesInitialize();
    usched_mtx_init(&g_driversLock, USCHED_MUTEX_PLAIN);
    usched_mtx_init(&g_startedDriversLock, USCHED_MUTEX_PLAIN);
    parent = calloc(1, sizeof(*parent));
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
    // One implementation can bind the same controller through either provider.
    {
        struct DriverCompatible compatible = { .Name = "generic-xhci" };
        ELEMENT_INIT(&compatible.ListHeader, 0, &compatible);
        list_append(&numeric->Configuration->Compatibles, &compatible.ListHeader);
        CHECK(DmDriverMatchScore(numeric->Configuration, &identification) == 2);
        identification.IsPlatform = 1;
        identification.Compatibles = "generic-xhci";
        identification.CompatibleLength = sizeof("generic-xhci");
        CHECK(DmDriverMatchScore(numeric->Configuration, &identification) == 1);
        identification.IsPlatform = 0;
        list_remove(&numeric->Configuration->Compatibles, &compatible.ListHeader);
    }
    CHECK(DmDriverMatchScore(numeric->Configuration, &identification) == 2);
    identification.IsPlatform = 1; identification.Compatibles = ethernet;
    identification.CompatibleLength = sizeof(ethernet);
    CHECK(DmDriverMatchScore(numeric->Configuration, &identification) == 0);
    CHECK(DmDriverMatchScore(fallback->Configuration, &identification) == 2);
    CHECK(DmDriverMatchScore(specific->Configuration, &identification) == 1);
    identification.CompatibleLength--;
    CHECK(DmDriverMatchScore(specific->Configuration, &identification) == 0);

    platform = __Platform(parentId, ethernet, sizeof(ethernet));
    CHECK(DmDeviceCreate(&platform->Base, 0, &ethId) == OS_EOK);
    DmDeviceRefreshDrivers();
    CHECK(!g_deliveries && !DmDeviceIsBindable(parentId));
    CHECK(DmHandleIoctl(700, ethId, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_EPERMISSIONS);
    CHECK(DmHandleIoctl2(700, ethId, 1, 4, 0, 4, &value) == OS_EPERMISSIONS);
    CHECK(DmDeviceEnableDriverBinding(ethId) == OS_EOK);
    CHECK(g_deliveries == 1 && g_lastDevice == ethId);
    CHECK(DmDeviceFindLocked(ethId)->DriverId == specific->Handle);
    CHECK(!fallback->Devices.count && specific->Devices.count == 1);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 1);

    platform = __Platform(parentId, "snps,dwc3", sizeof("snps,dwc3"));
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &usbId) == OS_EOK);
    CHECK(DmDeviceFindLocked(usbId)->State == DmDeviceNoDriver);
    usb = __Driver("driver:\n  compatibles: [\"snps,dwc3\"]\n", "dwc3", 0);
    DmDeviceRefreshDrivers();
    CHECK(g_spawns == 1 && usb->Devices.count == 1 && usb->State == DmDriverState_LOADING);
    DmHandleNotify(usb->Principal, usb->Id, 500);
    CHECK(g_deliveries == 2 && g_lastDevice == usbId && usb->State == DmDriverState_AVAILABLE);
    platform = __Platform(parentId, "snps,dwc3", sizeof("snps,dwc3"));
    g_failSend = 1;
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &lateId) == OS_EOK);
    CHECK(DmDeviceFindLocked(lateId)->State == DmDeviceDriverRequested && usb->Devices.count == 1);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 2 && g_lastDevice == usbId && usb->Devices.count == 1);
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == 2);

    queued = __Driver("driver:\n  compatibles: [\"test,pending\"]\n", "pending", 0);
    platform = __Platform(parentId, "test,pending", sizeof("test,pending"));
    CHECK(DmDeviceCreate(&platform->Base, DEVICE_REGISTER_FLAG_LOADDRIVER, &cancelledId) == OS_EOK);
    CHECK(queued->Devices.count == 1);
    CHECK(DmDeviceDestroy(cancelledId) == OS_EOK && !queued->Devices.count);
    deliveries = g_deliveries;
    DmHandleNotify(queued->Principal, queued->Id, 600);
    CHECK(g_deliveries == deliveries);
    CHECK(DmDeviceDestroy(parentId) == OS_EBUSY);
    CHECK(DmDeviceDestroy(ethId) == OS_EOK && !specific->Devices.count);
    CHECK(DmDeviceDestroy(usbId) == OS_EOK);
    CHECK(DmDeviceDestroy(lateId) == OS_EOK && !usb->Devices.count);
    CHECK(DmDeviceDestroy(parentId) == OS_EOK && !g_devices.count);
    CHECK(!DmDeviceIsBindable(ethId) && DmDevicesRegister(42, 500, ethId) == OS_ENOENT);
    __ExactMatchPrecedence();
    __TestControllerManifest();
    CHECK(!g_depth);
    while (g_drivers.head) {
        struct __DmDriver* driver = g_drivers.head->value;
        CHECK(!driver->Devices.count);
        list_remove(&g_drivers, &driver->ListHeader);
        __DestroyTestDriver(driver);
    }
    puts("Platform devices: real registry/discovery, YAML matching, codec, gates, late binding and removal passed");
    return 0;
}

// These older fixtures create no manager attachments. Catch unexpected kernel
// registration instead of pretending a real attachment was installed.
oserr_t DMAAttachmentRegister(uuid_t manager, const OSDmaAttachmentParameters_t* parameters, OSDmaAttachmentId_t* output)
{ (void)manager; (void)parameters; (void)output; CHECK(0); return OS_EUNKNOWN; }
oserr_t DMAAttachmentRemove(uuid_t manager, const OSDmaAttachmentId_t* id)
{ (void)manager; (void)id; CHECK(0); return OS_EUNKNOWN; }

oserr_t DmDmaDrainEventsLocked(void)
{ CHECK(g_dmDmaState.Devices == NULL); return OS_EOK; }

oserr_t DMAAttachmentBind(uuid_t manager, const OSDmaBindParameters_t* request, OSDmaBindingResult_t* result)
{ (void)manager; (void)request; (void)result; CHECK(0); return OS_ENOTSUPPORTED; }
