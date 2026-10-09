/** Deterministic v2 controller model: batches and events are independently queued.
 * Tests can delay/reorder/drop delivery without changing driver execution. Real
 * session/pool code owns all leases; the fake maps actual mock SHM for RX/TX.
 */
#include "adapter.h"
#include "net_shm_mock.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); abort(); } } while (0)
#define OK(x) CHECK((x) == OS_EOK)
#define OP(n) SERVICE_CTT_NETADAPTER_##n##_ID
#define TX CTT_NETADAPTER_DIRECTION_TX
#define RX CTT_NETADAPTER_DIRECTION_RX
#define MAX 128
static const struct ctt_netadapter_session Session = {10,42};
struct Cached { NetAdapterRequest_t Request; NetAdapterEvent_t Event; };
struct Fake {
    NetworkAdapter_t* Adapter;
    uint64_t Now, Run, Consumed, Retired, Highest, AckCompletion;
    struct ctt_netadapter_pool Pools[2];
    struct ctt_netadapter_packet Rx[16];
    uint32_t RxCount, Published, TxCallbacks, RxCallbacks, Events, Drains;
    uint64_t Cookies[64]; oserr_t Status[64];
    bool Running, StopGate, RejectOne, Busy, FailClose, DropAck, DropPush;
    struct Cached Cache[MAX];
    struct ctt_netadapter_completion Journal[MAX];
    NetAdapterEvent_t Queue[MAX];
};
static void Tx(void* context, uint64_t cookie, oserr_t status)
{
    struct Fake* f = context; CHECK(f->TxCallbacks < 64);
    f->Cookies[f->TxCallbacks] = cookie; f->Status[f->TxCallbacks++] = status;
}
static void Rx(void* context, const void* data, uint32_t length)
{
    struct Fake* f = context; CHECK(length == 64);
    for (uint32_t i = 0; i < length; ++i) CHECK(((const unsigned char*)data)[i] == 0x5a);
    f->RxCallbacks++;
}
static struct ctt_netadapter_info Info(void)
{
    return (struct ctt_netadapter_info){ .port_count=1, .min_version=2, .max_version=2,
        .framing=CTT_NETADAPTER_FRAMING_ETHERNET, .medium=CTT_NETADAPTER_MEDIUM_VIRTUAL,
        .min_mtu=576, .max_mtu=1500, .current_mtu=1500, .max_frame_size=1514,
        .buffer_alignment=64, .required_headroom=17, .required_tailroom=7,
        .max_pools=2, .max_pool_bytes=131072, .max_registered_bytes=262144,
        .max_slots_per_pool=16, .max_queue_pairs=1, .max_segments=1,
        .max_batch_size=2, .max_pending_batches=4, .max_outstanding_tx=8,
        .max_outstanding_rx=8, .max_unacked_completions=16, .min_rx_slots=2 };
}
static void Create(struct Fake* f)
{
    memset(f,0,sizeof(*f)); NetAdapterConfig_t c; NetAdaterConfigInitializeDefault(&c);
    c.TxSlots=c.RxSlots=8; c.RetryMilliseconds=20; c.PollMilliseconds=30;
    NetAdapterCallbacks_t cb={.Receive=Rx,.Transmitted=Tx,.Context=f};
    OK(NetAdapterCreate(1,2,0,&c,&cb,&f->Adapter));
}
static struct ctt_netadapter_progress Progress(struct Fake* f)
{ return (struct ctt_netadapter_progress){f->Retired,f->AckCompletion,f->Consumed,f->Highest}; }
static void Enqueue(struct Fake* f, NetAdapterEvent_t e)
{ CHECK(f->Events<MAX); e.Session=Session; e.Progress=Progress(f); f->Queue[f->Events++]=e; }
static NetAdapterEvent_t Pop(struct Fake* f, uint32_t index)
{
    CHECK(index<f->Events); NetAdapterEvent_t e=f->Queue[index];
    memmove(f->Queue+index,f->Queue+index+1,(--f->Events-index)*sizeof(e)); return e;
}
static void Deliver(struct Fake* f, uint32_t index)
{
    NetAdapterEvent_t e=Pop(f,index);
    oserr_t status=NetAdapterEvent(f->Adapter,2,&e,f->Now);
    CHECK(status==OS_EOK || status==OS_ENOENT);
}
static void Ack(struct Fake* f, const struct ctt_netadapter_ack* ack)
{
    CHECK(ack->through_batch_id<=f->Consumed && ack->through_completion_sequence<=f->Highest);
    if (ack->through_batch_id>f->Retired) f->Retired=ack->through_batch_id;
    if (ack->through_completion_sequence>f->AckCompletion) f->AckCompletion=ack->through_completion_sequence;
}
static void Record(struct Fake* f, const struct ctt_netadapter_packet* packet, bool rx, bool cancel)
{
    CHECK(f->Highest+1<MAX);
    struct ctt_netadapter_pool* pool=&f->Pools[rx?1:0];
    unsigned char* data=(unsigned char*)NetTestShmData(pool->buffer_handle)+pool->region_offset+
        (uint64_t)packet->id.slot_id*pool->slot_size+packet->data_offset;
    if (!cancel) {
        if (rx) memset(data,0x5a,64);
        else for (uint32_t i=0;i<packet->length;++i) CHECK(data[i]==0xa5);
    }
    struct ctt_netadapter_completion c={.completion_sequence=++f->Highest,
        .direction=rx?RX:TX,.id=packet->id,
        .status=cancel?CTT_NETADAPTER_COMPLETION_STATUS_CANCELLED:CTT_NETADAPTER_COMPLETION_STATUS_SUCCESS,
        .detail=cancel?OS_ECANCELLED:OS_EOK,.length=cancel?0:(rx?64:packet->length)};
    f->Journal[f->Highest]=c;
    if (!f->DropPush) { NetAdapterEvent_t e={.Operation=OP(EVENT_COMPLETIONS),.Count=1}; e.Completions[0]=c; Enqueue(f,e); }
}
static void Handle(struct Fake* f, const NetAdapterRequest_t* request)
{
    NetAdapterRequest_t r=*request; // callbacks may change the core's next envelope
    if (NetAdapterRequestIsControl(&r)) {
        NetAdapterReply_t reply={0};
        switch(r.Operation) {
            case OP(GET_INFO): reply.Info=Info(); break;
            case OP(OPEN): reply.Session=Session; reply.Link=(struct ctt_netadapter_link){1,CTT_NETADAPTER_LINK_STATUS_UP,CTT_NETADAPTER_DUPLEX_FULL,1000000000}; break;
            case OP(REGISTER_POOL): f->Pools[r.Pool.direction==TX?0:1]=r.Pool; reply.PoolId=r.Pool.direction==TX?100:101; break;
            case OP(CONFIGURE): break;
            case OP(PREPARE_RUN): CHECK(r.Run==f->Run+1); f->Run=r.Run; f->StopGate=false; break;
            case OP(START_RUN): CHECK(r.Run==f->Run && r.Value==f->Consumed && f->RxCount>=2); f->Running=true; break;
            case OP(STOP_RUN):
                CHECK(r.Run==f->Run && r.Value==f->Consumed);
                f->StopGate=true; f->Running=false;
                while(f->RxCount) Record(f,&f->Rx[--f->RxCount],true,true);
                reply.Value=f->Highest; break;
            case OP(CLOSE):
                if(f->FailClose) reply.Status=OS_EDEVFAULT;
                else { f->StopGate=true; f->RxCount=0; }
                break;
            case OP(GET_LINK): reply.Link=(struct ctt_netadapter_link){1,CTT_NETADAPTER_LINK_STATUS_UP,CTT_NETADAPTER_DUPLEX_FULL,1000000000}; break;
            case OP(GET_COUNTERS): break;
            default: CHECK(false);
        }
        oserr_t status=HandleAdapterRequest(f->Adapter,2,r.Serial,&reply,f->Now);
        if (status!=OS_EOK && !(f->FailClose && status==OS_EDEVFAULT) && !(g_netShm.FailCall && status==OS_EOOM)) fprintf(stderr,"control op=%u status=%u now=%llu\n",r.Operation,status,(unsigned long long)f->Now);
        CHECK(status==OS_EOK || (f->FailClose && status==OS_EDEVFAULT) || (g_netShm.FailCall && status==OS_EOOM));
        return;
    }
    Ack(f,&r.Ack);
    if(r.Operation==OP(ACKNOWLEDGE)) {
        if(!f->DropAck) Enqueue(f,(NetAdapterEvent_t){.Operation=OP(EVENT_ACK_PROGRESS),.Ack=r.Ack});
    } else if(r.Operation==OP(DRAIN)) {
        f->Drains++; CHECK(r.After>=f->AckCompletion && r.After<=f->Highest);
        uint32_t count=(uint32_t)(f->Highest-r.After); if(count>r.Count) count=r.Count;
        if(count) {
            NetAdapterEvent_t e={.Operation=OP(EVENT_COMPLETIONS),.Id=r.Value,.Count=count};
            for(uint32_t i=0;i<count;++i) e.Completions[i]=f->Journal[r.After+i+1];
            Enqueue(f,e);
        }
        Enqueue(f,(NetAdapterEvent_t){.Operation=OP(EVENT_DRAIN_END),.Id=r.Value,.After=r.After,
            .Through=r.After+count,.Count=count,.Highest=f->Highest});
    } else {
        CHECK(r.Operation==OP(POST_RX_BATCH) || r.Operation==OP(SUBMIT_TX_BATCH));
        CHECK(r.Value<MAX);
        NetAdapterEvent_t e={.Operation=OP(EVENT_BATCH_ADMITTED),.Id=r.Value,.Run=r.Run};
        if(r.Value<=f->Retired || r.Run!=f->Run) e.Status=OS_ENOENT;
        else if(r.Value<=f->Consumed) {
            struct Cached* cache=&f->Cache[r.Value];
            CHECK(cache->Request.Operation==r.Operation && cache->Request.Count==r.Count);
            CHECK(!memcmp(cache->Request.Packets,r.Packets,r.Count*sizeof(r.Packets[0])));
            e=cache->Event;
        } else if(f->Busy || r.Value!=f->Consumed+1 || f->Consumed-f->Retired>=4) e.Status=OS_EBUSY;
        else {
            CHECK(!f->StopGate); if(r.Operation==OP(SUBMIT_TX_BATCH)) CHECK(f->Running);
            f->Consumed++; e.Count=r.Count;
            for(uint32_t i=0;i<r.Count;++i) {
                e.Admissions[i].id=r.Packets[i].id;
                if(f->RejectOne) { e.Admissions[i].status=OS_EBUFFER; f->RejectOne=false; continue; }
                f->Published++;
                if(r.Operation==OP(POST_RX_BATCH)) { CHECK(f->RxCount<16); f->Rx[f->RxCount++]=r.Packets[i]; }
                else Record(f,&r.Packets[i],false,false); // completion deliberately precedes admission
            }
            f->Cache[r.Value]=(struct Cached){r,e};
        }
        Enqueue(f,e);
    }
}
static bool Next(struct Fake* f, NetAdapterRequest_t* out)
{
    const NetAdapterRequest_t* r;
    oserr_t status=NetAdapterNextRequest(f->Adapter,f->Now,&r);
    CHECK(status==OS_EOK || status==OS_ENOENT || status==OS_ETIMEOUT);
    if(status==OS_EOK) { *out=*r; return true; } return false;
}
static void Step(struct Fake* f)
{
    if(f->Events) Deliver(f,0);
    NetAdapterRequest_t r; if(Next(f,&r)) Handle(f,&r);
    f->Now++;
}
static NetAdapterSnapshot_t Snapshot(struct Fake* f)
{ NetAdapterSnapshot_t s; NetAdapterSnapshot(f->Adapter,&s); return s; }
static void Until(struct Fake* f, enum NetAdapterState state)
{
    for(int i=0;i<1000 && Snapshot(f).State!=state;++i) Step(f);
    CHECK(Snapshot(f).State==state);
}
static void Settle(struct Fake* f)
{
    for(int i=0;i<500;++i) {
        NetAdapterSnapshot_t s=Snapshot(f);
        if(!f->Events && !s.PendingBatches && !s.Buffers.UnackedCompletions && f->RxCount==8) return;
        Step(f);
    }
    CHECK(false);
}
static void Send(struct Fake* f,uint64_t cookie)
{ unsigned char data[64]; memset(data,0xa5,sizeof(data)); OK(NetAdapterSend(f->Adapter,data,sizeof(data),cookie)); }
static void Cleanup(struct Fake* f)
{ f->Busy=f->FailClose=f->DropAck=false; NetAdapterClose(f->Adapter); Until(f,NET_ADAPTER_CLOSED); OK(NetAdapterDestroy(&f->Adapter)); CHECK(!g_netShm.Live); }

