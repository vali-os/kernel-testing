// Exercise production IPC sending with a deterministic reservation/wait fixture.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ds/streambuffer.h>
#include <ipc_context.h>
#include <memoryspace.h>

static MSContextSecurity_t g_security = {.Sender = 11, .Capabilities = 7};
static streambuffer_t      g_stream;
static IPCMessage_t*       g_waitingMessage;
static size_t              g_changedLength;
static size_t              g_reservedLength;
static size_t              g_writtenLength;
static unsigned int        g_reservations;
static unsigned int        g_notifications;
static unsigned char       g_packet[128];

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return &g_security;
}

oserr_t
LookupHandleByPath(
    _In_  const char* path,
    _Out_ uuid_t*     handle)
{
    (void)path;
    (void)handle;
    assert(!"These tests use handle addresses");
    return OS_ENOENT;
}

oserr_t
SHMKernelMapping(
    _In_  uuid_t handle,
    _Out_ void** mapping)
{
    assert(handle == 42);
    *mapping = &g_stream;
    return OS_EOK;
}

size_t
streambuffer_write_packet_start(
    _In_  streambuffer_t*            stream,
    _In_  size_t                     length,
    _In_  streambuffer_rw_options_t* options,
    _Out_ streambuffer_packet_ctx_t* packetCtx)
{
    (void)options;
    assert(stream == &g_stream);
    g_reservations++;
    g_reservedLength = length;
    if (length > sizeof(g_packet)) {
        return 0;
    }

    // Model a full queue: the sender changes its descriptor before space is
    // released. The reservation still belongs to the original length.
    if (g_waitingMessage != NULL) {
        g_waitingMessage->Length = g_changedLength;
    }
    memset(packetCtx, 0, sizeof(*packetCtx));
    packetCtx->_stream = stream;
    packetCtx->_length = length;
    return length;
}

void
streambuffer_write_packet_data(
    _In_    void*                      data,
    _In_    size_t                     length,
    _InOut_ streambuffer_packet_ctx_t* packetCtx)
{
    // Assert the caller's reservation contract before copying. The real ring
    // writer accepts longer writes, which would overwrite other packet data.
    assert(length <= packetCtx->_length - g_writtenLength);
    memcpy(g_packet + g_writtenLength, data, length);
    g_writtenLength += length;
}

void
streambuffer_write_packet_end(
    _In_ streambuffer_packet_ctx_t* packetCtx)
{
    assert(g_writtenLength == packetCtx->_length);
}

oserr_t
MarkHandle(
    _In_ uuid_t       handle,
    _In_ unsigned int events)
{
    assert(handle == 42 && events == 1);
    g_notifications++;
    return OS_EOK;
}

static void
__TestLengthChange(
    _In_ size_t changedLength)
{
    IPCAddress_t        address = IPC_ADDRESS_HANDLE_INIT(42);
    const char          payload[] = "hello";
    IPCMessage_t        message = {23, &address, payload, sizeof(payload)};
    IPCMessage_t*       messages[] = {&message};
    IPCMessageContext_t credentials;
    size_t              i;

    memset(g_packet, 0xa5, sizeof(g_packet));
    g_writtenLength = 0;
    g_reservations = 0;
    g_notifications = 0;
    g_waitingMessage = &message;
    g_changedLength = changedLength;

    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EOK);
    assert(message.Length == changedLength);
    assert(g_reservations == 1 && g_notifications == 1);
    assert(g_reservedLength == sizeof(credentials) + sizeof(payload));
    memcpy(&credentials, g_packet, sizeof(credentials));
    assert(credentials.Sender == 11 && credentials.Capabilities == 7);
    assert(credentials.SenderHandle == 23);
    assert(memcmp(g_packet + sizeof(credentials), payload, sizeof(payload)) == 0);
    for (i = g_reservedLength; i < sizeof(g_packet); i++) {
        assert(g_packet[i] == 0xa5);
    }
    g_waitingMessage = NULL;
}

static void
__TestOverflow(
    _In_ size_t length,
    _In_ unsigned int expectedReservations)
{
    IPCAddress_t  address = IPC_ADDRESS_HANDLE_INIT(42);
    IPCMessage_t  message = {23, &address, "", length};
    IPCMessage_t* messages[] = {&message};

    g_writtenLength = 0;
    g_reservations = 0;
    g_notifications = 0;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    assert(g_reservations == expectedReservations);
    assert(g_writtenLength == 0 && g_notifications == 0);
}

int
main(void)
{
    __TestLengthChange(128);
    __TestLengthChange(0);
    __TestLengthChange(6);
    __TestOverflow(SIZE_MAX, 0);
    __TestOverflow(SIZE_MAX - sizeof(IPCMessageContext_t) + 1, 0);
    __TestOverflow(SIZE_MAX - sizeof(IPCMessageContext_t), 1);
    puts("IPC send length: reservation bounds, descriptor mutation and overflow passed");
    return 0;
}
