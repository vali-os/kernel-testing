// Exercise the real kernel creation and userspace IPC creation paths together.
#include <machine.h>
#include <shm.h>
#include <handle.h>
#include <os/ipc.h>
#include <os/futex.h>
#include <ds/streambuffer.h>
#include "../kernel/memory/private.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static SystemMachine_t g_machine;
static MemorySpace_t   g_space;
static SHMHandle_t     g_handle;
static void*           g_resource;
static unsigned int   g_maps;
static unsigned int   g_publications;
static unsigned int   g_allocations;
static unsigned int   g_handles;
static const char     g_message[] = "sent during publication";
static _Alignas(streambuffer_t) unsigned char g_storage[4096];

void*
kmalloc(
    _In_ size_t length)
{
    void* allocation = malloc(length);
    CHECK(allocation != NULL);
    g_allocations++;
    return allocation;
}

void
kfree(
    _In_ void* allocation)
{
    CHECK(g_allocations != 0);
    g_allocations--;
    free(allocation);
}

SystemMachine_t*
GetMachine(void)
{
    return &g_machine;
}

MemorySpace_t*
GetCurrentMemorySpace(void)
{
    return &g_space;
}

size_t
GetMemorySpacePageSize(void)
{
    return 4096;
}

void
MutexConstruct(
    _In_ Mutex_t*     mutex,
    _In_ unsigned int flags)
{
    (void)mutex;
    (void)flags;
}

void
MutexDestruct(
    _In_ Mutex_t* mutex)
{
    (void)mutex;
}

void
FreePhysicalMemory(
    _In_ int              count,
    _In_ const uintptr_t* pages)
{
    (void)count;
    (void)pages;
}

uuid_t
CreateHandle(
    _In_ HandleType_t       type,
    _In_ HandleDestructorFn destructor,
    _In_ void*              resource)
{
    CHECK(type == HandleTypeSHM && destructor != NULL);
    g_resource = resource;
    g_handles++;
    return 1;
}

oserr_t
MemorySpaceMap(
    _In_  MemorySpace_t*                space,
    _In_  struct MemorySpaceMapOptions* options,
    _Out_ vaddr_t*                      mapping)
{
    CHECK(space == &g_space && options->Length == sizeof(g_storage));
    // The two guest mappings share the same physical storage.
    *mapping = (vaddr_t)g_storage;
    g_maps++;
    return OS_EOK;
}

oserr_t
RegisterHandlePath(
    _In_ uuid_t      handle,
    _In_ const char* path)
{
    streambuffer_t*           stream = (streambuffer_t*)g_storage;
    streambuffer_packet_ctx_t packet;
    streambuffer_rw_options_t options = { .flags = STREAMBUFFER_NO_BLOCK };
    size_t                   written;

    CHECK(handle == 1 && strcmp(path, "/test/ipc-publication") == 0);
    CHECK(g_maps == 2);
    CHECK(stream->capacity == sizeof(g_storage) - sizeof(streambuffer_t));
    CHECK(stream->options == (STREAMBUFFER_GLOBAL | STREAMBUFFER_MULTIPLE_WRITERS));

    // Simulate another CPU sending as soon as lookup can find the endpoint.
    // The creator has not returned from SHMCreate yet.
    written = streambuffer_write_packet_start(
        stream,
        sizeof(g_message),
        &options,
        &packet
    );
    CHECK(written == sizeof(g_message));
    streambuffer_write_packet_data((void*)g_message, sizeof(g_message), &packet);
    streambuffer_write_packet_end(&packet);
    g_publications++;
    return OS_EOK;
}

oserr_t
IpcTestSHMCreate(
    _In_  SHM_t*      shm,
    _Out_ OSHandle_t* handle)
{
    oserr_t status;

    // Only the syscall boundary is replaced. Both creation implementations
    // and the stream buffer operations are the production code.
    status = SHMCreate(shm, &g_handle);
    if (status != OS_EOK) {
        return status;
    }
    handle->ID = g_handle.ID;
    handle->Payload = &g_handle;
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    return ((SHMHandle_t*)handle->Payload)->Buffer;
}

size_t
SHMBufferLength(
    _In_ OSHandle_t* handle)
{
    return ((SHMHandle_t*)handle->Payload)->Length;
}

void*
dsalloc(
    _In_ size_t length)
{
    return malloc(length);
}

void
dsfree(
    _In_ void* allocation)
{
    free(allocation);
}

void
dswait(
    _In_ OSFutexParameters_t* options,
    _In_ OSAsyncContext_t*    async)
{
    (void)options;
    (void)async;
    CHECK(0);
}

void
dswake(
    _In_ OSFutexParameters_t* options)
{
    (void)options;
    CHECK(0);
}

int
main(void)
{
    IPCAddress_t             address = { .Type = IPC_ADDRESS_PATH, .Data.Path = "/test/ipc-publication" };
    OSHandle_t               handle;
    streambuffer_packet_ctx_t packet;
    streambuffer_rw_options_t options = { .flags = STREAMBUFFER_NO_BLOCK };
    char                     received[sizeof(g_message)];
    size_t                   receivedLength;
    SHM_t                    small = { .Flags = SHM_IPC, .Size = sizeof(streambuffer_t) };

    // Allocated pages need not be zeroed. A missing initialization must fail
    // reliably instead of depending on what the last owner left behind.
    memset(g_storage, 0xa5, sizeof(g_storage));
    CHECK(IPCContextCreate(sizeof(g_storage), &address, &handle) == OS_EOK);
    CHECK(g_publications == 1);

    // Userspace must preserve the message sent before creation returned.
    receivedLength = streambuffer_read_packet_start(
        (streambuffer_t*)g_storage,
        &options,
        &packet
    );
    CHECK(receivedLength == sizeof(g_message));
    streambuffer_read_packet_data(received, sizeof(received), &packet);
    streambuffer_read_packet_end(&packet);
    CHECK(memcmp(received, g_message, sizeof(received)) == 0);
    kfree(g_resource);

    // Reject a buffer without room for data before mapping or publishing it.
    CHECK(SHMCreate(&small, &g_handle) == OS_EINVALPARAMS);
    CHECK(g_maps == 2 && g_publications == 1);
    CHECK(g_handles == 1 && g_allocations == 0);
    puts("IPC publication: initialized before lookup, early message preserved, small buffer rejected");
    return 0;
}