static void TestPipeline(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    for(int i=0;i<8;++i) Send(f,100+i);
    NetAdapterRequest_t requests[4]; uint32_t count=0;
    // No event delivery between these sends: all four batches must enter flight.
    for(int i=0;i<20 && count<4;++i) {
        NetAdapterRequest_t r; CHECK(Next(f,&r));
        Handle(f,&r);
        if(r.Operation==OP(SUBMIT_TX_BATCH)) requests[count++]=r;
    }
    CHECK(count==4 && Snapshot(f).PendingBatches==4 && f->TxCallbacks==0);
    // Deliver admission events in reverse order; the cumulative batch ACK cannot
    // skip an earlier missing result. Completions remain queued independently.
    uint64_t before=Snapshot(f).AdmittedBatch;
    for(int n=3;n>=0;--n) {
        uint32_t i=0; while(i<f->Events && !(f->Queue[i].Operation==OP(EVENT_BATCH_ADMITTED) && f->Queue[i].Id==requests[n].Value)) ++i;
        Deliver(f,i); CHECK(Snapshot(f).AdmittedBatch==(n?before:requests[3].Value));
    }
    Settle(f); CHECK(f->TxCallbacks==8);
    for(int i=0;i<8;++i) CHECK(f->Cookies[i]==(uint64_t)(100+i) && f->Status[i]==OS_EOK);
    CHECK(f->Published==16); Cleanup(f); free(f);
}
static void TestReplayAndRecovery(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    f->DropPush=true; Send(f,7); NetAdapterRequest_t first={0};
    for(int i=0;i<50 && !first.Serial;++i) { NetAdapterRequest_t r; if(Next(f,&r)) { Handle(f,&r); if(r.Operation==OP(SUBMIT_TX_BATCH)) first=r; } f->Now++; }
    CHECK(first.Serial); f->Events=0; // drop admission after actual execution
    uint32_t published=f->Published;
    for(int i=0;i<300 && !f->TxCallbacks;++i) Step(f);
    CHECK(f->TxCallbacks==1 && f->Published==published && f->Drains);
    // Final RX push disappears too: no later completion exists to reveal a gap.
    Record(f,&f->Rx[--f->RxCount],true,false);
    for(int i=0;i<300 && !f->RxCallbacks;++i) Step(f);
    CHECK(f->RxCallbacks==1);
    Settle(f); NetAdapterStop(f->Adapter); Until(f,NET_ADAPTER_STOPPED);
    uint64_t run=Snapshot(f).Run; OK(NetAdapterStart(f->Adapter)); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    CHECK(Snapshot(f).Run==run+1);
    NetAdapterEvent_t stale={.Operation=OP(EVENT_BATCH_ADMITTED),.Session=Session,.Run=run,.Id=first.Value,.Progress=Progress(f)};
    CHECK(NetAdapterEvent(f->Adapter,2,&stale,f->Now)==OS_ENOENT);
    Cleanup(f); free(f);
}
static void TestAckLossAndPartialAdmission(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    f->RejectOne=true; Send(f,1); Send(f,2);
    for(int i=0;i<200 && f->TxCallbacks<2;++i) Step(f);
    CHECK(f->TxCallbacks==2 && f->Status[0]==OS_EBUFFER && f->Status[1]==OS_EOK);
    f->DropAck=true;
    // Suppress every event confirmation briefly, then recover with no new TX.
    for(int i=0;i<10;++i) Step(f);
    f->DropAck=false; Settle(f);
    CHECK(Snapshot(f).RetiredBatch==Snapshot(f).AdmittedBatch);
    Cleanup(f); free(f);
}
static void TestMalformedAndForeign(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    Send(f,1); Send(f,2);
    NetAdapterRequest_t r; do { CHECK(Next(f,&r)); Handle(f,&r); } while(r.Operation!=OP(SUBMIT_TX_BATCH));
    // Recombine two valid terminal records and corrupt the second. Preflight
    // must prevent delivering the valid first member of this malformed event.
    NetAdapterEvent_t e={.Operation=OP(EVENT_COMPLETIONS),.Session=Session,.Count=2,.Progress=Progress(f)};
    e.Completions[0]=f->Journal[f->Highest-1]; e.Completions[1]=f->Journal[f->Highest];
    CHECK(NetAdapterEvent(f->Adapter,999,&e,f->Now)==OS_ENOENT);
    e.Session.generation++; CHECK(NetAdapterEvent(f->Adapter,2,&e,f->Now)==OS_ENOENT); e.Session=Session;
    e.Completions[1].id.slot_id=999;
    CHECK(NetAdapterEvent(f->Adapter,2,&e,f->Now)==OS_EPROTOCOL);
    CHECK(!f->TxCallbacks && g_netShm.Live==2);
    Cleanup(f); CHECK(f->TxCallbacks==2); free(f);
}
static void TestQuarantine(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f);
    NetAdapterRequest_t r; CHECK(Next(f,&r)); Handle(f,&r); CHECK(Next(f,&r)); CHECK(r.Operation==OP(OPEN));
    uint64_t serial=r.Serial;
    for(int i=0;i<100;++i) { f->Now++; if(Next(f,&r)) CHECK(r.Operation==OP(OPEN) && r.Serial==serial); }
    CHECK(Snapshot(f).State==NET_ADAPTER_QUARANTINED); CHECK(NetAdapterDestroy(&f->Adapter)==OS_EBUSY);
    OK(NetAdapterRetry(f->Adapter)); CHECK(Next(f,&r)); Handle(f,&r); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    f->FailClose=true; NetAdapterClose(f->Adapter); Until(f,NET_ADAPTER_QUARANTINED);
    CHECK(g_netShm.Live==2 && NetAdapterDestroy(&f->Adapter)==OS_EBUSY);
    f->FailClose=false; OK(NetAdapterRetry(f->Adapter)); Until(f,NET_ADAPTER_CLOSED);
    OK(NetAdapterDestroy(&f->Adapter)); CHECK(!g_netShm.Live); free(f);
}
static void TestBusyAndSetupFailure(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    f->Busy=true; Send(f,3); Until(f,NET_ADAPTER_CLOSED);
    CHECK(f->TxCallbacks==1 && f->Status[0]==OS_ECANCELLED); OK(NetAdapterDestroy(&f->Adapter));
    Create(f); g_netShm.FailCall=g_netShm.Calls+2; Until(f,NET_ADAPTER_CLOSED);
    g_netShm.FailCall=0; OK(NetAdapterDestroy(&f->Adapter)); CHECK(!g_netShm.Live); free(f);
}
static void TestIdleAckReplay(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    f->DropAck=true; Send(f,55);
    for(int i=0;i<100 && !f->TxCallbacks;++i) Step(f);
    CHECK(f->TxCallbacks==1);
    struct ctt_netadapter_ack first={0}; int acknowledgements=0;
    // Suppress alternate progress sources: only replaying the standalone ACK
    // can unblock retirement. No additional TX or incoming RX is generated.
    f->Events=0;
    for(int i=0;i<100 && acknowledgements<2;++i) {
        NetAdapterRequest_t r;
        if(Next(f,&r)) {
            if(r.Operation==OP(ACKNOWLEDGE)) {
                if(!acknowledgements) first=r.Ack;
                else {
                    CHECK(first.through_batch_id==r.Ack.through_batch_id &&
                        first.through_completion_sequence==r.Ack.through_completion_sequence);
                    f->DropAck=false;
                }
                acknowledgements++; Handle(f,&r);
            } else if(r.Operation!=OP(DRAIN)) Handle(f,&r);
        }
        f->Now++;
    }
    CHECK(acknowledgements==2 && Snapshot(f).RetiredBatch<Snapshot(f).AdmittedBatch);
    CHECK(f->Events==1); Deliver(f,0);
    CHECK(Snapshot(f).RetiredBatch==Snapshot(f).AdmittedBatch);
    Settle(f); CHECK(f->TxCallbacks==1); Cleanup(f); free(f);
}

