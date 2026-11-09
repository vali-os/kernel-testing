/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include "transport.h"
#include "test_protocol.h"
#include "protocol_buffers_test.h"
#include <ctt_graphics_service_client.h>
#include <ctt_graphics_v3d_service_client.h>
#include <aio.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static gracht_conn_t g_accepted;

/** Both sides of one real socket connection, driven by the test's executor. */
struct __Peer {
    gracht_client_t* Client;
    gracht_conn_t    Endpoint;
};

static void
__Connected(
    _In_    gracht_conn_t endpoint)
{
    g_accepted = endpoint;
}

static struct __Peer
__Connect(
    _In_    const char*      name,
    _In_    gracht_server_t* server,
    _In_    int              listener)
{
    struct __Peer peer;

    peer.Client = GraphicsHostConnect(name);
    assert(gracht_server_handle_event(server, listener, 0) == 0);
    peer.Endpoint = g_accepted;
    return peer;
}

static void
__Exchange(
    _In_    gracht_server_t*               server,
    _In_    struct __Peer*                 peer,
    _InOut_ struct gracht_message_context* call)
{
    // Requests pass through real encoding, transport and generated dispatch.
    // Explicitly driving the executor avoids sleeps and scheduler timing races.
    assert(gracht_server_handle_event(server, peer->Endpoint, 0) == 0);
    assert(gracht_client_await(peer->Client, call, GRACHT_AWAIT_ALL) == 0);
}

static struct ctt_graphicssessionref
__Open(
    _In_    gracht_server_t* server,
    _InOut_ struct __Peer*   peer)
{
    struct gracht_message_context call = { 0 };
    struct ctt_graphicssessionref session;
    oserr_t status;

    assert(ctt_graphics_open(peer->Client, &call, 1, 1, 0) == 0);
    __Exchange(server, peer, &call);
    assert(ctt_graphics_open_result(peer->Client, &call, &status, &session) == 0);
    assert(status == OS_EOK);
    return session;
}

static void
__Disconnect(
    _In_    gracht_server_t* server,
    _InOut_ struct __Peer*   peer)
{
    gracht_aio_event_t events[8];
    bool disconnected = false;
    int count;
    int i;

    gracht_client_shutdown(peer->Client);
    // Reading an empty stream alone does not deliver Gracht's hangup event.
    // Observe the actual epoll flags, as the production server loop does.
    while (!disconnected) {
        count = epoll_wait(gracht_server_get_aio_handle(server), events, 8, 1000);
        assert(count > 0);
        for (i = 0; i < count; i++) {
            if (events[i].data.fd == peer->Endpoint &&
                (events[i].events & GRACHT_AIO_EVENT_DISCONNECT)) {
                disconnected = true;
            }
            assert(gracht_server_handle_event(server, events[i].data.fd, events[i].events) == 0);
        }
    }
}

static struct ctt_graphicsobjectref
__Create(
    _In_    gracht_server_t*                     server,
    _InOut_ struct __Peer*                       peer,
    _In_    const struct ctt_graphicssessionref* session,
    _In_    uint64_t                             creationId)
{
    struct gracht_message_context call = { 0 };
    struct ctt_graphicsobjectref context;
    oserr_t status;

    assert(ctt_graphics_create_context(peer->Client, &call, session, creationId, 0) == 0);
    __Exchange(server, peer, &call);
    assert(ctt_graphics_create_context_result(peer->Client, &call, &status, &context) == 0);
    assert(status == OS_EOK);
    return context;
}

static void
__TruncatedRequest(
    _In_    gracht_server_t*                  server,
    _In_    struct __Peer*                    peer,
    _In_    const struct GraphicsTestBackend* backend)
{
    unsigned char bytes[GRACHT_MESSAGE_HEADER_SIZE] = {
        0xff, 0xff, 0xff, 0xff,
        GRACHT_MESSAGE_HEADER_SIZE, 0, 0, 0,
        SERVICE_CTT_GRAPHICS_ID,
        SERVICE_CTT_GRAPHICS_CREATE_CONTEXT_ID,
        MESSAGE_FLAG_SYNC
    };
    uint32_t calls = backend->CreateCalls;

    // A valid frame header with missing arguments must stop in generated
    // decoding. It must never reach the backend with zero/default coordinates.
    assert(send(gracht_client_iod(peer->Client), bytes, sizeof(bytes), 0) == sizeof(bytes));
    assert(gracht_server_handle_event(server, peer->Endpoint, 0) == 0);
    assert(backend->CreateCalls == calls);
}

