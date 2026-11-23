/*
 * Devices that work as GPIO and clock controllers: firmware decoding, the
 * enable and disable rules, the clock owner, forwarding to a fake controller
 * driver, the internal GPIO line interface, and holding devices back until
 * their dependencies have owners.
 */

#include <ddk/devices/controller.h>

static struct PlatformEntry* __FindCompatible(struct PlatformBus* bus, const char* compatible);
static unsigned int g_lockCount;

// Controller keys of the manifest of every driver in this test, as the fake
// DmDiscoverControllerManifest in platform_bus_test.c returns them. A case
// sets them before it enables a controller. g_manifestStatus fails the lookup.
static struct DriverControllerManifest g_manifest;
static oserr_t                         g_manifestStatus;
static unsigned int                    g_manifestLookups;

enum __FakeControllerMode {
    FakeControllerAnswers,
    FakeControllerSilent,
    FakeControllerSendFails,
    FakeControllerTransportError
};

// One fake controller driver. Each request is answered at once unless Mode
// says otherwise. Result is the driver's answer for every request.
static struct {
    enum __FakeControllerMode Mode;
    oserr_t                   Result;
    int                       Clocks[32];
    uint64_t                  Rate;
    uint32_t                  Lines[64];
    uint32_t                  Directions[64];
    unsigned int              Requests;
    unsigned int              GpioRequests;
    unsigned int              SetEnabled;
    unsigned int              Abandoned;
    uint32_t                  LastClock;
    uuid_t                    LastDevice;
    int                       PendingEnabled;
    uint32_t                  PendingValue;
} g_fakeController;

static struct timespec g_fakeNow = { .tv_sec = 1000 };
static unsigned int    g_fakeSleeps;

static struct {
    usched_task_fn Entry;
    void*          Argument;
} g_controllerJobs[16];
static unsigned int g_controllerJobCount;

static uint64_t     g_ioctlPrincipal = 1;
static unsigned int g_ioctls;
static void         (*g_ioctlAnswer)(enum OSIOCtlRequest request, void* buffer, size_t length);

gracht_client_t*
GetGrachtClient(void)
{
    return (gracht_client_t*)&g_fakeController;
}

int
gracht_client_get_status(gracht_client_t* client, struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    if (g_fakeController.Mode == FakeControllerSilent) {
        return GRACHT_MESSAGE_INPROGRESS;
    }
    if (g_fakeController.Mode == FakeControllerTransportError) {
        return GRACHT_MESSAGE_ERROR;
    }
    return GRACHT_MESSAGE_COMPLETED;
}

int
gracht_client_abandon(gracht_client_t* client, struct gracht_message_context* context)
{
    (void)client;
    (void)context;
    g_fakeController.Abandoned++;
    return 0;
}

int
__TestTimespecGet(struct timespec* time, int base)
{
    CHECK(base == TIME_UTC);
    *time = g_fakeNow;
    return TIME_UTC;
}

int
__TestJobSleep(const struct timespec* until)
{
    // A wait without a deadline would sleep forever. Fail instead of hanging.
    CHECK(++g_fakeSleeps < 10000);
    CHECK(until->tv_sec > g_fakeNow.tv_sec ||
          (until->tv_sec == g_fakeNow.tv_sec && until->tv_nsec > g_fakeNow.tv_nsec));
    g_fakeNow = *until;
    return 0;
}

static uint64_t
__FakeElapsedMs(const struct timespec* start)
{
    return (uint64_t)(g_fakeNow.tv_sec - start->tv_sec) * 1000 +
           (uint64_t)((g_fakeNow.tv_nsec - start->tv_nsec) / 1000000);
}

static int
__FakeSend(uuid_t deviceId)
{
    // Requests are only sent with the bus lock held.
    CHECK(g_lockCount >= 1);
    g_fakeController.Requests++;
    g_fakeController.LastDevice = deviceId;
    return g_fakeController.Mode == FakeControllerSendFails ? -1 : 0;
}

static int
__FakeApplies(void)
{
    return g_fakeController.Mode == FakeControllerAnswers && g_fakeController.Result == OS_EOK;
}

