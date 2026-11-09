/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef __GRAPHICS_HOST_TRANSPORT_H__
#define __GRAPHICS_HOST_TRANSPORT_H__
#include <graphics/protocol.h>
#include <gracht/client.h>

/**
 * @brief Open a Linux abstract Unix socket for the host acceptance harness.
 * @param name Unique socket name; no filesystem entry is created or removed.
 * @param connected Optional callback recording accepted server endpoints.
 * @param disconnected Optional fixture hook; defaults to core disconnect cleanup.
 * @param listenerOut Receives the descriptor used to drive accept events.
 * @return Created server. Setup failures abort this test process.
 */
gracht_server_t*
GraphicsHostListen(
    _In_ const char* name,
    _In_ void (*connected)(gracht_conn_t),
    _In_ void (*disconnected)(gracht_conn_t),
    _Out_ int* listenerOut);

/**
 * @brief Connect a client for calls through the generated graphics bindings.
 * @param name Socket name selected by GraphicsHostListen.
 * @return Connected client. Setup failures abort this test process.
 */
gracht_client_t*
GraphicsHostConnect(
    _In_ const char* name);

/**
 * @brief Convert authenticated Linux connection credentials for host testing.
 * @param message Request received from an AF_UNIX stream connection.
 * @param callerOut Receives peer PID and Gracht endpoint.
 * @return OS_EOK or OS_EPERMISSIONS. This adapter is never linked into Vali.
 */
oserr_t
GraphicsHostAuthenticate(
    _In_ const struct gracht_message* message,
    _Out_ struct GraphicsCaller* callerOut);

#endif
