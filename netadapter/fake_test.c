/** Deterministic host check of the SAME fake controller served in Vali. Calls
 * typed handlers directly; this checks controller/session interoperability but
 * deliberately does not claim to test IPC. ipc_test.c supplies the guest test.
 */
#define NETADAPTER_FAKE_HOST_TEST
#include "fake.c"
#include "adapter.h"
#include "net_shm_mock.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "fake_test:%d: %s\n", __LINE__, #x); abort(); } } while (0)
#define OP(name) SERVICE_CTT_NETADAPTER_##name##_ID
static NetAdapterReply_t reply;
static NetAdapterEvent_t events[128];
static unsigned eventCount, txCount, rxCount;
static uint64_t txSeen, now;
static NetworkAdapter_t* adapter;
static struct gracht_message message = { .client = 3 };

int gracht_server_register_client(const struct gracht_message* m) { (void)m; return 0; }
int ctt_netadapter_get_info_response(struct gracht_message* m, oserr_t s, struct ctt_netadapter_info* i)
{ (void)m; reply.Status=s; reply.Info=*i; return 0; }
int ctt_netadapter_open_response(struct gracht_message* m, oserr_t s, struct ctt_netadapter_session* session, uint64_t f, struct ctt_netadapter_link* link)
{ (void)m; reply.Status=s; reply.Session=*session; reply.Value=f; reply.Link=*link; return 0; }
int ctt_netadapter_register_pool_response(struct gracht_message* m, oserr_t s, uint32_t id)
{ (void)m; reply.Status=s; reply.PoolId=id; return 0; }
#define STATUS_RESPONSE(name) int ctt_netadapter_##name##_response(struct gracht_message* m, oserr_t s) { (void)m; reply.Status=s; return 0; }
STATUS_RESPONSE(configure)
STATUS_RESPONSE(unregister_pool)
STATUS_RESPONSE(close)
STATUS_RESPONSE(prepare_run)
STATUS_RESPONSE(start_run)
int ctt_netadapter_stop_run_response(struct gracht_message* m, oserr_t s, uint64_t sequence)
{ (void)m; reply.Status=s; reply.Value=sequence; return 0; }
int ctt_netadapter_get_link_response(struct gracht_message* m, oserr_t s, struct ctt_netadapter_link* link)
{ (void)m; reply.Status=s; reply.Link=*link; return 0; }
int ctt_netadapter_get_counters_response(struct gracht_message* m, oserr_t s, struct ctt_netadapter_counters* c)
{ (void)m; reply.Status=s; reply.Counters=*c; return 0; }
static NetAdapterEvent_t* Event(gracht_server_t* server, gracht_conn_t owner,
    const struct ctt_netadapter_session* s, const struct ctt_netadapter_progress* p, uint8_t op)
{
    (void)server; CHECK(owner==3 && eventCount<128);
    NetAdapterEvent_t* e=&events[eventCount++];
    *e=(NetAdapterEvent_t){ .Session=*s, .Progress=*p, .Operation=op }; return e;
}
int ctt_netadapter_event_batch_admitted_single(gracht_server_t* server, gracht_conn_t owner,
    const struct ctt_netadapter_session* s, uint64_t run, uint64_t id, oserr_t status,
    const struct ctt_netadapter_admission* records, uint32_t count, const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e=Event(server,owner,s,p,OP(EVENT_BATCH_ADMITTED));
    e->Run=run; e->Id=id; e->Status=status; e->Count=count;
    if(count) memcpy(e->Admissions,records,count*sizeof(*records)); return 0;
}
int ctt_netadapter_event_completions_single(gracht_server_t* server, gracht_conn_t owner,
    const struct ctt_netadapter_session* s, uint64_t id, const struct ctt_netadapter_completion* records,
    uint32_t count, const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e=Event(server,owner,s,p,OP(EVENT_COMPLETIONS)); e->Id=id; e->Count=count;
    memcpy(e->Completions,records,count*sizeof(*records)); return 0;
}
int ctt_netadapter_event_drain_end_single(gracht_server_t* server, gracht_conn_t owner,
    const struct ctt_netadapter_session* s, uint64_t id, oserr_t status, uint64_t after,
    uint64_t through, uint32_t count, uint64_t highest, const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e=Event(server,owner,s,p,OP(EVENT_DRAIN_END));
    e->Id=id; e->Status=status; e->After=after; e->Through=through; e->Count=count; e->Highest=highest; return 0;
}
int ctt_netadapter_event_ack_progress_single(gracht_server_t* server, gracht_conn_t owner,
    const struct ctt_netadapter_session* s, const struct ctt_netadapter_ack* ack, oserr_t status,
    const struct ctt_netadapter_progress* p)
{
    NetAdapterEvent_t* e=Event(server,owner,s,p,OP(EVENT_ACK_PROGRESS)); e->Ack=*ack; e->Status=status; return 0;
}

