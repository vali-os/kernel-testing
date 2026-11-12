/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include <graphics/cpu_access.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

/** Independent identities make cross-session references fail even at equal slots. */
struct __Client {
    struct GraphicsCaller Caller;
    struct GraphicsSessionRef Session;
    struct GraphicsObjectRef Context;
    struct GraphicsObjectRef Buffer;
};

static struct __Client
__OpenClient(
    _In_    struct GraphicsService* service,
    _In_    uint64_t                identity)
{
    struct __Client client = { .Caller = { identity, identity * 10 } };

    assert(GraphicsSessionOpen(service, &client.Caller, 1, 1, GRAPHICS_FEATURE_BUFFERS,
        &client.Session) == OS_EOK);
    assert(GraphicsContextCreate(service, &client.Caller, &client.Session, 1, 0,
        &client.Context) == OS_EOK);
    assert(GraphicsBufferCreate(service, &client.Caller, &client.Session, 1, 64,
        &client.Buffer) == OS_EOK);
    return client;
}

static struct GraphicsJobRequest
__Request(
    _In_    const struct __Client* client,
    _In_    uint64_t               id)
{
    struct GraphicsJobRequest request = {
        .RequestId = id,
        .Context = client->Context,
        .Buffer = client->Buffer,
        .Length = 64
    };
    return request;
}

static struct GraphicsObjectRef
__Submit(
    _In_    struct GraphicsService*          service,
    _In_    const struct __Client*           client,
    _In_    const struct GraphicsJobRequest* request,
    _In_    uint32_t                         value,
    _In_    uint32_t                         delay)
{
    struct GraphicsTestFill fill = { value, delay };
    struct GraphicsObjectRef completion;

    assert(GraphicsTestSubmit(service, &client->Caller, &client->Session,
        request, &fill, &completion) == OS_EOK);
    return completion;
}

static void
__ReadValue(
    _In_    struct GraphicsService* service,
    _In_    const struct __Client*  client,
    _In_    uint8_t                 value)
{
    uint8_t bytes[64];
    size_t i;

    memset(bytes, 0xcc, sizeof(bytes));
    assert(GraphicsBufferRead(service, &client->Caller, &client->Session,
        &client->Buffer, 0, bytes, sizeof(bytes)) == OS_EOK);
    for (i = 0; i < sizeof(bytes); i++) {
        assert(bytes[i] == value);
    }
}

static void
__Poll(
    _In_    struct GraphicsService*         service,
    _In_    const struct __Client*          client,
    _In_    const struct GraphicsObjectRef* completion,
    _In_    oserr_t                         expectedStatus,
    _In_    oserr_t                         expectedResult)
{
    oserr_t result = OS_EUNKNOWN;

    assert(GraphicsCompletionPoll(service, &client->Caller, &client->Session,
        completion, &result) == expectedStatus);
    assert(result == (expectedStatus == OS_EOK ? expectedResult : OS_EUNKNOWN));
}

static void
__Destroy(
    _In_    struct GraphicsService*           service,
    _In_    const struct GraphicsTestBackend* backend)
{
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend->LiveContexts == 0);
    assert(backend->Memory.LiveBuffers == 0);
    assert(backend->Execution.LiveJobs == 0);
}

