/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef __GRAPHICS_TEST_PROTOCOL_H__
#define __GRAPHICS_TEST_PROTOCOL_H__
#include "backend.h"
#include <graphics/protocol.h>

/**
 * @brief Register software commands beside an already attached common protocol.
 * @param server Serialized host test server; only one fixture can attach.
 * @param backend Test device used by the attached common core.
 * @param service That core, valid until Detach.
 * @return OS_EOK or registration error. This API is never linked into Vali.
 */
oserr_t
GraphicsTestProtocolAttach(
    _In_    gracht_server_t*            server,
    _In_    struct GraphicsTestBackend* backend,
    _In_    struct GraphicsService*     service);

/**
 * @brief Remove the test binding before detaching common protocols or teardown.
 * @param server The server passed to Attach.
 */
void
GraphicsTestProtocolDetach(
    _In_    gracht_server_t* server);
#endif