static void Handle(const NetAdapterRequest_t* input)
{
    NetAdapterRequest_t r=*input; reply=(NetAdapterReply_t){0};
    switch(r.Operation) {
        case OP(GET_INFO): ctt_netadapter_get_info_invocation(&message,r.Device,r.Port); break;
        case OP(OPEN): ctt_netadapter_open_invocation(&message,r.Device,r.Port,0); break;
        case OP(REGISTER_POOL): ctt_netadapter_register_pool_invocation(&message,&r.Session,r.Value,&r.Pool); break;
        case OP(CONFIGURE): ctt_netadapter_configure_invocation(&message,&r.Session,r.Mtu,3); break;
        case OP(PREPARE_RUN): ctt_netadapter_prepare_run_invocation(&message,&r.Session,r.Run); break;
        case OP(START_RUN): ctt_netadapter_start_run_invocation(&message,&r.Session,r.Run,r.Value); break;
        case OP(STOP_RUN): ctt_netadapter_stop_run_invocation(&message,&r.Session,r.Run,r.Value); break;
        case OP(CLOSE): ctt_netadapter_close_invocation(&message,&r.Session); break;
        case OP(GET_LINK): ctt_netadapter_get_link_invocation(&message,&r.Session); break;
        case OP(GET_COUNTERS): ctt_netadapter_get_counters_invocation(&message,&r.Session); break;
        case OP(POST_RX_BATCH): ctt_netadapter_post_rx_batch_invocation(&message,&r.Session,r.Run,r.Value,&r.Ack,r.Packets,r.Count); break;
        case OP(SUBMIT_TX_BATCH): ctt_netadapter_submit_tx_batch_invocation(&message,&r.Session,r.Run,r.Value,&r.Ack,r.Packets,r.Count); break;
        case OP(ACKNOWLEDGE): ctt_netadapter_acknowledge_invocation(&message,&r.Session,&r.Ack); break;
        case OP(DRAIN): ctt_netadapter_drain_invocation(&message,&r.Session,r.Value,r.After,r.Count,&r.Ack); break;
        default: CHECK(false);
    }
    if (r.Operation == OP(SUBMIT_TX_BATCH) && r.Run == 1) {
        uint64_t packets = g_fake.Counters.tx_packets;
        unsigned queued = eventCount;
        // Identical replay keeps cached admission and never repeats payload
        // access. A changed descriptor is a non-consuming protocol error.
        ctt_netadapter_submit_tx_batch_invocation(&message,&r.Session,r.Run,r.Value,&r.Ack,r.Packets,r.Count);
        CHECK(g_fake.Counters.tx_packets == packets && eventCount == queued + 1 && events[queued].Status == OS_EOK);
        eventCount = queued;
        r.Packets[0].flags = 1;
        ctt_netadapter_submit_tx_batch_invocation(&message,&r.Session,r.Run,r.Value,&r.Ack,r.Packets,r.Count);
        CHECK(g_fake.Counters.tx_packets == packets && eventCount == queued + 1 && events[queued].Status == OS_EPROTOCOL);
        eventCount = queued;
    }
    if(NetAdapterRequestIsControl(&r)) {
        if(reply.Status) fprintf(stderr,"operation %u status %u\n",r.Operation,reply.Status);
        CHECK(HandleAdapterRequest(adapter,2,r.Serial,&reply,now)==OS_EOK);
    }
}
static void Pump(void)
{
    const NetAdapterRequest_t* r;
    for(unsigned i=0;i<16 && NetAdapterNextRequest(adapter,now,&r)==OS_EOK;++i) Handle(r);
    for(unsigned i=0;i<eventCount;++i) CHECK(NetAdapterEvent(adapter,2,&events[i],now)==OS_EOK);
    eventCount=0; now+=10;
}
static void State(enum NetAdapterState state)
{
    NetAdapterSnapshot_t s;
    for(unsigned i=0;i<3000;++i) {
        NetAdapterSnapshot(adapter,&s);
        if(s.State==state) return;
        CHECK(s.State!=NET_ADAPTER_FAILED && s.State!=NET_ADAPTER_QUARANTINED);
        Pump();
    }
    fprintf(stderr,"state %u wanted %u error %u\n",s.State,state,s.LastError); CHECK(false);
}
static void Receive(void* unused,const void* bytes,uint32_t length)
{
    (void)unused; CHECK(length==NETADAPTER_TEST_FRAME_BYTES);
    for(unsigned i=0;i<length;++i) CHECK(((const unsigned char*)bytes)[i]==(unsigned char)(rxCount+i));
    rxCount++;
}
static void Transmitted(void* unused,uint64_t cookie,oserr_t status)
{
    (void)unused; CHECK(status==OS_EOK && cookie<32 && !(txSeen&(1ull<<cookie)));
    txSeen|=1ull<<cookie; txCount++;
}
int main(void)
{
    g_fake.Session=(struct ctt_netadapter_session){1,1};
    g_fake.Link=(struct ctt_netadapter_link){1,CTT_NETADAPTER_LINK_STATUS_UP,CTT_NETADAPTER_DUPLEX_FULL,1000000000};
    NetAdapterConfig_t config; NetAdapterConfigInitializeDefault(&config);
    NetAdapterCallbacks_t callbacks={.Receive=Receive,.Transmitted=Transmitted};
    CHECK(NetAdapterCreate(NETADAPTER_FAKE_DEVICE,2,0,&config,&callbacks,&adapter)==OS_EOK);
    for(unsigned run=1;run<=2;++run) {
        State(NET_ADAPTER_RUNNING);
        for(unsigned id=(run-1)*16;id<run*16;++id) {
            unsigned char frame[NETADAPTER_TEST_FRAME_BYTES];
            for(unsigned i=0;i<sizeof(frame);++i) frame[i]=(unsigned char)(id+i);
            CHECK(NetAdapterSend(adapter,frame,sizeof(frame),id)==OS_EOK);
        }
        for(unsigned i=0;i<3000 && (txCount<run*16 || rxCount<run*16);++i) Pump();
        CHECK(txCount==run*16 && rxCount==run*16);
        NetAdapterStop(adapter); State(NET_ADAPTER_STOPPED);
        CHECK(!g_fake.Outstanding && g_fake.Progress.retired_batch_id==g_fake.Progress.consumed_batch_id &&
            g_fake.Progress.retired_completion_sequence==g_fake.Progress.highest_completion_sequence);
        CHECK(g_fake.Counters.tx_packets==run*16 && g_fake.Counters.rx_packets==run*16);
        NetAdapterRefreshCounters(adapter);
        NetAdapterSnapshot_t snapshot;
        for (unsigned i=0;i<100;++i) {
            Pump(); NetAdapterSnapshot(adapter,&snapshot);
            if (snapshot.Counters.tx_packets==run*16) break;
        }
        CHECK(snapshot.State==NET_ADAPTER_STOPPED && snapshot.Counters.tx_packets==run*16 &&
            snapshot.Counters.rx_packets==run*16);
        if(run==1) CHECK(NetAdapterStart(adapter)==OS_EOK);
    }
    CHECK(g_fake.LostAdmission && g_fake.LostDrain==2);
    // Invalid ACKs must not partially retire one cursor; strangers cannot close.
    struct ctt_netadapter_progress before=g_fake.Progress;
    CHECK(Acknowledge(&(struct ctt_netadapter_ack){UINT64_MAX,0})==OS_EINVALPARAMS);
    CHECK(!memcmp(&before,&g_fake.Progress,sizeof(before)));
    message.client=9; ctt_netadapter_close_invocation(&message,&g_fake.Session);
    CHECK(reply.Status==OS_ENOENT && !g_fake.Closed); message.client=3;
    NetAdapterClose(adapter); State(NET_ADAPTER_CLOSED);
    CHECK(g_fake.Closed && !g_fake.Pools[0].Mapped && !g_fake.Pools[1].Mapped);
    ctt_netadapter_close_invocation(&message,&g_fake.Session); CHECK(reply.Status==OS_EOK);
    CHECK(NetAdapterDestroy(&adapter)==OS_EOK && !g_netShm.Live);
    puts("netadapter fake: loopback, loss recovery, restart and detach passed");
    return 0;
}
