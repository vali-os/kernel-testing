#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../modules/eth/genet/genet.h"
#include <ddk/utils.h>

static unsigned g_stops;
static unsigned g_cleanups;
static int g_cleanupFails;
static unsigned g_frees;
static unsigned g_isolates;
static unsigned g_releases;
static int g_stopFails;
static int g_freeFails;
static int g_platformFails;
static int g_unmapFails;

// Order in which GenetCreate and GenetDestroy reach the platform and the interrupt.
enum Step {
    StepPrepare,
    StepInterruptRegister,
    StepInterruptUnregister,
    StepStart,
    StepPublish,
    StepCount
};

static unsigned g_stepOrder[16];
static unsigned g_stepCount;
static int g_interruptRegisterFails;
static int g_interruptUnregisterFails;
static unsigned g_interruptUnregisters;
static unsigned g_stopsAtUnregister;
static unsigned g_cleanupsAtUnregister;

static void
__Record(
    _In_ enum Step step)
{
    assert(g_stepCount < sizeof(g_stepOrder) / sizeof(g_stepOrder[0]));
    g_stepOrder[g_stepCount++] = step;
}

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
EthernetCleanup(
    _In_ EthernetDevice_t* device)
{
    assert(g_stops && !g_stopFails);
    g_cleanups++;
    return g_cleanupFails ? OS_EBUSY : OS_EOK;
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
GenetWrapperRestorePhy(
    _In_ struct GenetWrapperSettings* state)
{
    state->Registers = NULL;
    return OS_EOK;
}

oserr_t
DeviceActivate(
    _In_ uuid_t device,
    _In_ enum DeviceActivation stage)
{
    assert(device == 42);
    if (stage == DevicePrepare) {
        __Record(StepPrepare);
    } else if (stage == DeviceStart) {
        __Record(StepStart);
    } else {
        assert(stage == DeviceStop);
    }
    return g_platformFails && stage == DeviceStop ? OS_EDEVFAULT : OS_EOK;
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
    // GenetCreate stops the DMA engines once before the MAC reset.
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

const struct MdioOperations g_genetMdioOperations = {0};

oserr_t
OSStall(
    _In_ UInteger64_t* duration)
{
    return OS_EOK;
}

size_t
ReadDeviceIo(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    return 0;
}

oserr_t
GenetInterruptRegister(
    _InOut_ struct Genet* controller)
{
    __Record(StepInterruptRegister);
    return g_interruptRegisterFails ? OS_ENOTSUPPORTED : OS_EOK;
}

oserr_t
GenetInterruptUnregister(
    _InOut_ struct Genet* controller)
{
    __Record(StepInterruptUnregister);
    g_interruptUnregisters++;
    g_stopsAtUnregister = g_stops;
    g_cleanupsAtUnregister = g_cleanups;
    return g_interruptUnregisterFails ? OS_EBUSY : OS_EOK;
}

int
GenetIsDevice(
    _In_ const Device_t* device)
{
    return 1;
}

oserr_t
DeviceGetEthernetConfiguration(
    _In_  uuid_t                              device,
    _Out_ struct DeviceEthernetConfiguration* configuration)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->MacPresent = 1;
    configuration->Mac[0] = 2;
    configuration->Mac[5] = 1;
    return OS_EOK;
}

oserr_t
DeviceGetResources(
    _In_  uuid_t                 device,
    _Out_ struct DeviceResources* resources)
{
    memset(resources, 0, sizeof(*resources));
    resources->Count = 1;
    return OS_EOK;
}

oserr_t
AcquireDeviceIo(
    _In_ DeviceIo_t* registers)
{
    return OS_EOK;
}

oserr_t
GenetWrapperConfigurePhy(
    _InOut_ struct GenetWrapperSettings*       state,
    _In_    DeviceIo_t*                        registers,
    _In_    const struct DeviceEthernetConfiguration* configuration)
{
    state->Registers = registers;
    return OS_EOK;
}

oserr_t
GenetHardwareReleaseReset(
    _In_ struct Genet* controller)
{
    return OS_EOK;
}

oserr_t
GenetHardwareInitialize(
    _In_ struct Genet* controller)
{
    return OS_EOK;
}

oserr_t
DeviceResetEthernetPhy(
    _In_ uuid_t device)
{
    return OS_EOK;
}

oserr_t
MdioInitialize(
    _Out_ MdioBus_t*                   bus,
    _In_  const struct MdioOperations* operations,
    _In_  void*                        context,
    _In_  uint32_t                     address)
{
    return OS_EOK;
}

oserr_t
PhyInitialize(
    _Out_ PhyDevice_t*                          phy,
    _In_  MdioBus_t*                            bus,
    _In_  const struct DeviceEthernetConfiguration* configuration)
{
    return OS_EOK;
}

oserr_t
DMAOpen(
    _In_  uuid_t              device,
    _In_  uint64_t            addressLimit,
    _Out_ struct DMAContext** context)
{
    *context = (void*)2;
    return OS_EOK;
}

oserr_t
EthernetRegister(
    _InOut_ EthernetDevice_t*                        device,
    _In_    uuid_t                                   deviceId,
    _In_    const uint8_t*                           mac,
    _In_    const struct EthernetRegistrationConfig* config)
{
    __Record(StepPublish);
    return OS_EOK;
}

static void
__ExpectSteps(
    _In_ const unsigned* expected,
    _In_ unsigned        count)
{
    assert(g_stepCount == count);
    for (unsigned i = 0; i < count; i++) {
        assert(g_stepOrder[i] == expected[i]);
    }
}

static void
__Create(void)
{
    struct Genet controller;
    Device_t device = { .Id = 42 };
    const unsigned ready[] = { StepPrepare, StepInterruptRegister, StepStart, StepPublish };
    const unsigned fallback[] = {
        StepPrepare, StepInterruptRegister, StepInterruptUnregister, StepStart, StepPublish
    };
    const unsigned stuck[] = { StepPrepare, StepInterruptRegister, StepInterruptUnregister };

    // The platform expects interrupt handlers before the device starts.
    memset(&controller, 0, sizeof(controller));
    g_stepCount = 0;
    assert(GenetCreate(&controller, &device) == OS_EOK);
    assert(controller.Published);
    __ExpectSteps(ready, 4);

    // A missing interrupt is not fatal. The partial setup is released and the
    // driver keeps polling.
    memset(&controller, 0, sizeof(controller));
    g_stepCount = 0;
    g_interruptRegisterFails = 1;
    assert(GenetCreate(&controller, &device) == OS_EOK);
    assert(controller.Published);
    __ExpectSteps(fallback, 5);

    // If the partial setup cannot be released the handler may still run, so
    // the device must not start.
    memset(&controller, 0, sizeof(controller));
    g_stepCount = 0;
    g_interruptUnregisterFails = 1;
    assert(GenetCreate(&controller, &device) == OS_EBUSY);
    assert(!controller.Published);
    __ExpectSteps(stuck, 3);
    g_interruptRegisterFails = 0;
    g_interruptUnregisterFails = 0;
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
    controller.PhyNeedsStop = true;
    controller.Integration.Registers = &controller.Registers;
    controller.Queues.Storage.Buffer = (void*)1;
    controller.Dma = (void*)2;
    g_stopFails = 1;
    assert(GenetDestroy(&controller) == OS_ETIMEOUT);
    assert(!controller.Published && !controller.TeardownQueuesStopped);
    assert(g_frees == 0 && g_isolates == 0 && g_releases == 0);
    assert(g_cleanups == 0 && g_interruptUnregisters == 0);
    g_stopFails = 0;
    g_cleanupFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.TeardownQueuesStopped && g_cleanups == 1);
    assert(g_frees == 0 && g_isolates == 0 && g_releases == 0);

    // The interrupt goes after the queues stop and before common cleanup.
    assert(g_interruptUnregisters == 1);
    assert(g_stopsAtUnregister == 2 && g_cleanupsAtUnregister == 0);

    // A release that fails keeps everything behind it alive for the next attempt.
    g_interruptUnregisterFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(g_interruptUnregisters == 2 && g_cleanups == 1);
    assert(g_frees == 0 && g_isolates == 0 && g_releases == 0);
    g_interruptUnregisterFails = 0;
    g_cleanupFails = 0;
    g_freeFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.TeardownQueuesStopped && !controller.PhyNeedsStop);
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
    assert(!controller.Mapped);
    assert(g_stops == 2 && g_isolates == 1 && g_frees == 1);
    __Create();
    puts("GENET teardown failure tests passed");
    return 0;
}
