/**
 * Grant contract regression: exact bytes, conversions and generated dispatch.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ddk/convert/grants.h>
#include <sys_grant_test_service_client.h>
#include <sys_grant_test_service_server.h>

static const SHMTransfer_t g_transfer = {
    .Token = {
        .Version = GRANT_TOKEN_VERSION,
        .ControlHandleID = 0x78563412,
        .ExportGlobalHandleID = 0x78563412,
        .Options = {
            .ResourceType = GrantTokenResourceTypeSHM,
            .Data.SHM.Offset = 0xfedcba98,
            .Data.SHM.Length = 0x90abcdef,
            .TargetGlobalHandleID = 0x87654321,
            .Scope = GrantTokenScopeRead | GrantTokenScopeWrite,
            .TargetType = GrantTokenTargetTypeSpecific
        },
        .Secret = { 0x00, 0x11, 0x22, 0x83, 0x44, 0x55, 0x66, 0xf7,
                    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff }
    },
    .RangeMode = SHMTransferRangeSubrange,
    .RelativeOffset = 0x11223344,
    .RequestedLength = 0x55667788
};

// Independent wire fixture, including asymmetric bytes and high bits.
static const uint8_t g_expected[60] = {
    0x02, 0x00, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78,
    0x01, 0x00, 0x00, 0x00,
    0x98, 0xba, 0xdc, 0xfe, 0xef, 0xcd, 0xab, 0x90,
    0x21, 0x43, 0x65, 0x87, 0x03, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x11, 0x22, 0x83,
    0x44, 0x55, 0x66, 0xf7, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x01, 0x00, 0x00, 0x00,
    0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55
};

static char         g_message[71];
static uint32_t     g_messageLength;
static unsigned int g_calls;

int
gracht_client_get_buffer(
    _In_  gracht_client_t* client,
    _Out_ gracht_buffer_t* buffer)
{
    (void)client;
    memset(g_message, 0, sizeof(g_message));
    *buffer = (gracht_buffer_t) { .data = g_message, .limit = sizeof(g_message) };
    return 0;
}

int
gracht_client_invoke(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ gracht_buffer_t*               buffer)
{
    (void)client;
    (void)context;
    assert(buffer->error == 0);
    g_messageLength = buffer->index;
    return 0;
}

static void
__AssertTokenEqual(
    _In_ const GrantToken_t* expected,
    _In_ const GrantToken_t* actual)
{
    assert(actual->Version == expected->Version);
    assert(actual->ControlHandleID == expected->ControlHandleID);
    assert(actual->ExportGlobalHandleID == expected->ExportGlobalHandleID);
    assert(actual->Options.ResourceType == expected->Options.ResourceType);
    assert(actual->Options.Data.SHM.Offset == expected->Options.Data.SHM.Offset);
    assert(actual->Options.Data.SHM.Length == expected->Options.Data.SHM.Length);
    assert(actual->Options.Scope == expected->Options.Scope);
    assert(actual->Options.TargetType == expected->Options.TargetType);
    assert(actual->Options.TargetGlobalHandleID == expected->Options.TargetGlobalHandleID);
    assert(memcmp(actual->Secret, expected->Secret, 16) == 0);
}

void
sys_grant_test_transfer_invocation(
    _In_ struct gracht_message*         message,
    _In_ const struct sys_shm_transfer* description)
{
    SHMTransfer_t decoded;

    (void)message;
    assert(__FromSysSHMTransfer(description, &decoded) == OS_EOK);
    __AssertTokenEqual(&g_transfer.Token, &decoded.Token);
    assert(decoded.RangeMode == g_transfer.RangeMode);
    assert(decoded.RelativeOffset == g_transfer.RelativeOffset);
    assert(decoded.RequestedLength == g_transfer.RequestedLength);
    g_calls++;
}

void
sys_grant_test_token_invocation(
    _In_ struct gracht_message*        message,
    _In_ const struct sys_grant_token* description)
{
    GrantToken_t decoded;

    (void)message;
    assert(__FromSysGrantToken(description, &decoded) == OS_EOK);
    __AssertTokenEqual(&g_transfer.Token, &decoded);
    g_calls++;
}

static void
__TestGeneratedRequests(void)
{
    struct sys_shm_transfer transfer;
    struct sys_grant_token token;
    gracht_buffer_t        buffer;
    uint32_t               size;
    unsigned int           method;
    unsigned int           calls;
    void (*dispatch)(struct gracht_message*, gracht_buffer_t*);

    assert(__ToSysSHMTransfer(&g_transfer, &transfer) == OS_EOK);
    assert(__ToSysGrantToken(&g_transfer.Token, &token) == OS_EOK);
    assert(token.secret0 == 0x83221100 && token.secret3 == 0xffeeddcc);
    for (method = 0; method < 2; method++) {
        size = method == 0 ? 60 : 48;
        if (method == 0) {
            assert(sys_grant_test_transfer(NULL, NULL, &transfer) == 0);
        } else {
            assert(sys_grant_test_token(NULL, NULL, &token) == 0);
        }
        assert(g_messageLength == 11 + size);
        assert(memcmp(g_message + 11, g_expected, size) == 0);
        calls = g_calls;
        dispatch = sys_grant_test_server_protocol.functions[method].address;
        buffer = (gracht_buffer_t) { .data = g_message, .index = 11, .limit = g_messageLength };
        dispatch(NULL, &buffer);
        assert(buffer.error == 0 && buffer.index == g_messageLength);
        assert(g_calls == calls + 1);

        // Every shortened payload must fail before the service callback runs.
        for (size = 11; size < g_messageLength; size++) {
            calls = g_calls;
            buffer = (gracht_buffer_t) { .data = g_message, .index = 11, .limit = size };
            dispatch(NULL, &buffer);
            assert(buffer.error == EMSGSIZE);
            assert(g_calls == calls);
        }
    }
}

/**
 * @brief One invalid field expressed in native and contract coordinates.
 */
