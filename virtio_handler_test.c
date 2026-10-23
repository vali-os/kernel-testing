// The runner compiles each production fast handler with its own driver header.
#define OnFastInterrupt NetFastInterrupt
#include "../modules/virtio/net/virtio-net.h"
#undef OnFastInterrupt
#define OnFastInterrupt BlkFastInterrupt
#include "../modules/virtio/blk/virtio-blk.h"
#undef OnFastInterrupt
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned int g_reads;
static unsigned int g_signals;
static size_t g_status;

static size_t
__ReadIo(
    _In_ DeviceIo_t* io,
    _In_ size_t offset,
    _In_ size_t width)
{
    CHECK(offset == 17 && width == 1);
    g_reads++;
    return g_status;
}

static oserr_t
__Signal(
    _In_ uuid_t handle)
{
    CHECK(handle == 42);
    g_signals++;
    return OS_EOK;
}

int
main(void)
{
    VirtioNetInterruptResource_t net = { 0 };
    VirtioBlkInterruptResource_t blk = { 0 };
    InterruptResourceTable_t resources = { 0 };
    InterruptFunctionTable_t functions = { 0 };
    unsigned int mode;

    functions.ReadIoSpace = __ReadIo;
    functions.EventSignal = __Signal;
    resources.HandleResource = 42;
    net.IsrOffset = blk.IsrOffset = 17;
    atomic_init(&net.PendingStatus, 0);
    atomic_init(&blk.PendingStatus, 0);
    for (mode = 0; mode < 2; mode++) {
        net.MsixEnabled = blk.MsixEnabled = mode;
        g_reads = g_signals = 0;
        g_status = mode ? 0 : VIRTIO_ISR_QUEUE_INTERRUPT;
        resources.MemoryResources[0].Address = (uintptr_t)&net;
        CHECK(NetFastInterrupt(&functions, &resources) == IRQSTATUS_HANDLED);
        resources.MemoryResources[0].Address = (uintptr_t)&blk;
        CHECK(BlkFastInterrupt(&functions, &resources) == IRQSTATUS_HANDLED);
        CHECK(g_signals == 2 && g_reads == (mode ? 0 : 2));
        CHECK(atomic_exchange(&net.PendingStatus, 0) == (mode ? 3 : 1));
        CHECK(atomic_exchange(&blk.PendingStatus, 0) == (mode ? 3 : 1));
    }
    net.MsixEnabled = blk.MsixEnabled = 0;
    g_status = 0;
    resources.MemoryResources[0].Address = (uintptr_t)&net;
    CHECK(NetFastInterrupt(&functions, &resources) == IRQSTATUS_NOT_HANDLED);
    resources.MemoryResources[0].Address = (uintptr_t)&blk;
    CHECK(BlkFastInterrupt(&functions, &resources) == IRQSTATUS_NOT_HANDLED);
    CHECK(g_signals == 2);
    puts("Virtio handlers: MSI-X wakeup without ISR, legacy acknowledgment and shared-line rejection passed");
    return 0;
}
