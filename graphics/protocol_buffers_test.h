/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef __GRAPHICS_PROTOCOL_BUFFERS_TEST_H__
#define __GRAPHICS_PROTOCOL_BUFFERS_TEST_H__
#include "backend.h"
#include "transport.h"
#include <ctt_graphics_service_client.h>

/**
 * @brief Exercise buffer/job RPC behavior on an existing two-client fixture.
 * @param server Attached common/test server on the calling executor.
 * @param backendService Core used for retrying a failed CPU End.
 * @param client First client's connection.
 * @param endpoint First client's accepted server endpoint.
 * @param session First client's active session.
 * @param context First client's active context.
 * @param otherSession Other client's session, used for authorization failures.
 * @param backend Test state for controlled storage failure injection.
 */
void
GraphicsTestProtocolBuffers(
    _In_    gracht_server_t*                     server,
    _In_    struct GraphicsService*              backendService,
    _In_    gracht_client_t*                     client,
    _In_    gracht_conn_t                        endpoint,
    _In_    const struct ctt_graphicssessionref* session,
    _In_    const struct ctt_graphicsobjectref*  context,
    _In_    const struct ctt_graphicssessionref* otherSession,
    _InOut_ struct GraphicsTestBackend*          backend);
#endif
