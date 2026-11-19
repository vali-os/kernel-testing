// Reuse the registry, discovery, transport stubs and lock checks from the platform suite.
#define main __PlatformTestMain
#include "platform_device_test.c"
#undef main
#ifndef REQUESTS_SOURCE
#define REQUESTS_SOURCE "../../services/deviced/core/requests.c"
#endif
static oserr_t __TestIoctl(uint64_t principal, uuid_t deviceId, enum OSIOCtlRequest request,
                           void* buffer, size_t length);
static int __TestIoctlReply(struct gracht_message* message, uint8_t* in, uint32_t count, oserr_t result);
#define DmHandleIoctl __TestIoctl
#define sys_device_ioctl_response __TestIoctlReply
#include REQUESTS_SOURCE
#undef DmHandleIoctl
#undef sys_device_ioctl_response

// Input requests: the handler sees the request body, which carries selectors
// such as a supply name, and the reply carries the handler's answer.
static uint8_t  g_ioctlSeen[8];
static uint8_t  g_ioctlReply[8];
static uint32_t g_ioctlReplyCount;

static oserr_t
__TestIoctl(uint64_t principal, uuid_t deviceId, enum OSIOCtlRequest request, void* buffer, size_t length)
{
    CHECK(principal == 7 && deviceId == 3 && length == sizeof(g_ioctlSeen));
    CHECK(request == OSIOCTLREQUEST_SUPPLY_CONTROL);
    memcpy(g_ioctlSeen, buffer, length);
    ((uint8_t*)buffer)[0] = 0xEE;
    return OS_EOK;
}

static int
__TestIoctlReply(struct gracht_message* message, uint8_t* in, uint32_t count, oserr_t result)
{
    (void)message;
    CHECK(result == OS_EOK && count == sizeof(g_ioctlReply));
    memcpy(g_ioctlReply, in, count);
    g_ioctlReplyCount = count;
    return 0;
}