static void
__BuffersAndRanges(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client first;
    struct __Client second;
    struct GraphicsObjectRef retry;
    struct GraphicsObjectRef extra;
    struct GraphicsObjectRef invalid;
    struct GraphicsBufferInfo info;
    uint8_t bytes[64];

    assert(GraphicsTestCreate(&backend, 51, &service) == OS_EOK);
    first = __OpenClient(service, 1);
    second = __OpenClient(service, 2);
    __ReadValue(service, &first, 0);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 1, 64, &retry) == OS_EOK);
    assert(retry.Generation == first.Buffer.Generation && backend.Memory.CreateCalls == 2);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 1, 32, &retry) == OS_EPROTOCOL);
    assert(GraphicsBufferQuery(service, &second.Caller, &first.Session, &first.Buffer, &info) == OS_EPERMISSIONS);
    assert(GraphicsBufferQuery(service, &second.Caller, &second.Session, &first.Buffer, &info) == OS_ENOENT);
    invalid = first.Buffer;
    invalid.Type = GraphicsObjectContext;
    assert(GraphicsBufferRelease(service, &first.Caller, &first.Session, &invalid) == OS_ENOENT);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 0, 64, &retry) == OS_EINVALPARAMS);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 2, UINT64_MAX, &retry) == OS_EINVALPARAMS);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 2, 0, &retry) == OS_EINVALPARAMS);
    backend.Memory.FailCreate = true;
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 2, 4096, &extra) == OS_EOOM);
    backend.Memory.FailCreate = false;
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 2, 4096, &extra) == OS_EOK);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 3, 4096, &retry) == OS_EBUSY);

    memset(bytes, 0x35, sizeof(bytes));
    assert(GraphicsBufferWrite(service, &first.Caller, &first.Session, &first.Buffer,
        0, bytes, sizeof(bytes)) == OS_EOK);
    __ReadValue(service, &first, 0x35);
    assert(GraphicsBufferRead(service, &first.Caller, &first.Session, &first.Buffer,
        UINT64_MAX, bytes, 2) == OS_EINVALPARAMS);
    assert(GraphicsBufferRead(service, &first.Caller, &first.Session, &first.Buffer,
        63, bytes, 2) == OS_EINVALPARAMS);
    assert(GraphicsBufferWrite(service, &first.Caller, &first.Session, &first.Buffer,
        0, bytes, 0) == OS_EINVALPARAMS);
    assert(GraphicsBufferWrite(service, &first.Caller, &first.Session, &first.Buffer,
        63, bytes, 1) == OS_EOK);

    backend.Memory.BusyDestroy = true;
    assert(GraphicsBufferRelease(service, &first.Caller, &first.Session, &extra) == OS_EOK);
    assert(GraphicsBufferQuery(service, &first.Caller, &first.Session, &extra, &info) == OS_ENOENT);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 3, 4096, &retry) == OS_EBUSY);
    backend.Memory.BusyDestroy = false;
    assert(GraphicsServiceCollect(service) == OS_EOK);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 3, 4096, &retry) == OS_EOK);
    assert(retry.Slot == extra.Slot && retry.Generation != extra.Generation);
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session, 2, 4096, &extra) == OS_ENOENT);
    __Destroy(service, &backend);
}

