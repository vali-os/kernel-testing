/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include "shm_host.h"
#include <graphics/shm_storage.h>
#include <graphics/cpu_access.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define TEST_BUFFER_SIZE 257U

/** One native-adapter service, using real libos wrappers above the host boundary. */
struct __Fixture {
    struct GraphicsTestBackend Backend;
    struct GraphicsService* Service;
    struct GraphicsCaller Caller;
    struct GraphicsSessionRef Session;
    struct GraphicsObjectRef Context;
    struct GraphicsObjectRef Buffer;
};

static void
__CreateFixture(
    _Out_   struct __Fixture* fixture)
{
    GraphicsShmHostReset();
    memset(fixture, 0, sizeof(*fixture));
    fixture->Backend.Storage = GraphicsShmStorageOperations();
    fixture->Caller = (struct GraphicsCaller) { 42, 420 };
    assert(GraphicsTestCreate(&fixture->Backend, 71, &fixture->Service) == OS_EOK);
    assert(GraphicsSessionOpen(fixture->Service, &fixture->Caller, 1, 1,
        GRAPHICS_FEATURE_BUFFERS, &fixture->Session) == OS_EOK);
    assert(GraphicsContextCreate(fixture->Service, &fixture->Caller, &fixture->Session,
        1, 0, &fixture->Context) == OS_EOK);
    assert(GraphicsBufferCreate(fixture->Service, &fixture->Caller, &fixture->Session,
        1, TEST_BUFFER_SIZE, &fixture->Buffer) == OS_EOK);
}

static void
__DestroyFixture(
    _InOut_ struct __Fixture* fixture)
{
    assert(GraphicsServiceDestroy(fixture->Service) == OS_EOK);
    assert(fixture->Backend.LiveContexts == 0 && fixture->Backend.Execution.LiveJobs == 0);
    assert(g_graphicsShmHost.LiveBuffers == 0 && g_graphicsShmHost.LiveSessions == 0);
    assert(g_graphicsShmHost.ActiveSessions == 0);
}

static oserr_t
__Begin(
    _In_    struct __Fixture*       fixture,
    _In_    enum GraphicsCpuMode    mode,
    _Out_   struct GraphicsCpuView* view)
{
    return GraphicsBufferBeginCpu(fixture->Service, &fixture->Caller, &fixture->Session,
        &fixture->Buffer, 0, TEST_BUFFER_SIZE, mode, view);
}

static oserr_t
__End(
    _In_    struct __Fixture*             fixture,
    _In_    const struct GraphicsCpuView* view,
    _In_    oserr_t                       result)
{
    return GraphicsBufferEndCpu(fixture->Service, &fixture->Caller,
        &fixture->Session, &view->Access, result);
}

static oserr_t
__Submit(
    _In_    struct __Fixture*         fixture,
    _In_    uint64_t                  requestId,
    _Out_   struct GraphicsObjectRef* completion)
{
    struct GraphicsJobRequest request = {
        .RequestId = requestId, .Context = fixture->Context, .Buffer = fixture->Buffer,
        .Offset = 32, .Length = 128
    };
    struct GraphicsTestFill fill = { 0x77, 0 };

    return GraphicsTestSubmit(fixture->Service, &fixture->Caller, &fixture->Session,
        &request, &fill, completion);
}