int
ctt_clock_get_enabled(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t clock)
{
    (void)client;
    (void)context;
    CHECK(clock < 32);
    g_fakeController.LastClock = clock;
    g_fakeController.PendingEnabled = g_fakeController.Clocks[clock];
    return __FakeSend(deviceId);
}

int
ctt_clock_get_enabled_result(gracht_client_t* client, struct gracht_message_context* context,
    oserr_t* result, uint8_t* enabled)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    *enabled = (uint8_t)g_fakeController.PendingEnabled;
    return 0;
}

int
ctt_clock_set_enabled(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t clock, const uint8_t enabled)
{
    (void)client;
    (void)context;
    CHECK(clock < 32 && enabled <= 1);
    g_fakeController.LastClock = clock;
    g_fakeController.SetEnabled++;
    if (__FakeSend(deviceId) != 0) {
        return -1;
    }
    if (__FakeApplies()) {
        g_fakeController.Clocks[clock] = enabled;
    }
    return 0;
}

int
ctt_clock_set_enabled_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* result)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    return 0;
}

int
ctt_clock_get_rate(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t clock)
{
    (void)client;
    (void)context;
    g_fakeController.LastClock = clock;
    return __FakeSend(deviceId);
}

int
ctt_clock_get_rate_result(gracht_client_t* client, struct gracht_message_context* context,
    oserr_t* result, uint64_t* rate)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    *rate = g_fakeController.Rate;
    return 0;
}

int
ctt_gpio_set_direction(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t line, const enum ctt_gpio_direction direction, const uint32_t value)
{
    (void)client;
    (void)context;
    CHECK(line < 64);
    g_fakeController.GpioRequests++;
    if (__FakeSend(deviceId) != 0) {
        return -1;
    }
    if (__FakeApplies()) {
        g_fakeController.Directions[line] = direction;
        if (direction == CTT_GPIO_DIRECTION_OUTPUT) {
            g_fakeController.Lines[line] = value;
        }
    }
    return 0;
}

int
ctt_gpio_set_direction_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* result)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    return 0;
}

int
ctt_gpio_set_value(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t line, const uint32_t value)
{
    (void)client;
    (void)context;
    CHECK(line < 64);
    g_fakeController.GpioRequests++;
    if (__FakeSend(deviceId) != 0) {
        return -1;
    }
    if (__FakeApplies()) {
        g_fakeController.Lines[line] = value;
    }
    return 0;
}

int
ctt_gpio_set_value_result(gracht_client_t* client, struct gracht_message_context* context, oserr_t* result)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    return 0;
}

int
ctt_gpio_get_value(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t deviceId, const uint32_t line)
{
    (void)client;
    (void)context;
    CHECK(line < 64);
    g_fakeController.GpioRequests++;
    g_fakeController.PendingValue = g_fakeController.Lines[line];
    return __FakeSend(deviceId);
}

int
ctt_gpio_get_value_result(gracht_client_t* client, struct gracht_message_context* context,
    oserr_t* result, uint32_t* value)
{
    (void)client;
    (void)context;
    *result = g_fakeController.Result;
    *value = g_fakeController.PendingValue;
    return 0;
}

uuid_t
usched_job_queue(usched_task_fn entry, void* argument)
{
    CHECK(g_controllerJobCount < 16);
    g_controllerJobs[g_controllerJobCount].Entry = entry;
    g_controllerJobs[g_controllerJobCount].Argument = argument;
    g_controllerJobCount++;
    return g_controllerJobCount;
}

static void
__ControllerJobsReset(void)
{
    g_controllerJobCount = 0;
}

static void
__ControllerJobsRun(void)
{
    unsigned int count = g_controllerJobCount;

    // Jobs run later on their own, never under the bus or DMA lock.
    CHECK(g_lockCount == 0);
    g_controllerJobCount = 0;
    for (unsigned int i = 0; i < count; i++) {
        g_controllerJobs[i].Entry(g_controllerJobs[i].Argument, NULL);
    }
}