static void TestDrainLoss(void)
{
    for (int loseEnd=0; loseEnd<2; ++loseEnd) {
        struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
        f->DropPush=true; Record(f,&f->Rx[--f->RxCount],true,false);
        NetAdapterRequest_t r={0};
        for(int i=0;i<100 && r.Operation!=OP(DRAIN);++i) {
            if(Next(f,&r)) { if(r.Operation!=OP(DRAIN)) Handle(f,&r); }
            f->Now++;
        }
        CHECK(r.Operation==OP(DRAIN)); Handle(f,&r); uint64_t first=r.Value;
        CHECK(f->Events==2);
        if(loseEnd) { Deliver(f,0); CHECK(f->RxCallbacks==1); (void)Pop(f,0); }
        else { Deliver(f,1); CHECK(!f->RxCallbacks); (void)Pop(f,0); }
        r.Operation=0;
        for(int i=0;i<100 && r.Operation!=OP(DRAIN);++i) {
            if(f->Events) Deliver(f,0);
            if(Next(f,&r)) { if(r.Operation!=OP(DRAIN)) Handle(f,&r); }
            f->Now++;
        }
        CHECK(r.Operation==OP(DRAIN) && r.Value>first); Handle(f,&r);
        Settle(f); CHECK(f->RxCallbacks==1);
        Cleanup(f); free(f);
    }
}

