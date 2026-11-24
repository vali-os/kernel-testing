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
static int g_closeFails;
static int g_restoreFails;
static unsigned g_platformStops;
static unsigned g_unpublishes;
// Lines written to the system log. Only present without diagnostics, where
// the setup progress messages are compiled out and every line is a failure.
static unsigned g_logs;

enum __CreateFailure {
    CreateNoFailure,
    CreateDevice,
    CreateConfiguration,
    CreatePrepare,
    CreateResources,
    CreateMap,
    CreateWrapperValidation,
    CreateWrapperWrite,
    CreateResetRelease,
    CreateMac,
    CreateStop,
    CreateHardware,
    CreatePhyReset,
    CreateMdio,
    CreatePhy,
    CreateDma,
    CreateStart,
    CreatePublish
};

static enum __CreateFailure g_createFailure;

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
    g_unpublishes++;
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
    assert(!g_stopFails);
    g_cleanups++;
    return g_cleanupFails ? OS_EBUSY : OS_EOK;
}

oserr_t
PhyStop(
    _In_ PhyDevice_t* phy)
{
    assert(phy->Bus != NULL);
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
    if (g_closeFails) {
        return OS_EBUSY;
    }
    *context = NULL;
    return OS_EOK;
}

oserr_t
GenetWrapperRestorePhy(
    _In_ GenetWrapper_t* state)
{
    if (g_restoreFails) {
        return OS_EDEVFAULT;
    }
    state->Registers = NULL;
    return OS_EOK;
}