// The libddk wrappers reach deviced through this call. Route it straight into
// the registry, so the wrappers are tested against the real request path.
oserr_t
OSDeviceIOCtl(uuid_t device, enum OSIOCtlRequest request, void* buffer, size_t length)
{
    oserr_t status;

    g_ioctls++;
    status = DmHandleIoctl(g_ioctlPrincipal, device, request, buffer, length);

    // Lets a test damage an answer on its way back to a libddk wrapper.
    if (status == OS_EOK && g_ioctlAnswer != NULL) {
        g_ioctlAnswer(request, buffer, length);
    }
    return status;
}

struct __NamedNode {
    const char* Name;
    uint32_t    Node;
};

static void
__FindNamed(const struct FdtResources* nodes, int depth, void* context)
{
    struct __NamedNode* search = context;

    if (!strcmp(nodes[depth].Name, search->Name)) {
        CHECK(search->Node == UINT32_MAX);
        search->Node = nodes[depth].NodeOffset;
    }
}

static uint32_t
__NodeNamed(struct PlatformBus* bus, const char* name)
{
    struct __NamedNode search = { .Name = name, .Node = UINT32_MAX };

    CHECK(FdtWalkResources(bus->Tree, __FindNamed, &search) == OS_EOK);
    CHECK(search.Node != UINT32_MAX);
    return search.Node;
}

static struct PlatformEntry*
__EntryNamed(struct PlatformBus* bus, const char* name)
{
    struct PlatformEntry* entry;

    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        if (!strcmp(entry->Firmware.Base.Identification.Description, name)) {
            return entry;
        }
    }
    CHECK(0);
    return NULL;
}

static uint32_t
__Phandle(struct PlatformBus* bus, uint32_t phandle)
{
    struct FdtResources resources;

    CHECK(FdtFindResources(bus->Tree, phandle, &resources) == OS_EOK);
    return resources.NodeOffset;
}

static oserr_t
__Stage(struct PlatformEntry* entry, enum DeviceStage stage)
{
    return DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE, &stage, sizeof(stage));
}

static oserr_t
__ControllerRequest(struct PlatformEntry* entry, enum OSIOCtlRequest request, uint32_t kinds)
{
    struct DeviceControllerRequest body = { .Kinds = kinds };

    return DmHandleIoctl(1, entry->DeviceId, request, &body, sizeof(body));
}

static oserr_t
__SupplyAcquire(void* context, const struct FdtPlatformDependency* dependency, void** token)
{
    (void)context;
    CHECK(dependency->Kind == FdtPlatformSupply);
    *token = (void*)(uintptr_t)(dependency->NodeOffset + 1);
    return OS_EOK;
}

static oserr_t
__SupplyRelease(void* context, void* token)
{
    (void)context;
    CHECK(token != NULL);
    return OS_EOK;
}

static const struct PlatformDependencyOperations g_testSupplyOperations = {
    .Acquire = __SupplyAcquire, .Release = __SupplyRelease
};

// Every platform device that a driver matches today must reach its driver as
// before. GENET is the only one on the Pi 4. The Pi 5 xHCI and Ethernet
// controllers are RP1 children and are not on this bus.
static void
__ControllerMatchedDevices(struct PlatformBus* bus)
{
    static const char* const matched[] = { "brcm,bcm2711-genet-v5", "generic-xhci", "snps,dwc3" };
    struct PlatformEntry*    entry;

    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        for (unsigned int i = 0; i < sizeof(matched) / sizeof(matched[0]); i++) {
            if (FdtStringListContains((const uint8_t*)entry->Firmware.Compatibles,
                entry->Firmware.CompatibleLength, matched[i])) {
                CHECK(!entry->Waiting && DmDeviceIsBindable(entry->DeviceId));
            }
        }
    }
}

