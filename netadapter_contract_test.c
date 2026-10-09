/**
 * Wire regression for the asynchronous netadapter contract.
 *
 * Exercise actual generated request/event encoders and event decoders. Only the
 * transport allocation/send boundary is substituted. Link-time section removal
 * discards unrelated generated callbacks, so no fake driver implementation is
 * needed merely to check message flags, array bounds, correlation and framing.
 */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <ctt_netadapter_service_client.h>
#include <ctt_netadapter_service_server.h>

#define OP(name) SERVICE_CTT_NETADAPTER_##name##_ID
enum { PEER = 77, LIMIT = CTT_NETADAPTER_LIMIT_BATCH_RECORDS };
static char Wire[CTT_NETADAPTER_LIMIT_FRAME_BYTES];
static uint32_t Length;
static unsigned int Calls;
static uint64_t RequestId;
static const struct ctt_netadapter_session Session = {123, 456};
static const struct ctt_netadapter_ack Ack = {4, 5};
static const struct ctt_netadapter_progress Progress = {4, 5, 6, 49};

/* The v2 service identity and operation IDs stay stable after retiring v1. */
_Static_assert(SERVICE_CTT_NETADAPTER_ID == 148, "service identity changed");

int gracht_client_get_buffer(gracht_client_t* client, gracht_buffer_t* buffer)
{
    (void)client;
    memset(Wire, 0, sizeof(Wire));
    *buffer = (gracht_buffer_t){ .data = Wire, .limit = sizeof(Wire) };
    return 0;
}
int gracht_server_get_buffer(gracht_server_t* server, gracht_buffer_t* buffer)
{ (void)server; return gracht_client_get_buffer(NULL, buffer); }

static int Publish(gracht_buffer_t* buffer)
{
    if (buffer->error) { errno = buffer->error; return -1; }
    assert(buffer->index <= sizeof(Wire));
    Length = buffer->index;
    memcpy(Wire + 4, &Length, 4);
    return 0;
}
int gracht_client_invoke(gracht_client_t* client, struct gracht_message_context* context, gracht_buffer_t* buffer)
{ (void)client; (void)context; return Publish(buffer); }
int gracht_server_send_event(gracht_server_t* server, gracht_conn_t client, gracht_buffer_t* buffer, unsigned int flags)
{ (void)server; assert(client == PEER && flags == 0); return Publish(buffer); }

static gracht_buffer_t Payload(uint8_t operation, uint8_t flags, uint32_t length)
{
    assert(Length == length && (uint8_t)Wire[8] == SERVICE_CTT_NETADAPTER_ID);
    assert((uint8_t)Wire[9] == operation && (uint8_t)Wire[10] == flags);
    return (gracht_buffer_t){ .data = Wire, .index = 11, .limit = Length };
}
static void MatchSession(const struct ctt_netadapter_session* session)
{ assert(session->id == Session.id && session->generation == Session.generation); }
static void MatchProgress(const struct ctt_netadapter_progress* progress)
{
    assert(progress->retired_batch_id == 4 && progress->retired_completion_sequence == 5);
    assert(progress->consumed_batch_id == 6 && progress->highest_completion_sequence == 49);
}

