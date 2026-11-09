#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../modules/eth/genet/genet.h"
#include <ddk/utils.h>

static unsigned g_stops;
static unsigned g_frees;
static unsigned g_isolates;
static unsigned g_releases;
static int g_stopFails;
static int g_freeFails;
static int g_platformFails;
static int g_unmapFails;

void
EthernetUnregister(
    _In_ EthernetDevice_t* device)
{
    assert(device);
}

oserr_t
EthernetQueuesReset(
    _In_ EthernetDevice_t* device)
{
    g_stops++;
    return g_stopFails ? OS_ETIMEOUT : OS_EOK;
}

oserr_t
EthernetPoolsDetach(
    _In_ EthernetDevice_t* device)
{
    return OS_EOK;
}

oserr_t
PhyStop(
    _In_ PhyDevice_t* phy)
{
    g_isolates++;
    return OS_EDEVFAULT;
}

oserr_t
DMAFree(
    _In_ struct DMABuffer** buffer)
{
    if (*buffer) {
        if (g_freeFails) {
            return OS_EBUSY;
        }
        g_frees++;
        *buffer = NULL;
    }
    return OS_EOK;
}

oserr_t
DMAClose(
    _In_ struct DMAContext** context)
{
    *context = NULL;
    return OS_EOK;
}

oserr_t
GenetIntegrationRelease(
    _In_ struct GenetIntegration* state)
{
    state->Registers = NULL;
    return OS_EOK;
}

oserr_t
DeviceActivate(
    _In_ uuid_t device,
    _In_ enum DeviceActivation stage)
{
    assert(device == 42 && stage == DeviceStop);
    return g_platformFails ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* registers)
{
    g_releases++;
    return g_unmapFails ? OS_EBUSY : OS_EOK;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
}

oserr_t
GenetQueuesPrepare(
    _In_ EthernetDevice_t* device)
{
    assert(0);
    return OS_EOK;
}
oserr_t
GenetQueuesStart(
    _In_ EthernetDevice_t* device)
{
    assert(0);
    return OS_EOK;
}
oserr_t
GenetQueuesStop(
    _In_ EthernetDevice_t* device)
{
    assert(0);
    return OS_EOK;
}
oserr_t
GenetQueuePacket(
    _In_ EthernetDevice_t* device,
    _In_ EthernetSlot_t* slot)
{
    assert(0);
    return OS_EOK;
}
oserr_t
GenetReadLink(
    _In_ EthernetDevice_t* device,
    _In_ struct PhyLink* link)
{
    assert(0);
    return OS_EOK;
}
void
GenetPoll(
    _In_ EthernetDevice_t* device)
{
    assert(0);
}

int
main(void)
{
    struct Genet controller = {0};
    Device_t device = { .Id = 42 };

    controller.Device = &device;
    controller.Prepared = true;
    controller.Mapped = true;
    controller.Published = true;
    controller.PhyConfigured = true;
    controller.Integration.Registers = &controller.Registers;
    controller.Queues.Storage.Buffer = (void*)1;
    controller.Dma = (void*)2;
    controller.Adapter.Closed = malloc(sizeof(*controller.Adapter.Closed));
    g_stopFails = 1;
    assert(GenetDestroy(&controller) == OS_ETIMEOUT);
    assert(!controller.Published && !controller.Quiesced);
    assert(g_frees == 0 && g_isolates == 0 && g_releases == 0);
    g_stopFails = 0;
    g_freeFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.Quiesced && !controller.PhyConfigured);
    assert(g_isolates == 1 && g_frees == 0);
    g_freeFails = 0;
    g_platformFails = 1;
    assert(GenetDestroy(&controller) == OS_EDEVFAULT);
    assert(controller.Prepared && controller.Mapped && g_frees == 1);
    g_platformFails = 0;
    g_unmapFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(!controller.Prepared && controller.Mapped);
    g_unmapFails = 0;
    assert(GenetDestroy(&controller) == OS_EOK);
    assert(!controller.Mapped && !controller.Adapter.Closed);
    assert(g_stops == 2 && g_isolates == 1 && g_frees == 1);
    puts("GENET teardown failure tests passed");
    return 0;
}
