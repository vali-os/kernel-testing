/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "shm_host.h"
#include <os/handle.h>
#include <internal/_syscalls.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#define SHM_HOST_CAPACITY 64U

/** Host memory models attachment/mapping state, not kernel page or cache behavior. */
struct __HostBuffer {
    uuid_t ID;
    void* Bytes;
    size_t Size;
    bool Unmapped;
};

/** Every access session remains associated with its attachment until Close. */
struct __HostAccess {
    uuid_t ID;
    struct __HostBuffer* Buffer;
    bool Active;
};

static struct __HostBuffer g_buffers[SHM_HOST_CAPACITY];
static struct __HostAccess g_accesses[SHM_HOST_CAPACITY];
static uuid_t g_nextID = 1;
struct GraphicsShmHostState g_graphicsShmHost;
extern const OSHandleOps_t g_shmOps;

static struct __HostBuffer*
__FindBuffer(
    _In_    uuid_t id)
{
    uint32_t i;

    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        if (g_buffers[i].ID == id) {
            return &g_buffers[i];
        }
    }
    assert(0 && "invalid SHM attachment");
    return NULL;
}

static struct __HostAccess*
__FindAccess(
    _In_    uuid_t id)
{
    uint32_t i;

    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        if (g_accesses[i].ID == id) {
            return &g_accesses[i];
        }
    }
    assert(0 && "invalid SHM access session");
    return NULL;
}

static oserr_t
__Create(
    _In_    const SHM_t* parameters,
    _Out_   SHMHandle_t* result)
{
    struct __HostBuffer* buffer;
    uint32_t i;
    size_t rounded;

    g_graphicsShmHost.CreateCalls++;
    assert(parameters->Flags == (SHM_COMMIT | SHM_CLEAN | SHM_PRIVATE));
    assert(parameters->Access == (SHM_ACCESS_READ | SHM_ACCESS_WRITE));
    assert(parameters->Size != 0);
    if (g_graphicsShmHost.FailCreate) {
        return OS_EOOM;
    }
    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        buffer = &g_buffers[i];
        if (buffer->ID != 0) {
            continue;
        }
        rounded = (parameters->Size + 4095) & ~(size_t)4095;
        buffer->Bytes = calloc(1, rounded);
        assert(buffer->Bytes != NULL);
        buffer->ID = g_nextID++;
        buffer->Size = parameters->Size;
        *result = (SHMHandle_t) {
            .ID = buffer->ID, .Capacity = rounded,
            .Buffer = buffer->Bytes, .Length = rounded
        };
        g_graphicsShmHost.LiveBuffers++;
        return OS_EOK;
    }
    return OS_EOOM;
}

static oserr_t
__Open(
    _In_    uuid_t  bufferID,
    _In_    size_t  offset,
    _In_    size_t  length,
    _Out_   uuid_t* result)
{
    struct __HostBuffer* buffer = __FindBuffer(bufferID);
    uint32_t i;

    g_graphicsShmHost.OpenCalls++;
    assert(!buffer->Unmapped && offset == 0 && length == buffer->Size);
    if (g_graphicsShmHost.FailOpen) {
        return OS_EOOM;
    }
    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        if (g_accesses[i].ID == 0) {
            g_accesses[i].ID = g_nextID++;
            g_accesses[i].Buffer = buffer;
            *result = g_accesses[i].ID;
            g_graphicsShmHost.LiveSessions++;
            return OS_EOK;
        }
    }
    return OS_EOOM;
}

static oserr_t
__Begin(
    _In_    struct __HostAccess*                   access,
    _In_    const struct SHMAccessBeginParameters* parameters)
{
    uint32_t i;

    g_graphicsShmHost.BeginCalls++;
    assert(parameters->Kind == SHMAccessCpu && parameters->Mode == SHMWaitNonBlocking);
    assert(parameters->Offset == 0 && parameters->Length == 0);
    assert(!access->Buffer->Unmapped && !access->Active);
    if (g_graphicsShmHost.FailBegin) {
        return OS_EBUSY;
    }
    // This fixture models conservative whole-buffer exclusion. It deliberately
    // has no DMA/cache simulation; those require native kernel/device testing.
    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        if (g_accesses[i].Active && g_accesses[i].Buffer == access->Buffer) {
            return OS_EBUSY;
        }
    }
    access->Active = true;
    g_graphicsShmHost.ActiveSessions++;
    if (parameters->Write) {
        g_graphicsShmHost.WriteBegins++;
    } else {
        g_graphicsShmHost.ReadBegins++;
    }
    return OS_EOK;
}