static void TestStopWindowAndLink(void)
{
    struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    struct ctt_netadapter_link link={2,CTT_NETADAPTER_LINK_STATUS_DOWN,CTT_NETADAPTER_DUPLEX_FULL,0};
    OK(NetAdapterLinkChanged(f->Adapter,2,&Session,&link));
    unsigned char bytes[64]={0}; CHECK(NetAdapterSend(f->Adapter,bytes,64,0)==OS_ENOTCONNECTED);
    link.sequence=1; link.status=CTT_NETADAPTER_LINK_STATUS_UP;
    OK(NetAdapterLinkChanged(f->Adapter,2,&Session,&link));
    CHECK(NetAdapterSend(f->Adapter,bytes,64,0)==OS_ENOTCONNECTED);
    link.sequence=3; OK(NetAdapterLinkChanged(f->Adapter,2,&Session,&link));
    for(int i=0;i<4;++i) Send(f,10+i);
    uint32_t batches=0;
    while(batches<2) { NetAdapterRequest_t r; CHECK(Next(f,&r)); Handle(f,&r); batches+=r.Operation==OP(SUBMIT_TX_BATCH); }
    Send(f,99); // locally queued work must be cancelled rather than published
    NetAdapterStop(f->Adapter);
    NetAdapterRequest_t r;
    if(Next(f,&r)) { CHECK(r.Operation!=OP(STOP_RUN)); Handle(f,&r); }
    CHECK(f->TxCallbacks==1 && f->Cookies[0]==99 && f->Status[0]==OS_ECANCELLED);
    Until(f,NET_ADAPTER_STOPPED); CHECK(f->TxCallbacks==5 && f->StopGate && !f->RxCount);
    OK(NetAdapterStart(f->Adapter)); Until(f,NET_ADAPTER_RUNNING); Settle(f);
    CHECK(Snapshot(f).Run==2); Cleanup(f); free(f);
}

