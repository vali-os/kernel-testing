// Exercise IPC routing and SHM validation with the production handle tables.
#include "../kernel/handle.c"
#include "handle_set_stubs.h"
#include "../kernel/memory/shm/private.h"
#include "../kernel/memory/space/private.h"
#include <ds/streambuffer.h>
#include <ipc_context.h>
#include <os/futex.h>
#include <shm/shm.h>
#include <stdio.h>
#include <stdlib.h>

_Static_assert(sizeof(IPCMessageContext_t) == 16, "IPC message prefix size changed");
_Static_assert(offsetof(IPCMessageContext_t, Capabilities) == 8, "IPC capabilities offset changed");
_Static_assert(offsetof(IPCMessageContext_t, SenderGlobalHandleID) == 12, "IPC sender endpoint offset changed");

static MSContextSecurity_t g_sender;
static MSContextSecurity_t g_receiver;
static MSContextSecurity_t* g_current;
static unsigned int g_live;
static unsigned int g_deletedBuffers;
static unsigned int g_notifications;
static uuid_t g_destinationGlobalHandleID;
static uuid_t g_senderHandleID;
static IPCMessage_t* g_waitingMessage;

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return g_current;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* memory = calloc(1, size);

    assert(memory != NULL);
    g_live++;
    return memory;
}

void
kfree(
    _In_ void* memory)
{
    if (memory != NULL) {
        assert(g_live != 0);
        g_live--;
        free(memory);
    }
}

void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_ void* memory)
{
    free(memory);
}