static void
__PersistentMappingAndExplicitAccess(void)
{
    struct __Fixture fixture;
    struct GraphicsCpuView first;
    struct GraphicsCpuView second;
    struct GraphicsObjectRef completion;
    struct GraphicsBufferInfo info;
    struct GraphicsCaller other;
    void* address;
    oserr_t result;
    uint32_t i;

    __CreateFixture(&fixture);
    assert(g_graphicsShmHost.CreateCalls == 1 && g_graphicsShmHost.OpenCalls == 0);
    assert(__Begin(&fixture, GraphicsCpuRead, &first) == OS_EOK);
    address = first.Address;
    assert(first.Length == TEST_BUFFER_SIZE);
    for (i = 0; i < first.Length; i++) {
        assert(((uint8_t*)first.Address)[i] == 0);
    }
    assert(__Begin(&fixture, GraphicsCpuWrite, &second) == OS_EBUSY);
    assert(__Submit(&fixture, 1, &completion) == OS_EBUSY);
    assert(fixture.Backend.Execution.PrepareCalls == 0);
    assert(GraphicsBufferQuery(fixture.Service, &fixture.Caller, &fixture.Session,
        &fixture.Buffer, &info) == OS_EOK && info.PendingUses == 1);
    other = fixture.Caller;
    other.Principal++;
    assert(GraphicsBufferEndCpu(fixture.Service, &other, &fixture.Session,
        &first.Access, OS_EOK) == OS_EPERMISSIONS);
    assert(__End(&fixture, &first, OS_EOK) == OS_EOK);
    assert(__End(&fixture, &first, OS_EOK) == OS_EOK);
    assert(__End(&fixture, &first, OS_EDEVFAULT) == OS_EPROTOCOL);

    assert(__Begin(&fixture, GraphicsCpuOverwrite, &second) == OS_EOK);
    assert(second.Address == address && second.Access.Generation != first.Access.Generation);
    assert(__End(&fixture, &first, OS_EOK) == OS_ENOENT);
    memset(second.Address, 0x35, second.Length);
    assert(__End(&fixture, &second, OS_EOK) == OS_EOK);
    assert(__Submit(&fixture, 1, &completion) == OS_EOK);
    assert(__Begin(&fixture, GraphicsCpuRead, &first) == OS_EBUSY);
    assert(GraphicsTestAdvance(&fixture.Backend, fixture.Service, 2) == OS_EOK);
    assert(GraphicsCompletionPoll(fixture.Service, &fixture.Caller, &fixture.Session,
        &completion, &result) == OS_EOK && result == OS_EOK);
    assert(__Begin(&fixture, GraphicsCpuRead, &first) == OS_EOK);
    assert(first.Address == address);
    for (i = 0; i < first.Length; i++) {
        assert(((uint8_t*)first.Address)[i] == (i >= 32 && i < 160 ? 0x77 : 0x35));
    }
    assert(__End(&fixture, &first, OS_EOK) == OS_EOK);
    assert(g_graphicsShmHost.OpenCalls == 1 && g_graphicsShmHost.CreateCalls == 1);
    assert(g_graphicsShmHost.ReadBegins == 2 && g_graphicsShmHost.WriteBegins == 2);
    __DestroyFixture(&fixture);
}

static void
__BeginAndEndFailures(void)
{
    struct __Fixture fixture;
    struct GraphicsCpuView view = { .Address = (void*)(uintptr_t)1 };
    struct GraphicsObjectRef extra;
    struct GraphicsObjectRef completion;
    uint8_t bytes[TEST_BUFFER_SIZE];
    uint32_t begins;
    uint32_t i;

    __CreateFixture(&fixture);
    g_graphicsShmHost.FailCreate = true;
    assert(GraphicsBufferCreate(fixture.Service, &fixture.Caller, &fixture.Session,
        2, 64, &extra) == OS_EOOM);
    assert(g_graphicsShmHost.LiveBuffers == 1);
    g_graphicsShmHost.FailCreate = false;
    assert(GraphicsBufferCreate(fixture.Service, &fixture.Caller, &fixture.Session,
        2, 64, &extra) == OS_EOK);
    g_graphicsShmHost.FailOpen = true;
    assert(__Begin(&fixture, GraphicsCpuRead, &view) == OS_EOOM);
    assert(view.Address == (void*)(uintptr_t)1 && g_graphicsShmHost.LiveSessions == 0);
    g_graphicsShmHost.FailOpen = false;
    g_graphicsShmHost.FailBegin = true;
    assert(__Begin(&fixture, GraphicsCpuWrite, &view) == OS_EBUSY);
    assert(view.Address == (void*)(uintptr_t)1 && g_graphicsShmHost.ActiveSessions == 0);
    g_graphicsShmHost.FailBegin = false;
    // Rejected access must not poison valid contents. The subsequent read works.
    assert(__Begin(&fixture, GraphicsCpuRead, &view) == OS_EOK);
    assert(__End(&fixture, &view, OS_EOK) == OS_EOK);

    memset(bytes, 0x26, sizeof(bytes));
    g_graphicsShmHost.FailEnd = true;
    assert(GraphicsBufferWrite(fixture.Service, &fixture.Caller, &fixture.Session,
        &fixture.Buffer, 0, bytes, sizeof(bytes)) == OS_EDEVFAULT);
    begins = g_graphicsShmHost.BeginCalls;
    assert(g_graphicsShmHost.ActiveSessions == 1);
    assert(__Submit(&fixture, 1, &completion) == OS_EBUSY);
    assert(__Begin(&fixture, GraphicsCpuRead, &view) == OS_EBUSY);
    memset(bytes, 0x99, sizeof(bytes));
    assert(GraphicsServiceCollect(fixture.Service) == OS_EDEVFAULT);
    g_graphicsShmHost.FailEnd = false;
    assert(GraphicsServiceCollect(fixture.Service) == OS_EOK);
    assert(g_graphicsShmHost.BeginCalls == begins && g_graphicsShmHost.ActiveSessions == 0);
    assert(__Begin(&fixture, GraphicsCpuRead, &view) == OS_EOK);
    for (i = 0; i < view.Length; i++) {
        assert(((uint8_t*)view.Address)[i] == 0x26);
    }
    assert(__End(&fixture, &view, OS_EOK) == OS_EOK);
    __DestroyFixture(&fixture);
}