static void
__ControllerDecoding(struct PlatformBus* bus)
{
    struct FdtControllerNodes nodes;
    struct FdtControllerNodes saved;
    struct FdtGpioLine        line;
    struct FdtGpioLine        savedLine;
    uint32_t                  mailbox = __EntryNamed(bus, "mailbox@2000")->Firmware.FirmwareNode;
    uint32_t                  other = __EntryNamed(bus, "mailbox@2100")->Firmware.FirmwareNode;
    uint32_t                  shared = __EntryNamed(bus, "mailbox@2200")->Firmware.FirmwareNode;
    uint32_t                  own = __Phandle(bus, 206);
    uint32_t                  ownClock = __Phandle(bus, 212);
    uint32_t                  lines = __NodeNamed(bus, "lines");

    // The expander and clocks behind the mailbox the client node names. No
    // vendor compatible is involved.
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK,
        NULL, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == __Phandle(bus, 203) && nodes.ClockNode == __Phandle(bus, 204));
    CHECK(nodes.RouteCount == 0);
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, NULL, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == FDT_CONTROLLER_NONE && nodes.ClockNode == __Phandle(bus, 204));

    // The other client node names another mailbox. Its expander is disabled
    // and it has no clocks, so neither kind is found. Outputs stay unchanged.
    memset(&nodes, 0xa5, sizeof(nodes));
    saved = nodes;
    CHECK(FdtFindControllerNodes(bus->Tree, other, DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_ENOENT);
    CHECK(FdtFindControllerNodes(bus->Tree, other, DEVICE_CONTROLLER_CLOCK, NULL, &nodes) == OS_ENOENT);
    CHECK(!memcmp(&nodes, &saved, sizeof(nodes)));

    // Two client nodes of one mailbox with a clock child each, and two GPIO
    // controllers below the first one: both kinds are ambiguous.
    CHECK(FdtFindControllerNodes(bus->Tree, shared, DEVICE_CONTROLLER_CLOCK, NULL, &nodes) == OS_ENOTSUPPORTED);
    CHECK(FdtFindControllerNodes(bus->Tree, shared, DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_ENOTSUPPORTED);
    CHECK(!memcmp(&nodes, &saved, sizeof(nodes)));

    // A GPIO controller that is the device itself.
    CHECK(FdtFindControllerNodes(bus->Tree, own, DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == own && nodes.ClockNode == FDT_CONTROLLER_NONE);
    CHECK(FdtFindControllerNodes(bus->Tree, own, DEVICE_CONTROLLER_CLOCK, NULL, &nodes) == OS_ENOENT);

    // A clock controller that is the device itself.
    CHECK(FdtFindControllerNodes(bus->Tree, ownClock, DEVICE_CONTROLLER_CLOCK, NULL, &nodes) == OS_EOK);
    CHECK(nodes.ClockNode == ownClock && nodes.GpioNode == FDT_CONTROLLER_NONE && nodes.RouteCount == 0);
    CHECK(FdtFindControllerNodes(bus->Tree, ownClock, DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_ENOENT);

    // Malformed and unsupported #gpio-cells, and bad kind masks.
    saved = nodes;
    CHECK(FdtFindControllerNodes(bus->Tree, __Phandle(bus, 207), DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_EINVALPARAMS);
    CHECK(FdtFindControllerNodes(bus->Tree, __Phandle(bus, 208), DEVICE_CONTROLLER_GPIO, NULL, &nodes) == OS_ENOTSUPPORTED);
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, 0, NULL, &nodes) == OS_EINVALPARAMS);
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, 4, NULL, &nodes) == OS_EINVALPARAMS);
    CHECK(!memcmp(&nodes, &saved, sizeof(nodes)));

    // Line references: phandle, line, flags. Entries can name different controllers.
    CHECK(FdtGpioReference(bus->Tree, lines, "enable-gpios", 0, &line) == OS_EOK);
    CHECK(line.Controller == __Phandle(bus, 203) && line.Line == 4 && line.Flags == FDT_GPIO_ACTIVE_LOW);
    CHECK(FdtGpioReference(bus->Tree, lines, "gpios", 0, &line) == OS_EOK);
    CHECK(line.Controller == own && line.Line == 1 && line.Flags == 0);
    CHECK(FdtGpioReference(bus->Tree, lines, "gpios", 1, &line) == OS_EOK);
    CHECK(line.Controller == __Phandle(bus, 203) && line.Line == 6 && line.Flags == 0);
    savedLine = line;
    CHECK(FdtGpioReference(bus->Tree, lines, "gpios", 2, &line) == OS_ENOENT);
    CHECK(FdtGpioReference(bus->Tree, lines, "bad-gpios", 0, &line) == OS_EINVALPARAMS);
    CHECK(FdtGpioReference(bus->Tree, lines, "three-gpios", 0, &line) == OS_ENOTSUPPORTED);
    CHECK(FdtGpioReference(bus->Tree, lines, "reset-gpios", 0, &line) == OS_ENOENT);
    CHECK(FdtGpioReference(bus->Tree, lines, "clocks", 0, &line) == OS_EINVALPARAMS);
    CHECK(!memcmp(&line, &savedLine, sizeof(line)));
}