int
main(void)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    struct GraphicsCaller caller;
    struct gracht_message unauthenticated = { 0 };
    struct __Peer first;
    struct __Peer second;
    struct ctt_graphicssessionref firstSession;
    struct ctt_graphicssessionref secondSession;
    struct ctt_graphicssessionref invalidSession;
    struct ctt_graphicsobjectref firstContext;
    struct ctt_graphicsobjectref secondContext;
    struct ctt_graphicsobjectref replacement;
    struct ctt_graphicsdeviceinfo device;
    struct ctt_graphicscontextinfo contextInfo;
    struct ctt_graphicsv3dinfo gpu;
    struct ctt_graphicsv3dcommandlist commands = { 0 };
    struct ctt_graphicsobjectref completion;
    struct gracht_message_context call = { 0 };
    gracht_server_t* server;
    oserr_t status;
    char name[80];
    int listener;

    snprintf(name, sizeof(name), "vali-graphics-protocol-%ld", (long)getpid());
    assert(GraphicsTestCreate(&backend, 1, &service) == OS_EOK);
    server = GraphicsHostListen(name, __Connected, NULL, &listener);
    assert(GraphicsProtocolAttach(server, service, GraphicsHostAuthenticate) == OS_EOK);
    assert(GraphicsProtocolAttach(server, service, GraphicsHostAuthenticate) == OS_EBUSY);
    assert(GraphicsHostAuthenticate(&unauthenticated, &caller) == OS_EPERMISSIONS);
    assert(GraphicsAuthenticateVali(&unauthenticated, &caller) == OS_EPERMISSIONS);
    assert(GraphicsTestProtocolAttach(server, &backend, service) == OS_EOK);
    first = __Connect(name, server, listener);
    second = __Connect(name, server, listener);

    assert(ctt_graphics_get_device(first.Client, &call, 0) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_get_device_result(first.Client, &call, &status, &device) == 0);
    assert(status == OS_EOK && device.Backend == CTT_GRAPHICSBACKEND_TEST);
    assert(device.ExtensionVersion == 0 && device.Features == GRAPHICS_FEATURE_BUFFERS);
    assert(device.MaxSessions == 2 && device.MaxContextsPerSession == 2);
    assert(ctt_graphics_get_device(first.Client, &call, 1) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_get_device_result(first.Client, &call, &status, &device) == 0);
    assert(status == OS_ENOENT && device.DeviceId == 0);
    assert(ctt_graphics_open(first.Client, &call, 1, 2, 0) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_open_result(first.Client, &call, &status, &invalidSession) == 0);
    assert(status == OS_ENOTSUPPORTED && invalidSession.Slot == 0);

    firstSession = __Open(server, &first);
    secondSession = __Open(server, &second);
    firstContext = __Create(server, &first, &firstSession, 1);
    secondContext = __Create(server, &second, &secondSession, 1);
    GraphicsTestProtocolBuffers(server, first.Client, first.Endpoint,
        &firstSession, &firstContext, &secondSession, &backend);
    __TruncatedRequest(server, &first, &backend);
    replacement = __Create(server, &first, &firstSession, 1);
    assert(replacement.Generation == firstContext.Generation && backend.CreateCalls == 2);

    assert(ctt_graphics_query_context(second.Client, &call, &firstSession, &firstContext) == 0);
    __Exchange(server, &second, &call);
    assert(ctt_graphics_query_context_result(second.Client, &call, &status, &contextInfo) == 0);
    assert(status == OS_EPERMISSIONS && contextInfo.Flags == 0);
    assert(ctt_graphics_query_context(second.Client, &call, &secondSession, &firstContext) == 0);
    __Exchange(server, &second, &call);
    assert(ctt_graphics_query_context_result(second.Client, &call, &status, &contextInfo) == 0);
    assert(status == OS_ENOENT);

    assert(ctt_graphics_v3d_describe_gpu(first.Client, &call, &firstSession, 1) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_v3d_describe_gpu_result(first.Client, &call, &status, &gpu) == 0);
    assert(status == OS_ENOTSUPPORTED && gpu.Version == 0);
    assert(ctt_graphics_v3d_submit_cl(
        first.Client, &call, &firstSession, 1, &firstContext, 1, &commands
    ) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_v3d_submit_cl_result(first.Client, &call, &status, &completion) == 0);
    assert(status == OS_ENOTSUPPORTED && completion.Slot == 0);

    assert(ctt_graphics_release_context(first.Client, &call, &firstSession, &firstContext) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_release_context_result(first.Client, &call, &status) == 0);
    assert(status == OS_EOK);
    replacement = __Create(server, &first, &firstSession, 2);
    assert(replacement.Generation != firstContext.Generation);
    assert(ctt_graphics_query_context(first.Client, &call, &firstSession, &firstContext) == 0);
    __Exchange(server, &first, &call);
    assert(ctt_graphics_query_context_result(first.Client, &call, &status, &contextInfo) == 0);
    assert(status == OS_ENOENT);

    // Drop a connection with a live context instead of sending close. This
    // exercises the actual Gracht disconnect callback and surviving-client path.
    __Disconnect(server, &first);
    assert(backend.LiveContexts == 1);
    assert(ctt_graphics_query_context(second.Client, &call, &secondSession, &secondContext) == 0);
    __Exchange(server, &second, &call);
    assert(ctt_graphics_query_context_result(second.Client, &call, &status, &contextInfo) == 0);
    assert(status == OS_EOK && !contextInfo.Closing);
    assert(ctt_graphics_close(second.Client, &call, &secondSession) == 0);
    __Exchange(server, &second, &call);
    assert(ctt_graphics_close_result(second.Client, &call, &status) == 0);
    assert(status == OS_EOK && backend.LiveContexts == 0);
    __Disconnect(server, &second);
    GraphicsTestProtocolDetach(server);
    assert(GraphicsProtocolDetach(server) == OS_EOK);
    gracht_server_request_shutdown(server);
    (void)gracht_server_handle_event(server, listener, 0);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    puts("graphics protocol: generated bindings, two clients and disconnect isolation passed");
    return 0;
}
