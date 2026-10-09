/** Exercise production IPCContextRecv's payload/sender boundary. The stream
 * shim supplies packet bytes; none of the receive/count logic is duplicated. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ds/streambuffer.h>
#include <os/ipc.h>
#include <os/shm.h>
static unsigned char packet[64];
static size_t packetLength, position;
static int ended;
void* SHMBuffer(OSHandle_t* handle) { return handle->Payload; }
size_t streambuffer_read_packet_start(streambuffer_t* stream, streambuffer_rw_options_t* options, streambuffer_packet_ctx_t* context)
{ (void)stream; (void)context; assert(options->flags & STREAMBUFFER_NO_BLOCK); position=0; ended=0; return packetLength; }
void streambuffer_read_packet_data(void* out, size_t length, streambuffer_packet_ctx_t* context)
{ (void)context; assert(position+length<=packetLength); memcpy(out,packet+position,length); position+=length; }
void streambuffer_read_packet_end(streambuffer_packet_ctx_t* context) { (void)context; ended=1; }
int main(void)
{
    OSHandle_t handle={.Payload=packet};
    uuid_t from=0, sender=42;
    size_t copied=99;
    unsigned char out[32];
    memcpy(packet,&sender,sizeof(sender)); memcpy(packet+sizeof(sender),"hello",5); packetLength=sizeof(sender)+5;
    memset(out,0xa5,sizeof(out));
    assert(IPCContextRecv(&handle,out,sizeof(out),IPC_DONTWAIT,NULL,&from,&copied)==OS_EOK);
    assert(from==sender && copied==5 && ended && !memcmp(out,"hello",5) && out[5]==0xa5);
    memset(out,0xa5,sizeof(out));
    assert(IPCContextRecv(&handle,out,2,IPC_DONTWAIT,NULL,&from,&copied)==OS_EOK);
    assert(from==sender && copied==2 && ended && !memcmp(out,"he",2) && out[2]==0xa5);
    packetLength=0;
    assert(IPCContextRecv(&handle,out,sizeof(out),IPC_DONTWAIT,NULL,&from,&copied)==OS_EOK);
    assert(from==UUID_INVALID && !copied && !ended);
    packetLength=sizeof(sender)-1;
    assert(IPCContextRecv(&handle,out,sizeof(out),IPC_DONTWAIT,NULL,&from,&copied)==OS_EPROTOCOL);
    assert(from==UUID_INVALID && !copied && ended);
    assert(IPCContextRecv(&handle,out,sizeof(out),IPC_DONTWAIT,NULL,NULL,&copied)==OS_EINVALPARAMS);
    assert(IPCContextRecv(&handle,out,sizeof(out),IPC_DONTWAIT,NULL,&from,NULL)==OS_EINVALPARAMS);
    puts("IPC receive: payload counts, truncation and malformed sender prefix passed");
    return 0;
}
