/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include "../../modules/graphics/common/private.h"
#include <assert.h>
#include <stdio.h>

static const struct GraphicsCaller g_first = { .Principal = 11, .Endpoint = 101 };
static const struct GraphicsCaller g_second = { .Principal = 22, .Endpoint = 202 };

static struct GraphicsSessionRef
__Open(
    _In_ struct GraphicsService* service,
    _In_ const struct GraphicsCaller* caller)
{
    struct GraphicsSessionRef reference = { 0 };

    assert(GraphicsSessionOpen(service, caller, 1, 1, 0, &reference) == OS_EOK);
    return reference;
}

static struct GraphicsObjectRef
__Create(
    _In_ struct GraphicsService* service,
    _In_ const struct GraphicsCaller* caller,
    _In_ const struct GraphicsSessionRef* session,
    _In_ uint64_t creationId)
{
    struct GraphicsObjectRef reference = { 0 };

    assert(GraphicsContextCreate(service, caller, session, creationId, 0, &reference) == OS_EOK);
    return reference;
}

static void
__AuthenticationAndReferences(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsCaller attacker;
    struct GraphicsSessionRef first;
    struct GraphicsSessionRef second;
    struct GraphicsSessionRef stale;
    struct GraphicsSessionRef retry;
    struct GraphicsSessionInfo sessionInfo;
    struct GraphicsContextInfo contextInfo;
    struct GraphicsObjectRef context;
    struct GraphicsObjectRef other;
    struct GraphicsObjectRef invalid;

    assert(GraphicsTestCreate(&backend, 7, &service) == OS_EOK);
    first = __Open(service, &g_first);
    second = __Open(service, &g_second);
    retry = __Open(service, &g_first);
    assert(retry.Slot == first.Slot && retry.Generation == first.Generation);

    // Test principal and endpoint independently. Checking only one would allow
    // another process or another connection to use a copied session reference.
    attacker = g_first;
    attacker.Principal = 0;
    assert(GraphicsSessionQuery(service, &attacker, &first, &sessionInfo) == OS_EPERMISSIONS);
    attacker.Principal = g_second.Principal;
    assert(GraphicsSessionQuery(service, &attacker, &first, &sessionInfo) == OS_EPERMISSIONS);
    attacker = g_first;
    attacker.Endpoint = g_second.Endpoint;
    assert(GraphicsSessionQuery(service, &attacker, &first, &sessionInfo) == OS_EPERMISSIONS);
    assert(GraphicsSessionClose(service, &g_second, &first) == OS_EPERMISSIONS);

    context = __Create(service, &g_first, &first, 1);
    other = __Create(service, &g_second, &second, 1);
    assert(context.Slot == other.Slot && context.Generation != other.Generation);
    assert(GraphicsContextQuery(service, &g_first, &first, &other, &contextInfo) == OS_ENOENT);
    invalid = context;
    invalid.Type++;
    assert(GraphicsContextRelease(service, &g_first, &first, &invalid) == OS_ENOENT);
    invalid = context;
    invalid.Slot = UINT32_MAX;
    assert(GraphicsContextRelease(service, &g_first, &first, &invalid) == OS_ENOENT);

    assert(GraphicsContextRelease(service, &g_first, &first, &context) == OS_EOK);
    invalid = __Create(service, &g_first, &first, 2);
    assert(invalid.Slot == context.Slot && invalid.Generation != context.Generation);
    assert(GraphicsContextQuery(service, &g_first, &first, &context, &contextInfo) == OS_ENOENT);
    assert(GraphicsSessionClose(service, &g_first, &first) == OS_EOK);
    stale = first;
    first = __Open(service, &g_first);
    assert(first.Slot == stale.Slot && first.Generation != stale.Generation);
    assert(GraphicsSessionClose(service, &g_first, &stale) == OS_ENOENT);
    stale = first;
    stale.DeviceGeneration++;
    assert(GraphicsSessionQuery(service, &g_first, &stale, &sessionInfo) == OS_ENOENT);
    assert(GraphicsContextQuery(service, &g_second, &second, &other, &contextInfo) == OS_EOK);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend.LiveContexts == 0);
}