oserr_t
DeviceSetStage(
    _In_ uuid_t device,
    _In_ enum DeviceStage stage)
{
    assert(device == 42);
    if (stage == DevicePrepare) {
        __Record(StepPrepare);
        if (g_createFailure == CreatePrepare) {
            return OS_EDEVFAULT;
        }
    } else if (stage == DeviceStart) {
        __Record(StepStart);
        if (g_createFailure == CreateStart) {
            return OS_EDEVFAULT;
        }
    } else {
        assert(stage == DeviceStop);
        g_platformStops++;
    }
    return g_platformFails && stage == DeviceStop ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceIoRelease(
    _In_ DeviceIo_t* registers)
{
    g_releases++;
    return g_unmapFails ? OS_EBUSY : OS_EOK;
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    g_logs++;
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
    return g_createFailure == CreateStop ? OS_EDEVFAULT : OS_EOK;
}
oserr_t
GenetTransferSubmit(
    _In_ EthernetDevice_t* device,
    _In_ EthernetSlot_t* slot)
{
    assert(0);
    return OS_EOK;
}
oserr_t
GenetLinkRead(
    _In_ EthernetDevice_t* device,
    _In_ struct PhyLink* link)
{
    assert(0);
    return OS_EOK;
}
void
GenetTransferPoll(
    _In_ EthernetDevice_t* device)
{
    assert(0);
}

const struct MdioOperations g_genetMdioOperations = {0};

size_t
DeviceIoRead(
    _In_ DeviceIo_t* io,
    _In_ size_t      offset,
    _In_ size_t      width)
{
    return 0;
}

oserr_t
GenetInterruptRegister(
    _InOut_ GenetController_t* controller)
{
    __Record(StepInterruptRegister);
    return g_interruptRegisterFails ? OS_ENOTSUPPORTED : OS_EOK;
}

oserr_t
GenetInterruptUnregister(
    _InOut_ GenetController_t* controller)
{
    __Record(StepInterruptUnregister);
    g_interruptUnregisters++;
    g_stopsAtUnregister = g_stops;
    g_cleanupsAtUnregister = g_cleanups;
    return g_interruptUnregisterFails ? OS_EBUSY : OS_EOK;
}

bool
GenetWrapperMatchesDevice(
    _In_ const Device_t* device)
{
    return g_createFailure != CreateDevice;
}

oserr_t
DeviceEthernetGetConfiguration(
    _In_  uuid_t                              device,
    _Out_ struct DeviceEthernetConfiguration* configuration)
{
    memset(configuration, 0, sizeof(*configuration));
    configuration->MacPresent = 1;
    configuration->Mac[0] = g_createFailure == CreateMac ? 1 : 2;
    configuration->Mac[5] = 1;
    return g_createFailure == CreateConfiguration ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceGetResources(
    _In_  uuid_t                 device,
    _Out_ struct DeviceResources* resources)
{
    memset(resources, 0, sizeof(*resources));
    resources->Count = 1;
    return g_createFailure == CreateResources ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceIoAcquire(
    _In_ DeviceIo_t* registers)
{
    return g_createFailure == CreateMap ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
GenetWrapperConfigurePhy(
    _InOut_ GenetWrapper_t*                           state,
    _In_    DeviceIo_t*                               registers,
    _In_    const struct DeviceEthernetConfiguration* configuration)
{
    if (g_createFailure == CreateWrapperValidation) {
        return OS_ENOTSUPPORTED;
    }
    state->Registers = registers;
    return g_createFailure == CreateWrapperWrite ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
GenetWrapperReleaseReset(
    _InOut_ GenetWrapper_t* wrapper,
    _In_    DeviceIo_t*     registers)
{
    return g_createFailure == CreateResetRelease ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
GenetMacInitialize(
    _In_ GenetController_t* controller)
{
    return g_createFailure == CreateHardware ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DeviceEthernetResetPhy(
    _In_ uuid_t device)
{
    return g_createFailure == CreatePhyReset ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
MdioInitialize(
    _Out_ MdioBus_t*                   bus,
    _In_  const struct MdioOperations* operations,
    _In_  void*                        context,
    _In_  uint32_t                     address)
{
    return g_createFailure == CreateMdio ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
PhyInitialize(
    _Out_ PhyDevice_t*                          phy,
    _In_  MdioBus_t*                            bus,
    _In_  const struct DeviceEthernetConfiguration* configuration)
{
    // Production PhyInitialize records the bus before any fallible step.
    phy->Bus = bus;
    return g_createFailure == CreatePhy ? OS_EDEVFAULT : OS_EOK;
}

oserr_t
DMAOpen(
    _In_  uuid_t              device,
    _In_  uint64_t            addressLimit,
    _Out_ struct DMAContext** context)
{
    if (g_createFailure == CreateDma) {
        return OS_EDEVFAULT;
    }
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
    return g_createFailure == CreatePublish ? OS_EOOM : OS_EOK;
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
    GenetController_t controller;
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
    assert(controller.State == GENET_STATE_READY);
    __ExpectSteps(ready, 4);
    assert(GenetCreate(&controller, &device) == OS_EBUSY);
    __ExpectSteps(ready, 4);

    // A missing interrupt is not fatal. The partial setup is released and the
    // driver keeps polling.
    memset(&controller, 0, sizeof(controller));
    g_stepCount = 0;
    g_interruptRegisterFails = 1;
    assert(GenetCreate(&controller, &device) == OS_EOK);
    assert(controller.State == GENET_STATE_READY);
    __ExpectSteps(fallback, 5);

    // If the partial setup cannot be released the handler may still run, so
    // the device must not start.
    memset(&controller, 0, sizeof(controller));
    g_stepCount = 0;
    g_interruptUnregisterFails = 1;
    assert(GenetCreate(&controller, &device) == OS_EBUSY);
    assert(controller.State != GENET_STATE_READY);
    __ExpectSteps(stuck, 3);
    g_interruptRegisterFails = 0;
    g_interruptUnregisterFails = 0;
}

static void
__DestroyRetries(void)
{
    GenetController_t controller = {0};
    Device_t device = { .Id = 42 };

    controller.Device = &device;
    controller.State = GENET_STATE_READY;
    controller.Phy.Bus = &controller.Bus;
    controller.Wrapper.Registers = &controller.Registers;
    controller.Queues.Storage.Buffer = (void*)1;
    controller.Dma = (void*)2;
    g_stopFails = 1;
    assert(GenetDestroy(&controller) == OS_ETIMEOUT);
    assert(controller.State == GENET_STATE_STOPPING);
    assert(g_unpublishes == 1 && GenetRegistersMapped(&controller));
    assert(g_frees == 0 && g_isolates == 0 && g_releases == 0);
    assert(g_cleanups == 0 && g_interruptUnregisters == 0);
    assert(GenetCreate(&controller, &device) == OS_EBUSY);
    g_stopFails = 0;
    g_cleanupFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.State == GENET_STATE_STOPPED && g_cleanups == 1);
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
    assert(controller.State == GENET_STATE_STOPPED && controller.Phy.Bus == NULL);
    assert(g_isolates == 1 && g_frees == 0);
    g_freeFails = 0;
    g_closeFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.State == GENET_STATE_STOPPED && controller.Dma != NULL);
    assert(g_frees == 1 && g_platformStops == 0 && g_releases == 0);
    g_closeFails = 0;
    g_restoreFails = 1;
    assert(GenetDestroy(&controller) == OS_EDEVFAULT);
    assert(controller.State == GENET_STATE_STOPPED && controller.Dma == NULL);
    assert(controller.Wrapper.Registers != NULL && g_platformStops == 0);
    g_restoreFails = 0;
    g_platformFails = 1;
    assert(GenetDestroy(&controller) == OS_EDEVFAULT);
    assert(controller.State == GENET_STATE_STOPPED && g_frees == 1);
    g_platformFails = 0;
    g_unmapFails = 1;
    assert(GenetDestroy(&controller) == OS_EBUSY);
    assert(controller.State == GENET_STATE_UNMAPPING);
    assert(GenetRegistersMapped(&controller) && g_platformStops == 2);
    assert(g_unpublishes == 1);
    g_interruptUnregisterFails = 1;
    g_cleanupFails = 1;
    g_platformFails = 1;
    g_unmapFails = 0;
    // After DeviceStop succeeds only unmapping remains. Earlier cleanup calls
    // must not run again against the stopped platform.
    assert(GenetDestroy(&controller) == OS_EOK);
    assert(controller.State == GENET_STATE_DESTROYED);
    assert(!GenetRegistersMapped(&controller) && g_platformStops == 2);
    assert(g_stops == 2 && g_isolates == 1 && g_frees == 1);
    assert(GenetDestroy(&controller) == OS_EOK);
    assert(g_releases == 2);
    assert(GenetCreate(&controller, &device) == OS_EBUSY);
    g_interruptUnregisterFails = 0;
    g_cleanupFails = 0;
    g_platformFails = 0;
}

static void
__CreateFailures(void)
{
    // Logs is the number of log lines controller.c writes for the failure.
    // A step that fails inside another GENET function is logged there, and
    // those functions are stubs here, so their count is zero.
    const struct {
        enum __CreateFailure Failure;
        enum GenetState      State;
        unsigned             Stops;
        unsigned             Isolates;
        unsigned             Logs;
    } cases[] = {
        { CreateDevice, GENET_STATE_NEW, 0, 0, 1 },
        { CreateConfiguration, GENET_STATE_NEW, 0, 0, 1 },
        { CreatePrepare, GENET_STATE_NEW, 0, 0, 1 },
        { CreateResources, GENET_STATE_PREPARED, 0, 0, 1 },
        { CreateMap, GENET_STATE_PREPARED, 0, 0, 1 },
        { CreateWrapperValidation, GENET_STATE_MAPPED, 0, 0, 0 },
        { CreateWrapperWrite, GENET_STATE_MAPPED, 1, 0, 0 },
        { CreateResetRelease, GENET_STATE_MAPPED, 1, 0, 0 },
        { CreateMac, GENET_STATE_MAPPED, 1, 0, 1 },
        { CreateStop, GENET_STATE_MAPPED, 1, 0, 0 },
        { CreateHardware, GENET_STATE_MAPPED, 1, 0, 0 },
        { CreatePhyReset, GENET_STATE_MAPPED, 1, 0, 1 },
        { CreateMdio, GENET_STATE_MAPPED, 1, 0, 1 },
        { CreatePhy, GENET_STATE_MAPPED, 1, 1, 1 },
        { CreateDma, GENET_STATE_MAPPED, 1, 1, 1 },
        { CreateStart, GENET_STATE_MAPPED, 1, 1, 1 },
        { CreatePublish, GENET_STATE_MAPPED, 1, 1, 1 }
    };
    GenetController_t controller;
    Device_t          device = { .Id = 42 };
    unsigned          logs;
    unsigned          i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        memset(&controller, 0, sizeof(controller));
        g_stepCount = 0;
        g_stops = 0;
        g_isolates = 0;
        g_releases = 0;
        g_platformStops = 0;
        g_unpublishes = 0;
        g_createFailure = cases[i].Failure;
        logs = g_logs;
        assert(GenetCreate(&controller, &device) != OS_EOK);
        assert(controller.State == cases[i].State);

        // Each failure is logged once, where it is detected. With diagnostics
        // the progress messages are logged as well, so only check that the
        // failure left a line.
#ifdef VALI_ETHERNET_DIAGNOSTICS
        assert(g_logs >= logs + cases[i].Logs);
#else
        assert(g_logs == logs + cases[i].Logs);
#endif

        // A prepare that succeeded still needs a platform stop if resource
        // lookup or register mapping failed. Preserve that step on failure.
        if (controller.State == GENET_STATE_PREPARED) {
            g_platformFails = 1;
            assert(GenetDestroy(&controller) == OS_EDEVFAULT);
            assert(controller.State == GENET_STATE_PREPARED && g_releases == 0);
            g_platformFails = 0;
            g_platformStops = 0;
        }
        assert(GenetDestroy(&controller) == OS_EOK);
        assert(controller.State == GENET_STATE_DESTROYED && !GenetRegistersMapped(&controller));
        assert(g_stops == cases[i].Stops && g_isolates == cases[i].Isolates);
        assert(g_releases == (cases[i].State == GENET_STATE_MAPPED));
        assert(g_platformStops == (cases[i].State != GENET_STATE_NEW));
        assert(g_unpublishes == 0 && controller.Phy.Bus == NULL);
        assert(controller.Dma == NULL && controller.Wrapper.Registers == NULL);
        assert(GenetDestroy(&controller) == OS_EOK);
        assert(g_releases == (cases[i].State == GENET_STATE_MAPPED));
    }
    g_createFailure = CreateNoFailure;
}

int
main(void)
{
    __DestroyRetries();
    __Create();
    __CreateFailures();
    puts("GENET lifecycle failure tests passed");
    return 0;
}
