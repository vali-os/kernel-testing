// Run real usbd handlers on deterministic cooperative fibers. Every simulated
// USB RPC yields so another topology event can race the suspended enumeration.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static void* __TestAllocate(size_t size);
static void* __TestCalloc(size_t count, size_t size);
static char* __TestDuplicate(const char* value);
static void __TestFree(void* pointer);
#define malloc __TestAllocate
#define calloc __TestCalloc
#define strdup __TestDuplicate
#define free __TestFree
#include "../services/usbd/controllers.c"
#include "../services/usbd/hubs.c"
#include "../services/usbd/port_event.c"
#include "../services/usbd/devices.c"
#include "../services/usbd/topology.c"
#include "../librt/libds/list.c"
#include "../librt/libds/hashtable.c"
#undef malloc
#undef calloc
#undef strdup
#undef free

static void* g_allocations[512];
static int g_liveAllocations, g_failAllocationAfter = -1;
static void* __TestAllocate(size_t size)
{
    void* result;
    int i;
    if (g_failAllocationAfter == 0) return NULL;
    if (g_failAllocationAfter > 0) g_failAllocationAfter--;
    result = malloc(size);
    CHECK(result != NULL);
    for (i = 0; i < 512; i++) if (!g_allocations[i]) {
        g_allocations[i] = result; g_liveAllocations++; return result;
    }
    abort();
}
static void* __TestCalloc(size_t count, size_t size)
{
    void* result = __TestAllocate(count * size);
    if (result) memset(result, 0, count * size);
    return result;
}
static char* __TestDuplicate(const char* value)
{
    char* result = __TestAllocate(strlen(value) + 1);
    if (result) strcpy(result, value);
    return result;
}
static void __TestFree(void* pointer)
{
    int i;
    if (!pointer) return;
    for (i = 0; i < 512; i++) if (g_allocations[i] == pointer) {
        g_allocations[i] = NULL; g_liveAllocations--; break;
    }
    free(pointer);
}


__declspec(dllimport) void* __stdcall ConvertThreadToFiber(void*);
__declspec(dllimport) void* __stdcall CreateFiber(size_t, void (__stdcall*)(void*), void*);
__declspec(dllimport) void __stdcall SwitchToFiber(void*);
__declspec(dllimport) void __stdcall DeleteFiber(void*);

struct TestJob {
    void* Fiber;
    struct usched_cnd* Waiting;
    int Kind;
    int Port;
    int Done;
};
static struct TestJob g_jobs[5];
static usched_task_fn g_workerEntry;
static void* g_workerArgument;
static int g_failQueue, g_failJoin, g_usbCleanups, g_initFailure;
static int g_counts[4], g_snapshotIds[4], g_order[32], g_orderCount;
static oserr_t g_stopStatus;
static int g_stopAfterReset, g_lateAdmission;
static int g_checkWorkerStop;
static int g_spuriousCompletion;

static void* g_main;
static int g_current = -1, g_owner = -1, g_waiters;
static USBPortDescriptor_t g_ports[4];
static int g_zeroPort = -1, g_resets, g_disables, g_detaches, g_registered;
static int g_failInitial, g_failAddress, g_failDisable, g_failConfig, g_failRegister;
static int g_failReset, g_disabledReset;
static long g_sleepDuration;
static int g_errno;
int* __errno(void) { return &g_errno; }
int timespec_get(struct timespec* time, int base)
{ memset(time, 0, sizeof(*time)); return base; }
void spinlock_init(spinlock_t* lock) { memset(lock, 0, sizeof(*lock)); }
void spinlock_acquire(spinlock_t* lock) { CHECK(lock->next == 0); lock->next = 1; }
void spinlock_release(spinlock_t* lock) { CHECK(lock->next == 1); lock->next = 0; }