void ctt_netadapter_event_completions_invocation(gracht_client_t* client,
    const struct ctt_netadapter_session* session, const uint64_t request_id,
    const struct ctt_netadapter_completion* records, const uint32_t count,
    const struct ctt_netadapter_progress* progress)
{
    (void)client; assert(request_id == RequestId && count == LIMIT);
    MatchSession(session); MatchProgress(progress);
    for (uint32_t i = 0; i < count; ++i) {
        assert(records[i].completion_sequence == 6 + i);
        assert(records[i].id.submission_sequence == 100 + i);
        assert(records[i].direction == (i % 2 ? CTT_NETADAPTER_DIRECTION_RX : CTT_NETADAPTER_DIRECTION_TX));
    }
    ++Calls;
}
void ctt_netadapter_event_batch_admitted_invocation(gracht_client_t* client,
    const struct ctt_netadapter_session* session, const uint64_t run_id, const uint64_t batch_id,
    const oserr_t status, const struct ctt_netadapter_admission* admissions, const uint32_t count,
    const struct ctt_netadapter_progress* progress)
{
    (void)client; assert(run_id == 3 && batch_id == 6 && status == OS_EOK);
    MatchSession(session); MatchProgress(progress); assert(count == LIMIT);
    for (uint32_t i = 0; i < count; ++i) assert(admissions[i].id.submission_sequence == 100 + i);
    ++Calls;
}
void ctt_netadapter_event_drain_end_invocation(gracht_client_t* client,
    const struct ctt_netadapter_session* session, const uint64_t request_id, const oserr_t status,
    const uint64_t after_sequence, const uint64_t through_sequence, const uint32_t count,
    const uint64_t snapshot_highest, const struct ctt_netadapter_progress* progress)
{
    (void)client; assert(request_id == RequestId && status == OS_EOK);
    MatchSession(session); MatchProgress(progress);
    assert(after_sequence == 5 && through_sequence == 49 && count == LIMIT && snapshot_highest == 49);
    ++Calls;
}
void ctt_netadapter_event_ack_progress_invocation(gracht_client_t* client,
    const struct ctt_netadapter_session* session, const struct ctt_netadapter_ack* ack,
    const oserr_t status, const struct ctt_netadapter_progress* progress)
{
    (void)client; assert(status == OS_EOK);
    MatchSession(session); MatchProgress(progress);
    assert(ack->through_batch_id == 4 && ack->through_completion_sequence == 5);
    ++Calls;
}

extern void __ctt_netadapter_completions_internal(gracht_client_t*, gracht_buffer_t*);
extern void __ctt_netadapter_batch_admitted_internal(gracht_client_t*, gracht_buffer_t*);
extern void __ctt_netadapter_drain_end_internal(gracht_client_t*, gracht_buffer_t*);
extern void __ctt_netadapter_ack_progress_internal(gracht_client_t*, gracht_buffer_t*);