static void
__LimitsAndReplay(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsCaller third = { .Principal = 33, .Endpoint = 303 };
    struct GraphicsSessionRef first;
    struct GraphicsSessionRef second;
    struct GraphicsSessionRef unused = { 0 };
    struct GraphicsObjectRef context;
    struct GraphicsObjectRef retry;
    struct GraphicsObjectRef secondContext;

    assert(GraphicsTestCreate(&backend, 8, &service) == OS_EOK);
    assert(GraphicsSessionOpen(service, &g_first, 2, 1, 0, &unused) == OS_ENOENT);
    assert(GraphicsSessionOpen(service, &g_first, 1, 2, 0, &unused) == OS_ENOTSUPPORTED);
    assert(GraphicsSessionOpen(service, &g_first, 1, 1, 2, &unused) == OS_ENOTSUPPORTED);
    first = __Open(service, &g_first);
    second = __Open(service, &g_second);
    assert(GraphicsSessionOpen(service, &third, 1, 1, 0, &unused) == OS_EBUSY);
    assert(unused.Slot == 0);
    assert(GraphicsContextCreate(service, &g_first, &first, 0, 0, &retry) == OS_EINVALPARAMS);
    assert(GraphicsContextCreate(service, &g_first, &first, 1, 1, &retry) == OS_ENOTSUPPORTED);

    // A backend failure consumes neither capacity nor the creation ID. The
    // identical request must be usable once the allocation failure is resolved.
    backend.FailCreate = true;
    assert(GraphicsContextCreate(service, &g_first, &first, 1, 0, &retry) == OS_EOOM);
    backend.FailCreate = false;
    context = __Create(service, &g_first, &first, 1);
    secondContext = __Create(service, &g_first, &first, 2);
    retry = __Create(service, &g_first, &first, 1);
    assert(retry.Generation == context.Generation);
    assert(backend.CreateCalls == 3 && backend.LiveContexts == 2);
    assert(GraphicsContextCreate(service, &g_first, &first, 1, 1, &retry) == OS_EPROTOCOL);
    assert(GraphicsContextCreate(service, &g_first, &first, 3, 0, &retry) == OS_EBUSY);
    assert(GraphicsContextRelease(service, &g_first, &first, &context) == OS_EOK);
    assert(GraphicsContextCreate(service, &g_first, &first, 1, 0, &retry) == OS_ENOENT);
    retry = __Create(service, &g_first, &first, 3);
    assert(retry.Generation != context.Generation);
    assert(GraphicsSessionClose(service, &g_second, &second) == OS_EOK);
    unused = __Open(service, &third);
    assert(unused.Slot == second.Slot);
    assert(GraphicsContextRelease(service, &g_first, &first, &secondContext) == OS_EOK);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend.LiveContexts == 0);
}

