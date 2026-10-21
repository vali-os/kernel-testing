/** Exercise actual userspace receive and framing without a receive syscall. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <os/ipc.h>
#include <os/futex.h>
#include <ds/streambuffer.h>

static oserr_t g_sendStatus;
static unsigned g_asyncWaits;
void* dsalloc(size_t size) { return malloc(size); }
void dsfree(void* pointer) { free(pointer); }
void* SHMBuffer(OSHandle_t* handle) { return handle->Payload; }
void dswait(OSFutexParameters_t* params, OSAsyncContext_t* async)
{ (void)params; (void)async; assert(!"Receive must not wait in these cases"); }
void dswake(OSFutexParameters_t* params) { (void)params; }
void usched_wait_async(void) { g_asyncWaits++; }
oserr_t Syscall_IPCSend(IPCMessage_t** messages, int count, OSTimestamp_t* deadline, OSAsyncContext_t* async)
{ (void)messages; (void)count; (void)deadline; (void)async; return g_sendStatus; }

static void
__Enqueue(streambuffer_t* stream, IPCMessageContext_t credentials, size_t length)
{
    streambuffer_packet_ctx_t packet;
    streambuffer_rw_options_t options = {.flags = STREAMBUFFER_NO_BLOCK};
    assert(streambuffer_write_packet_start(stream, length, &options, &packet) == length);
    streambuffer_write_packet_data(&credentials, length < sizeof(credentials) ? length : sizeof(credentials), &packet);
    if (length > sizeof(credentials)) {
        streambuffer_write_packet_data("hello", length - sizeof(credentials), &packet);
    }
    streambuffer_write_packet_end(&packet);
}

int
main(void)
{
    streambuffer_t* stream;
    OSHandle_t endpoint;
    IPCMessageContext_t credentials, expected = {11, 1, 42};
    unsigned char output[16];
    size_t received;
    uuid_t sender;
    OSAsyncContext_t async = {.ErrorCode = OS_EOK};
    IPCAddress_t address = IPC_ADDRESS_HANDLE_INIT(7);

    assert(streambuffer_create(512, 0, &stream) == OS_EOK);
    endpoint = (OSHandle_t){.ID = 7, .Payload = stream};
    __Enqueue(stream, expected, sizeof(expected) + 5);
    memset(output, 0xa5, sizeof(output));
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), IPC_DONTWAIT,
        NULL, &credentials, &received) == OS_EOK);
    assert(!memcmp(&credentials, &expected, sizeof(expected)));
    assert(received == 5 && !memcmp(output, "hello", 5) && output[5] == 0xa5);
    __Enqueue(stream, expected, sizeof(expected) + 5);
    assert(IPCContextRecv(&endpoint, output, 2, 0, NULL, &sender, &received) == OS_EOK);
    assert(received == 2 && sender == 42 && !memcmp(output, "he", 2));
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), IPC_DONTWAIT,
        NULL, &credentials, &received) == OS_EOK);
    assert(received == 0 && credentials.Sender == 0 && credentials.SenderHandle == UUID_INVALID);

    __Enqueue(stream, expected, 1);
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), 0,
        NULL, &credentials, &received) == OS_EPROTOCOL);
    assert(received == 0 && credentials.Capabilities == 0);
    __Enqueue(stream, (IPCMessageContext_t){0, 0, UUID_INVALID}, sizeof(expected) + 5);
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), 0,
        NULL, &credentials, &received) == OS_EOK);
    assert(received == 0 && credentials.Sender == 0);
    __Enqueue(stream, (IPCMessageContext_t){0, 0, UUID_INVALID}, sizeof(expected) + 5);
    __Enqueue(stream, expected, sizeof(expected) + 5);
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), 0,
        NULL, &credentials, &received) == OS_EOK && received == 5);

    // Owner mutation is intentionally within the receiver's trust boundary.
    __Enqueue(stream, expected, sizeof(expected) + 5);
    streambuffer_packet_ctx_t edit = {._stream = stream,
        ._state = atomic_load(&stream->consumer_index) + sizeof(size_t)};
    expected.Capabilities = 7;
    streambuffer_write_packet_data(&expected, sizeof(expected), &edit);
    assert(IPCContextRecvWithCredentials(&endpoint, output, sizeof(output), 0,
        NULL, &credentials, &received) == OS_EOK && credentials.Capabilities == 7);

    g_sendStatus = OS_EOK;
    assert(IPCContextSend(&endpoint, &address, output, 1, NULL, &async) == OS_EOK && g_asyncWaits == 0);
    g_sendStatus = OS_EFORKED;
    assert(IPCContextSend(&endpoint, &address, output, 1, NULL, &async) == OS_EOK && g_asyncWaits == 1);
    async.ErrorCode = OS_ETIMEOUT;
    assert(IPCContextSend(&endpoint, &address, output, 1, NULL, &async) == OS_ETIMEOUT && g_asyncWaits == 2);
    free(stream);
    puts("IPC receive: direct userspace copy, credentials, truncation, discard, owner writes and async send passed");
    return 0;
}
