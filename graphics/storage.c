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
__ReadStorage(
    _In_    void*    device,
    _In_    void*    storage,
    _In_    uint64_t offset,
    _Out_   void*    bytes,
    _In_    size_t   length)
{
    struct GraphicsTestBackend* backend = device;

    memcpy(bytes, (uint8_t*)storage + (size_t)offset, length);
    // Returning an error after copying checks that RPC replies clear partial
    // readback. Native access errors must not leak uninitialized output bytes.
    return backend->Memory.FailRead ? OS_EDEVFAULT : OS_EOK;
}

static oserr_t
__WriteStorage(
    _In_    void*       device,
    _InOut_ void*       storage,
    _In_    uint64_t    offset,
    _In_    const void* bytes,
    _In_    size_t      length)
{
    struct GraphicsTestBackend* backend = device;

    memcpy((uint8_t*)storage + (size_t)offset, bytes, length);
    return backend->Memory.FailWrite ? OS_EDEVFAULT : OS_EOK;
}

const struct GraphicsStorageOps g_graphicsTestStorage = {
    .Create = __CreateStorage,
    .Destroy = __DestroyStorage,
    .Read = __ReadStorage,
    .Write = __WriteStorage
};