static void
__DeferredCleanup(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsSessionRef first;
    struct GraphicsSessionRef second;
    struct GraphicsSessionRef unused;
    struct GraphicsObjectRef context;
    struct GraphicsObjectRef other;
    struct GraphicsObjectRef unusedContext;
    struct GraphicsSessionInfo sessionInfo;
    struct GraphicsContextInfo contextInfo;

    assert(GraphicsTestCreate(&backend, 9, &service) == OS_EOK);
    first = __Open(service, &g_first);
    second = __Open(service, &g_second);
    context = __Create(service, &g_first, &first, 1);
    other = __Create(service, &g_second, &second, 1);
    backend.BusyDestroy = true;
    assert(GraphicsContextRelease(service, &g_first, &first, &context) == OS_EBUSY);
    assert(GraphicsContextQuery(service, &g_first, &first, &context, &contextInfo) == OS_EOK);
    assert(contextInfo.Closing);
    assert(GraphicsContextCreate(service, &g_first, &first, 1, 0, &unusedContext) == OS_EBUSY);
    backend.BusyDestroy = false;
    assert(GraphicsContextRelease(service, &g_first, &first, &context) == OS_EOK);
    assert(GraphicsContextQuery(service, &g_first, &first, &context, &contextInfo) == OS_ENOENT);
    context = __Create(service, &g_first, &first, 2);
    backend.BusyDestroy = true;
    assert(GraphicsServiceDisconnect(service, g_first.Endpoint) == OS_EBUSY);
    assert(GraphicsSessionQuery(service, &g_first, &first, &sessionInfo) == OS_EOK);
    assert(sessionInfo.Closing && sessionInfo.ContextCount == 1);
    assert(GraphicsSessionOpen(service, &g_first, 1, 1, 0, &unused) == OS_EBUSY);
    assert(GraphicsContextCreate(service, &g_first, &first, 2, 0, &unusedContext) == OS_EBUSY);
    assert(backend.LiveContexts == 2);

    // Cleanup progresses without a request from the disconnected client. The
    // surviving client's context must remain usable throughout that recovery.
    backend.BusyDestroy = false;
    assert(GraphicsServiceCollect(service) == OS_EOK);
    assert(GraphicsSessionQuery(service, &g_first, &first, &sessionInfo) == OS_ENOENT);
    assert(GraphicsContextQuery(service, &g_second, &second, &other, &contextInfo) == OS_EOK);
    assert(backend.LiveContexts == 1);

    backend.BusyDestroy = true;
    assert(GraphicsServiceDestroy(service) == OS_EBUSY);
    assert(GraphicsSessionOpen(service, &g_first, 1, 1, 0, &unused) == OS_EBUSY);
    backend.BusyDestroy = false;
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend.LiveContexts == 0);
}

static void
__ServiceReplacement(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsSessionRef previous;
    struct GraphicsSessionRef current;
    struct GraphicsSessionInfo info;

    assert(GraphicsTestCreate(&backend, 40, &service) == OS_EOK);
    previous = __Open(service, &g_first);
    assert(GraphicsServiceDestroy(service) == OS_EOK);

    // Slot counters restart inside a new core. The module's new device
    // generation is what distinguishes its references from the previous core.
    assert(GraphicsTestCreate(&backend, 41, &service) == OS_EOK);
    current = __Open(service, &g_first);
    assert(previous.Slot == current.Slot && previous.Generation == current.Generation);
    assert(GraphicsSessionQuery(service, &g_first, &previous, &info) == OS_ENOENT);
    assert(GraphicsSessionQuery(service, &g_first, &current, &info) == OS_EOK);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
}

static void
__GenerationExhaustion(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsSessionRef session;
    struct GraphicsSessionRef replacement;
    struct GraphicsObjectRef context;
    struct GraphicsObjectRef unused;

    assert(GraphicsTestCreate(&backend, 10, &service) == OS_EOK);
    // Only this boundary test touches private state: reaching UINT64_MAX through
    // normal allocations would be impractical. All checks use public operations.
    service->Sessions[0].Generation = UINT64_MAX - 1;
    service->NextObjectGeneration = UINT64_MAX;
    session = __Open(service, &g_first);
    assert(session.Generation == UINT64_MAX);
    context = __Create(service, &g_first, &session, 1);
    assert(context.Generation == UINT64_MAX);
    assert(GraphicsContextCreate(service, &g_first, &session, 2, 0, &unused) == OS_EOVERFLOW);
    assert(GraphicsSessionClose(service, &g_first, &session) == OS_EOK);
    replacement = __Open(service, &g_first);
    assert(replacement.Slot != session.Slot);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend.LiveContexts == 0);
}

int
main(void)
{
    __AuthenticationAndReferences();
    __LimitsAndReplay();
    __DeferredCleanup();
    __GenerationExhaustion();
    __ServiceReplacement();
    puts("graphics core: authentication, references, quotas, replay and cleanup passed");
    return 0;
}