static void __Yield(void)
{
    // The worker must never hold queue synchronization across a USB RPC.
    if (g_current == 4) CHECK(g_owner == -1);
    if (g_current >= 0) SwitchToFiber(g_main);
}
void usched_mtx_init(struct usched_mtx* mutex, int type)
{ (void)mutex; (void)type; g_owner = -1; }
void usched_mtx_lock(struct usched_mtx* mutex)
{
    CHECK(mutex == &g_topology.Mutex);
    while (g_owner != -1) { g_waiters++; __Yield(); }
    g_owner = g_current;
}
void usched_mtx_unlock(struct usched_mtx* mutex)
{ CHECK(mutex == &g_topology.Mutex && g_owner == g_current); g_owner = -1; }
void timespec_add(const struct timespec* a, const struct timespec* b, struct timespec* result)
{ *result = *a; g_sleepDuration = b->tv_nsec; }
int usched_job_sleep(const struct timespec* until)
{
    (void)until;
    __Yield();
    if (g_sleepDuration == 10 * NSEC_PER_MSEC) g_zeroPort = -1;
    return 0;
}
void SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{ (void)level; (void)format; }
void* dsalloc(size_t size) { return malloc(size); }
void dsfree(void* ptr) { free(ptr); }
void dstrace(const char* format, ...) { (void)format; }
void mstr_delete(mstring_t* value) { CHECK(value == NULL); }
char* mstr_u8(mstring_t* value) { (void)value; abort(); }
oserr_t UsbInitialize(void) { return g_initFailure ? OS_EOOM : OS_EOK; }
void UsbCleanup(void) { g_usbCleanups++; }


oserr_t UsbHubQueryPort(uuid_t driver, uuid_t hub, uint8_t port, USBPortDescriptor_t* out)
{
    CHECK(driver == 73 && hub == 10 && port < 4);
    __Yield();
    *out = g_ports[port];
    return OS_EOK;
}
oserr_t UsbHubResetPort(uuid_t driver, uuid_t hub, uint8_t port, USBPortDescriptor_t* out)
{
    CHECK(driver == 73 && hub == 10 && port < 4);
    CHECK(g_zeroPort == -1 || g_zeroPort == port);
    g_zeroPort = port;
    if (g_checkWorkerStop) CHECK(UsbTopologyStop() == OS_EBUSY);
    g_resets++;
    if (g_resets == 1) g_order[g_orderCount++] = 1;
    g_ports[port].Enabled = 1;
    __Yield();
    if (port == 0) CHECK(!g_jobs[0].Done);
    if (g_disabledReset) { g_ports[port].Enabled = 0; g_zeroPort = -1; }
    *out = g_ports[port];
    return g_failReset == g_resets ? OS_ETIMEOUT : OS_EOK;
}
oserr_t UsbHubDisablePort(uuid_t driver, uuid_t hub, uint8_t port)
{
    CHECK(driver == 73 && hub == 10 && port < 4);
    g_disables++;
    __Yield();
    if (g_failDisable) return OS_ETIMEOUT;
    g_ports[port].Enabled = 0;
    g_zeroPort = -1;
    return OS_EOK;
}
enum USBTransferCode UsbExecutePacket(usb_device_context_t* device, uint8_t direction,
    uint8_t type, uint8_t low, uint8_t high, uint16_t index, uint16_t length, void* data)
{
    (void)direction; (void)type; (void)low; (void)high; (void)index;
    CHECK(length == 8 && g_zeroPort == device->port_address);
    __Yield();
    ((usb_device_descriptor_t*)data)->MaxPacketSize = 8;
    return g_failInitial ? USBTRANSFERCODE_STALL : USBTRANSFERCODE_SUCCESS;
}
enum USBTransferCode UsbSetAddress(usb_device_context_t* device, int address)
{
    CHECK(g_zeroPort == device->port_address && address != 0);
    __Yield();
    return g_failAddress ? USBTRANSFERCODE_STALL : USBTRANSFERCODE_SUCCESS;
}
enum USBTransferCode UsbGetDeviceDescriptor(usb_device_context_t* device, usb_device_descriptor_t* out)
{
    CHECK(device->device_address != 0 && g_zeroPort == -1);
    __Yield();
    memset(out, 0, sizeof(*out)); out->Class = 3; out->MaxPacketSize = 8;
    return USBTRANSFERCODE_SUCCESS;
}
enum USBTransferCode UsbGetStringDescriptor(usb_device_context_t* device, size_t language,
    size_t index, mstring_t** out)
{ (void)device; (void)language; (void)index; __Yield(); *out = NULL; return USBTRANSFERCODE_SUCCESS; }
enum USBTransferCode UsbGetActiveConfigDescriptor(usb_device_context_t* device, usb_device_configuration_t* out)
{ (void)device; __Yield(); memset(out, 0, sizeof(*out)); out->base.ConfigurationValue = 1; return USBTRANSFERCODE_SUCCESS; }
void UsbFreeConfigDescriptor(usb_device_configuration_t* config) { (void)config; }
enum USBTransferCode UsbSetConfiguration(usb_device_context_t* device, int config)
{ (void)device; CHECK(config == 1); __Yield(); return g_failConfig ? USBTRANSFERCODE_STALL : USBTRANSFERCODE_SUCCESS; }
uuid_t RegisterDevice(Device_t* device, unsigned int flags)
{ (void)device; (void)flags; __Yield(); g_registered++; g_order[g_orderCount++] = 2; return g_failRegister ? UUID_INVALID : 100 + g_registered; }
oserr_t UnregisterDevice(uuid_t device)
{ CHECK(device >= 100); __Yield(); return OS_EOK; }
void UsbDetachDevice(uuid_t controller, usb_device_context_t* device)
{ CHECK(controller == 10 && UsbCoreControllerGet(controller) != NULL); (void)device; __Yield(); g_detaches++; g_order[g_orderCount++] = 5; }

