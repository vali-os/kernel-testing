/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "protocol_buffers_test.h"
#include <ctt_graphics_test_service_client.h>
#include <assert.h>
#include <string.h>

/** One route whose events are driven directly, with no timing assumptions. */
struct __Route {
    gracht_server_t* Server;
    gracht_client_t* Client;
    gracht_conn_t Endpoint;
    struct gracht_message_context Call;
};

static void
__Exchange(
    _InOut_ struct __Route* route)
{
    assert(gracht_server_handle_event(route->Server, route->Endpoint, 0) == 0);
    assert(gracht_client_await(route->Client, &route->Call, GRACHT_AWAIT_ALL) == 0);
}

static void
__ReadError(
    _InOut_ struct __Route*                      route,
    _In_    const struct ctt_graphicssessionref* session,
    _In_    const struct ctt_graphicsobjectref*  buffer,
    _In_    uint64_t                             offset,
    _In_    uint32_t                             length,
    _In_    oserr_t                              expected)
{
    uint8_t bytes[256];
    uint32_t count = sizeof(bytes);
    oserr_t status;

    memset(bytes, 0x7b, sizeof(bytes));
    assert(ctt_graphics_test_read(route->Client, &route->Call, session, buffer, offset, length) == 0);
    __Exchange(route);
    assert(ctt_graphics_test_read_result(route->Client, &route->Call, &status, bytes, &count) == 0);
    assert(status == expected && count == 0 && bytes[0] == 0x7b);
}

