// Run the PS/2 driver's device registration on the host. The module server
// runs each message in its own job, and a job lets the others run while it
// sleeps or waits for a reply. deviced sends a port device to the driver
// before DeviceRegister returns that device's id to PS2Initialize. The port's
// registration must wait until PS2Initialize has stored the ids of both ports.
// Each job is a host thread here, and the driver's lock is a spin lock that
// yields the thread while it waits.
#ifndef PS2_MAIN_SOURCE
#define PS2_MAIN_SOURCE "../modules/input/ps2/main.c"
#endif
#include PS2_MAIN_SOURCE
#include <stdarg.h>
#include <stdio.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// The host's pthread.h clashes with the Vali headers that main.c includes, so
// the host thread calls are declared here. glibc's pthread_t is an unsigned long.
typedef unsigned long HostThread_t;
extern int __HostThreadCreate(HostThread_t*, const void*, void* (*)(void*), void*) __asm__("pthread_create");
extern int __HostThreadJoin(HostThread_t, void**) __asm__("pthread_join");
extern int __HostThreadYield(void) __asm__("sched_yield");
extern int __HostSleepMicroseconds(unsigned int) __asm__("usleep");

// Ids given to the controller and to the devices on its two ports.
#define TEST_CONTROLLER_ID 8
#define TEST_PORT_ID_BASE  9

// Time the controller job keeps a port's id back after deviced has sent that
// port's device. Without the lock, the port's registration finishes in this time.
#define TEST_REGISTER_DELAY_US 100000

// Signatures returned by the identify command: an MF2 keyboard without
// translation on the first port, and a standard mouse on the second.
static const unsigned int g_signatures[PS2_MAXPORTS] = { 0xAB83, 0x0000 };

gracht_protocol_t ctt_driver_server_protocol;
gracht_protocol_t ctt_input_server_protocol;

static int             g_jobLock;
static HostThread_t    g_deviced[PS2_MAXPORTS];
static volatile int    g_devicedStarted[PS2_MAXPORTS];
static int             g_portsInitialized;
static uint8_t         g_lastCommand;
static int             g_keyboardCalls;
static int             g_mouseCalls;
static int             g_warnings;

void
usched_mtx_init(
    _In_ struct usched_mtx* mutex,
    _In_ int                type)
{
    (void)mutex;
    (void)type;
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    CHECK(mutex == &g_registrationLock);
    while (__atomic_exchange_n(&g_jobLock, 1, __ATOMIC_ACQUIRE)) {
        __HostThreadYield();
    }
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    CHECK(mutex == &g_registrationLock);
    CHECK(__atomic_exchange_n(&g_jobLock, 0, __ATOMIC_RELEASE) == 1);
}

// Vali's errno macro calls __errno, which the host C library does not have.
int*
__errno(void)
{
    static _Thread_local int value;
    return &value;
}

int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    (void)until;
    return 0;
}

void
timespec_add(
    _In_ const struct timespec* a,
    _In_ const struct timespec* b,
    _In_ struct timespec*       result)
{
    (void)a;
    (void)b;
    (void)result;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    va_list arguments;

    if (level == OSSYSLOGLEVEL_WARNING || level == OSSYSLOGLEVEL_ERROR) {
        __atomic_add_fetch(&g_warnings, 1, __ATOMIC_SEQ_CST);
    }
    va_start(arguments, format);
    vprintf(format, arguments);
    va_end(arguments);
    printf("\n");
}

gracht_server_t*
__crt_get_module_server(void)
{
    return NULL;
}

int
gracht_server_register_protocol(
    _In_ gracht_server_t*   server,
    _In_ gracht_protocol_t* protocol)
{
    (void)server;
    (void)protocol;
    return 0;
}

oserr_t
OSServiceWaitForDevice(
    _In_ size_t timeout)
{
    (void)timeout;
    return OS_EOK;
}

oserr_t
DeviceIoAcquire(
    _In_ DeviceIo_t* ioSpace)
{
    (void)ioSpace;
    return OS_EOK;
}

oserr_t
DeviceIoRelease(
    _In_ DeviceIo_t* ioSpace)
{
    (void)ioSpace;
    return OS_EOK;
}

// The status register always reports a byte to read and room to write. The
// data register answers the controller self test and reads 0 otherwise.
size_t
DeviceIoRead(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    (void)offset;
    (void)length;
    if (ioSpace == Ps2Controller->Command) {
        return PS2_STATUS_OUTPUT_FULL;
    }
    return g_lastCommand == PS2_SELFTEST ? PS2_SELFTEST_OK : 0;
}