struct __InvalidField {
    size_t   NativeOffset;
    size_t   WireOffset;
    uint32_t Value;
};

static void
__TestInvalidTokens(void)
{
    static const struct __InvalidField cases[] = {
        { offsetof(GrantToken_t, Version), 0, 0 },
        { offsetof(GrantToken_t, Version), 0, 1 },
        { offsetof(GrantToken_t, Version), 0, 3 },
        { offsetof(GrantToken_t, ExportGlobalHandleID), 4, 0 },
        { offsetof(GrantToken_t, Options.ResourceType), 8, 0 },
        { offsetof(GrantToken_t, Options.ResourceType), 8, 2 },
        { offsetof(GrantToken_t, Options.ResourceType), 8, UINT32_MAX },
        { offsetof(GrantToken_t, Options.Data.SHM.Length), 16, 0 },
        { offsetof(GrantToken_t, Options.TargetGlobalHandleID), 20, 0 },
        { offsetof(GrantToken_t, Options.Scope), 24, 0 },
        { offsetof(GrantToken_t, Options.Scope), 24, 4 },
        { offsetof(GrantToken_t, Options.Scope), 24, 5 },
        { offsetof(GrantToken_t, Options.Scope), 24, 6 },
        { offsetof(GrantToken_t, Options.Scope), 24, 7 },
        { offsetof(GrantToken_t, Options.Scope), 24, UINT32_MAX },
        { offsetof(GrantToken_t, Options.TargetType), 28, 0 },
        { offsetof(GrantToken_t, Options.TargetType), 28, 2 },
        { offsetof(GrantToken_t, Options.TargetType), 28, UINT32_MAX }
    };
    GrantToken_t          native;
    GrantToken_t          decoded;
    struct sys_grant_token contract;
    struct sys_grant_token output;
    uint8_t               before[sizeof(output)];
    uint8_t               nativeBefore[sizeof(decoded)];
    char                  wire[48];
    gracht_buffer_t       buffer;
    size_t                i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        native = g_transfer.Token;
        memcpy((char*)&native + cases[i].NativeOffset, &cases[i].Value, 4);
        memset(&output, 0xa5, sizeof(output));
        memcpy(before, &output, sizeof(output));
        assert(__ToSysGrantToken(&native, &output) == OS_EINVALPARAMS);
        assert(memcmp(before, &output, sizeof(output)) == 0);

        memcpy(wire, g_expected, sizeof(wire));
        memcpy(wire + cases[i].WireOffset, &cases[i].Value, 4);
        buffer = (gracht_buffer_t) { .data = wire, .limit = sizeof(wire) };
        deserialize_sys_grant_token(&buffer, &contract);
        assert(buffer.error == 0);
        memset(&decoded, 0xa5, sizeof(decoded));
        memcpy(nativeBefore, &decoded, sizeof(decoded));
        assert(__FromSysGrantToken(&contract, &decoded) == OS_EINVALPARAMS);
        assert(memcmp(nativeBefore, &decoded, sizeof(decoded)) == 0);
    }

    native = g_transfer.Token;
    native.ControlHandleID++;
    assert(__ToSysGrantToken(&native, &output) == OS_EINVALPARAMS);
    native = g_transfer.Token;
    native.Options.TargetType = GrantTokenTargetTypeAny;
    native.Options.TargetGlobalHandleID = UUID_INVALID;
    for (i = 1; i <= 3; i++) {
        native.Options.Scope = (GrantTokenScope_t)i;
        assert(__ToSysGrantToken(&native, &output) == OS_EOK);
        assert(__FromSysGrantToken(&output, &decoded) == OS_EOK);
        __AssertTokenEqual(&native, &decoded);
    }

    // Conversion is structural validation, not authentication of a secret.
    output.secret0 ^= 1;
    assert(__FromSysGrantToken(&output, &decoded) == OS_EOK);
    assert(decoded.Secret[0] != native.Secret[0]);
    assert(__ToSysGrantToken(NULL, &output) == OS_EINVALPARAMS);
    assert(__ToSysGrantToken(&native, NULL) == OS_EINVALPARAMS);
    assert(__FromSysGrantToken(NULL, &decoded) == OS_EINVALPARAMS);
    assert(__FromSysGrantToken(&output, NULL) == OS_EINVALPARAMS);
}

