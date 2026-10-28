/** Production IPC/SHM/streambuffers, with host memory and scheduler fixtures. */
#define IPC_SECURITY_TEST
#define main __MemoryOwnershipTests
#include "shm_memory_ownership_test.c"
#undef main
#include <ds/streambuffer.h>
#include <ipc_context.h>
#include <futex.h>
#include <threading.h>
#include <os/ipc.h>

static struct MSContext g_sender, g_receiver;
static streambuffer_t* g_drain;
static streambuffer_t* g_corrupt;
static const void* g_badPointer;
static oserr_t g_forkResult = OS_EFORKED;
static unsigned g_forks, g_waits, g_notifications;
static IPCMessageContext_t g_drained;
static uuid_t g_pathDestination;

oserr_t ArchSHMTypeToPageMask(enum OSMemoryConformity conformity, size_t* mask)
{ (void)conformity; *mask = SIZE_MAX; return OS_EOK; }
oserr_t RegisterHandlePath(uuid_t handle, const char* path)
{ (void)handle; (void)path; return OS_EEXISTS; }
oserr_t LookupHandleByPath(const char* path, uuid_t* handle)
{ assert(!strcmp(path, "/test")); *handle = g_pathDestination; return OS_EOK; }
oserr_t MarkHandle(uuid_t handle, unsigned int flags)
{ (void)handle; (void)flags; g_notifications++; return OS_EOK; }
void* SHMBuffer(OSHandle_t* handle) { return handle->Payload; }
oserr_t MemorySpaceCommit(MemorySpace_t* space, vaddr_t address, paddr_t* pages,
                          size_t length, size_t mask, unsigned int flags)
{ (void)space; (void)address; (void)pages; (void)length; (void)mask; (void)flags; abort(); }

oserr_t
MemorySpaceCopyUser(void* user, void* kernel, size_t length, bool toUser)
{
    if (user == NULL || user == g_badPointer) {
        return OS_EINVALPARAMS;
    }
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    if (g_corrupt != NULL) {
        g_corrupt->capacity = SIZE_MAX;
        g_corrupt->options = STREAMBUFFER_DISABLED;
    }
    return OS_EOK;
}

oserr_t
ThreadFork(OSAsyncContext_t* context)
{
    (void)context;
    g_forks++;
    atomic_store(&g_sender.Security.Capabilities, CAPABILITY_ALL);
    return g_forkResult;
}

oserr_t
FutexWait(OSAsyncContext_t* async, atomic_int* futex, int expected, int flags,
          atomic_int* second, int count, int operation, OSTimestamp_t* deadline)
{
    streambuffer_rw_options_t options = {.flags = STREAMBUFFER_NO_BLOCK};
    streambuffer_packet_ctx_t packet;
    (void)futex; (void)expected; (void)flags; (void)second;
    (void)count; (void)operation; (void)deadline;
    assert(async == NULL);
    g_waits++;
    if (g_drain == NULL) {
        return OS_ETIMEOUT;
    }
    assert(streambuffer_read_packet_start(g_drain, &options, &packet) != 0);
    streambuffer_read_packet_data(&g_drained, sizeof(g_drained), &packet);
    streambuffer_read_packet_end(&packet);
    g_drain = NULL;
    return OS_EOK;
}
void dswait(OSFutexParameters_t* parameters, OSAsyncContext_t* context)
{ (void)parameters; (void)context; assert(!"Unexpected blocking stream helper"); }
void dswake(OSFutexParameters_t* parameters) { (void)parameters; }
void usched_wait_async(void) { assert(!"Unexpected userspace async wait"); }

static void
__Receive(OSHandle_t* endpoint, unsigned expectedLength, uint32_t expectedCapabilities)
{
    IPCMessageContext_t credentials;
    unsigned char output[32];
    size_t received;

    g_space.Context = &g_receiver;
    assert(IPCContextRecvWithCredentials(endpoint, output, sizeof(output), IPC_DONTWAIT,
        NULL, &credentials, &received) == OS_EOK);
    assert(received == expectedLength);
    assert(credentials.Sender == (expectedLength ? 11 : 0));
    assert(credentials.Capabilities == expectedCapabilities);
    g_space.Context = &g_sender;
}

