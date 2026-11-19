// Run the PS/2 mouse fast handler and its event handler on the host. The fast
// handler must signal once per packet, after the last byte of the packet is
// stored. The event handler must report every complete packet in the ring and
// leave a partial packet for the next event. The packet boundary must stay in
// place when the indices pass the end of the ring.
#ifndef PS2_MOUSE_SOURCE
#define PS2_MOUSE_SOURCE "../modules/input/ps2/devices/mouse.c"
#endif
#include PS2_MOUSE_SOURCE
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// Handle given to the fast handler for the port's event.
#define TEST_EVENT_HANDLE 42

// More cursor events than any test expects. A drain that reads past the
// stored bytes stops here instead of running for a long time.
#define MAX_CURSOR_EVENTS 64

struct __CursorEvent {
    int16_t X;
    int16_t Y;
    int16_t Z;
};

static PS2Port_t                g_port;
static DeviceIo_t               g_io;
static InterruptResourceTable_t g_resources;
static InterruptFunctionTable_t g_functions;
static uint8_t                  g_nextByte;
static unsigned int             g_signals;
static unsigned int             g_writeIndexAtSignal;
static struct __CursorEvent     g_cursorEvents[MAX_CURSOR_EVENTS];
static unsigned int             g_cursorEventCount;
static unsigned int             g_buttonEventCount;

static size_t
__ReadIoSpace(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      length)
{
    CHECK(io == &g_io && offset == PS2_REGISTER_DATA && length == 1);
    return g_nextByte;
}

static oserr_t
__EventSignal(
    _In_ uuid_t handle)
{
    CHECK(handle == TEST_EVENT_HANDLE);
    g_signals++;
    g_writeIndexAtSignal = g_port.ResponseWriteIndex;
    return OS_EOK;
}

gracht_server_t*
__crt_get_module_server(void)
{
    return NULL;
}

int
ctt_input_event_cursor_event_all(
    _In_ gracht_server_t* server,
    _In_ const uuid_t     deviceId,
    _In_ const uint16_t   flags,
    _In_ const int16_t    relX,
    _In_ const int16_t    relY,
    _In_ const int16_t    relZ)
{
    (void)server;
    (void)deviceId;
    (void)flags;
    CHECK(g_cursorEventCount < MAX_CURSOR_EVENTS);
    g_cursorEvents[g_cursorEventCount++] = (struct __CursorEvent) { relX, relY, relZ };
    return 0;
}

int
ctt_input_event_button_event_all(
    _In_ gracht_server_t* server,
    _In_ const uuid_t     deviceId,
    _In_ const uint8_t    keyCode,
    _In_ const uint16_t   modifiers)
{
    (void)server;
    (void)deviceId;
    (void)keyCode;
    (void)modifiers;
    g_buttonEventCount++;
    return 0;
}

// The commands and the interrupt setup are not tested.
oserr_t
PS2PortExecuteCommand(
    _In_ PS2Port_t* port,
    _In_ uint8_t    commandValue,
    _In_ uint8_t*   response)
{
    (void)port;
    (void)commandValue;
    (void)response;
    abort();
}

oserr_t
PS2PortRegisterInterrupt(
    _In_ PS2Port_t*         port,
    _In_ DeviceIo_t*        data,
    _In_ InterruptHandler_t handler)
{
    (void)port;
    (void)data;
    (void)handler;
    abort();
}

oserr_t
DeviceInterruptDestroy(
    _InOut_ DeviceInterrupt_t* interrupt)
{
    (void)interrupt;
    abort();
}

// Prepares a port in the given mouse mode. The ring keeps its old bytes, as
// it would on a port that has been in use.
static void
__Reset(
    _In_ uint8_t mode)
{
    g_port.ActiveCommand.State = PS2Free;
    g_port.ResponseWriteIndex = 0;
    g_port.ResponseReadIndex = 0;
    g_port.device_data.mouse.mode = mode;
    g_port.device_data.mouse.buttonState = 0;
    g_resources.HandleResource = TEST_EVENT_HANDLE;
    g_resources.IoResources[0] = &g_io;
    g_resources.MemoryResources[0].Address = (uintptr_t)&g_port;
    g_functions.ReadIoSpace = __ReadIoSpace;
    g_functions.EventSignal = __EventSignal;
    g_signals = 0;
    g_writeIndexAtSignal = 0;
    g_cursorEventCount = 0;
    g_buttonEventCount = 0;
}