static void __stdcall __WorkerJob(void* argument)
{
    (void)argument;
    g_workerEntry(g_workerArgument, NULL);
    g_jobs[4].Done = 1;
    SwitchToFiber(g_main);
    abort();
}
uuid_t usched_job_queue(usched_task_fn entry, void* argument)
{
    if (g_failQueue) return UUID_INVALID;
    CHECK(g_jobs[4].Fiber == NULL);
    g_workerEntry = entry; g_workerArgument = argument;
    g_jobs[4].Done = 0; g_jobs[4].Waiting = NULL;
    g_jobs[4].Fiber = CreateFiber(0, __WorkerJob, NULL);
    CHECK(g_jobs[4].Fiber != NULL);
    return 5;
}
uuid_t usched_job_current(void) { return g_current + 1; }
int usched_job_join(uuid_t job, int* result)
{
    CHECK(job == 5 && g_owner == -1 && g_current != 4);
    if (g_failJoin) return -1;
    // A condition wake is not completion. Accepted handlers must keep waiting
    // even if shutdown has closed admission while their callback is suspended.
    if (g_spuriousCompletion) {
        CHECK(g_jobs[0].Waiting != NULL);
        usched_cnd_notify_one(g_jobs[0].Waiting);
        g_spuriousCompletion = 0;
    }
    while (!g_jobs[4].Done) __Yield();
    DeleteFiber(g_jobs[4].Fiber); g_jobs[4].Fiber = NULL;
    *result = 0;
    return 0;
}
void usched_cnd_init(struct usched_cnd* condition) { memset(condition, 0, sizeof(*condition)); }
void usched_cnd_wait(struct usched_cnd* condition, struct usched_mtx* mutex)
{
    CHECK(g_current >= 0);
    g_jobs[g_current].Waiting = condition;
    g_waiters++;
    usched_mtx_unlock(mutex);
    __Yield();
    CHECK(g_jobs[g_current].Waiting == NULL);
    usched_mtx_lock(mutex);
}
void usched_cnd_notify_one(struct usched_cnd* condition)
{
    int i;
    for (i = 0; i < 5; i++) {
        if (g_jobs[i].Waiting == condition) { g_jobs[i].Waiting = NULL; break; }
    }
}
int sys_usb_get_controller_count_response(struct gracht_message* message, const int count)
{
    struct TestJob* job = (struct TestJob*)message;
    CHECK(job >= g_jobs && job < g_jobs + 4);
    g_counts[job - g_jobs] = count;
    g_order[g_orderCount++] = 3;
    __Yield();
    CHECK(!job->Done);
    return 0;
}
int sys_usb_get_controller_response(struct gracht_message* message, uint8_t* data, const uint32_t length)
{
    struct TestJob* job = (struct TestJob*)message;
    const USBControllerDevice_t* snapshot = (const USBControllerDevice_t*)data;
    CHECK(job >= g_jobs && job < g_jobs + 4 && length == sizeof(*snapshot));
    g_snapshotIds[job - g_jobs] = snapshot->Device.Base.Id;
    g_order[g_orderCount++] = 4;
    __Yield();
    CHECK(!job->Done);
    return 0;
}
int sys_usb_register_hub_response(struct gracht_message* message, const oserr_t status)
{
    // These queue-stress registrations have no matching enumerated USB device.
    CHECK(status == OS_ENOENT || status == OS_EBUSY);
    return 0;
}
static void __stdcall __RunJob(void* argument)
{
    struct TestJob* job = argument;
    int i;
    struct sys_device registration = { 0 };
    char description[32] = "test controller";
    if (g_lateAdmission && job == &g_jobs[3]) {
        while (g_topology.Running) __Yield();
    }
    if (job->Kind == 0) sys_usb_port_event_invocation(NULL, 10, job->Port);
    else if (job->Kind == 1) sys_usb_port_error_invocation(NULL, 10, job->Port);
    else if (job->Kind == 2) sys_usb_unregister_controller_invocation(NULL, 10);
    else if (job->Kind == 3) {
        for (i = 200; i < 250; i++) sys_usb_register_hub_invocation(NULL, 10, i, 73, 0, 0);
    } else if (job->Kind == 4) {
        while (g_stopAfterReset && !g_resets) __Yield();
        g_stopStatus = UsbCoreDestroy();
    } else if (job->Kind == 5) {
        sys_usb_get_controller_count_invocation((struct gracht_message*)job);
    } else if (job->Kind == 6) {
        sys_usb_get_controller_invocation((struct gracht_message*)job, 0);
    } else if (job->Kind == 7) {
        registration.content_type = SYS_DEVICE_CONTENT_BASE;
        registration.content.base.id = job->Port;
        registration.content.base.identification.description = description;
        sys_usb_register_controller_invocation(NULL, 73, &registration, USBCONTROLLER_KIND_UHCI, 4);
        memset(description, 0xCC, sizeof(description));
    } else if (job->Kind == 8) {
        sys_usb_unregister_hub_invocation(NULL, 10);
    }
    job->Done = 1;
    SwitchToFiber(g_main);
    abort();
}
static void __Run(int count)
{
    int i, done, steps = 0;
    for (i = 0; i < count; i++) {
        g_jobs[i].Done = 0; g_jobs[i].Waiting = NULL;
        g_jobs[i].Fiber = CreateFiber(0, __RunJob, &g_jobs[i]);
        CHECK(g_jobs[i].Fiber != NULL);
    }
    do {
        done = 1;
        for (i = 0; i < 5; i++) {
            if (i >= count && i != 4) continue;
            if (!g_jobs[i].Done && g_jobs[i].Fiber && !g_jobs[i].Waiting) {
                g_current = i; SwitchToFiber(g_jobs[i].Fiber);
            }
        }
        for (i = 0; i < count; i++) if (!g_jobs[i].Done) done = 0;
        CHECK(++steps < 4000);
    } while (!done);
    g_current = -1;
    for (i = 0; i < count; i++) { DeleteFiber(g_jobs[i].Fiber); g_jobs[i].Fiber = NULL; }
}
static void __Init(void)
{
    Device_t* device = calloc(1, sizeof(*device));
    int i;
    CHECK(device != NULL); device->Id = 10;
    CHECK(UsbCoreInitialize() == OS_EOK);
    CHECK(UsbCoreControllerRegister(73, device, USBCONTROLLER_KIND_UHCI, 4) == OS_EOK);
    for (i = 0; i < 4; i++) g_ports[i] = (USBPortDescriptor_t){ .Connected = 1, .Speed = USBSPEED_FULL };
    g_zeroPort = -1; g_resets = g_disables = g_detaches = g_registered = g_waiters = 0;
    g_failInitial = g_failAddress = g_failDisable = g_failConfig = g_failRegister = 0;
    g_failReset = g_disabledReset = g_stopAfterReset = g_orderCount = 0;
    g_lateAdmission = g_checkWorkerStop = 0;
    memset(g_jobs, 0, sizeof(g_jobs[0]) * 4);
    memset(g_counts, 0xFF, sizeof(g_counts));
    memset(g_snapshotIds, 0xFF, sizeof(g_snapshotIds));
}
static void __Finish(void)
{
    g_jobs[0].Kind = 4; g_stopAfterReset = 0;
    __Run(1);
    CHECK(g_stopStatus == OS_EOK && g_topology.Job == UUID_INVALID);
    CHECK(g_liveAllocations == 0);
}
static void __TestConcurrentEnumeration(void)
{
    UsbHub_t* hub;
    __Init();
    g_jobs[0].Port = 0; g_jobs[1].Port = 1; g_jobs[2].Port = 0; g_jobs[3].Kind = 3;
    __Run(4);
    hub = UsbCoreHubsGet(10);
    CHECK(g_waiters > 0 && g_resets == 4 && g_registered == 2);
    CHECK(hub->Ports[0]->Device->Base.device_address == 1);
    CHECK(hub->Ports[1]->Device->Base.device_address == 2);
    CHECK(g_zeroPort == -1 && !UsbCoreControllerGet(10)->EnumerationRecovery.Pending);
    __Finish();
    CHECK(g_detaches == 2);
}
static void __TestFailure(int stage)
{
    UsbController_t* controller;
    __Init();
    controller = UsbCoreControllerGet(10);
    g_failInitial = stage == 0; g_failAddress = stage == 1;
    g_failConfig = stage == 2; g_failRegister = stage == 3;
    g_failReset = stage == 4 ? 1 : (stage == 6 ? 2 : 0);
    g_disabledReset = stage == 5;
    __Run(1);
    CHECK(g_disables == (stage == 5 ? 0 : 1) && !g_ports[0].Enabled);
    CHECK(controller->AddressMap[0] == 1 && !controller->EnumerationRecovery.Pending);
    CHECK(UsbCoreHubsGet(10)->Ports[0]->Device == NULL);
    g_failInitial = g_failAddress = g_failConfig = g_failRegister = 0;
    g_failReset = g_disabledReset = 0;
    g_jobs[0].Port = 1; __Run(1);
    CHECK(UsbCoreHubsGet(10)->Ports[1]->Device->Base.device_address == 1);
    __Finish();
}
static void __TestFailedIsolation(void)
{
    __Init(); g_failInitial = g_failDisable = 1;
    __Run(1);
    CHECK(UsbCoreControllerGet(10)->EnumerationRecovery.Pending && g_resets == 1);
    g_failInitial = 0; g_jobs[0].Port = 1; __Run(1);
    CHECK(g_resets == 1 && g_registered == 0);
    g_failDisable = 0; __Run(1);
    CHECK(g_resets == 3 && g_registered == 1 && !g_ports[0].Enabled);
    __Finish();
}
static void __TestRemovalDuringEnumeration(void)
{
    __Init(); g_jobs[1].Kind = 2;
    __Run(2);
    CHECK(UsbCoreControllerGet(10) == NULL && UsbCoreHubsGet(10) == NULL);
    CHECK(g_registered == 1 && g_detaches == 1 && g_waiters > 0);
    __Finish();
}
static void __TestAddressBitmap(void)
{
    UsbController_t controller = { .AddressMap = { 1 } };
    int i, address;
    for (i = 1; i < 128; i++) {
        CHECK(UsbCoreControllerReserveAddress(&controller, &address) == OS_EOK && address == i);
    }
    CHECK(UsbCoreControllerReserveAddress(&controller, &address) == OS_ENOENT);
    UsbCoreControllerReleaseAddress(&controller, 63);
    CHECK(UsbCoreControllerReserveAddress(&controller, &address) == OS_EOK && address == 63);
}
static void __TestRemovedHubIsolation(void)
{
    UsbController_t* controller;
    __Init();
    controller = UsbCoreControllerGet(10);
    CHECK(UsbCoreHubsRegister(10, 200, 74, 1, 0) == OS_EOK);
    CHECK(UsbCoreHubsRegister(200, 201, 75, 1, 0) == OS_EOK);
    // A registered child hub is represented by its parent port's device.
    UsbCoreHubsGetPort(UsbCoreHubsGet(200), 0)->Device = calloc(1, sizeof(UsbPortDevice_t));
    UsbCoreHubsGet(200)->Ports[0]->Device->DeviceId = 201;
    controller->EnumerationRecovery = (struct UsbEnumerationRecovery) {
        .HubDeviceId = 201, .HubDriverId = 75, .PortAddress = 0, .Pending = true
    };
    UsbCoreHubsUnregister(200);
    CHECK(UsbCoreHubsGet(200) == NULL && UsbCoreHubsGet(201) == NULL);
    CHECK(controller->EnumerationRecovery.Pending && controller->EnumerationRecovery.HubDeviceId == 10);
    CHECK(controller->EnumerationRecovery.HubDriverId == 73 && controller->EnumerationRecovery.PortAddress == 0);
    g_ports[0].Enabled = 1; g_failDisable = 1; g_jobs[0].Port = 1;
    __Run(1);
    CHECK(g_resets == 0 && controller->EnumerationRecovery.Pending);
    // Physical disconnection is sufficient even if the disable RPC fails.
    g_ports[0].Connected = 0;
    __Run(1);
    CHECK(g_resets == 2 && !controller->EnumerationRecovery.Pending);
    __Finish();
}