/**
 * @brief Selector cases with independently specified expected validity.
 */
struct __RangeCase {
    uint32_t Mode;
    uint32_t Offset;
    uint32_t Length;
    uint32_t TokenLength;
    bool     Valid;
};

static void
__TestRanges(void)
{
    static const struct __RangeCase cases[] = {
        { 0, 0, 0, 1, true },
        { 0, 0, 0, UINT32_MAX, true },
        { 0, 1, 0, 10, false },
        { 0, 0, 1, 10, false },
        { 0, 0, 0, 0, false },
        { 1, 0, 1, 1, true },
        { 1, 3, 7, 10, true },
        { 1, 3, 8, 10, false },
        { 1, 10, 1, 10, false },
        { 1, 11, 1, 10, false },
        { 1, 0, 0, 10, false },
        { 1, 0, UINT32_MAX, UINT32_MAX, true },
        { 1, UINT32_MAX - 1, 1, UINT32_MAX, true },
        { 1, UINT32_MAX - 1, 2, UINT32_MAX, false },
        { 1, UINT32_MAX, UINT32_MAX, UINT32_MAX, false },
        { 2, 0, 0, 10, false },
        { UINT32_MAX, 0, 0, 10, false }
    };
    SHMTransfer_t           native;
    SHMTransfer_t           decoded;
    struct sys_shm_transfer contract;
    struct sys_shm_transfer output;
    uint8_t                 before[sizeof(output)];
    uint8_t                 nativeBefore[sizeof(decoded)];
    char                    wire[60];
    gracht_buffer_t         buffer;
    size_t                  i;
    oserr_t                 expected;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        native = g_transfer;
        native.Token.Options.Data.SHM.Offset = UINT32_MAX;
        native.Token.Options.Data.SHM.Length = cases[i].TokenLength;
        native.RangeMode = (SHMTransferRangeMode_t)cases[i].Mode;
        native.RelativeOffset = cases[i].Offset;
        native.RequestedLength = cases[i].Length;
        expected = cases[i].Valid ? OS_EOK : OS_EINVALPARAMS;
        memset(&output, 0xa5, sizeof(output));
        memcpy(before, &output, sizeof(output));
        assert(__ToSysSHMTransfer(&native, &output) == expected);
        if (!cases[i].Valid) {
            assert(memcmp(before, &output, sizeof(output)) == 0);
        }

        assert(__ToSysSHMTransfer(&g_transfer, &contract) == OS_EOK);
        contract.token.data0 = UINT32_MAX;
        contract.token.data1 = cases[i].TokenLength;
        contract.range_mode = cases[i].Mode;
        contract.relative_offset = cases[i].Offset;
        contract.requested_length = cases[i].Length;
        memset(&decoded, 0xa5, sizeof(decoded));
        memcpy(nativeBefore, &decoded, sizeof(decoded));
        assert(__FromSysSHMTransfer(&contract, &decoded) == expected);
        if (cases[i].Valid) {
            __AssertTokenEqual(&native.Token, &decoded.Token);
            assert(decoded.RangeMode == native.RangeMode);
            assert(decoded.RelativeOffset == native.RelativeOffset);
            assert(decoded.RequestedLength == native.RequestedLength);
        } else {
            assert(memcmp(nativeBefore, &decoded, sizeof(decoded)) == 0);
        }
    }

    native = g_transfer;
    native.RangeMode = SHMTransferRangeFull;
    native.RelativeOffset = 0;
    native.RequestedLength = 0;
    assert(__ToSysSHMTransfer(&native, &contract) == OS_EOK);
    buffer = (gracht_buffer_t) { .data = wire, .limit = sizeof(wire) };
    serialize_sys_shm_transfer(&buffer, &contract);
    assert(buffer.error == 0 && buffer.index == 60);
    assert(memcmp(wire, g_expected, 48) == 0);
    for (i = 48; i < 60; i++) {
        assert(wire[i] == 0);
    }
    assert(__ToSysSHMTransfer(NULL, &output) == OS_EINVALPARAMS);
    assert(__ToSysSHMTransfer(&native, NULL) == OS_EINVALPARAMS);
    assert(__FromSysSHMTransfer(NULL, &decoded) == OS_EINVALPARAMS);
    assert(__FromSysSHMTransfer(&contract, NULL) == OS_EINVALPARAMS);
}

int
main(void)
{
    __TestGeneratedRequests();
    __TestInvalidTokens();
    __TestRanges();
    puts("grant contract tests passed");
    return 0;
}
