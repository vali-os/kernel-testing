/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "transport.h"
#include <ctt_graphics_service_client.h>
#include <ctt_graphics_test_service_client.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

int
main(
    _In_    int    argc,
    _In_    char** argv)
{
    gracht_client_t* client;
    struct gracht_message_context call = { 0 };
    struct ctt_graphicsdeviceinfo device;
    struct ctt_graphicssessionref session;
    struct ctt_graphicsobjectref context;
    struct ctt_graphicscontextinfo contextInfo;
    struct ctt_graphicsobjectref buffer;
    struct ctt_graphicsobjectref completion;
    struct ctt_graphicsobjectref retry;
    struct ctt_graphicsobjectref noDependency = { 0 };
    uint8_t bytes[64];
    uint32_t length;
    uint32_t i;
    oserr_t result;
    oserr_t status;

    if (argc != 2) {
        fprintf(stderr, "usage: graphics-test-client SOCKET_NAME\n");
        return 2;
    }
    client = GraphicsHostConnect(argv[1]);

    // Discovery supplies the version and identity used by open. A client must
    // negotiate against the service it reached, not guess a hardware ABI.
    assert(ctt_graphics_get_device(client, &call, 0) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_get_device_result(client, &call, &status, &device) == 0);
    assert(status == OS_EOK && device.Backend == CTT_GRAPHICSBACKEND_TEST);
    assert(ctt_graphics_open(client, &call, device.DeviceId, device.CommonVersion, 0) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_open_result(client, &call, &status, &session) == 0);
    assert(status == OS_EOK);

    assert(ctt_graphics_create_context(client, &call, &session, 1, 0) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_create_context_result(client, &call, &status, &context) == 0);
    assert(status == OS_EOK);
    assert(ctt_graphics_query_context(client, &call, &session, &context) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_query_context_result(client, &call, &status, &contextInfo) == 0);
    assert(status == OS_EOK && !contextInfo.Closing);

    assert(ctt_graphics_create_buffer(client, &call, &session, 1, sizeof(bytes)) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_create_buffer_result(client, &call, &status, &buffer) == 0);
    assert(status == OS_EOK);
    memset(bytes, 0x31, sizeof(bytes));
    assert(ctt_graphics_test_write(client, &call, &session, &buffer, 0, bytes, sizeof(bytes)) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_test_write_result(client, &call, &status) == 0 && status == OS_EOK);

    assert(ctt_graphics_test_fill(client, &call, &session, 1, &context, &buffer,
        16, 32, 0xa6, 3, &noDependency) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_test_fill_result(client, &call, &status, &completion) == 0 && status == OS_EOK);
    // Repeat as if the acceptance reply had been lost. The operation must still
    // have exactly one completion, independent of any progress between replies.
    assert(ctt_graphics_test_fill(client, &call, &session, 1, &context, &buffer,
        16, 32, 0xa6, 3, &noDependency) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_test_fill_result(client, &call, &status, &retry) == 0 && status == OS_EOK);
    assert(retry.Generation == completion.Generation && retry.Slot == completion.Slot);

    for (i = 0; i < 16; i++) {
        assert(ctt_graphics_poll_completion(client, &call, &session, &completion) == 0);
        assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
        assert(ctt_graphics_poll_completion_result(client, &call, &status, &result) == 0);
        if (status == OS_EOK) {
            break;
        }
        assert(status == OS_EINPROGRESS && result == OS_EOK);
        assert(ctt_graphics_test_advance(client, &call, &session, 1) == 0);
        assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
        assert(ctt_graphics_test_advance_result(client, &call, &status) == 0);
        assert(status == OS_EOK || status == OS_EBUSY);
    }
    assert(i < 16 && status == OS_EOK && result == OS_EOK);
    length = sizeof(bytes);
    assert(ctt_graphics_test_read(client, &call, &session, &buffer, 0, length) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_test_read_result(client, &call, &status, bytes, &length) == 0);
    assert(status == OS_EOK && length == sizeof(bytes));
    for (i = 0; i < sizeof(bytes); i++) {
        assert(bytes[i] == (i >= 16 && i < 48 ? 0xa6 : 0x31));
    }
    assert(ctt_graphics_release_completion(client, &call, &session, &completion) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_release_completion_result(client, &call, &status) == 0 && status == OS_EOK);

    // Leave a second job pending, with both public references removed. The
    // server's own timer must finish it after this process disconnects.
    assert(ctt_graphics_test_fill(client, &call, &session, 2, &context, &buffer,
        0, 64, 0x77, 20, &noDependency) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_test_fill_result(client, &call, &status, &completion) == 0 && status == OS_EOK);
    assert(ctt_graphics_release_buffer(client, &call, &session, &buffer) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_release_buffer_result(client, &call, &status) == 0 && status == OS_EOK);
    assert(ctt_graphics_release_completion(client, &call, &session, &completion) == 0);
    assert(gracht_client_await(client, &call, GRACHT_AWAIT_ALL) == 0);
    assert(ctt_graphics_release_completion_result(client, &call, &status) == 0 && status == OS_EOK);
    gracht_client_shutdown(client);
    puts("graphics client: uploaded, submitted, replayed, completed and verified partial-fill readback");
    return 0;
}