static void
__FailedWriterSetupCleanup(void)
{
    struct __Fixture fixture;
    struct GraphicsCpuView view;
    struct GraphicsCpuView failed = { .Address = (void*)(uintptr_t)1 };
    struct GraphicsObjectRef completion;

    __CreateFixture(&fixture);
    assert(__Begin(&fixture, GraphicsCpuWrite, &view) == OS_EOK);
    assert(__End(&fixture, &view, OS_EDEVFAULT) == OS_EOK);
    // Kernel Begin succeeds, but libos rejects the failed prior contents. If
    // ending that setup access also fails, no view escapes and jobs stay blocked.
    g_graphicsShmHost.FailEnd = true;
    assert(__Begin(&fixture, GraphicsCpuRead, &failed) == OS_EDEVFAULT);
    assert(failed.Address == (void*)(uintptr_t)1 && g_graphicsShmHost.ActiveSessions == 1);
    assert(__Submit(&fixture, 1, &completion) == OS_EBUSY);
    assert(__End(&fixture, &view, OS_EDEVFAULT) == OS_ENOENT);
    g_graphicsShmHost.FailEnd = false;
    assert(GraphicsServiceCollect(fixture.Service) == OS_EOK);
    assert(__Begin(&fixture, GraphicsCpuOverwrite, &view) == OS_EOK);
    memset(view.Address, 0, view.Length);
    assert(__End(&fixture, &view, OS_EOK) == OS_EOK);
    assert(__Begin(&fixture, GraphicsCpuRead, &view) == OS_EOK);
    assert(__End(&fixture, &view, OS_EOK) == OS_EOK);
    __DestroyFixture(&fixture);
}

static void
__ReleaseAndCloseFailures(void)
{
    struct __Fixture fixture;
    struct GraphicsCpuView view;
    struct GraphicsObjectRef previous;
    uint32_t closes;

    __CreateFixture(&fixture);
    assert(__Begin(&fixture, GraphicsCpuWrite, &view) == OS_EOK);
    previous = fixture.Buffer;
    assert(GraphicsBufferRelease(fixture.Service, &fixture.Caller, &fixture.Session,
        &fixture.Buffer) == OS_EOK);
    assert(g_graphicsShmHost.ReleaseCalls == 0 && g_graphicsShmHost.ActiveSessions == 1);
    memset(view.Address, 0x81, view.Length);
    g_graphicsShmHost.FailClose = true;
    assert(__End(&fixture, &view, OS_EOK) == OS_EOK);
    assert(GraphicsServiceCollect(fixture.Service) == OS_EBUSY);
    assert(g_graphicsShmHost.LiveBuffers == 1 && g_graphicsShmHost.LiveSessions == 1);
    g_graphicsShmHost.FailClose = false;
    g_graphicsShmHost.FailReleaseAfterUnmap = true;
    assert(GraphicsServiceCollect(fixture.Service) == OS_EUNKNOWN);
    closes = g_graphicsShmHost.CloseCalls;
    assert(g_graphicsShmHost.LiveSessions == 0 && g_graphicsShmHost.LiveBuffers == 1);
    assert(GraphicsServiceCollect(fixture.Service) == OS_EUNKNOWN);
    assert(g_graphicsShmHost.CloseCalls == closes);
    g_graphicsShmHost.FailReleaseAfterUnmap = false;
    assert(GraphicsServiceCollect(fixture.Service) == OS_EOK);
    assert(GraphicsBufferCreate(fixture.Service, &fixture.Caller, &fixture.Session,
        2, TEST_BUFFER_SIZE, &fixture.Buffer) == OS_EOK);
    assert(fixture.Buffer.Slot == previous.Slot && fixture.Buffer.Generation != previous.Generation);
    assert(__End(&fixture, &view, OS_EOK) == OS_ENOENT);
    __DestroyFixture(&fixture);
}