static void
__SubmissionAndSnapshots(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client client;
    struct GraphicsJobRequest request;
    struct GraphicsTestFill fill = { 0x42, 2 };
    struct GraphicsObjectRef first;
    struct GraphicsObjectRef second;
    struct GraphicsObjectRef retry;
    struct GraphicsBufferInfo info;
    uint8_t bytes[64];

    assert(GraphicsTestCreate(&backend, 52, &service) == OS_EOK);
    client = __OpenClient(service, 1);
    request = __Request(&client, 1);
    backend.Execution.FailPrepare = true;
    assert(GraphicsTestSubmit(service, &client.Caller, &client.Session,
        &request, &fill, &first) == OS_EOOM);
    assert(backend.Execution.LiveJobs == 0);
    backend.Execution.FailPrepare = false;
    first = __Submit(service, &client, &request, 0x42, 2);
    retry = __Submit(service, &client, &request, 0x42, 2);
    assert(first.Generation == retry.Generation && backend.Execution.PrepareCalls == 2);
    fill.Value++;
    assert(GraphicsTestSubmit(service, &client.Caller, &client.Session,
        &request, &fill, &retry) == OS_EPROTOCOL);
    request = __Request(&client, 2);
    second = __Submit(service, &client, &request, 0x73, 4);
    assert(GraphicsBufferQuery(service, &client.Caller, &client.Session, &client.Buffer, &info) == OS_EOK);
    assert(info.PendingUses == 2);
    __Poll(service, &client, &first, OS_EINPROGRESS, 0);
    assert(GraphicsBufferRead(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EBUSY);
    assert(GraphicsBufferWrite(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EBUSY);

    assert(GraphicsTestAdvance(&backend, service, 3) == OS_EOK);
    __Poll(service, &client, &first, OS_EOK, OS_EOK);
    __Poll(service, &client, &second, OS_EINPROGRESS, 0);
    // Completion A stays complete while the same context executes B. Buffer
    // readback waits for all uses, not merely for the first returned fence.
    __Poll(service, &client, &first, OS_EOK, OS_EOK);
    assert(GraphicsBufferRead(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EBUSY);
    assert(GraphicsTestAdvance(&backend, service, 4) == OS_EOK);
    __Poll(service, &client, &second, OS_EOK, OS_EOK);
    __ReadValue(service, &client, 0x73);
    assert(backend.Execution.StartCalls == 2 && backend.Execution.CompletedJobs == 2);
    assert(GraphicsCompletionRelease(service, &client.Caller, &client.Session, &first) == OS_EOK);
    __Poll(service, &client, &first, OS_ENOENT, 0);
    request = __Request(&client, 1);
    assert(GraphicsTestSubmit(service, &client.Caller, &client.Session,
        &request, &fill, &retry) == OS_ENOENT);
    __Destroy(service, &backend);
}

static void
__FailuresAndRecovery(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client client;
    struct GraphicsJobRequest request;
    struct GraphicsObjectRef completion;
    struct GraphicsObjectRef dependent;
    struct GraphicsContextInfo contextInfo;
    uint8_t bytes[64];
    struct GraphicsCpuView view;

    assert(GraphicsTestCreate(&backend, 53, &service) == OS_EOK);
    client = __OpenClient(service, 1);
    request = __Request(&client, 1);
    completion = __Submit(service, &client, &request, 1, 0);
    backend.Execution.FailStart = true;
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EOK);
    __Poll(service, &client, &completion, OS_EOK, OS_EDEVFAULT);
    assert(GraphicsBufferRead(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EDEVFAULT);
    backend.Execution.FailStart = false;

    // Explicit dependency failures prevent execution even for full overwrite.
    // Releasing the source's public handle cannot erase a captured dependency.
    request = __Request(&client, 2);
    request.Dependency = completion;
    dependent = __Submit(service, &client, &request, 2, 0);
    assert(GraphicsCompletionRelease(service, &client.Caller, &client.Session, &completion) == OS_EOK);
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EOK);
    __Poll(service, &client, &dependent, OS_EOK, OS_EDEVFAULT);
    assert(backend.Execution.StartCalls == 1);

    request = __Request(&client, 3);
    completion = __Submit(service, &client, &request, 3, 0);
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EOK);
    backend.Execution.PollError = true;
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EUNKNOWN);
    __Poll(service, &client, &completion, OS_EINPROGRESS, 0);
    assert(GraphicsContextRelease(service, &client.Caller, &client.Session, &client.Context) == OS_EBUSY);
    assert(GraphicsContextQuery(service, &client.Caller, &client.Session, &client.Context, &contextInfo) == OS_EOK);
    assert(contextInfo.Closing && backend.LiveContexts == 1 && backend.Memory.LiveBuffers == 1);
    backend.Execution.PollError = false;
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EOK);
    __Poll(service, &client, &completion, OS_EOK, OS_EOK);
    __ReadValue(service, &client, 3);
    assert(GraphicsContextRelease(service, &client.Caller, &client.Session, &client.Context) == OS_EOK);

    // A failed partial write leaves invalid contents. A complete CPU overwrite
    // repairs that history through the same libos reservation used by jobs.
    memset(bytes, 0x66, sizeof(bytes));
    assert(GraphicsBufferBeginCpu(service, &client.Caller, &client.Session,
        &client.Buffer, 1, 1, GraphicsCpuWrite, &view) == OS_EOK);
    memset(view.Address, 0x66, 1);
    assert(GraphicsBufferEndCpu(service, &client.Caller, &client.Session,
        &view.Access, OS_EDEVFAULT) == OS_EOK);
    assert(GraphicsBufferRead(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EDEVFAULT);
    assert(GraphicsBufferWrite(service, &client.Caller, &client.Session,
        &client.Buffer, 0, bytes, sizeof(bytes)) == OS_EOK);
    __ReadValue(service, &client, 0x66);
    __Destroy(service, &backend);
}

