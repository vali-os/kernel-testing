/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include <stdlib.h>
#include <string.h>

static oserr_t
__CreateStorage(
    _In_    void*    device,
    _In_    uint64_t size,
    _Out_   void**   storageOut)
{
    struct GraphicsTestBackend* backend = device;
    void* bytes;

    backend->Memory.CreateCalls++;
    if (backend->Memory.FailCreate) {
        return OS_EOOM;
    }
    // The host fixture models private storage only. calloc makes unwritten
    // readback deterministic and prevents a recycled allocation exposing bytes.
    bytes = calloc(1, (size_t)size);
    if (bytes == NULL) {
        return OS_EOOM;
    }
    backend->Memory.LiveBuffers++;
    *storageOut = bytes;
    return OS_EOK;
}

static oserr_t
__DestroyStorage(
    _In_    void* device,
    _InOut_ void* storage)
{
    struct GraphicsTestBackend* backend = device;

    if (backend->Memory.BusyDestroy) {
        return OS_EBUSY;
    }
    free(storage);
    backend->Memory.LiveBuffers--;
    return OS_EOK;
}

static oserr_t
__BeginCpu(
    _In_    void*                device,
    _InOut_ void*                storage,
    _In_    uint64_t             offset,
    _In_    size_t               length,
    _In_    enum GraphicsCpuMode mode,
    _Out_   void**               addressOut)
{
    struct GraphicsTestBackend* backend = device;

    (void)length;
    (void)mode;
    backend->Memory.BeginCalls++;
    if (backend->Memory.FailBegin) {
        return OS_EBUSY;
    }
    backend->Memory.ActiveCpu++;
    *addressOut = (uint8_t*)storage + (size_t)offset;
    return OS_EOK;
}

static oserr_t
__EndCpu(
    _In_    void* device,
    _InOut_ void* storage)
{
    struct GraphicsTestBackend* backend = device;

    (void)storage;
    backend->Memory.EndCalls++;
    if (backend->Memory.FailEnd) {
        return OS_EDEVFAULT;
    }
    backend->Memory.ActiveCpu--;
    return OS_EOK;
}

const struct GraphicsStorageOps g_graphicsTestStorage = {
    .Create = __CreateStorage,
    .Destroy = __DestroyStorage,
    .BeginCpu = __BeginCpu,
    .EndCpu = __EndCpu
};