// Controller rules from a driver manifest. Controller compatibles pick the
// children of client nodes that the driver serves. A clock route applies to
// the one enabled node with its compatible and a single clock cell. A route
// without such a node is skipped, two such nodes are ambiguous, and both
// lists are bounded.
static void
__ControllerRuleDecoding(struct PlatformBus* bus)
{
    const char*               gpioOnly[] = { "test,gpio-expander" };
    const char*               served[] = { "test,gpio-expander", "test,clocks-a" };
    const char*               tooMany[FDT_CONTROLLER_MAX_COMPATIBLES + 1];
    struct FdtClockRouteRule  routes[FDT_CONTROLLER_MAX_ROUTES + 1];
    struct FdtControllerRules rules = { 0 };
    struct FdtControllerNodes nodes;
    struct FdtControllerNodes saved;
    uint32_t                  mailbox = __EntryNamed(bus, "mailbox@2000")->Firmware.FirmwareNode;
    uint32_t                  other = __EntryNamed(bus, "mailbox@2100")->Firmware.FirmwareNode;
    uint32_t                  shared = __EntryNamed(bus, "mailbox@2200")->Firmware.FirmwareNode;
    uint32_t                  target = __NodeNamed(bus, "route-a");

    // Compatibles pick one GPIO controller among two, and one clock child
    // among the children of two client nodes.
    rules.Compatibles = gpioOnly;
    rules.CompatibleCount = 1;
    CHECK(FdtFindControllerNodes(bus->Tree, shared, DEVICE_CONTROLLER_GPIO, &rules, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == __NodeNamed(bus, "gpio-a"));
    CHECK(FdtFindControllerNodes(bus->Tree, shared, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_ENOENT);
    rules.Compatibles = served;
    rules.CompatibleCount = 2;
    CHECK(FdtFindControllerNodes(bus->Tree, shared, DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK,
        &rules, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == __NodeNamed(bus, "gpio-a"));
    CHECK(!strcmp(FdtTreeNode(bus->Tree, nodes.ClockNode)->Name, "clocks") &&
          FdtCompatible(&FdtTreeNode(bus->Tree, nodes.ClockNode)->View, "test,clocks-a"));

    // The device's own node does not need to be named.
    CHECK(FdtFindControllerNodes(bus->Tree, __Phandle(bus, 206), DEVICE_CONTROLLER_GPIO, &rules, &nodes) == OS_EOK);
    CHECK(nodes.GpioNode == __Phandle(bus, 206));

    rules = (struct FdtControllerRules) { .Routes = routes, .RouteCount = 4 };
    routes[0] = (struct FdtClockRouteRule) { "test,route-target", 51, 12 };
    routes[1] = (struct FdtClockRouteRule) { "test,route-missing", 1, 2 };
    routes[2] = (struct FdtClockRouteRule) { "test,route-wide", 3, 4 };
    routes[3] = (struct FdtClockRouteRule) { "test,route-disabled", 5, 6 };
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_EOK);
    CHECK(nodes.ClockNode == __Phandle(bus, 204) && nodes.RouteCount == 1);
    CHECK(nodes.Routes[0].Node == target && nodes.Routes[0].SourceClock == 51);
    CHECK(nodes.Routes[0].ControllerClock == 12);

    // GPIO alone resolves no routes.
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_GPIO, &rules, &nodes) == OS_EOK);
    CHECK(nodes.RouteCount == 0);

    // A device whose only clocks are routes is still a clock controller.
    rules.RouteCount = 1;
    CHECK(FdtFindControllerNodes(bus->Tree, other, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_EOK);
    CHECK(nodes.ClockNode == FDT_CONTROLLER_NONE && nodes.RouteCount == 1 && nodes.Routes[0].Node == target);
    rules.Routes = &routes[1];
    rules.RouteCount = 3;
    CHECK(FdtFindControllerNodes(bus->Tree, other, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_ENOENT);

    // Ambiguous routes, oversized lists and missing lists change nothing.
    saved = nodes;
    routes[1] = (struct FdtClockRouteRule) { "test,route-double", 1, 2 };
    rules = (struct FdtControllerRules) { .Routes = routes, .RouteCount = 2 };
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_ENOTSUPPORTED);
    for (uint32_t i = 0; i < FDT_CONTROLLER_MAX_ROUTES + 1; i++) {
        routes[i] = (struct FdtClockRouteRule) { "test,route-target", i, i };
    }
    rules.RouteCount = FDT_CONTROLLER_MAX_ROUTES + 1;
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_ENOTSUPPORTED);
    for (uint32_t i = 0; i < FDT_CONTROLLER_MAX_COMPATIBLES + 1; i++) {
        tooMany[i] = "test,clocks";
    }
    rules = (struct FdtControllerRules) { .Compatibles = tooMany, .CompatibleCount = FDT_CONTROLLER_MAX_COMPATIBLES + 1 };
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_ENOTSUPPORTED);
    rules = (struct FdtControllerRules) { .CompatibleCount = 1 };
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_EINVALPARAMS);
    rules = (struct FdtControllerRules) { .RouteCount = 1 };
    CHECK(FdtFindControllerNodes(bus->Tree, mailbox, DEVICE_CONTROLLER_CLOCK, &rules, &nodes) == OS_EINVALPARAMS);
    CHECK(!memcmp(&nodes, &saved, sizeof(nodes)));
}