static void TestMalformedAdmissionAndProgress(void)
{
    for(int progress=0;progress<2;++progress) {
        struct Fake* f=calloc(1,sizeof(*f)); CHECK(f); Create(f); Until(f,NET_ADAPTER_RUNNING); Settle(f);
        Send(f,1); Send(f,2);
        NetAdapterRequest_t r; do { CHECK(Next(f,&r)); Handle(f,&r); } while(r.Operation!=OP(SUBMIT_TX_BATCH));
        NetAdapterEvent_t e=f->Cache[r.Value].Event; e.Session=Session; e.Progress=Progress(f);
        if(progress) e.Progress.retired_batch_id=f->Consumed; // netd has not ACKed this result
        else e.Admissions[1].id.slot_id=999;
        CHECK(NetAdapterEvent(f->Adapter,2,&e,f->Now)==OS_EPROTOCOL);
        CHECK(!f->TxCallbacks && g_netShm.Live==2);
        Cleanup(f); free(f);
    }
}

int main(void)
{
    TestPipeline(); TestReplayAndRecovery(); TestAckLossAndPartialAdmission();
    TestMalformedAndForeign(); TestQuarantine(); TestBusyAndSetupFailure();
    TestIdleAckReplay(); TestDrainLoss(); TestStopWindowAndLink(); TestMalformedAdmissionAndProgress();
    puts("net_adapter_test: v2 sessions passed"); return 0;
}
