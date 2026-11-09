/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef __GRAPHICS_TEST_BACKEND_H__
#define __GRAPHICS_TEST_BACKEND_H__
#include <graphics/job.h>

/** Allocation counters and failures, independent of already accepted work. */
struct GraphicsTestMemory {
    uint32_t LiveBuffers;
    uint32_t CreateCalls;
    bool FailCreate;
    bool BusyDestroy;
    bool FailRead;
    bool FailWrite;
};

/** Deterministic execution time and separate preparation/start/poll failures. */
struct GraphicsTestExecution {
    uint64_t Tick;
    uint32_t LiveJobs;
    uint32_t PrepareCalls;
    uint32_t StartCalls;
    uint32_t CompletedJobs;
    bool FailPrepare;
    bool FailStart;
    bool PollError;
    bool Hang;
    oserr_t Result;
};

/** Failure controls and counters for a software device with no GPU mappings. */
struct GraphicsTestBackend {
    uint32_t LiveContexts;
    uint32_t CreateCalls;
    uint32_t DestroyCalls;
    bool FailCreate;
    bool BusyDestroy;
    struct GraphicsTestMemory Memory;
    struct GraphicsTestExecution Execution;
};

/**
 * @brief Create a test core with small limits that tests can exhaust cheaply.
 * @param backend Zero-initialized test state, valid through core destruction.
 * @param generation Nonzero service-instance generation.
 * @param serviceOut Receives the service on success.
 * @return Core initialization result.
 */
oserr_t
GraphicsTestCreate(
    _InOut_ struct GraphicsTestBackend* backend,
    _In_    uint64_t                    generation,
    _Out_   struct GraphicsService**    serviceOut);
/** One typed software command, copied by Prepare before acceptance. */
struct GraphicsTestFill {
    uint32_t Value;
    uint32_t DelayTicks;
};

extern const struct GraphicsStorageOps g_graphicsTestStorage;
extern const struct GraphicsJobOps g_graphicsTestJobs;

/**
 * @brief Submit a typed test fill through the common admission path.
 * @param service Test core on its serialized executor.
 * @param caller Authenticated caller.
 * @param session Active session.
 * @param request Common references/range/ID; Key is set from the fill command.
 * @param fill Byte value and bounded deterministic delay.
 * @param completionOut Accepted completion on success; unchanged on failure.
 * @return Validation or common admission status.
 */
oserr_t
GraphicsTestSubmit(
    _InOut_ struct GraphicsService*          service,
    _In_    const struct GraphicsCaller*     caller,
    _In_    const struct GraphicsSessionRef* session,
    _In_    const struct GraphicsJobRequest* request,
    _In_    const struct GraphicsTestFill*   fill,
    _Out_   struct GraphicsObjectRef*        completionOut);

/**
 * @brief Advance deterministic test time and common work without sleeping.
 * @param backend Live test device.
 * @param service Associated serialized core, including a closing core.
 * @param ticks Number of progress steps; no client is needed for this call.
 * @return First progress/cleanup error while still attempting every step.
 */
oserr_t
GraphicsTestAdvance(
    _InOut_ struct GraphicsTestBackend* backend,
    _InOut_ struct GraphicsService*     service,
    _In_    uint32_t                    ticks);
#endif
