/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include <stdlib.h>

/** A distinct allocation makes double release and leaked contexts observable. */
struct __TestContext {
    uint32_t Flags;
};

static oserr_t
__CreateContext(
    _In_    void*    device,
    _In_    uint32_t flags,
    _Out_   void**   contextOut)
{
    struct GraphicsTestBackend* backend = device;
    struct __TestContext*       context;

    backend->CreateCalls++;
    if (backend->FailCreate) {
        return OS_EOOM;
    }
    context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return OS_EOOM;
    }
    context->Flags = flags;
    backend->LiveContexts++;
    *contextOut = context;
    return OS_EOK;
}

static oserr_t
__DestroyContext(
    _In_    void* device,
    _InOut_ void* context)
{
    struct GraphicsTestBackend* backend = device;

    backend->DestroyCalls++;
    if (backend->BusyDestroy) {
        // The pointer stays valid. Tests clear the busy flag and retry through
        // the same cleanup path used after a real backend reports progress.
        return OS_EBUSY;
    }
    free(context);
    backend->LiveContexts--;
    return OS_EOK;
}

static const struct GraphicsContextOps g_contextOps = {
    .Create = __CreateContext,
    .Destroy = __DestroyContext
};

oserr_t
GraphicsTestCreate(
    _InOut_ struct GraphicsTestBackend* backend,
    _In_    uint64_t                    generation,
    _Out_   struct GraphicsService**    serviceOut)
{
    struct GraphicsDeviceInfo info = {
        .DeviceId = 1,
        .DeviceGeneration = generation,
        .Backend = GraphicsBackendTest,
        .MaxSessions = 2,
        .MaxContextsPerSession = 2,
        .Resources = {
            .MaxBuffers = 4,
            .MaxJobs = 8,
            .MaxBufferBytes = 4096,
            .MaxSessionBytes = 8192
        }
    };
    struct GraphicsBackend operations = {
        .Contexts = &g_contextOps,
        .Device = backend,
        .Storage = &g_graphicsTestStorage,
        .Jobs = &g_graphicsTestJobs
    };

    return GraphicsServiceCreate(&info, &operations, serviceOut);
}