oserr_t
DeviceIoWrite(
    _In_ DeviceIo_t* ioSpace,
    _In_ size_t      offset,
    _In_ size_t      value,
    _In_ size_t      length)
{
    (void)offset;
    (void)length;
    if (ioSpace == Ps2Controller->Command) {
        g_lastCommand = (uint8_t)value;
    }
    return OS_EOK;
}

// Plays deviced: sends the port's device to the driver as a separate job.
static void*
__DevicedSendPort(
    _In_ void* context)
{
    int                    index = (int)(uintptr_t)context;
    struct sys_device_base base = { 0 };
    struct sys_device      device = { 0 };

    base.id = TEST_PORT_ID_BASE + index;
    device.content_type = SYS_DEVICE_CONTENT_BASE;
    device.content.base = base;
    g_devicedStarted[index] = 1;
    ctt_driver_register_device_invocation(NULL, &device);
    return NULL;
}

// Stands in for port.c. Like PS2RegisterDevice, the id is stored only after
// deviced has already sent the device to the driver.
oserr_t
PS2PortInitialize(
    _In_ PS2Port_t* port)
{
    port->Signature = g_signatures[port->Index];
    port->State = PortStateConnected;
    CHECK(__HostThreadCreate(&g_deviced[port->Index], NULL, __DevicedSendPort,
                         (void*)(uintptr_t)port->Index) == 0);
    while (!g_devicedStarted[port->Index]) {
        __HostThreadYield();
    }
    __HostSleepMicroseconds(TEST_REGISTER_DELAY_US);
    port->DeviceId = TEST_PORT_ID_BASE + port->Index;
    __atomic_add_fetch(&g_portsInitialized, 1, __ATOMIC_SEQ_CST);
    return OS_EOK;
}

oserr_t
PS2KeyboardInitialize(
    _In_ PS2Controller_t* controller,
    _In_ int              portIndex,
    _In_ int              translation)
{
    CHECK(controller == Ps2Controller);
    CHECK(__atomic_load_n(&g_portsInitialized, __ATOMIC_SEQ_CST) == PS2_MAXPORTS);
    CHECK(portIndex == 0 && translation == 0);
    g_keyboardCalls++;
    return OS_EOK;
}

oserr_t
PS2MouseInitialize(
    _In_ PS2Controller_t* controller,
    _In_ int              index)
{
    CHECK(controller == Ps2Controller);
    CHECK(__atomic_load_n(&g_portsInitialized, __ATOMIC_SEQ_CST) == PS2_MAXPORTS);
    CHECK(index == 1);
    g_mouseCalls++;
    return OS_EOK;
}

oserr_t PS2KeyboardCleanup(PS2Controller_t* controller, int portIndex) { (void)controller; (void)portIndex; return OS_EOK; }
oserr_t PS2MouseCleanup(PS2Controller_t* controller, int index) { (void)controller; (void)index; return OS_EOK; }
void PS2KeyboardInterrupt(PS2Port_t* port) { (void)port; }
void PS2MouseInterrupt(PS2Port_t* port) { (void)port; }

int
main(void)
{
    struct sys_device_bus bus = { 0 };
    struct sys_device     controller = { 0 };
    int                   i;

    CHECK(OnLoad() == OS_EOK);

    // The controller registration runs on this thread and registers both
    // ports. Their registrations run on deviced threads.
    bus.id = TEST_CONTROLLER_ID;
    controller.content_type = SYS_DEVICE_CONTENT_BUS;
    controller.content.bus = bus;
    ctt_driver_register_device_invocation(NULL, &controller);
    for (i = 0; i < PS2_MAXPORTS; i++) {
        CHECK(__HostThreadJoin(g_deviced[i], NULL) == 0);
    }

    CHECK(Ps2Controller->Ports[0].DeviceId == TEST_PORT_ID_BASE);
    CHECK(Ps2Controller->Ports[1].DeviceId == TEST_PORT_ID_BASE + 1);
    CHECK(g_keyboardCalls == 1);
    CHECK(g_mouseCalls == 1);
    CHECK(g_warnings == 0);
    printf("register: both port devices registered after the controller\n");

    free(Ps2Controller->Device);
    OnUnload();
    return 0;
}
