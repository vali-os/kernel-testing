/** Shared host fake for kernel-owned SHM, with live allocation accounting. */
#include "net_shm_mock.h"
#include <os/handle.h>
#include <os/memory.h>
#include <os/shm.h>
#include <stdlib.h>
#include <string.h>

struct FakeMemory { void* Data; size_t Size; unsigned References; uuid_t Id; struct FakeMemory* Next; };
struct NetTestShmState g_netShm;
static struct FakeMemory* g_memory;
static uuid_t g_nextId = 1;
size_t MemoryPageSize(void) { return 4096; }
oserr_t SHMCreate(SHM_t* request, OSHandle_t* handle)
{
    g_netShm.Calls++;
    if (g_netShm.FailCall == g_netShm.Calls) return OS_EOOM;
    if (request->Flags != (SHM_DEVICE | SHM_CLEAN) || request->Access != (SHM_ACCESS_READ | SHM_ACCESS_WRITE)) abort();
    struct FakeMemory* memory = calloc(1, sizeof(*memory));
    if (!memory) return OS_EOOM;
    if (posix_memalign(&memory->Data, 4096, request->Size)) { free(memory); return OS_EOOM; }
    memset(memory->Data, 0, request->Size);
    memory->Size = request->Size; memory->References = 1; memory->Id = g_nextId++;
    memory->Next = g_memory; g_memory = memory;
    *handle = (OSHandle_t) { .ID = memory->Id, .Payload = memory };
    g_netShm.Live++;
    return OS_EOK;
}
void* SHMBuffer(OSHandle_t* handle) { return ((struct FakeMemory*)handle->Payload)->Data; }
size_t SHMBufferLength(OSHandle_t* handle) { return ((struct FakeMemory*)handle->Payload)->Size; }
void* NetTestShmData(uuid_t id)
{
    for (struct FakeMemory* p = g_memory; p; p = p->Next) if (p->Id == id) return p->Data;
    return NULL;
}
void OSHandleDestroy(OSHandle_t* handle)
{
    if (!handle->Payload || !g_netShm.Live) abort();
    struct FakeMemory* memory = handle->Payload;
    if (--memory->References) { memset(handle, 0, sizeof(*handle)); return; }
    struct FakeMemory** link = &g_memory;
    while (*link && *link != memory) link = &(*link)->Next;
    if (!*link) abort();
    *link = memory->Next;
    free(memory->Data); free(memory); memset(handle, 0, sizeof(*handle));
    g_netShm.Live--;
}

/** Host attachments share the allocation but own a separate lifetime reference.
 * This models close detaching the fake before netd frees the final pool handle.
 * Only zero-offset mappings used by the fake are supported by this test shim.
 */
oserr_t SHMAttach(uuid_t id, OSHandle_t* handle)
{
    for (struct FakeMemory* p = g_memory; p; p = p->Next) if (p->Id == id) {
        p->References++;
        *handle = (OSHandle_t){ .ID = id, .Payload = p };
        return OS_EOK;
    }
    return OS_ENOENT;
}
oserr_t SHMMap(OSHandle_t* handle, size_t offset, size_t length, unsigned int access)
{
    return !offset && length <= SHMBufferLength(handle) &&
        (access == SHM_ACCESS_READ || access == SHM_ACCESS_WRITE || access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE)) ? OS_EOK : OS_EINVALPARAMS;
}
size_t SHMBufferCapacity(OSHandle_t* handle) { return SHMBufferLength(handle); }
