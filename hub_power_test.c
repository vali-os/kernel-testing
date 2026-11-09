// Exercise descriptor parsing and power-before-scan ordering in the real hub driver.
#include <threads.h>
static int __TestSleep(const struct timespec*, struct timespec*);
#define thrd_sleep __TestSleep
#include "../modules/serial/usb/hub/hub.c"
#undef thrd_sleep
#include <stdio.h>
#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint8_t g_delay;
static uint8_t g_ports;
static uint16_t g_characteristics;
static unsigned int g_powered;
static unsigned int g_queried;
static unsigned int g_sleeps;
static unsigned int g_failPort;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format, ...)
{
}

enum USBTransferCode
UsbExecutePacket(
    _In_ usb_device_context_t* context,
    _In_ uint8_t direction,
    _In_ uint8_t type,
    _In_ uint8_t valueLow,
    _In_ uint8_t valueHigh,
    _In_ uint16_t index,
    _In_ uint16_t length,
    _In_ void* buffer)
{
    UsbHubDescriptor_t* usb2 = buffer;
    UsbHubSuperDescriptor_t* usb3 = buffer;

    CHECK(type == USBPACKET_TYPE_GET_DESC && length == 8);
    memset(buffer, 0, length);
    if (context->speed >= USBSPEED_SUPER) {
        CHECK(valueHigh == DESCRIPTOR_TYPE_HUB_SUPERSPEED);
        usb3->NumberOfPorts = g_ports;
        usb3->PowerOnDelay = g_delay;
        usb3->HubCharacteristics = g_characteristics;
    } else {
        CHECK(valueHigh == DESCRIPTOR_TYPE_HUB);
        usb2->NumberOfPorts = g_ports;
        usb2->PowerOnDelay = g_delay;
        usb2->HubCharacteristics = g_characteristics;
    }
    return USBTRANSFERCODE_SUCCESS;
}

oserr_t
HubPowerOnPort(
    _In_ HubDevice_t* hub,
    _In_ uint8_t port)
{
    CHECK(g_sleeps == 0 && g_queried == 0);
    CHECK(port == ++g_powered && port <= hub->PortCount);
    return port == g_failPort ? OS_EDEVFAULT : OS_EOK;
}

static int
__TestSleep(
    _In_ const struct timespec* duration,
    _Out_ struct timespec* remaining)
{
    CHECK(g_powered == g_ports && g_queried == 0 && g_sleeps++ == 0);
    CHECK(duration->tv_sec == 0);
    CHECK(duration->tv_nsec == (long)g_delay * 2 * NSEC_PER_MSEC);
    return 0;
}

oserr_t
HubGetPortStatus(
    _In_ HubDevice_t* hub,
    _In_ uint8_t port,
    _Out_ PortStatus_t* status)
{
    CHECK(g_powered == g_ports && g_sleeps == 1);
    CHECK(port == ++g_queried);
    memset(status, 0, sizeof(*status));
    return OS_EOK;
}

oserr_t
HubPortClearChange(
    _In_ HubDevice_t* hub,
    _In_ uint8_t port,
    _In_ uint8_t change)
{
    CHECK(false); // No attached devices in this fixture.
    return OS_EOK;
}

oserr_t
UsbEventPort(
    _In_ uuid_t device,
    _In_ uint8_t port)
{
    CHECK(false);
    return OS_EOK;
}

static void
__TestPowerSequence(
    _In_ enum USBSpeed speed,
    _In_ uint8_t powerMode,
    _In_ uint8_t delay,
    _In_ uint8_t ports,
    _In_ uint8_t failPort)
{
    UsbDevice_t device = { .DeviceContext.speed = speed };
    HubDevice_t hub = { .Base = &device };

    g_delay = delay;
    g_ports = ports;
    g_characteristics = powerMode;
    g_failPort = failPort;
    g_powered = g_queried = g_sleeps = 0;
    CHECK(__GetHubDescriptor(&hub) == OS_EOK);
    CHECK(hub.PowerOnDelay == (unsigned int)delay * 2);
    CHECK(hub.PortCount == ports && hub.HubCharacteristics == powerMode);
    HubEnumeratePorts(&hub);
    CHECK(g_powered == ports && g_queried == ports && g_sleeps == 1);
}

int
main(void)
{
    static const uint8_t delays[] = { 0, 1, 25, 26, 100, 255 };
    unsigned int mode;
    unsigned int delay;

    for (mode = 0; mode < 4; mode++) {
        for (delay = 0; delay < sizeof(delays); delay++) {
            __TestPowerSequence(USBSPEED_HIGH, mode, delays[delay], 4, 0);
            __TestPowerSequence(USBSPEED_SUPER, mode, delays[delay], 4, 0);
        }
    }
    __TestPowerSequence(USBSPEED_HIGH, 0, 255, 4, 2);
    __TestPowerSequence(USBSPEED_SUPER, 1, 255, 255, 0);
    puts("USB hub power: all ports, ganged/individual modes, full USB2/USB3 delay and power failure passed");
    return 0;
}