static void
__ReleaseAndDisconnect(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client first;
    struct __Client second;
    struct GraphicsJobRequest request;
    struct GraphicsObjectRef completion;
    struct GraphicsBufferInfo info;
    struct GraphicsSessionInfo sessionInfo;

    assert(GraphicsTestCreate(&backend, 54, &service) == OS_EOK);
    first = __OpenClient(service, 1);
    second = __OpenClient(service, 2);
    request = __Request(&first, 1);
    completion = __Submit(service, &first, &request, 0xab, 0);
    assert(GraphicsBufferRelease(service, &first.Caller, &first.Session, &first.Buffer) == OS_EOK);
    assert(GraphicsBufferQuery(service, &first.Caller, &first.Session, &first.Buffer, &info) == OS_ENOENT);
    assert(GraphicsCompletionRelease(service, &first.Caller, &first.Session, &completion) == OS_EOK);
    assert(backend.Memory.LiveBuffers == 2 && backend.Execution.LiveJobs == 1);
    assert(GraphicsServiceDisconnect(service, first.Caller.Endpoint) == OS_EBUSY);
    backend.Execution.Hang = true;
    assert(GraphicsTestAdvance(&backend, service, 4) == OS_EBUSY);
    assert(backend.Memory.LiveBuffers == 2 && backend.LiveContexts == 2);
    __ReadValue(service, &second, 0);
    assert(GraphicsSessionQuery(service, &first.Caller, &first.Session, &sessionInfo) == OS_EOK);
    assert(sessionInfo.Closing);
    backend.Execution.Hang = false;
    assert(GraphicsTestAdvance(&backend, service, 1) == OS_EOK);
    assert(GraphicsSessionQuery(service, &first.Caller, &first.Session, &sessionInfo) == OS_ENOENT);
    assert(backend.Memory.LiveBuffers == 1 && backend.LiveContexts == 1);
    assert(backend.Execution.CompletedJobs == 1);

    request = __Request(&second, 1);
    completion = __Submit(service, &second, &request, 0x22, 0);
    assert(GraphicsServiceDestroy(service) == OS_EBUSY);
    assert(GraphicsTestAdvance(&backend, service, 2) == OS_EBUSY);
    // Advance reports the first intermediate busy cleanup, even if a later tick
    // drains it. Final teardown must succeed without a client to finish the job.
    __Destroy(service, &backend);
}

static void
__ReleasedDependencyCapacity(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client clients[2];
    struct GraphicsObjectRef temporaryBuffers[2];
    struct GraphicsObjectRef temporaryContexts[2];
    struct GraphicsObjectRef temporary;
    struct GraphicsJobRequest request;
    uint32_t round;
    uint32_t i;
    uint64_t ids[2] = { 1, 1 };
    uint8_t bytes[64];

    assert(GraphicsTestCreate(&backend, 55, &service) == OS_EOK);
    for (i = 0; i < 2; i++) {
        clients[i] = __OpenClient(service, i + 1);
        assert(GraphicsBufferCreate(service, &clients[i].Caller, &clients[i].Session,
            2, 64, &temporaryBuffers[i]) == OS_EOK);
        assert(GraphicsContextCreate(service, &clients[i].Caller, &clients[i].Session,
            2, 0, &temporaryContexts[i]) == OS_EOK);
        request = __Request(&clients[i], ids[i]++);
        (void)__Submit(service, &clients[i], &request, 1, 1024);
    }
    for (round = 0; round < 6; round++) {
        for (i = 0; i < 2; i++) {
            request = __Request(&clients[i], ids[i]++);
            request.Buffer = temporaryBuffers[i];
            request.Context = temporaryContexts[i];
            temporary = __Submit(service, &clients[i], &request, 2, 0);
            assert(GraphicsTestAdvance(&backend, service, 2) == OS_EOK);
            __Poll(service, &clients[i], &temporary, OS_EOK, OS_EOK);
            request = __Request(&clients[i], ids[i]++);
            request.Dependency = temporary;
            (void)__Submit(service, &clients[i], &request, 3, 0);
            assert(GraphicsCompletionRelease(service, &clients[i].Caller,
                &clients[i].Session, &temporary) == OS_EOK);
        }
    }
    // Fourteen live jobs also keep twelve distinct older dependency fences.
    // Their released job slots must not make the fence pool underestimate use.
    assert(GraphicsBufferRead(service, &clients[0].Caller, &clients[0].Session,
        &temporaryBuffers[0], 0, bytes, sizeof(bytes)) == OS_EOK);
    assert(GraphicsTestAdvance(&backend, service, 1040) == OS_EOK);
    __Destroy(service, &backend);
}