static void
__ControllerRules(struct PlatformBus* bus, struct PlatformEntry* mailbox,
    struct PlatformEntry* held, struct PlatformEntry* never)
{
    struct DeviceControllerRequest body = { .Kinds = DEVICE_CONTROLLER_GPIO };
    unsigned int                   ioctls = g_ioctls;
    unsigned int                   lookups;

    // Devices wait for their missing owners. Devices without any stay bindable.
    CHECK(held->Waiting && !DmDeviceIsBindable(held->DeviceId));
    CHECK(never->Waiting && !DmDeviceIsBindable(never->DeviceId));
    CHECK(!mailbox->Waiting && !__FindCompatible(bus, "test,device")->Waiting);

    // The wrappers reject bad masks without a request.
    CHECK(DeviceControllerEnable(mailbox->DeviceId, 0) == OS_EINVALPARAMS);
    CHECK(DeviceControllerEnable(mailbox->DeviceId, 4) == OS_EINVALPARAMS);
    CHECK(DeviceControllerDisable(mailbox->DeviceId, 0) == OS_EINVALPARAMS);
    CHECK(g_ioctls == ioctls);

    // Only the assigned driver, only after DevicePrepare, only valid requests.
    g_ioctlPrincipal = 2;
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EPERMISSIONS);
    g_ioctlPrincipal = 1;
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EBUSY);
    CHECK(DmHandleIoctl(1, mailbox->DeviceId, OSIOCTLREQUEST_CONTROLLER_ENABLE, &body, 8) == OS_EINVALPARAMS);
    body.Kinds = 8;
    CHECK(DmHandleIoctl(1, mailbox->DeviceId, OSIOCTLREQUEST_CONTROLLER_ENABLE, &body, sizeof(body)) == OS_EINVALPARAMS);
    CHECK(__Stage(mailbox, DevicePrepare) == OS_EOK);
    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_ENOENT);
    CHECK(mailbox->Controller == NULL);

    // The controller rules come from the manifest of the driver the device
    // record names. A failed lookup fails the enable before anything changes.
    memset(&g_manifest, 0, sizeof(g_manifest));
    g_manifestStatus = OS_ENOENT;
    lookups = g_manifestLookups;
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_ENOENT);
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_ENOENT);
    CHECK(mailbox->Controller == NULL && g_manifestLookups == lookups + 2);
    g_manifestStatus = OS_EOK;

    // Enabling hands the waiting device to its driver from a job. The device
    // whose reset controller never gets an owner keeps waiting.
    __ControllerJobsReset();
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(!held->Waiting && g_controllerJobCount == 1 && !DmDeviceIsBindable(held->DeviceId));
    __ControllerJobsRun();
    CHECK(DmDeviceIsBindable(held->DeviceId));
    CHECK(never->Waiting && !DmDeviceIsBindable(never->DeviceId));

    // Enabling twice fails. The controller's device cannot stop while enabled.
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_EEXISTS);
    CHECK(__Stage(mailbox, DeviceStop) == OS_EBUSY);
}