static oserr_t
__Release(
    _InOut_ SHMHandle_t* handle)
{
    struct __HostBuffer* buffer = __FindBuffer(handle->ID);
    uint32_t i;

    g_graphicsShmHost.ReleaseCalls++;
    for (i = 0; i < SHM_HOST_CAPACITY; i++) {
        assert(g_accesses[i].Buffer != buffer);
    }
    // Model an unmap succeeding before detach fails. The same payload must be
    // retried; querying or reopening the mapping after this point is invalid.
    handle->Buffer = NULL;
    handle->Length = 0;
    buffer->Unmapped = true;
    if (g_graphicsShmHost.FailReleaseAfterUnmap) {
        return OS_EUNKNOWN;
    }
    free(buffer->Bytes);
    memset(buffer, 0, sizeof(*buffer));
    g_graphicsShmHost.LiveBuffers--;
    return OS_EOK;
}

SCTYPE
_syscall(
    _In_    SCTYPE function,
    _In_    SCTYPE arg0,
    _In_    SCTYPE arg1,
    _In_    SCTYPE arg2,
    _In_    SCTYPE arg3,
    _In_    SCTYPE arg4)
{
    struct __HostAccess* access;

    (void)arg4;
    // These numbers are the SHM entries in internal/_syscalls.h. The real libos
    // wrappers construct every request; only the kernel boundary is replaced.
    switch (function) {
        case 54:
            return __Create((void*)(uintptr_t)arg0, (void*)(uintptr_t)arg1);
        case 60:
            return __Release((void*)(uintptr_t)arg0);
        case 83:
            return __Open((uuid_t)arg0, (size_t)arg1, (size_t)arg2, (void*)(uintptr_t)arg3);
        case 84:
            assert(arg0 == 0);
            return __Begin(__FindAccess((uuid_t)arg1), (void*)(uintptr_t)arg2);
        case 85:
            access = __FindAccess((uuid_t)arg0);
            assert(arg1 == SHMAccessCpu && arg2 == 0 && access->Active);
            g_graphicsShmHost.EndCalls++;
            if (g_graphicsShmHost.FailEnd) {
                return OS_EDEVFAULT;
            }
            access->Active = false;
            g_graphicsShmHost.ActiveSessions--;
            return OS_EOK;
        case 86:
            access = __FindAccess((uuid_t)arg0);
            assert(!access->Active);
            g_graphicsShmHost.CloseCalls++;
            if (g_graphicsShmHost.FailClose) {
                return OS_EBUSY;
            }
            memset(access, 0, sizeof(*access));
            g_graphicsShmHost.LiveSessions--;
            return OS_EOK;
        default:
            assert(0 && "unexpected kernel request in graphics SHM fixture");
            return OS_ENOTSUPPORTED;
    }
}

oserr_t
OSHandleWrap(
    _In_    uuid_t            id,
    _In_    enum OSHandleType type,
    _In_    void*             payload,
    _In_    bool              managed,
    _Out_   struct OSHandle*  handle)
{
    assert(managed && type == OSHANDLE_SHM);
    *handle = (OSHandle_t) { .ID = id, .Type = type, .Payload = payload };
    return OS_EOK;
}

oserr_t
OSHandleRelease(
    _InOut_ struct OSHandle* handle)
{
    oserr_t status;

    // This fixture has exactly one local attachment reference. Use the real SHM
    // callbacks while leaving registry aliases to the existing libos tests.
    status = g_shmOps.Release(handle);
    if (status == OS_EOK) {
        g_shmOps.Destroy(handle);
        memset(handle, 0, sizeof(*handle));
    }
    return status;
}

void
usched_wait_async(void)
{
    assert(0 && "SHM CPU access must never wait on the graphics executor");
}

void
GraphicsShmHostReset(void)
{
    assert(g_graphicsShmHost.LiveBuffers == 0);
    assert(g_graphicsShmHost.LiveSessions == 0);
    assert(g_graphicsShmHost.ActiveSessions == 0);
    memset(&g_graphicsShmHost, 0, sizeof(g_graphicsShmHost));
}