static void
__JobLimitsAndIndependentProgress(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct __Client first;
    struct __Client second;
    struct GraphicsJobRequest request;
    struct GraphicsTestFill fill = { 0x44, 0 };
    struct GraphicsObjectRef completions[8];
    struct GraphicsObjectRef independent;
    struct GraphicsObjectRef unused = { 0 };
    struct GraphicsObjectRef stale;
    struct GraphicsObjectRef extraBuffer;
    uint32_t i;

    assert(GraphicsTestCreate(&backend, 56, &service) == OS_EOK);
    first = __OpenClient(service, 1);
    second = __OpenClient(service, 2);
    for (i = 0; i < 8; i++) {
        request = __Request(&first, i + 1);
        completions[i] = __Submit(service, &first, &request, 0x44, i == 0 ? 8 : 0);
    }
    request = __Request(&first, 9);
    assert(GraphicsTestSubmit(service, &first.Caller, &first.Session,
        &request, &fill, &unused) == OS_EBUSY);
    assert(unused.Generation == 0);
    request = __Request(&first, 1);
    stale = __Submit(service, &first, &request, 0x44, 8);
    assert(stale.Generation == completions[0].Generation && backend.Execution.PrepareCalls == 8);
    assert(GraphicsCompletionRelease(service, &first.Caller, &first.Session, &completions[0]) == OS_EOK);
    request = __Request(&first, 9);
    assert(GraphicsTestSubmit(service, &first.Caller, &first.Session,
        &request, &fill, &unused) == OS_EBUSY);

    // Full tables in one session leave capacity for another. Independent work
    // may complete first, while accepted same-context work stays ordered.
    request = __Request(&second, 1);
    independent = __Submit(service, &second, &request, 0x55, 0);
    assert(GraphicsTestAdvance(&backend, service, 2) == OS_EOK);
    __Poll(service, &second, &independent, OS_EOK, OS_EOK);
    __Poll(service, &first, &completions[1], OS_EINPROGRESS, 0);
    __ReadValue(service, &second, 0x55);
    assert(GraphicsTestAdvance(&backend, service, 20) == OS_EOK);
    for (i = 1; i < 8; i++) {
        __Poll(service, &first, &completions[i], OS_EOK, OS_EOK);
    }
    request = __Request(&first, 9);
    unused = __Submit(service, &first, &request, 0x44, 0);
    assert(unused.Slot == stale.Slot && unused.Generation != stale.Generation);
    __Poll(service, &first, &stale, OS_ENOENT, 0);
    assert(GraphicsTestAdvance(&backend, service, 2) == OS_EOK);
    __ReadValue(service, &first, 0x44);

    // Buffer slot limits are independent of the byte quota.
    for (i = 2; i <= 4; i++) {
        assert(GraphicsBufferCreate(service, &first.Caller, &first.Session,
            i, 1, &extraBuffer) == OS_EOK);
    }
    assert(GraphicsBufferCreate(service, &first.Caller, &first.Session,
        5, 1, &extraBuffer) == OS_EBUSY);
    __Destroy(service, &backend);
}

int
main(void)
{
    __BuffersAndRanges();
    __SubmissionAndSnapshots();
    __FailuresAndRecovery();
    __ReleaseAndDisconnect();
    __ReleasedDependencyCapacity();
    __JobLimitsAndIndependentProgress();
    puts("graphics buffers: readback, snapshots, dependency capacity, failures and draining passed");
    return 0;
}