void
GraphicsTestProtocolBuffers(
    _In_    gracht_server_t*                     server,
    _In_    struct GraphicsService*              backendService,
    _In_    gracht_client_t*                     client,
    _In_    gracht_conn_t                        endpoint,
    _In_    const struct ctt_graphicssessionref* session,
    _In_    const struct ctt_graphicsobjectref*  context,
    _In_    const struct ctt_graphicssessionref* otherSession,
    _InOut_ struct GraphicsTestBackend*          backend)
{
    struct __Route route = { .Server = server, .Client = client, .Endpoint = endpoint };
    struct ctt_graphicsobjectref buffer;
    struct ctt_graphicsobjectref completion;
    struct ctt_graphicsobjectref retry;
    struct ctt_graphicsobjectref none = { 0 };
    struct ctt_graphicsresourcelimits limits;
    uint8_t bytes[257];
    uint32_t count;
    uint32_t i;
    uint32_t version;
    uint32_t maxTransfer;
    oserr_t status;
    oserr_t result;

    assert(ctt_graphics_get_resource_limits(client, &route.Call, session) == 0);
    __Exchange(&route);
    assert(ctt_graphics_get_resource_limits_result(client, &route.Call, &status, &limits) == 0);
    assert(status == OS_EOK && limits.MaxJobs == 8 && limits.MaxBufferBytes == 4096);
    assert(ctt_graphics_test_describe(client, &route.Call, session) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_describe_result(client, &route.Call, &status, &version, &maxTransfer) == 0);
    assert(status == OS_EOK && version == 1 && maxTransfer == 256);
    assert(ctt_graphics_create_buffer(client, &route.Call, session, 1, 64) == 0);
    __Exchange(&route);
    assert(ctt_graphics_create_buffer_result(client, &route.Call, &status, &buffer) == 0);
    assert(status == OS_EOK);
    __ReadError(&route, otherSession, &buffer, 0, 64, OS_EPERMISSIONS);
    __ReadError(&route, session, &buffer, UINT64_MAX, 64, OS_EINVALPARAMS);
    __ReadError(&route, session, &buffer, 0, 257, OS_EINVALPARAMS);
    __ReadError(&route, session, &buffer, 0, 0, OS_EINVALPARAMS);
    backend->Memory.FailEnd = true;
    __ReadError(&route, session, &buffer, 0, 64, OS_EDEVFAULT);
    backend->Memory.FailEnd = false;
    assert(GraphicsServiceCollect(backendService) == OS_EOK);

    memset(bytes, 0x38, sizeof(bytes));
    assert(ctt_graphics_test_write(client, &route.Call, session, &buffer, 0, bytes, sizeof(bytes)) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_write_result(client, &route.Call, &status) == 0);
    assert(status == OS_EINVALPARAMS);
    assert(ctt_graphics_test_write(client, &route.Call, session, &buffer, 0, bytes, 64) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_write_result(client, &route.Call, &status) == 0 && status == OS_EOK);

    assert(ctt_graphics_test_fill(client, &route.Call, otherSession, 1, context,
        &buffer, 0, 64, 0xa1, 2, &none) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_fill_result(client, &route.Call, &status, &completion) == 0);
    assert(status == OS_EPERMISSIONS && completion.Generation == 0);
    assert(ctt_graphics_test_fill(client, &route.Call, session, 1, context,
        &buffer, UINT64_MAX, 64, 0xa1, 2, &none) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_fill_result(client, &route.Call, &status, &completion) == 0);
    assert(status == OS_EINVALPARAMS && completion.Slot == 0 && backend->Execution.PrepareCalls == 0);

    assert(ctt_graphics_test_fill(client, &route.Call, session, 1, context,
        &buffer, 8, 48, 0xa1, 2, &none) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_fill_result(client, &route.Call, &status, &completion) == 0 && status == OS_EOK);
    assert(ctt_graphics_test_fill(client, &route.Call, session, 1, context,
        &buffer, 8, 48, 0xa1, 2, &none) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_fill_result(client, &route.Call, &status, &retry) == 0 && status == OS_EOK);
    assert(retry.Generation == completion.Generation && backend->Execution.PrepareCalls == 1);
    assert(ctt_graphics_test_fill(client, &route.Call, session, 1, context,
        &buffer, 8, 48, 0xa2, 2, &none) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_fill_result(client, &route.Call, &status, &retry) == 0);
    assert(status == OS_EPROTOCOL && retry.Generation == 0);
    __ReadError(&route, session, &buffer, 0, 64, OS_EBUSY);
    assert(ctt_graphics_poll_completion(client, &route.Call, session, &completion) == 0);
    __Exchange(&route);
    assert(ctt_graphics_poll_completion_result(client, &route.Call, &status, &result) == 0);
    assert(status == OS_EINPROGRESS && result == OS_EOK);
    assert(ctt_graphics_poll_completion(client, &route.Call, otherSession, &completion) == 0);
    __Exchange(&route);
    assert(ctt_graphics_poll_completion_result(client, &route.Call, &status, &result) == 0);
    assert(status == OS_EPERMISSIONS && result == OS_EOK);

    assert(ctt_graphics_test_advance(client, &route.Call, session, 3) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_advance_result(client, &route.Call, &status) == 0 && status == OS_EOK);
    assert(ctt_graphics_poll_completion(client, &route.Call, session, &completion) == 0);
    __Exchange(&route);
    assert(ctt_graphics_poll_completion_result(client, &route.Call, &status, &result) == 0);
    assert(status == OS_EOK && result == OS_EOK);
    count = sizeof(bytes);
    assert(ctt_graphics_test_read(client, &route.Call, session, &buffer, 0, 64) == 0);
    __Exchange(&route);
    assert(ctt_graphics_test_read_result(client, &route.Call, &status, bytes, &count) == 0);
    assert(status == OS_EOK && count == 64);
    for (i = 0; i < count; i++) {
        assert(bytes[i] == (i >= 8 && i < 56 ? 0xa1 : 0x38));
    }
    assert(ctt_graphics_release_buffer(client, &route.Call, session, &buffer) == 0);
    __Exchange(&route);
    assert(ctt_graphics_release_buffer_result(client, &route.Call, &status) == 0 && status == OS_EOK);
    __ReadError(&route, session, &buffer, 0, 64, OS_ENOENT);
    assert(ctt_graphics_release_completion(client, &route.Call, session, &completion) == 0);
    __Exchange(&route);
    assert(ctt_graphics_release_completion_result(client, &route.Call, &status) == 0 && status == OS_EOK);
    assert(ctt_graphics_poll_completion(client, &route.Call, session, &completion) == 0);
    __Exchange(&route);
    assert(ctt_graphics_poll_completion_result(client, &route.Call, &status, &result) == 0);
    assert(status == OS_ENOENT && result == OS_EOK);
}