static void
__DrainJanitor(void)
{
    element_t* entry;

    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

static void
__DeleteBuffer(
    _In_ void* resource)
{
    struct SHMBuffer* buffer = resource;

    kfree((void*)buffer->KernelMapping);
    kfree(buffer);
    g_deletedBuffers++;
}

static uuid_t
__CreateBuffer(
    _In_ unsigned int flags,
    _In_ bool         initialized)
{
    struct SHMBuffer* buffer;
    streambuffer_t*   stream;

    buffer = kmalloc(sizeof(*buffer));
    buffer->Flags = flags;
    buffer->Length = sizeof(*stream) + 256;
    if (initialized) {
        stream = kmalloc(buffer->Length);
        streambuffer_construct(stream, 256, STREAMBUFFER_GLOBAL | STREAMBUFFER_MULTIPLE_WRITERS);
        buffer->KernelMapping = (vaddr_t)stream;
    }
    buffer->ID = CreateHandle(HandleTypeSHM, NULL, __DeleteBuffer, buffer);
    assert(buffer->ID != UUID_INVALID);
    return buffer->ID;
}

static int
__References(
    _In_ uuid_t globalHandleID)
{
    struct ResourceHandle* handle = __LookupGlobalSafe(globalHandleID);

    assert(handle != NULL);
    return handle->References;
}

oserr_t
MarkHandle(
    _In_ uuid_t       globalHandleID,
    _In_ unsigned int flags)
{
    assert(globalHandleID == g_destinationGlobalHandleID && flags == IOSETIN);
    assert(LookupGlobalHandle(globalHandleID, HandleTypeSHM) != NULL);
    g_notifications++;
    return OS_EOK;
}

void
dswake(
    _In_ OSFutexParameters_t* parameters)
{
    (void)parameters;
}

void
dswait(
    _In_ OSFutexParameters_t* parameters,
    _In_ OSAsyncContext_t*    asyncContext)
{
    struct SHMBuffer* buffer;
    streambuffer_t*   stream;
    unsigned int     deleted = g_deletedBuffers;

    (void)parameters;
    (void)asyncContext;
    assert(g_waitingMessage != NULL && g_current == &g_sender);
    assert(__References(g_destinationGlobalHandleID) == 2);
    assert(__References(g_senderHandleID) == 2);

    // Remove both process grants while the send is suspended. Its independent
    // references must keep both buffers valid through publication.
    g_current = &g_receiver;
    assert(DestroyHandle(g_destinationGlobalHandleID) == OS_EOK);
    g_current = &g_sender;
    assert(DestroyHandle(g_senderHandleID) == OS_EOK);
    __DrainJanitor();
    assert(g_deletedBuffers == deleted);
    assert(__References(g_destinationGlobalHandleID) == 1);
    assert(__References(g_senderHandleID) == 1);
    g_waitingMessage->SenderHandleID = g_destinationGlobalHandleID;
    g_waitingMessage->Length = SIZE_MAX;
    g_waitingMessage = NULL;

    // Simulate the receiver consuming the earlier packet to free queue space.
    buffer = LookupGlobalHandle(g_destinationGlobalHandleID, HandleTypeSHM);
    stream = (streambuffer_t*)buffer->KernelMapping;
    atomic_store(&stream->consumer_comitted_index, atomic_load(&stream->producer_comitted_index));
    atomic_store(&stream->consumer_index, atomic_load(&stream->producer_comitted_index));
}

static void
__Receive(
    _In_ streambuffer_t* stream,
    _In_ uuid_t          senderGlobalHandleID,
    _In_ const void*     expected,
    _In_ size_t          length)
{
    streambuffer_rw_options_t options = { .flags = STREAMBUFFER_NO_BLOCK };
    streambuffer_packet_ctx_t packet;
    IPCMessageContext_t       context;
    unsigned char            payload[256];

    assert(length <= sizeof(payload));
    assert(streambuffer_read_packet_start(stream, &options, &packet) == sizeof(context) + length);
    streambuffer_read_packet_data(&context, sizeof(context), &packet);
    streambuffer_read_packet_data(payload, length, &packet);
    streambuffer_read_packet_end(&packet);
    assert(context.SenderGroupGlobalHandleID == g_current->GlobalHandleID);
    assert(context.Capabilities == g_current->Capabilities);
    assert(context.SenderGlobalHandleID == senderGlobalHandleID);
    assert(memcmp(payload, expected, length) == 0);
}

// The denied SHM map must stop at its grant check, before reaching the MMU.
MemorySpace_t*
GetCurrentMemorySpace(void)
{
    assert(false);
    return NULL;
}

size_t
GetMemorySpacePageSize(void)
{
    return 4096;
}

oserr_t
AreMemorySpacesRelated(
    _In_ MemorySpace_t* first,
    _In_ MemorySpace_t* second)
{
    (void)first;
    (void)second;
    assert(false);
    return OS_EPERMISSIONS;
}

void
MutexLock(
    _In_ Mutex_t* mutex)
{
    (void)mutex;
    assert(false);
}

void
MutexUnlock(
    _In_ Mutex_t* mutex)
{
    (void)mutex;
    assert(false);
}

oserr_t
AllocatePhysicalMemory(
    _In_ size_t   mask,
    _In_ int      count,
    _Out_ paddr_t* pages)
{
    (void)mask;
    (void)count;
    (void)pages;
    assert(false);
    return OS_ENOTSUPPORTED;
}

void
FreePhysicalMemory(
    _In_ int              count,
    _In_ const uintptr_t* pages)
{
    // This harness only supplies existing backing pages.
    (void)count;
    (void)pages;
    abort();
}

oserr_t
MemorySpaceMap(
    _In_ MemorySpace_t* space,
    _In_ struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t* address)
{
    (void)space;
    (void)options;
    (void)address;
    assert(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
MemorySpaceUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    (void)space;
    (void)address;
    (void)length;
    assert(false);
    return OS_ENOTSUPPORTED;
}

struct MSAllocation*
MSAllocationLookup(
    _In_ struct MSContext* context,
    _In_ vaddr_t           address)
{
    // IPC validation must reject inaccessible buffers before mapping them.
    (void)context;
    (void)address;
    abort();
}

oserr_t
MSMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      address)
{
    (void)space;
    (void)options;
    (void)address;
    abort();
}

oserr_t
MSUnmap(
    _In_ MemorySpace_t* space,
    _In_ vaddr_t        address,
    _In_ size_t         length)
{
    (void)space;
    (void)address;
    (void)length;
    abort();
}

int
main(void)
{
    IPCAddress_t address;
    IPCMessage_t message;
    IPCMessage_t* messages[] = { &message, &message };
    struct SHMBuffer* destination;
    struct SHMBuffer* source;
    SHMHandle_t denied = { 0 };
    const char payload[] = "request";
    unsigned char largePayload[208] = { 0 };
    void* mapping;
    uuid_t regularHandleID;
    uuid_t uninitializedHandleID;
    uuid_t wrongTypeHandleID;
    uuid_t resolvedGlobalHandleID;
    unsigned int notifications;
    unsigned int deleted;

    assert(InitializeHandles() == OS_EOK);
    g_sender.GlobalHandleID = 11;
    g_receiver.GlobalHandleID = 12;
    g_sender.Capabilities = CAPABILITY_GRANT_CAPABILITY;
    g_sender.HandleTable = CreateHandleGrantsTable();
    g_receiver.HandleTable = CreateHandleGrantsTable();
    assert(g_sender.HandleTable != NULL && g_receiver.HandleTable != NULL);

    g_current = &g_receiver;
    g_destinationGlobalHandleID = __CreateBuffer(SHM_IPC, true);
    assert(RegisterHandlePath(g_destinationGlobalHandleID, "/test/ipc") == OS_EOK);
    destination = LookupHandle(g_destinationGlobalHandleID, HandleTypeSHM);
    g_current = &g_sender;
    g_senderHandleID = __CreateBuffer(SHM_IPC, true);
    source = LookupHandle(g_senderHandleID, HandleTypeSHM);
    regularHandleID = __CreateBuffer(SHM_COMMIT, true);
    uninitializedHandleID = __CreateBuffer(SHM_IPC, false);
    wrongTypeHandleID = CreateHandle(HandleTypeGeneric, NULL, NULL, NULL);

    assert(LookupHandleByPath("/test/ipc", &resolvedGlobalHandleID) == OS_EOK);
    assert(resolvedGlobalHandleID == g_destinationGlobalHandleID);
    assert(LookupHandle(resolvedGlobalHandleID, HandleTypeSHM) == NULL);
    assert(SHMAttach(resolvedGlobalHandleID, &denied) == OS_ENOENT);
    denied.ID = resolvedGlobalHandleID;
    assert(SHMMap(&denied, 0, 64, SHM_ACCESS_WRITE) == OS_ENOENT);
    assert(SHMKernelMapping(resolvedGlobalHandleID, &mapping) == OS_ENOENT);

    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(resolvedGlobalHandleID);
    message = (IPCMessage_t){ g_senderHandleID, &address, payload, sizeof(payload) };
    assert(IpcContextSendMultiple(messages, 2, NULL, NULL) == OS_EOK);
    assert(__References(g_destinationGlobalHandleID) == 1 && __References(g_senderHandleID) == 1);
    __Receive((streambuffer_t*)destination->KernelMapping, g_senderHandleID, payload, sizeof(payload));
    __Receive((streambuffer_t*)destination->KernelMapping, g_senderHandleID, payload, sizeof(payload));
    address.Type = IPC_ADDRESS_PATH;
    address.Data.Path = "/test/ipc";
    message.SenderHandleID = UUID_INVALID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EOK);
    __Receive((streambuffer_t*)destination->KernelMapping, UUID_INVALID, payload, sizeof(payload));

    // Replying uses the global address from the request, without adding a grant.
    g_current = &g_receiver;
    assert(LookupHandle(g_senderHandleID, HandleTypeSHM) == NULL);
    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(g_senderHandleID);
    message.SenderHandleID = g_destinationGlobalHandleID;
    resolvedGlobalHandleID = g_destinationGlobalHandleID;
    g_destinationGlobalHandleID = g_senderHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EOK);
    __Receive((streambuffer_t*)source->KernelMapping, resolvedGlobalHandleID, payload, sizeof(payload));
    g_destinationGlobalHandleID = resolvedGlobalHandleID;
    g_current = &g_sender;

    notifications = g_notifications;
    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(g_destinationGlobalHandleID);
    // A destination is globally reachable but cannot be advertised as our source.
    message.SenderHandleID = g_destinationGlobalHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    message.SenderHandleID = regularHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    message.SenderHandleID = uninitializedHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    message.SenderHandleID = wrongTypeHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    message.SenderHandleID = g_senderHandleID;
    address.Data.GlobalHandleID = regularHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    address.Data.GlobalHandleID = uninitializedHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    address.Data.GlobalHandleID = wrongTypeHandleID;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    address.Type = -1;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    address.Type = IPC_ADDRESS_PATH;
    address.Data.Path = "/missing";
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(g_destinationGlobalHandleID);
    message.Length = SIZE_MAX;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    message.Length = 257;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);
    assert(g_notifications == notifications);
    assert(__References(g_destinationGlobalHandleID) == 1 && __References(g_senderHandleID) == 1);
    assert(__References(regularHandleID) == 1 && __References(uninitializedHandleID) == 1);
    assert(__References(wrongTypeHandleID) == 1);

    // Fill the ring, then close both grants in the deterministic wait callback.
    message.Payload = largePayload;
    message.Length = sizeof(largePayload);
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EOK);
    g_waitingMessage = &message;
    message.Payload = payload;
    message.Length = sizeof(payload);
    deleted = g_deletedBuffers;
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EOK);
    assert(g_waitingMessage == NULL);
    __Receive((streambuffer_t*)destination->KernelMapping, g_senderHandleID, payload, sizeof(payload));
    __DrainJanitor();
    assert(g_deletedBuffers == deleted + 2);
    assert(LookupHandleByPath("/test/ipc", &resolvedGlobalHandleID) == OS_ENOENT);
    message.SenderHandleID = UUID_INVALID;
    message.Length = sizeof(payload);
    assert(IpcContextSendMultiple(messages, 1, NULL, NULL) == OS_EINCOMPLETE);

    DestroyHandleGrantsTable(g_sender.HandleTable);
    DestroyHandleGrantsTable(g_receiver.HandleTable);
    g_current = NULL;
    __DrainJanitor();
    assert(g_live == 0 && g_handles.element_count == 0);
    hashtable_destroy(&g_handles);
    hashtable_destroy(&g_handlemappings);
    puts("IPC grants: global routing, source validation, private mapping and send references passed");
    return 0;
}
