/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "backend.h"
#include "transport.h"
#include "test_protocol.h"
#include <aio.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

static gracht_server_t* g_server;
static unsigned int g_disconnected;

static void
__Disconnected(
    _In_    gracht_conn_t client)
{
    GraphicsProtocolDisconnected(client);
    g_disconnected++;

}

int
main(
    _In_    int    argc,
    _In_    char** argv)
{
    struct GraphicsTestBackend backend = { 0 };
    struct GraphicsService* service;
    gracht_aio_event_t events[8];
    oserr_t status;
    int count;
    int i;
    int listener;

    if (argc != 2) {
        fprintf(stderr, "usage: graphics-test-server SOCKET_NAME\n");
        return 2;
    }
    assert(GraphicsTestCreate(&backend, (uint64_t)getpid(), &service) == OS_EOK);
    g_server = GraphicsHostListen(argv[1], NULL, __Disconnected, &listener);
    assert(GraphicsProtocolAttach(g_server, service, GraphicsHostAuthenticate) == OS_EOK);
    assert(GraphicsTestProtocolAttach(g_server, &backend, service) == OS_EOK);
    puts("READY");
    fflush(stdout);
    // The fixture has its own event-loop tick, so lost clients cannot be the
    // only source of progress. All callbacks still run on this single executor.
    while (g_disconnected < 2 || backend.Execution.LiveJobs != 0) {
        count = epoll_wait(gracht_server_get_aio_handle(g_server), events, 8, 10);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        assert(count >= 0);
        for (i = 0; i < count; i++) {
            assert(gracht_server_handle_event(g_server, events[i].data.fd, events[i].events) == 0);
        }
        status = GraphicsTestAdvance(&backend, service, 1);
        assert(status == OS_EOK || status == OS_EBUSY);
    }
    GraphicsTestProtocolDetach(g_server);
    assert(GraphicsProtocolDetach(g_server) == OS_EOK);
    gracht_server_request_shutdown(g_server);
    (void)gracht_server_handle_event(g_server, listener, 0);
    assert(GraphicsServiceDestroy(service) == OS_EOK);
    assert(backend.LiveContexts == 0);
    assert(backend.Memory.LiveBuffers == 0 && backend.Execution.LiveJobs == 0);
    assert(backend.Execution.CompletedJobs == 4);
    puts("graphics server: both clients disconnected; four jobs completed and all resources released");
    return 0;
}