static void
__ActiveViewAndDisconnect(void)
{
    struct __Fixture fixture;
    struct GraphicsCpuView view;

    __CreateFixture(&fixture);
    assert(__Begin(&fixture, GraphicsCpuWrite, &view) == OS_EOK);
    assert(GraphicsServiceDisconnect(fixture.Service, fixture.Caller.Endpoint) == OS_EBUSY);
    assert(GraphicsServiceDestroy(fixture.Service) == OS_EBUSY);
    assert(g_graphicsShmHost.ActiveSessions == 1 && g_graphicsShmHost.ReleaseCalls == 0);
    // The local task must stop using its view before End. Remote disconnection
    // alone cannot prove that this task has stopped touching the mapped bytes.
    memset(view.Address, 0x15, view.Length);
    g_graphicsShmHost.FailEnd = true;
    assert(__End(&fixture, &view, OS_ECANCELLED) == OS_EDEVFAULT);
    assert(__End(&fixture, &view, OS_EOK) == OS_EPROTOCOL);
    assert(GraphicsServiceDestroy(fixture.Service) == OS_EDEVFAULT);
    g_graphicsShmHost.FailEnd = false;
    __DestroyFixture(&fixture);
}

static void
__FillEndFailure(void)
{
    struct __Fixture fixture;
    struct GraphicsObjectRef completion;
    uint32_t begins;
    oserr_t result = OS_EUNKNOWN;

    __CreateFixture(&fixture);
    assert(__Submit(&fixture, 1, &completion) == OS_EOK);
    assert(GraphicsTestAdvance(&fixture.Backend, fixture.Service, 1) == OS_EOK);
    g_graphicsShmHost.FailEnd = true;
    assert(GraphicsTestAdvance(&fixture.Backend, fixture.Service, 1) == OS_EDEVFAULT);
    begins = g_graphicsShmHost.BeginCalls;
    assert(g_graphicsShmHost.ActiveSessions == 1);
    assert(GraphicsCompletionPoll(fixture.Service, &fixture.Caller, &fixture.Session,
        &completion, &result) == OS_EINPROGRESS && result == OS_EUNKNOWN);
    assert(GraphicsTestAdvance(&fixture.Backend, fixture.Service, 1) == OS_EDEVFAULT);
    assert(g_graphicsShmHost.BeginCalls == begins);
    g_graphicsShmHost.FailEnd = false;
    assert(GraphicsTestAdvance(&fixture.Backend, fixture.Service, 1) == OS_EOK);
    assert(g_graphicsShmHost.BeginCalls == begins && g_graphicsShmHost.ActiveSessions == 0);
    assert(fixture.Backend.Execution.CompletedJobs == 1);
    __DestroyFixture(&fixture);
}

int
main(void)
{
    __PersistentMappingAndExplicitAccess();
    __BeginAndEndFailures();
    __FailedWriterSetupCleanup();
    __ReleaseAndCloseFailures();
    __ActiveViewAndDisconnect();
    __FillEndFailure();
    puts("graphics SHM: persistent views, explicit access, failures and cleanup passed");
    return 0;
}