static void __TestWorkerOrdering(void)
{
    __Init();
    g_jobs[0].Kind = 0;
    g_jobs[1].Kind = 5;
    g_jobs[2].Kind = 6;
    g_jobs[3].Kind = 2;
    g_checkWorkerStop = 1;
    __Run(4);
    CHECK(g_counts[1] == 1 && g_snapshotIds[2] == 10);
    CHECK(g_orderCount == 5);
    CHECK(g_order[0] == 1 && g_order[1] == 2 && g_order[2] == 3);
    CHECK(g_order[3] == 4 && g_order[4] == 5);
    CHECK(UsbCoreControllerGet(10) == NULL);
    __Finish();
}
static void __TestWorkerShutdown(void)
{
    int cleanups;
    __Init();
    cleanups = g_usbCleanups;
    g_jobs[0].Kind = 0; g_jobs[1].Kind = 5;
    g_jobs[2].Kind = 4; g_jobs[3].Kind = 6;
    g_stopAfterReset = g_lateAdmission = 1;
    g_spuriousCompletion = 1;
    __Run(4);
    CHECK(g_stopStatus == OS_EOK && g_usbCleanups == cleanups + 1);
    CHECK(g_registered == 1 && g_detaches == 1);
    CHECK(g_counts[1] == 1 && g_snapshotIds[3] == 0);
    CHECK(g_topology.Head == NULL && g_topology.Job == UUID_INVALID);
    CHECK(g_hubs.elements == NULL && g_liveAllocations == 0);
    // Repeated teardown and post-close events cannot reopen or mutate state.
    g_stopAfterReset = g_lateAdmission = 0;
    g_jobs[0].Kind = 0; g_jobs[1].Kind = 5; g_jobs[2].Kind = 4;
    __Run(3);
    CHECK(g_counts[1] == 0 && g_usbCleanups == cleanups + 1 && g_resets == 2);
}
static void __TestJoinFailure(void)
{
    int cleanups;
    __Init(); cleanups = g_usbCleanups;
    g_failJoin = 1;
    g_jobs[0].Kind = 0; g_jobs[1].Kind = 4; g_stopAfterReset = 1;
    __Run(2);
    CHECK(g_stopStatus == OS_EUNKNOWN && g_topology.Job == 5);
    CHECK(g_registered == 1 && UsbCoreControllerGet(10) != NULL);
    CHECK(g_usbCleanups == cleanups && g_hubs.elements != NULL);
    g_failJoin = 0;
    __Finish();
    CHECK(g_usbCleanups == cleanups + 1 && g_detaches == 1);
}
static void __TestWorkerStartupFailure(void)
{
    int cleanups = g_usbCleanups;
    g_failQueue = 1;
    CHECK(UsbCoreInitialize() == OS_EOOM);
    CHECK(g_topology.Job == UUID_INVALID && !g_topology.Running);
    CHECK(g_hubs.elements == NULL && g_usbCleanups == cleanups + 1);
    CHECK(UsbCoreDestroy() == OS_EOK && g_usbCleanups == cleanups + 1);
    g_failQueue = 0;
    g_initFailure = 1;
    CHECK(UsbCoreInitialize() == OS_EOOM && g_topology.Job == UUID_INVALID);
    CHECK(g_hubs.elements == NULL);
    g_initFailure = 0;
    __Init(); __Finish();
}
static void __TestRegistrationLifetime(void)
{
    UsbController_t* controller;
    int live, failure;
    __Init();
    g_jobs[0].Kind = 7; g_jobs[0].Port = 20;
    g_jobs[1].Kind = 5;
    __Run(2);
    controller = UsbCoreControllerGet(20);
    CHECK(controller != NULL && g_counts[1] == 2);
    CHECK(!strcmp(controller->Device->Identification.Description, "test controller"));
    __Finish();
    for (failure = 0; failure < 3; failure++) {
        __Init(); live = g_liveAllocations;
        // Fail device conversion, then controller allocation after conversion.
        g_failAllocationAfter = failure;
        g_jobs[0].Kind = 7; g_jobs[0].Port = 20;
        __Run(1);
        CHECK(UsbCoreControllerGet(20) == NULL && g_liveAllocations == live);
        g_failAllocationAfter = -1;
        __Finish();
    }
}
static void __TestHubAndPortCleanup(void)
{
    __Init();
    g_jobs[0].Kind = 0; g_jobs[1].Kind = 1; g_jobs[2].Kind = 8;
    __Run(3);
    CHECK(g_registered == 1 && g_detaches == 1 && UsbCoreHubsGet(10) == NULL);
    __Finish();
}

int main(void)
{
    int i;
    g_main = ConvertThreadToFiber(NULL); CHECK(g_main != NULL);
    __TestWorkerStartupFailure();
    __TestConcurrentEnumeration();
    for (i = 0; i < 7; i++) __TestFailure(i);
    __TestFailedIsolation();
    __TestRemovalDuringEnumeration();
    __TestAddressBitmap();
    __TestRemovedHubIsolation();
    __TestWorkerOrdering();
    __TestWorkerShutdown();
    __TestJoinFailure();
    __TestRegistrationLifetime();
    __TestHubAndPortCleanup();
    puts("usbd worker: FIFO queries/events, stack lifetimes, drain/reject, startup/join failures, cleanup and enumeration recovery passed");
    return 0;
}