static void
__InputRequest(void)
{
    const uint8_t         body[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    struct gracht_message message = { 0 };

    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = 7;
    sys_device_ioctl_invocation(&message, 3, OSIOCTLREQUEST_SUPPLY_CONTROL, body, sizeof(body));
    CHECK(!memcmp(g_ioctlSeen, body, sizeof(body)));
    CHECK(g_ioctlReplyCount == sizeof(body) && g_ioctlReply[0] == 0xEE && g_ioctlReply[7] == 8);
}

static struct __DmDriver* g_startingDriver;
static uuid_t            g_startingDevice;

static void
__Notify(
    _In_ struct __DmDriver* driver,
    _In_ int                source,
    _In_ uint64_t           principal,
    _In_ uuid_t             endpoint)
{
    struct gracht_message message = { 0 };

    message.security.source = source;
    message.security.principal = principal;
    sys_device_notify_invocation(&message, driver->Id, endpoint);
}

static void
__ReadyAtSpawnPublication(void)
{
    // Deliver a ready message at the first unlock after spawn. The spawn stub
    // checks that discovery held its lock throughout the RPC. Both identity
    // fields must be visible before this waiting notification can run.
    g_discoveryUnlockHook = NULL;
    CHECK(g_depth == 0);
    CHECK(g_startingDriver->Process == 99);
    CHECK(g_startingDriver->Principal == 0x123456780000002aULL);
    CHECK(g_startingDriver->State == DmDriverState_LOADING);
    CHECK(DmDeviceFindLocked(g_startingDevice)->DriverPrincipal == 0);
    __Notify(g_startingDriver, GRACHT_SECURITY_KERNEL, g_startingDriver->Principal, 500);
}

int
main(void)
{
    struct DmPublicationGroup group = { 0 };
    struct __DmDriver*       driver;
    struct __DmDriver*       unstarted;
    struct __DmDriver*       failed;
    DmDevice_t*       device;
    struct gracht_message    message = { 0 };
    uuid_t                   deviceId = UUID_INVALID;
    uuid_t                   lateId = UUID_INVALID;
    unsigned int             deliveries;
    unsigned int             spawns;

    DmDevicesInitialize();
    usched_mtx_init(&g_driversLock, USCHED_MUTEX_PLAIN);
    __InputRequest();
    driver = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x5678\n", "verified", 0);
    unstarted = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x9999\n", "unstarted", 0);
    __RegisterMatchingBus(&group, 0x5678, &deviceId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);

    __Notify(driver, GRACHT_SECURITY_KERNEL, 42, 500);
    CHECK(driver->State == DmDriverState_NOTLOADED && driver->Handle == UUID_INVALID);
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    device = DmDeviceFindLocked(deviceId);
    CHECK(driver->State == DmDriverState_LOADING && g_spawns == 1);
    CHECK(device->DriverId == UUID_INVALID && device->DriverPrincipal == 0);

    // Kernel credentials, driver selection and all 64 principal bits matter.
    __Notify(driver, GRACHT_SECURITY_NONE, driver->Principal, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, 0, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, (uint32_t)driver->Principal, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Process, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, 500, 500);
    __Notify(unstarted, GRACHT_SECURITY_KERNEL, driver->Principal, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Principal, UUID_INVALID);
    message.security.source = GRACHT_SECURITY_KERNEL;
    message.security.principal = driver->Principal;
    sys_device_notify_invocation(&message, UUID_INVALID, 500);
    CHECK(driver->State == DmDriverState_LOADING && driver->Handle == UUID_INVALID);
    CHECK(device->DriverId == UUID_INVALID && device->DriverPrincipal == 0);
    CHECK(g_deliveries == 0 && g_subscriptions == 0);

    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Principal, 500);
    CHECK(driver->State == DmDriverState_AVAILABLE && driver->Handle == 500);
    CHECK(driver->Process == 99 && device->DriverId == 500);
    CHECK(device->DriverPrincipal == 0x123456780000002aULL);
    CHECK(g_deliveries == 1 && g_subscriptions == 1);
    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Principal, 500);
    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Principal, 501);
    __Notify(driver, GRACHT_SECURITY_KERNEL, driver->Principal + 1, 500);
    CHECK(driver->Handle == 500 && device->DriverPrincipal == driver->Principal);
    CHECK(g_deliveries == 1 && g_subscriptions == 1);
    CHECK(DmPublicationRemove(&group) == OS_EOK);

    // Uncertain sends preserve the assignment and must not trigger a retry.
    __RegisterMatchingBus(&group, 0x5678, &lateId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    g_failSend = 1;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    device = DmDeviceFindLocked(lateId);
    CHECK(device->DriverId == 500 && device->DriverPrincipal == driver->Principal);
    deliveries = g_deliveries;
    DmDeviceRefreshDrivers();
    CHECK(g_deliveries == deliveries);
    CHECK(device->DriverId == 500 && device->DriverPrincipal == driver->Principal);
    CHECK(DmPublicationRemove(&group) == OS_EOK);

    // Readiness queued during spawn runs only after identity publication.
    __RegisterMatchingBus(&group, 0x9999, &deviceId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    g_startingDriver = unstarted;
    g_startingDevice = deviceId;
    g_discoveryUnlockHook = __ReadyAtSpawnPublication;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    CHECK(unstarted->State == DmDriverState_AVAILABLE);
    CHECK(DmDeviceFindLocked(deviceId)->DriverPrincipal == unstarted->Principal);
    CHECK(DmPublicationRemove(&group) == OS_EOK);

    // An uncertain spawn is unavailable even when a sender guesses its ID.
    failed = __Driver("driver:\n  vendors:\n  - 0x1234:\n    - 0x1111\n", "lost-reply", 0);
    __RegisterMatchingBus(&group, 0x1111, &deviceId);
    CHECK(DmPublicationFinish(&group) == OS_EOK);
    CHECK(DmPublicationPrepareDma(&group) == OS_EOK);
    g_failSpawn = 1;
    CHECK(DmPublicationEnableBinding(&group) == OS_EOK);
    spawns = g_spawns;
    deliveries = g_deliveries;
    DmDeviceRefreshDrivers();
    DmDeviceRefreshDrivers();
    __Notify(failed, GRACHT_SECURITY_KERNEL, driver->Principal, 700);
    CHECK(g_spawns == spawns && g_deliveries == deliveries);
    CHECK(failed->Process == UUID_INVALID && failed->Principal == 0);
    CHECK(failed->State == DmDriverState_LOADING && failed->Handle == UUID_INVALID);
    CHECK(DmDeviceFindLocked(deviceId)->DriverPrincipal == 0);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, "__SpawnDriver: driver 3 remains unavailable; spawn attempt requires cleanup"));
    CHECK(DmPublicationRemove(&group) == OS_EOK);

    while (g_drivers.head != NULL) {
        driver = g_drivers.head->value;
        CHECK(driver->Devices.count == 0);
        list_remove(&g_drivers, &driver->ListHeader);
        __DestroyDriver(driver);
    }
    CHECK(g_devices.count == 0 && g_depth == 0);
    puts("Driver identity: authenticated readiness, publication ordering, duplicates and lost replies passed");
    return 0;
}