static void
__ControllerClocks(struct PlatformBus* bus, struct PlatformEntry* mailbox, struct PlatformEntry* held)
{
    struct timespec start;
    uint64_t        rate = 0;

    // Clock found off: deviced starts it and stops it again on release.
    g_fakeController.Clocks[7] = 0;
    g_fakeController.SetEnabled = 0;
    CHECK(__Stage(held, DevicePrepare) == OS_EOK);
    CHECK(g_fakeController.SetEnabled == 1 && g_fakeController.Clocks[7] == 1);
    CHECK(g_fakeController.LastDevice == mailbox->DeviceId && g_fakeController.LastClock == 7);
    g_fakeController.Rate = 54000000;
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformDependencyClockRate(held, 1, 0, &rate) == OS_EOK && rate == 54000000);
    usched_mtx_unlock(&bus->Lock);

    // Disabling fails while the clock is in use, and leaves GPIO enabled too.
    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_EBUSY);
    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EBUSY);
    CHECK(PlatformGpioIsReady(bus, __Phandle(bus, 203)));
    CHECK(__Stage(held, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.SetEnabled == 2 && g_fakeController.Clocks[7] == 0);

    // Clock found on: deviced neither starts nor stops it.
    g_fakeController.Clocks[7] = 1;
    g_fakeController.SetEnabled = 0;
    CHECK(__Stage(held, DevicePrepare) == OS_EOK);
    CHECK(__Stage(held, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.SetEnabled == 0 && g_fakeController.Clocks[7] == 1);

    // The driver's error reaches the device that is being prepared.
    g_fakeController.Clocks[7] = 0;
    g_fakeController.Result = OS_EDEVFAULT;
    CHECK(__Stage(held, DevicePrepare) == OS_EDEVFAULT && held->Activation == NULL);
    g_fakeController.Result = OS_EOK;

    // A driver that never answers fails the request at the deadline. The
    // request is abandoned and the controller stays enabled.
    g_fakeController.Mode = FakeControllerSilent;
    g_fakeController.Abandoned = 0;
    start = g_fakeNow;
    CHECK(__Stage(held, DevicePrepare) == OS_ETIMEOUT && held->Activation == NULL);
    CHECK(__FakeElapsedMs(&start) == PLATFORM_CONTROLLER_CALL_DEADLINE_MS);
    CHECK(g_fakeController.Abandoned == 1 && mailbox->Controller != NULL);
    g_fakeController.Mode = FakeControllerAnswers;
}

static void
__ControllerGpio(struct PlatformBus* bus, struct PlatformEntry* mailbox)
{
    uint32_t expander = __Phandle(bus, 203);
    uint32_t value = 7;

    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformGpioSetDirection(bus, expander, 4, PLATFORM_GPIO_OUTPUT, 1) == OS_EOK);
    CHECK(g_fakeController.Directions[4] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[4] == 1);
    CHECK(PlatformGpioSetValue(bus, expander, 4, 0) == OS_EOK && g_fakeController.Lines[4] == 0);
    CHECK(PlatformGpioGetValue(bus, expander, 4, &value) == OS_EOK && value == 0);
    CHECK(PlatformGpioSetValue(bus, expander, 4, 2) == OS_EINVALPARAMS);
    CHECK(PlatformGpioSetDirection(bus, expander, 4, PLATFORM_GPIO_INPUT, 1) == OS_EINVALPARAMS);
    CHECK(PlatformGpioSetDirection(bus, expander, 4, 3, 0) == OS_EINVALPARAMS);
    CHECK(PlatformGpioSetValue(bus, __Phandle(bus, 206), 1, 1) == OS_ENOENT);
    g_fakeController.Result = OS_EDEVFAULT;
    CHECK(PlatformGpioGetValue(bus, expander, 4, &value) == OS_EDEVFAULT);
    g_fakeController.Result = OS_EOK;
    CHECK(PlatformGpioAddUser(bus, expander) == OS_EOK);
    usched_mtx_unlock(&bus->Lock);

    // A GPIO user blocks disabling GPIO until it is gone.
    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EBUSY);
    usched_mtx_lock(&bus->Lock);
    PlatformGpioRemoveUser(bus, expander);
    usched_mtx_unlock(&bus->Lock);
    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EOK);
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformGpioSetValue(bus, expander, 4, 1) == OS_ENOENT);
    usched_mtx_unlock(&bus->Lock);
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EOK);
}

