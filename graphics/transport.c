/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "transport.h"
#include <gracht/link/socket.h>
#include <assert.h>
#include <string.h>
#include <sys/un.h>

static socklen_t
__Address(
    _In_ const char* name,
    _Out_ struct sockaddr_storage* storage)
{
    struct sockaddr_un* address = (struct sockaddr_un*)storage;
    size_t length = strlen(name);

    assert(length > 0 && length < sizeof(address->sun_path) - 1);
    memset(storage, 0, sizeof(*storage));
    address->sun_family = AF_UNIX;
    // A leading zero selects Linux's abstract socket namespace. The name
    // disappears when the server closes, even if a test terminates abruptly.
    memcpy(address->sun_path + 1, name, length);
    return (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + length);
}

gracht_server_t*
GraphicsHostListen(
    _In_ const char* name,
    _In_ void (*connected)(gracht_conn_t),
    _In_ void (*disconnected)(gracht_conn_t),
    _Out_ int* listenerOut)
{
    gracht_server_configuration_t configuration;
    struct sockaddr_storage address;
    socklen_t addressLength;
    struct gracht_link_socket* link;
    gracht_server_t* server;

    addressLength = __Address(name, &address);
    gracht_server_configuration_init(&configuration);
    gracht_server_configuration_set_num_workers(&configuration, 0);
    gracht_server_configuration_set_max_msg_size(&configuration, 16384);
    configuration.callbacks.clientConnected = connected;
    configuration.callbacks.clientDisconnected =
        disconnected != NULL ? disconnected : GraphicsProtocolDisconnected;
    assert(gracht_server_create(&configuration, &server) == 0);
    assert(gracht_link_socket_create(&link) == 0);
    gracht_link_socket_set_domain(link, AF_UNIX);
    gracht_link_socket_set_type(link, gracht_link_stream_based);
    gracht_link_socket_set_listen(link, 1);
    gracht_link_socket_set_bind_address(link, &address, addressLength);
    assert(gracht_server_add_link(server, (struct gracht_link*)link) == 0);
    *listenerOut = gracht_link_get_handle((struct gracht_link*)link);
    return server;
}

gracht_client_t*
GraphicsHostConnect(
    _In_ const char* name)
{
    struct gracht_client_configuration configuration;
    struct sockaddr_storage address;
    socklen_t addressLength;
    struct gracht_link_socket* link;
    gracht_client_t* client;

    addressLength = __Address(name, &address);
    assert(gracht_link_socket_create(&link) == 0);
    gracht_link_socket_set_domain(link, AF_UNIX);
    gracht_link_socket_set_type(link, gracht_link_stream_based);
    gracht_link_socket_set_connect_address(link, &address, addressLength);
    gracht_client_configuration_init(&configuration);
    gracht_client_configuration_set_link(&configuration, (struct gracht_link*)link);
    gracht_client_configuration_set_max_msg_size(&configuration, 16384);
    assert(gracht_client_create(&configuration, &client) == 0);
    assert(gracht_client_connect(client) == 0);
    // These contracts currently have replies but no events, so the generator
    // does not emit a client event-dispatch table to register.
    return client;
}

oserr_t
GraphicsHostAuthenticate(
    _In_ const struct gracht_message* message,
    _Out_ struct GraphicsCaller* callerOut)
{
    const struct gracht_security_context* security;

    security = gracht_server_get_security_context(message);
    if (security == NULL || security->source != GRACHT_SECURITY_KERNEL) {
        return OS_EPERMISSIONS;
    }
    if (security->capture != GRACHT_SECURITY_CAPTURE_CONNECTION || security->pid <= 0) {
        return OS_EPERMISSIONS;
    }
    // PID is sufficient for this host test because the connection is also part
    // of every lookup and disconnect is delivered before its descriptor reuse.
    // Production Vali uses its boot-stable principal through a separate adapter.
    callerOut->Principal = (uint64_t)security->pid;
    callerOut->Endpoint = (uint64_t)message->client;
    return OS_EOK;
}