int main(void)
{
    struct ctt_netadapter_packet packets[LIMIT] = {0};
    struct ctt_netadapter_completion records[LIMIT + 1] = {0};
    struct ctt_netadapter_admission admissions[LIMIT] = {0};
    for (uint32_t i = 0; i < LIMIT; ++i) {
        packets[i].id.submission_sequence = 100 + i;
        packets[i].length = 1514;
        records[i].id = admissions[i].id = packets[i].id;
        records[i].completion_sequence = 6 + i;
        records[i].direction = i % 2 ? CTT_NETADAPTER_DIRECTION_RX : CTT_NETADAPTER_DIRECTION_TX;
    }
    assert(ctt_netadapter_post_rx_batch(NULL, NULL, &Session, 3, 6, &Ack, packets, LIMIT) == 0);
    gracht_buffer_t b = Payload(OP(POST_RX_BATCH), MESSAGE_FLAG_ASYNC, 63 + 32 * LIMIT);
    struct ctt_netadapter_session decoded;
    deserialize_ctt_netadapter_session(&b, &decoded); MatchSession(&decoded);
    assert(deserialize_uint64(&b) == 3 && deserialize_uint64(&b) == 6);
    struct ctt_netadapter_ack ack;
    deserialize_ctt_netadapter_ack(&b, &ack);
    assert(ack.through_batch_id == 4 && ack.through_completion_sequence == 5);
    assert(deserialize_uint32(&b) == LIMIT);
    for (uint32_t i = 0; i < LIMIT; ++i) {
        struct ctt_netadapter_packet packet;
        deserialize_ctt_netadapter_packet(&b, &packet);
        assert(packet.id.submission_sequence == 100 + i && packet.length == 1514);
    }
    assert(!b.error && b.index == b.limit);
    assert(ctt_netadapter_submit_tx_batch(NULL, NULL, &Session, 3, 6, &Ack, packets, LIMIT) == 0);
    (void)Payload(OP(SUBMIT_TX_BATCH), MESSAGE_FLAG_ASYNC, 63 + 32 * LIMIT);
    assert(ctt_netadapter_acknowledge(NULL, NULL, &Session, &Ack) == 0);
    (void)Payload(OP(ACKNOWLEDGE), MESSAGE_FLAG_ASYNC, 43);
    assert(ctt_netadapter_drain(NULL, NULL, &Session, 9, 5, LIMIT, &Ack) == 0);
    (void)Payload(OP(DRAIN), MESSAGE_FLAG_ASYNC, 63);
    assert(ctt_netadapter_prepare_run(NULL, NULL, &Session, 3) == 0);
    (void)Payload(OP(PREPARE_RUN), MESSAGE_FLAG_SYNC, 35);
    assert(ctt_netadapter_start_run(NULL, NULL, &Session, 3, 6) == 0);
    (void)Payload(OP(START_RUN), MESSAGE_FLAG_SYNC, 43);
    assert(ctt_netadapter_stop_run(NULL, NULL, &Session, 3, 6) == 0);
    (void)Payload(OP(STOP_RUN), MESSAGE_FLAG_SYNC, 43);

    assert(ctt_netadapter_event_batch_admitted_single(NULL, PEER, &Session, 3, 6, OS_EOK, admissions, LIMIT, &Progress) == 0);
    b = Payload(OP(EVENT_BATCH_ADMITTED), MESSAGE_FLAG_EVENT, 83 + 24 * LIMIT);
    __ctt_netadapter_batch_admitted_internal(NULL, &b); assert(!b.error && b.index == b.limit);
    // Both live pushes and replay chunks use the same bounded batched encoding.
    for (uint32_t request = 0; request <= 9; request += 9) {
        RequestId = request;
        assert(ctt_netadapter_event_completions_single(NULL, PEER, &Session, request, records, LIMIT, &Progress) == 0);
        b = Payload(OP(EVENT_COMPLETIONS), MESSAGE_FLAG_EVENT, 71 + 44 * LIMIT);
        __ctt_netadapter_completions_internal(NULL, &b); assert(!b.error && b.index == b.limit);
    }
    // Every payload truncation must fail before an ownership callback. No state
    // machine is involved: malformed-wire rejection belongs in generated code.
    unsigned int calls = Calls;
    for (uint32_t limit = 11; limit < Length; ++limit) {
        b = (gracht_buffer_t){ .data = Wire, .index = 11, .limit = limit };
        __ctt_netadapter_completions_internal(NULL, &b);
        assert(b.error && Calls == calls);
    }
    uint32_t malicious_count = UINT32_MAX;
    memcpy(Wire + 35, &malicious_count, 4);
    b = (gracht_buffer_t){ .data = Wire, .index = 11, .limit = Length };
    __ctt_netadapter_completions_internal(NULL, &b); assert(b.error && Calls == calls);
    assert(ctt_netadapter_event_completions_single(NULL, PEER, &Session, 9, records, LIMIT + 1, &Progress) == -1);
    assert(errno == EMSGSIZE); // 45 records would exceed the mandatory envelope
    assert(ctt_netadapter_event_drain_end_single(NULL, PEER, &Session, 9, OS_EOK, 5, 49, LIMIT, 49, &Progress) == 0);
    RequestId = 9;
    b = Payload(OP(EVENT_DRAIN_END), MESSAGE_FLAG_EVENT, 99);
    __ctt_netadapter_drain_end_internal(NULL, &b); assert(!b.error && b.index == b.limit);
    assert(ctt_netadapter_event_ack_progress_single(NULL, PEER, &Session, &Ack, OS_EOK, &Progress) == 0);
    b = Payload(OP(EVENT_ACK_PROGRESS), MESSAGE_FLAG_EVENT, 79);
    __ctt_netadapter_ack_progress_internal(NULL, &b); assert(!b.error && b.index == b.limit);
    assert(Calls == 5);
    puts("netadapter v2 generated contract tests passed");
    return 0;
}