int
main(void)
{
    SHM_t configuration = {.Flags = SHM_IPC, .Size = PAGE_SIZE};
    SHMHandle_t reply, destination, attached;
    OSHandle_t receiver;
    IPCAddress_t address;
    IPCMessage_t message, fullMessage;
    IPCMessage_t* batch[] = {&message, &message};
    OSAsyncContext_t async = {0};
    unsigned char payload[PAGE_SIZE] = {0};
    char path[4096] = "/test";
    streambuffer_t* stream;
    unsigned heapCalls, baseline, notifications;
    int sgCount;
    SHMHandle_t forged;
    struct SHMDeviceContext* device;
    struct SHMDeviceMapping* deviceMapping = NULL;
    unsigned int position;

    __MemoryOwnershipTests();
    SystemMemoryConstruct(&g_machine.PhysicalMemory, 0, 0, PAGE_SIZE, 0);
    assert(SystemMemoryInitializeDeviceRegion(&g_machine.PhysicalMemory, REGION_BASE,
        REGION_LENGTH, true, g_storage, sizeof(g_storage)) == OS_EOK);
    g_sender.Security.Identity = 11;
    g_receiver.Security.Identity = 22;
    atomic_init(&g_sender.Security.Capabilities, CAPABILITY_GRANT_CAPABILITY);
    g_space.Context = &g_sender;
    assert(SHMCreate(&configuration, &reply) == OS_EOK);
    g_space.Context = &g_receiver;
    assert(SHMCreate(&configuration, &destination) == OS_EOK);
    assert(destination.Buffer != NULL && destination.Length == PAGE_SIZE);
    assert(SHMAttach(destination.ID, &attached) == OS_EOK);
    assert(DestroyHandle(attached.ID) == OS_EINCOMPLETE);
    receiver = (OSHandle_t){.ID = destination.ID, .Payload = destination.Buffer};
    stream = destination.Buffer;
    g_space.Context = &g_sender;
    baseline = g_heapLive;

    assert(SHMAttach(destination.ID, &attached) == OS_EPERMISSIONS);
    assert(g_handles[destination.ID].References == 1);
    assert(SHMMap(&destination, 0, 16, SHM_ACCESS_WRITE) == OS_EPERMISSIONS);
    assert(SHMBuildSG(destination.ID, &sgCount, NULL) == OS_EPERMISSIONS);
    assert(SHMCommit(destination.ID, destination.Buffer, destination.Buffer, 1) == OS_EPERMISSIONS);
    assert(SHMConform(destination.ID, NULL, 0, 0, 0, 1, &attached) == OS_EPERMISSIONS);
    forged = reply;
    forged.SourceID = destination.ID;
    forged.SourceFlags = SHM_CONFORM_BACKFILL_ON_UNMAP;
    assert(SHMUnmap(&forged, (vaddr_t)reply.Buffer, reply.Length) == OS_EPERMISSIONS);
    device = __CreateContext();
    assert(SHMDeviceMap(device, destination.ID, 0, 1, DMAToDevice,
        &deviceMapping) == OS_EPERMISSIONS);
    SHMDeviceContextRelease(&device);

    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(destination.ID);
    message = (IPCMessage_t){reply.ID, &address, payload, 5};
    message.SenderHandle = destination.ID;
    assert(IpcContextSendMultiple(batch, 1, NULL, &async) == OS_EPERMISSIONS);
    message.SenderHandle = reply.ID;
    heapCalls = g_heapCalls;
    assert(IpcContextSendMultiple(batch, 1, NULL, &async) == OS_EOK);
    assert(g_forks == 0 && g_heapCalls == heapCalls);
    atomic_store(&g_sender.Security.Capabilities, CAPABILITY_ALL);
    __Receive(&receiver, 5, CAPABILITY_GRANT_CAPABILITY);

    g_pathDestination = destination.ID;
    address.Type = IPC_ADDRESS_PATH;
    address.Data.Path = path;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    __Receive(&receiver, 5, CAPABILITY_ALL);
    g_badPointer = path;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EINVALPARAMS);
    g_badPointer = NULL;
    memset(path, 'x', sizeof(path));
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EINVALPARAMS);
    address = (IPCAddress_t)IPC_ADDRESS_HANDLE_INIT(destination.ID);

    // Mutable shared capacity/options cannot redirect or suppress kernel writes.
    position = stream->capacity - 11;
    atomic_store(&stream->producer_index, position);
    atomic_store(&stream->producer_comitted_index, position);
    atomic_store(&stream->consumer_index, position);
    atomic_store(&stream->consumer_comitted_index, position);
    g_corrupt = stream;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    g_corrupt = NULL;
    stream->capacity = PAGE_SIZE - sizeof(streambuffer_t);
    stream->options = STREAMBUFFER_GLOBAL | STREAMBUFFER_MULTIPLE_WRITERS;
    __Receive(&receiver, 5, CAPABILITY_ALL);

    message.Payload = &(IPCMessageContext_t){99, CAPABILITY_ALL, destination.ID};
    message.Length = sizeof(IPCMessageContext_t);
    atomic_store(&g_sender.Security.Capabilities, 0);
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    __Receive(&receiver, sizeof(IPCMessageContext_t), 0);
    message.Payload = payload;
    message.Length = 5;
    notifications = g_notifications;
    g_badPointer = payload;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EINVALPARAMS);
    g_badPointer = NULL;
    assert(g_notifications == notifications + 1);
    __Receive(&receiver, 0, 0);
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    __Receive(&receiver, 5, 0);

    // Failure in the second physical span also discards the whole packet.
    position = stream->capacity - sizeof(size_t) - sizeof(IPCMessageContext_t) - 2;
    atomic_store(&stream->producer_index, position);
    atomic_store(&stream->producer_comitted_index, position);
    atomic_store(&stream->consumer_index, position);
    atomic_store(&stream->consumer_comitted_index, position);
    g_badPointer = payload + 2;
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EINVALPARAMS);
    g_badPointer = NULL;
    __Receive(&receiver, 0, 0);

    // A partially submitted batch resumes once, with the original snapshot.
    fullMessage = message;
    fullMessage.Length = stream->capacity - sizeof(size_t) - sizeof(IPCMessageContext_t);
    batch[0] = &fullMessage;
    atomic_store(&g_sender.Security.Capabilities, CAPABILITY_GRANT_CAPABILITY);
    heapCalls = g_heapCalls;
    g_drain = stream;
    assert(IpcContextSendMultiple(batch, 2, NULL, &async) == OS_EOK);
    assert(g_forks == 1 && g_waits == 1 && g_heapCalls == heapCalls + 1);
    assert(g_drained.Capabilities == CAPABILITY_GRANT_CAPABILITY);
    __Receive(&receiver, 5, CAPABILITY_GRANT_CAPABILITY);
    __Receive(&receiver, 0, 0);

    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    batch[0] = &message;
    g_failHeapAt = g_heapCalls + 1;
    assert(IpcContextSendMultiple(batch, 1, NULL, &async) == OS_EOOM);
    g_failHeapAt = 0;
    g_forkResult = OS_EOOM;
    assert(IpcContextSendMultiple(batch, 1, NULL, &async) == OS_EOOM);
    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_ETIMEOUT);
    assert(g_heapLive == baseline && g_handles[reply.ID].References == 1);
    assert(g_handles[destination.ID].References == 1);
    __Receive(&receiver, 32, CAPABILITY_ALL);

    assert(IpcContextSendMultiple(batch, 1, NULL, NULL) == OS_EOK);
    assert(SHMDetach(&reply) == OS_EOK);
    __Receive(&receiver, 5, CAPABILITY_ALL);
    g_space.Context = &g_receiver;
    assert(SHMDetach(&destination) == OS_EOK);
    __ExpectEmpty();
    puts("IPC: userspace receive, owner isolation, trusted bounds, snapshots, discard and async cleanup passed");
    return 0;
}