// Delivers one byte through the fast handler, as one interrupt does.
static void
__Interrupt(
    _In_ uint8_t value)
{
    g_nextByte = value;
    CHECK(PS2MouseFastInterrupt(&g_functions, &g_resources) == IRQSTATUS_HANDLED);
}

// Delivers a packet that moves by x and y with no buttons held. Byte 0 has
// bit 3 set, as the mouse always sends it.
static void
__SendMove(
    _In_ uint8_t mode,
    _In_ uint8_t x,
    _In_ uint8_t y)
{
    __Interrupt(0x08);
    __Interrupt(x);
    __Interrupt(y);
    if (mode != 0) {
        __Interrupt(0);
    }
}

static void
__TestSignalAfterLastByte(void)
{
    // A 3-byte packet signals once, after the third byte is stored.
    __Reset(0);
    __Interrupt(0x08);
    __Interrupt(5);
    CHECK(g_signals == 0);
    __Interrupt(6);
    CHECK(g_signals == 1 && g_writeIndexAtSignal == 3);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 1);
    CHECK(g_cursorEvents[0].X == 5 && g_cursorEvents[0].Y == 6);
    CHECK(g_port.ResponseReadIndex == 3);

    // A 4-byte packet with the wheel byte signals once, after the fourth byte.
    __Reset(1);
    __Interrupt(0x08);
    __Interrupt(1);
    __Interrupt(2);
    CHECK(g_signals == 0);
    __Interrupt(0xFF);
    CHECK(g_signals == 1 && g_writeIndexAtSignal == 4);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 1);
    CHECK(g_cursorEvents[0].X == 1 && g_cursorEvents[0].Y == 2 && g_cursorEvents[0].Z == -1);
    CHECK(g_port.ResponseReadIndex == 4);
    puts("signal: 3-byte and 4-byte packets signal after their last byte");
}

static void
__TestDrain(void)
{
    // Several packets between two handler runs are all reported. With a
    // merged signal, the handler runs once for all of them.
    __Reset(0);
    __SendMove(0, 1, 0);
    __SendMove(0, 2, 0);
    __SendMove(0, 3, 0);
    CHECK(g_signals == 3);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 3);
    CHECK(g_cursorEvents[0].X == 1 && g_cursorEvents[1].X == 2 && g_cursorEvents[2].X == 3);

    // A partial packet stays in the ring until its last byte arrives.
    __Reset(0);
    __SendMove(0, 4, 0);
    __Interrupt(0x08);
    __Interrupt(9);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 1 && g_port.ResponseReadIndex == 3);
    __Interrupt(0);
    CHECK(g_signals == 2);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 2 && g_cursorEvents[1].X == 9);
    CHECK(g_port.ResponseReadIndex == 6);

    // A handler run with less than a packet stored reports nothing. The
    // ring holds old packets here, so a check that wraps below 0 reads them.
    __Reset(0);
    __Interrupt(0x08);
    PS2MouseInterrupt(&g_port);
    CHECK(g_cursorEventCount == 0 && g_port.ResponseReadIndex == 0);
    puts("drain: several packets, a partial packet and a short ring passed");
}

static void
__TestRingWrap(void)
{
    unsigned int packet;
    uint8_t      mode;

    // Send more packets than the ring holds, with the handler run after each
    // signal. Every packet must keep its own bytes past the end of the ring.
    for (mode = 0; mode < 2; mode++) {
        unsigned int packetBytes = mode == 0 ? 3 : 4;
        unsigned int packetCount = (3 * PS2_RINGBUFFER_SIZE) / packetBytes;

        __Reset(mode);
        for (packet = 0; packet < packetCount; packet++) {
            __SendMove(mode, (uint8_t)(packet + 1), 0x7F);
            CHECK(g_signals == packet + 1);
            PS2MouseInterrupt(&g_port);
            CHECK(g_cursorEventCount == packet + 1);
            CHECK(g_cursorEvents[packet].X == (int16_t)(packet + 1));
            CHECK(g_cursorEvents[packet].Y == 0x7F && g_cursorEvents[packet].Z == 0);
        }
        CHECK(g_port.ResponseReadIndex == packetCount * packetBytes);
        CHECK(g_buttonEventCount == 0);
    }
    puts("ring: packet boundaries stay in place past the end of the ring");
}

int
main(void)
{
    __TestSignalAfterLastByte();
    __TestDrain();
    __TestRingWrap();
    return 0;
}