static void
__ControllerGone(struct PlatformBus* bus, struct PlatformEntry* mailbox, struct PlatformEntry* held)
{
    uint32_t     expander = __Phandle(bus, 203);
    uint32_t     value;
    unsigned int requests;

    // A request that cannot be sent means the driver is gone. The controller
    // ends, and later requests fail inside deviced without being sent.
    g_fakeController.Mode = FakeControllerSendFails;
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformGpioGetValue(bus, expander, 4, &value) == OS_ECONNABORTED);
    CHECK(!PlatformGpioIsReady(bus, expander));
    requests = g_fakeController.Requests;
    CHECK(PlatformGpioSetValue(bus, expander, 4, 1) == OS_ENOTCONNECTED);
    CHECK(PlatformGpioAddUser(bus, expander) == OS_ENOTCONNECTED);
    CHECK(g_fakeController.Requests == requests);
    usched_mtx_unlock(&bus->Lock);

    // The ended clock owner no longer counts, and the record is freed by the
    // next request because nothing uses it.
    CHECK(__Stage(held, DevicePrepare) == OS_ENOTSUPPORTED);
    CHECK(mailbox->Controller == NULL);
    g_fakeController.Mode = FakeControllerTransportError;
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(__Stage(held, DevicePrepare) == OS_ECONNABORTED && mailbox->Controller == NULL);

    // Enabling again replaces the ended record.
    g_fakeController.Mode = FakeControllerAnswers;
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);

    // The device going away ends its controller. While a consumer still holds
    // a clock, the record stays, blocks a new enable, and is freed on release
    // without another request to the gone driver.
    g_fakeController.Clocks[7] = 0;
    CHECK(__Stage(held, DevicePrepare) == OS_EOK && g_fakeController.Clocks[7] == 1);
    usched_mtx_lock(&bus->Lock);
    PlatformControllerEndLocked(mailbox);
    usched_mtx_unlock(&bus->Lock);
    CHECK(mailbox->Controller != NULL);
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EBUSY);
    requests = g_fakeController.Requests;
    CHECK(__Stage(held, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.Requests == requests && mailbox->Controller == NULL);
    usched_mtx_lock(&bus->Lock);
    PlatformControllerEndLocked(mailbox);
    usched_mtx_unlock(&bus->Lock);
    CHECK(__Stage(mailbox, DeviceStop) == OS_EOK);
}

static void
__ControllerSynthetic(struct PlatformBus* bus)
{
    struct PlatformEntry* mailbox = __EntryNamed(bus, "mailbox@2000");
    struct PlatformEntry* held = __FindCompatible(bus, "test,held");
    struct PlatformEntry* never = __FindCompatible(bus, "test,never");

    memset(&g_fakeController, 0, sizeof(g_fakeController));
    __ControllerDecoding(bus);
    __ControllerRuleDecoding(bus);
    __ControllerRules(bus, mailbox, held, never);
    __ControllerClocks(bus, mailbox, held);
    __ControllerGpio(bus, mailbox);
    __ControllerGone(bus, mailbox, held);
    CHECK(mailbox->Controller == NULL && held->Activation == NULL && mailbox->Activation == NULL);
}
