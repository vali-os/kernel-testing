/** Exercise production IPC framing, sender context and SHM storage together. */
#define IPC_SECURITY_TEST
#define main __MemoryOwnershipTests
#include "shm_memory_ownership_test.c"
#undef main
#include <ds/streambuffer.h>
#include <ipc_context.h>
#include <os/ipc.h>
#include <os/futex.h>

static MSContextSecurity_t g_security = {
    .Handle = 11,
    .Capabilities = CAPABILITY_GRANT_CAPABILITY
};
static bool g_authenticated = true;
static unsigned int g_notifications;
static uuid_t g_destination;

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return g_authenticated ? &g_security : NULL;
}

oserr_t
ArchSHMTypeToPageMask(
    _In_ enum OSMemoryConformity conformity,
    _Out_ size_t* mask)
{
    (void)conformity;
    *mask = SIZE_MAX;
    return OS_EOK;
}

oserr_t
RegisterHandlePath(
    _In_ uuid_t handle,
    _In_ const char* path)
{
    (void)handle; (void)path;
    return OS_EEXISTS;
}

oserr_t
LookupHandleByPath(
    _In_ const char* path,
    _Out_ uuid_t* handle)
{
    if (strcmp(path, "/test") != 0) {
        return OS_ENOENT;
    }
    *handle = g_destination;
    return OS_EOK;
}

oserr_t
MarkHandle(
    _In_ uuid_t handle,
    _In_ unsigned int flags)
{
    assert(handle == g_destination && flags == IOSETIN);
    g_notifications++;
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return handle->Payload;
}

oserr_t
MemorySpaceCommit(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t address,
    _In_ paddr_t* pages,
    _In_ size_t length,
    _In_ size_t mask,
    _In_ unsigned int flags)
{
    (void)space; (void)address; (void)pages;
    (void)length; (void)mask; (void)flags;
    abort();
}

void
dswait(
    _In_ OSFutexParameters_t* parameters,
    _In_ OSAsyncContext_t* context)
{
    (void)parameters; (void)context;
    assert(!"The fixture must not fill the IPC buffer");
}

void
dswake(
    _In_ OSFutexParameters_t* parameters)
{
    (void)parameters;
}

static void
__Receive(
    _In_ OSHandle_t* endpoint,
    _In_ const void* expected,
    _In_ size_t length)
{
    IPCMessageContext_t context;
    unsigned char output[32];
    size_t received;

    assert(length <= sizeof(output));
    assert(IPCContextRecv(endpoint, output, sizeof(output), IPC_DONTWAIT,
        NULL, &context, &received) == OS_EOK);
    assert(received == length);
    if (length == 0) {
        assert(context.SenderHandle == UUID_INVALID);
        return;
    }
    assert(context.Sender == g_security.Handle);
    assert(context.Capabilities == g_security.Capabilities);
    assert(context.SenderHandle == 42);
    assert(memcmp(output, expected, length) == 0);
}

int
main(void)
{
    SHM_t configuration = {.Flags = SHM_IPC, .Size = PAGE_SIZE};
    SHMHandle_t destination;
    OSHandle_t receiver;
    IPCAddress_t address;
    IPCMessage_t message;
    IPCMessage_t* batch[] = {&message, &message};
    IPCMessageContext_t forged = {99, CAPABILITY_ALL, 77};
    const char payload[] = "message";
    unsigned int heapCalls;

    __MemoryOwnershipTests();
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, PAGE_SIZE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, REGION_BASE,
        REGION_LENGTH, true, g_storage, sizeof(g_storage)) == OS_EOK);
    assert(SHMCreate(&configuration, &destination) == OS_EOK);
    g_destination = destination.ID;
    receiver = (OSHandle_t){.ID = destination.ID, .Payload = destination.Buffer};
    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(destination.ID);
    message = (IPCMessage_t){42, &address, payload, sizeof(payload)};

    assert(IpcContextSendMultiple(NULL, 1, NULL, NULL) == OS_EINVALPARAMS);
    assert(IpcContextSendMultiple(batch, 0, NULL, NULL) == OS_EINVALPARAMS);
    g_authenticated = false;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EPERMISSIONS);
    assert(g_notifications == 0);
    __Receive(&receiver, NULL, 0);
    g_authenticated = true;

    heapCalls = g_heapCalls;
    assert(IpcContextSendMultiple(batch, 2, NULL, NULL) == OS_EOK);
    assert(g_notifications == 2 && g_heapCalls == heapCalls);
    __Receive(&receiver, payload, sizeof(payload));
    __Receive(&receiver, payload, sizeof(payload));
    __Receive(&receiver, NULL, 0);

    // Payload bytes cannot replace the header written from the sender context.
    message.Payload = &forged;
    message.Length = sizeof(forged);
    g_security.Capabilities = 0;
    address.Type = IPC_ADDRESS_PATH;
    address.Data.Path = "/test";
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    __Receive(&receiver, &forged, sizeof(forged));

    address.Data.Path = "/missing";
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EINCOMPLETE);
    assert(g_notifications == 3);
    __Receive(&receiver, NULL, 0);
    assert(SHMDetach(&destination) == OS_EOK);
    __ExpectEmpty();
    puts("IPC integration: required sender context, kernel header, batches, path lookup and SHM cleanup passed");
    return 0;
}
