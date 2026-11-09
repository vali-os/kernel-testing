/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include <stdlib.h>
#include <string.h>

/** Snapshotted command and validated storage range, never a pointer into IPC. */
struct __TestJob {
    struct GraphicsJobResources Resources;
    struct GraphicsTestFill Fill;
    uint64_t ReadyTick;
    oserr_t Result;
};

static oserr_t
__PrepareFill(
    _In_    void*                              device,
    _In_    const struct GraphicsJobResources* resources,
    _In_    const void*                        command,
    _Out_   void**                             preparedOut)
{
    struct GraphicsTestBackend* backend = device;
    struct __TestJob* job;

    backend->Execution.PrepareCalls++;
    if (backend->Execution.FailPrepare) {
        return OS_EOOM;
    }
    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return OS_EOOM;
    }
    job->Resources = *resources;
    job->Fill = *(const struct GraphicsTestFill*)command;
    job->Result = backend->Execution.Result;
    backend->Execution.LiveJobs++;
    *preparedOut = job;
    return OS_EOK;
}

static oserr_t
__StartFill(
    _In_    void* device,
    _InOut_ void* preparedJob)
{
    struct GraphicsTestBackend* backend = device;
    struct __TestJob* job = preparedJob;

    backend->Execution.StartCalls++;
    if (backend->Execution.FailStart) {
        return OS_EDEVFAULT;
    }
    // No host thread races the core. Delayed work becomes eligible only after
    // explicit device ticks; polling the public completion never runs the fill.
    job->ReadyTick = backend->Execution.Tick + job->Fill.DelayTicks;
    return OS_EOK;
}

static oserr_t
__PollFill(
    _In_    void*    device,
    _InOut_ void*    preparedJob,
    _Out_   oserr_t* resultOut)
{
    struct GraphicsTestBackend* backend = device;
    struct __TestJob* job = preparedJob;

    if (backend->Execution.PollError) {
        return OS_EUNKNOWN;
    }
    if (backend->Execution.Hang || backend->Execution.Tick < job->ReadyTick) {
        return OS_EINPROGRESS;
    }
    if (job->Result == OS_EOK) {
        memset((uint8_t*)job->Resources.Storage + (size_t)job->Resources.Offset,
            (int)job->Fill.Value, (size_t)job->Resources.Length);
    }
    backend->Execution.CompletedJobs++;
    *resultOut = job->Result;
    return OS_EOK;
}

static void
__DestroyFill(
    _In_    void* device,
    _InOut_ void* preparedJob)
{
    struct GraphicsTestBackend* backend = device;

    free(preparedJob);
    backend->Execution.LiveJobs--;
}

const struct GraphicsJobOps g_graphicsTestJobs = {
    .Start = __StartFill,
    .Poll = __PollFill,
    .Destroy = __DestroyFill
};

oserr_t
GraphicsTestSubmit(
    _InOut_ struct GraphicsService*          service,
    _In_    const struct GraphicsCaller*     caller,
    _In_    const struct GraphicsSessionRef* session,
    _In_    const struct GraphicsJobRequest* request,
    _In_    const struct GraphicsTestFill*   fill,
    _Out_   struct GraphicsObjectRef*        completionOut)
{
    struct GraphicsJobRequest native = *request;
    uint32_t i;

    if (fill->Value > UINT8_MAX || fill->DelayTicks > 1024) {
        return OS_EINVALPARAMS;
    }
    // Encode only defined fields in a canonical byte order. Comparing a raw C
    // command struct would make padding bytes part of duplicate-request checks.
    memset(native.Key, 0, sizeof(native.Key));
    native.Key[0] = 1;
    native.Key[1] = (uint8_t)fill->Value;
    for (i = 0; i < 4; i++) {
        native.Key[2 + i] = (uint8_t)(fill->DelayTicks >> (i * 8));
    }
    return GraphicsJobSubmit(service, caller, session, &native, __PrepareFill, fill, completionOut);
}

oserr_t
GraphicsTestAdvance(
    _InOut_ struct GraphicsTestBackend* backend,
    _InOut_ struct GraphicsService*     service,
    _In_    uint32_t                    ticks)
{
    oserr_t firstError = OS_EOK;
    oserr_t status;
    uint32_t i;

    for (i = 0; i < ticks; i++) {
        backend->Execution.Tick++;
        status = GraphicsServiceProgress(service);
        if (firstError == OS_EOK && status != OS_EOK) {
            firstError = status;
        }
    }
    return firstError;
}
