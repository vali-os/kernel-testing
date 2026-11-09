/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "test_protocol.h"
#include "../../modules/graphics/common/protocol_private.h"
#include <ctt_graphics_test_service_server.h>
#include <string.h>

#define GRAPHICS_TEST_MAX_TRANSFER 256U

/** The host fixture uses one serialized server, matching the common binding. */
static struct __TestBinding {
    gracht_server_t* Server;
    struct GraphicsTestBackend* Backend;
    struct GraphicsService* Service;
} g_test;

static oserr_t
__AuthenticateTest(
    _In_    const struct gracht_message*     message,
    _In_    const struct GraphicsSessionRef* session,
    _Out_   struct GraphicsRequest*          request)
{
    struct GraphicsSessionInfo info;
    oserr_t status;

    status = GraphicsRequestBegin(message, request);
    if (status != OS_EOK) {
        return status;
    }
    if (message->server != g_test.Server || request->Service != g_test.Service) {
        return OS_ENOTSUPPORTED;
    }
    status = GraphicsSessionQuery(request->Service, &request->Caller, session, &info);
    if (status == OS_EOK && info.Closing) {
        return OS_EBUSY;
    }
    return status;
}

void
ctt_graphics_test_describe_invocation(
    _In_    struct gracht_message*               message,
    _In_    const struct ctt_graphicssessionref* sessionRef)
{
    struct GraphicsRequest request;
    struct GraphicsSessionRef session = __GraphicsSessionFromWire(sessionRef);
    oserr_t status;

    status = __AuthenticateTest(message, &session, &request);
    ctt_graphics_test_describe_response(message, status,
        status == OS_EOK ? 1 : 0, status == OS_EOK ? GRAPHICS_TEST_MAX_TRANSFER : 0);
}

void
ctt_graphics_test_fill_invocation(
    _In_    struct gracht_message*               message,
    _In_    const struct ctt_graphicssessionref* sessionRef,
    _In_    uint64_t                             requestId,
    _In_    const struct ctt_graphicsobjectref*  contextRef,
    _In_    const struct ctt_graphicsobjectref*  bufferRef,
    _In_    uint64_t                             offset,
    _In_    uint64_t                             length,
    _In_    uint32_t                             value,
    _In_    uint32_t                             delayTicks,
    _In_    const struct ctt_graphicsobjectref*  dependencyRef)
{
    struct GraphicsRequest request;
    struct GraphicsSessionRef session = __GraphicsSessionFromWire(sessionRef);
    struct GraphicsJobRequest submission = {
        .RequestId = requestId,
        .Context = __GraphicsObjectFromWire(contextRef),
        .Buffer = __GraphicsObjectFromWire(bufferRef),
        .Dependency = __GraphicsObjectFromWire(dependencyRef),
        .Offset = offset,
        .Length = length
    };
    struct GraphicsTestFill fill = { .Value = value, .DelayTicks = delayTicks };
    struct GraphicsObjectRef completion;
    struct ctt_graphicsobjectref response = { 0 };
    oserr_t status;

    status = __AuthenticateTest(message, &session, &request);
    if (status == OS_EOK) {
        status = GraphicsTestSubmit(request.Service, &request.Caller, &session, &submission, &fill, &completion);
    }
    if (status == OS_EOK) {
        response.Slot = completion.Slot;
        response.Generation = completion.Generation;
        response.Type = completion.Type;
    }
    ctt_graphics_test_fill_response(message, status, &response);
}

void
ctt_graphics_test_write_invocation(
    _In_    struct gracht_message*               message,
    _In_    const struct ctt_graphicssessionref* sessionRef,
    _In_    const struct ctt_graphicsobjectref*  bufferRef,
    _In_    uint64_t                             offset,
    _In_    const uint8_t*                       bytes,
    _In_    uint32_t                             bytesCount)
{
    struct GraphicsRequest request;
    struct GraphicsSessionRef session = __GraphicsSessionFromWire(sessionRef);
    struct GraphicsObjectRef buffer = __GraphicsObjectFromWire(bufferRef);
    oserr_t status;

    status = __AuthenticateTest(message, &session, &request);
    if (status == OS_EOK && (bytesCount == 0 || bytesCount > GRAPHICS_TEST_MAX_TRANSFER)) {
        status = OS_EINVALPARAMS;
    }
    if (status == OS_EOK) {
        status = GraphicsBufferWrite(request.Service, &request.Caller, &session, &buffer, offset, bytes, bytesCount);
    }
    ctt_graphics_test_write_response(message, status);
}

void
ctt_graphics_test_read_invocation(
    _In_    struct gracht_message*               message,
    _In_    const struct ctt_graphicssessionref* sessionRef,
    _In_    const struct ctt_graphicsobjectref*  bufferRef,
    _In_    uint64_t                             offset,
    _In_    uint32_t                             length)
{
    struct GraphicsRequest request;
    struct GraphicsSessionRef session = __GraphicsSessionFromWire(sessionRef);
    struct GraphicsObjectRef buffer = __GraphicsObjectFromWire(bufferRef);
    uint8_t bytes[GRAPHICS_TEST_MAX_TRANSFER] = { 0 };
    oserr_t status;

    status = __AuthenticateTest(message, &session, &request);
    if (status == OS_EOK && (length == 0 || length > sizeof(bytes))) {
        status = OS_EINVALPARAMS;
    }
    if (status == OS_EOK) {
        status = GraphicsBufferRead(request.Service, &request.Caller, &session, &buffer, offset, bytes, length);
    }
    // A backend may copy some bytes before returning an access error. Error
    // replies send an empty array, never partial or uninitialized readback.
    ctt_graphics_test_read_response(message, status, bytes, status == OS_EOK ? length : 0);
}

void
ctt_graphics_test_advance_invocation(
    _In_    struct gracht_message*               message,
    _In_    const struct ctt_graphicssessionref* sessionRef,
    _In_    uint32_t                             ticks)
{
    struct GraphicsRequest request;
    struct GraphicsSessionRef session = __GraphicsSessionFromWire(sessionRef);
    oserr_t status;

    status = __AuthenticateTest(message, &session, &request);
    if (status == OS_EOK && (ticks == 0 || ticks > 1024)) {
        status = OS_EINVALPARAMS;
    }
    if (status == OS_EOK) {
        status = GraphicsTestAdvance(g_test.Backend, request.Service, ticks);
    }
    ctt_graphics_test_advance_response(message, status);
}

oserr_t
GraphicsTestProtocolAttach(
    _In_    gracht_server_t*            server,
    _In_    struct GraphicsTestBackend* backend,
    _In_    struct GraphicsService*     service)
{
    if (g_test.Server != NULL) {
        return OS_EBUSY;
    }
    if (gracht_server_register_protocol(server, &ctt_graphics_test_server_protocol)) {
        return OS_EUNKNOWN;
    }
    g_test.Server = server;
    g_test.Backend = backend;
    g_test.Service = service;
    return OS_EOK;
}

void
GraphicsTestProtocolDetach(
    _In_    gracht_server_t* server)
{
    gracht_server_unregister_protocol(server, &ctt_graphics_test_server_protocol);
    memset(&g_test, 0, sizeof(g_test));
}
