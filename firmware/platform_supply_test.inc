/*
 * Supplies and clocks for drivers: the regulator owners for regulator-fixed
 * and regulator-gpio against the fake GPIO controller of
 * platform_controller_test.inc, the supply rules of DevicePrepare and
 * DeviceStop, the opt-out, the clock rate request, the SD host's firmware
 * node as the SDHCI driver decodes it, the libddk wrappers, quiet decoding
 * and the hold-back log levels.
 */

#include <ddk/devices/clock.h>
#include <ddk/devices/firmware-node.h>
#include <ddk/devices/supply.h>

// Defined in platform_bus_test.c, after this file.
static unsigned int g_logCounts[8];
static char         g_logLast[256];
static const char*  g_knownDriver;
static unsigned int __LogProblems(void);

static uint64_t
__ElapsedUs(const struct timespec* start)
{
    return (uint64_t)(g_fakeNow.tv_sec - start->tv_sec) * 1000000 +
           (uint64_t)((g_fakeNow.tv_nsec - start->tv_nsec) / 1000);
}

static const struct PlatformRegulator*
__RegulatorNamed(struct PlatformBus* bus, const char* name)
{
    const struct PlatformRegulator* regulator;

    for (regulator = bus->Regulators; regulator != NULL; regulator = regulator->Next) {
        if (!strcmp(regulator->Name, name)) {
            return regulator;
        }
    }
    return NULL;
}

static void
__AnswerBadNode(enum OSIOCtlRequest request, void* buffer, size_t length)
{
    CHECK(request == OSIOCTLREQUEST_FIRMWARE_NODE && length == sizeof(struct DeviceFirmwareNodeRequest));
    ((struct DeviceFirmwareNodeRequest*)buffer)->Data[0] ^= 0xff;
}

static void
__AnswerZero(enum OSIOCtlRequest request, void* buffer, size_t length)
{
    (void)length;
    if (request == OSIOCTLREQUEST_CLOCK_RATE) {
        ((struct DeviceClockRequest*)buffer)->Rate = 0;
    } else if (request == OSIOCTLREQUEST_SUPPLY_CONTROL) {
        ((struct DeviceSupplyRequest*)buffer)->Value = 0;
    }
}

static oserr_t
__SupplyRaw(struct PlatformEntry* entry, struct DeviceSupplyRequest* request, size_t length)
{
    return DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_SUPPLY_CONTROL, request, length);
}

// The wrappers check their arguments before any request, and deviced checks
// every field of a request a driver could send by hand.
static void
__SupplyValidation(struct PlatformEntry* powered)
{
    struct DeviceSupplyRequest request = { .Operation = DeviceSupplyOperationManage };
    struct DeviceClockRequest  clock = { .Index = 0, .Reserved = 1 };
    unsigned int               ioctls = g_ioctls;
    uint32_t                   microvolts;
    uint64_t                   hz;

    CHECK(DeviceSupplyManage(powered->DeviceId, NULL) == OS_EINVALPARAMS);
    CHECK(DeviceSupplyManage(powered->DeviceId, "") == OS_EINVALPARAMS);
    CHECK(DeviceSupplyManage(powered->DeviceId, "abcdefghijklmnopqrstuvwxyz012345") == OS_EINVALPARAMS);
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vdd", NULL) == OS_EINVALPARAMS);
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vdd", 0) == OS_EINVALPARAMS);
    CHECK(DeviceClockGetRate(powered->DeviceId, 0, NULL) == OS_EINVALPARAMS);
    CHECK(g_ioctls == ioctls);

    // Names must be terminated, with zero bytes after the terminator.
    memset(request.Name, 'a', sizeof(request.Name));
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    memset(request.Name, 0, sizeof(request.Name));
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    strcpy(request.Name, "vdd");
    request.Name[10] = 'x';
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    request.Name[10] = 0;
    CHECK(__SupplyRaw(powered, &request, sizeof(request) - 1) == OS_EINVALPARAMS);

    // Unknown operations and values outside each operation's range.
    request.Operation = 9;
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    request.Operation = DeviceSupplyOperationManage;
    request.Value = 1;
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    request.Operation = DeviceSupplyOperationSetState;
    request.Value = 2;
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    request.Operation = DeviceSupplyOperationSetVoltage;
    request.Value = 0;
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    request.Operation = DeviceSupplyOperationGetVoltage;
    request.Value = 5;
    CHECK(__SupplyRaw(powered, &request, sizeof(request)) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl(1, powered->DeviceId, OSIOCTLREQUEST_CLOCK_RATE, &clock, sizeof(clock)) == OS_EINVALPARAMS);
    clock.Reserved = 0;
    clock.Rate = 5;
    CHECK(DmHandleIoctl(1, powered->DeviceId, OSIOCTLREQUEST_CLOCK_RATE, &clock, sizeof(clock)) == OS_EINVALPARAMS);

    // Only the assigned driver may call, before or after preparation.
    g_ioctlPrincipal = 2;
    CHECK(DeviceSupplyManage(powered->DeviceId, "vdd") == OS_EPERMISSIONS);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 1) == OS_EPERMISSIONS);
    CHECK(DeviceClockGetRate(powered->DeviceId, 0, &hz) == OS_EPERMISSIONS);
    g_ioctlPrincipal = 1;

    // Everything except the opt-out needs a prepared device. A supply the
    // device does not name is unknown, and so is a parent's or another node's.
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 1) == OS_EBUSY);
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vdd", &microvolts) == OS_EBUSY);
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vdd", 3300000) == OS_EBUSY);
    CHECK(DeviceClockGetRate(powered->DeviceId, 0, &hz) == OS_EBUSY);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vin") == OS_ENOENT);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vdd-supply") == OS_ENOENT);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vd") == OS_ENOENT);
}

// The plan check for holding devices back decodes quietly. The errors show
// when a driver prepares the device. Hold-back detail goes to trace level,
// and to NOTICE level only for devices that a known driver matches.
static void
__SupplyLogging(struct PlatformBus* bus)
{
    struct PlatformEntry* broken = __EntryNamed(bus, "reg-select");
    struct PlatformEntry* stuck = __EntryNamed(bus, "stuck@5200");
    struct PlatformEntry* entry;
    unsigned int          problems = __LogProblems();
    unsigned int          notices = g_logCounts[OSSYSLOGLEVEL_TRACE];
    unsigned int          waiting = 0;
    char                  summary[128];

    // reg-select's own plan has "gpios", which no driver sets up.
    usched_mtx_lock(&bus->Lock);
    CHECK(!PlatformWaitingCheckLocked(broken, PLATFORM_WAITING_LOG_NOTICE));
    CHECK(__LogProblems() == problems && g_logCounts[OSSYSLOGLEVEL_TRACE] == notices);
    CHECK(!FdtDecodeIsQuiet());
    usched_mtx_unlock(&bus->Lock);
    CHECK(__Stage(broken, DevicePrepare) == OS_ENOTSUPPORTED);
    CHECK(__LogProblems() > problems);

    // Trace lines are compiled out of this build. A NOTICE line names the
    // first missing dependency.
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformWaitingCheckLocked(stuck, PLATFORM_WAITING_LOG_TRACE));
    CHECK(g_logCounts[OSSYSLOGLEVEL_TRACE] == notices);
    CHECK(PlatformWaitingCheckLocked(stuck, PLATFORM_WAITING_LOG_NOTICE));
    CHECK(g_logCounts[OSSYSLOGLEVEL_TRACE] == notices + 1);
    CHECK(strstr(g_logLast, "stuck@5200 waits for supply reg-vin") != NULL);
    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        waiting += entry->Waiting && !entry->IsBus;
    }
    usched_mtx_unlock(&bus->Lock);

    // The report names only the device a known driver matches, plus a summary.
    notices = g_logCounts[OSSYSLOGLEVEL_TRACE];
    PlatformWaitingReport(bus);
    CHECK(g_logCounts[OSSYSLOGLEVEL_TRACE] == notices + 2);
    snprintf(summary, sizeof(summary),
             "Platform: %u devices wait for dependency owners, 1 of them have a matching driver", waiting);
    CHECK(!strcmp(g_logLast, summary));
}

static void
__SupplyDecoding(struct PlatformBus* bus)
{
    struct FdtRegulator regulator;
    uint32_t            gpio = __Phandle(bus, 206);

    // The flags cell says active low, but enable-active-high decides.
    CHECK(FdtRegulatorDescribe(bus->Tree, __Phandle(bus, 220), &regulator) == OS_EOK);
    CHECK(regulator.Type == FdtRegulatorFixed && regulator.MinimumMicrovolts == 3300000);
    CHECK(regulator.Enable.Present && regulator.Enable.Line.Controller == gpio && regulator.Enable.Line.Line == 10);
    CHECK(regulator.Enable.ActiveLow == 0 && regulator.StartupDelayUs == 1000 && regulator.OffOnDelayUs == 20000);
    CHECK(!regulator.AlwaysOn && !regulator.BootOn);

    // Legacy "gpio", no enable-active-high: active low.
    CHECK(FdtRegulatorDescribe(bus->Tree, __Phandle(bus, 221), &regulator) == OS_EOK);
    CHECK(regulator.Enable.Present && regulator.Enable.Line.Line == 11 && regulator.Enable.ActiveLow == 1);
    CHECK(regulator.BootOn && !regulator.AlwaysOn);

    CHECK(FdtRegulatorDescribe(bus->Tree, __Phandle(bus, 222), &regulator) == OS_EOK);
    CHECK(!regulator.Enable.Present && regulator.AlwaysOn && regulator.MaximumMicrovolts == 5000000);

    CHECK(FdtRegulatorDescribe(bus->Tree, __Phandle(bus, 223), &regulator) == OS_EOK);
    CHECK(regulator.Type == FdtRegulatorGpio && regulator.StateCount == 2 && regulator.SettlingTimeUs == 5000);
    CHECK(regulator.Selector.Line.Line == 12 && regulator.Selector.ActiveLow == 1);
    CHECK(regulator.Enable.Line.Line == 13 && regulator.Enable.ActiveLow == 0);
    CHECK(regulator.States[0].Microvolts == 1800000 && regulator.States[0].Value == 1);

    // vin-supply is not handled, so the regulator gets no owner at all.
    CHECK(FdtRegulatorDescribe(bus->Tree, __Phandle(bus, 224), &regulator) == OS_ENOTSUPPORTED);
    CHECK(__RegulatorNamed(bus, "reg-vin") == NULL);
    CHECK(__RegulatorNamed(bus, "reg-switched") != NULL && __RegulatorNamed(bus, "reg-select") != NULL);
}

static void
__SupplyPrepare(struct PlatformBus* bus, struct PlatformEntry* powered, struct PlatformEntry* gpio)
{
    struct timespec start = g_fakeNow;
    unsigned int    requests;
    uint64_t        hz;

    // The driver runs vio and vlow itself.
    CHECK(DeviceSupplyManage(powered->DeviceId, "vio") == OS_EOK);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vlow") == OS_EOK);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vlow") == OS_EOK);

    // Only vdd is switched on: its line goes high, with its start-up time.
    requests = g_fakeController.GpioRequests;
    g_fakeController.Rate = 54000000;
    CHECK(__Stage(powered, DevicePrepare) == OS_EOK);
    CHECK(g_fakeController.GpioRequests == requests + 1);
    CHECK(g_fakeController.Directions[10] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[10] == 1);
    CHECK(g_fakeController.Directions[11] == 0 && g_fakeController.Directions[12] == 0);
    CHECK(g_fakeController.Directions[13] == 0);
    CHECK(__ElapsedUs(&start) == 1000);

    // Every regulator line counts as a GPIO user: vdd, vio twice and vlow.
    CHECK(gpio->Controller->GpioUsers == 4);
    CHECK(DeviceSupplyManage(powered->DeviceId, "vdd") == OS_EBUSY);

    // Clock 0 comes from the clock controller driver, clock 1 is fixed.
    CHECK(DeviceClockGetRate(powered->DeviceId, 0, &hz) == OS_EOK && hz == 54000000);
    CHECK(g_fakeController.LastClock == 9);
    requests = g_fakeController.Requests;
    CHECK(DeviceClockGetRate(powered->DeviceId, 1, &hz) == OS_EOK && hz == 24000000);
    CHECK(g_fakeController.Requests == requests);
    CHECK(DeviceClockGetRate(powered->DeviceId, 2, &hz) == OS_ENOENT);
    g_ioctlAnswer = __AnswerZero;
    CHECK(DeviceClockGetRate(powered->DeviceId, 0, &hz) == OS_EPROTOCOL);
    g_ioctlAnswer = NULL;
}

static void
__SupplyVoltages(struct PlatformEntry* powered)
{
    struct timespec start;
    uint32_t        microvolts = 0;

    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vdd", &microvolts) == OS_EOK && microvolts == 3300000);
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vaux", &microvolts) == OS_EOK && microvolts == 5000000);
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vdd", 3300000) == OS_EOK);
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vdd", 1800000) == OS_ENOTSUPPORTED);

    // Before any selection, the voltage is read from the active-low selector
    // line: low on the wire is logical 1, which is 1.8 V.
    g_fakeController.Lines[12] = 0;
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vio", &microvolts) == OS_EOK && microvolts == 1800000);

    // 3.3 V is state value 0, high on the wire, and waits the settling time.
    start = g_fakeNow;
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vio", 3300000) == OS_EOK);
    CHECK(g_fakeController.Directions[12] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[12] == 1);
    CHECK(__ElapsedUs(&start) == 5000);
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vio", &microvolts) == OS_EOK && microvolts == 3300000);
    CHECK(DeviceSupplySetVoltage(powered->DeviceId, "vio", 2500000) == OS_ENOTSUPPORTED);
    CHECK(g_fakeController.Lines[12] == 1);
    g_ioctlAnswer = __AnswerZero;
    CHECK(DeviceSupplyGetVoltage(powered->DeviceId, "vio", &microvolts) == OS_EPROTOCOL);
    g_ioctlAnswer = NULL;
}

static void
__SupplySwitching(struct PlatformEntry* powered, struct PlatformEntry* shared)
{
    struct timespec start;

    // vio has an active-high enable line and a start-up time.
    start = g_fakeNow;
    CHECK(DeviceSupplySetState(powered->DeviceId, "vio", 1) == OS_EOK);
    CHECK(g_fakeController.Lines[13] == 1 && __ElapsedUs(&start) == 2000);

    // vlow is boot-on and believed on. Switching it off sets the direction of
    // its active-low line with the off level, which is high.
    CHECK(DeviceSupplySetState(powered->DeviceId, "vlow", 0) == OS_EOK);
    CHECK(g_fakeController.Directions[11] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[11] == 1);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vlow", 1) == OS_EOK && g_fakeController.Lines[11] == 0);

    // A supply without an enable line cannot be switched off.
    CHECK(DeviceSupplySetState(powered->DeviceId, "vaux", 0) == OS_ENOTSUPPORTED);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vaux", 1) == OS_EOK);

    // Shared vdd: it stays on while either device needs it.
    CHECK(__Stage(shared, DevicePrepare) == OS_EOK);
    CHECK(__Stage(shared, DeviceStop) == OS_EOK && g_fakeController.Lines[10] == 1);
    CHECK(__Stage(shared, DevicePrepare) == OS_EOK);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 0) == OS_EOK && g_fakeController.Lines[10] == 1);
    CHECK(__Stage(shared, DeviceStop) == OS_EOK && g_fakeController.Lines[10] == 0);

    // Back on: the off-on delay since the switch-off, then the start-up time.
    start = g_fakeNow;
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 1) == OS_EOK && g_fakeController.Lines[10] == 1);
    CHECK(__ElapsedUs(&start) == 21000);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 1) == OS_EOK);

    // A GPIO error reaches the driver, and the supply stays as it was.
    g_fakeController.Result = OS_EDEVFAULT;
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 0) == OS_EDEVFAULT);
    g_fakeController.Result = OS_EOK;
    CHECK(g_fakeController.Lines[10] == 1);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 0) == OS_EOK && g_fakeController.Lines[10] == 0);
    CHECK(DeviceSupplySetState(powered->DeviceId, "vdd", 1) == OS_EOK && g_fakeController.Lines[10] == 1);
}

static void
__SupplyStopping(struct PlatformBus* bus, struct PlatformEntry* powered, struct PlatformEntry* gpio)
{
    DmDevice_t*  registered = DmDeviceFindLocked(powered->DeviceId);
    unsigned int requests;

    // DeviceStop switches off what this device switched on: vdd, vio and
    // vlow. The always-on supply stays on. The GPIO users go away.
    CHECK(DeviceControllerDisable(gpio->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EBUSY);
    CHECK(__Stage(powered, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.Lines[10] == 0 && g_fakeController.Lines[13] == 0 && g_fakeController.Lines[11] == 1);
    CHECK(gpio->Controller->GpioUsers == 0);

    // The opt-out is kept for the same driver. A device released without
    // DeviceStop still has its supplies switched off.
    requests = g_fakeController.GpioRequests;
    CHECK(__Stage(powered, DevicePrepare) == OS_EOK);
    CHECK(g_fakeController.GpioRequests == requests + 1 && g_fakeController.Lines[10] == 1);
    CHECK(powered->SupplyManaged != 0);
    usched_mtx_lock(&bus->Lock);
    PlatformActivationEndLocked(powered);
    usched_mtx_unlock(&bus->Lock);
    CHECK(powered->Activation == NULL && powered->SupplyManaged == 0 && g_fakeController.Lines[10] == 0);
    CHECK(gpio->Controller->GpioUsers == 0);

    // A new driver does not inherit the opt-out of the last one.
    CHECK(DeviceSupplyManage(powered->DeviceId, "vlow") == OS_EOK);
    registered->DriverPrincipal = 2;
    CHECK(DmHandleIoctl(2, powered->DeviceId, OSIOCTLREQUEST_DEVICE_SET_STAGE,
        &(enum DeviceStage) { DevicePrepare }, sizeof(enum DeviceStage)) == OS_EOK);
    CHECK(powered->SupplyManaged == 0 && g_fakeController.Lines[11] == 0);
    CHECK(DmHandleIoctl(2, powered->DeviceId, OSIOCTLREQUEST_DEVICE_SET_STAGE,
        &(enum DeviceStage) { DeviceStop }, sizeof(enum DeviceStage)) == OS_EOK);
    registered->DriverPrincipal = 1;
    CHECK(g_fakeController.Lines[11] == 1);

    // A GPIO controller that is gone cannot switch anything off. The device
    // still stops, without a request, and the ended record is then freed.
    CHECK(__Stage(powered, DevicePrepare) == OS_EOK && g_fakeController.Lines[10] == 1);
    usched_mtx_lock(&bus->Lock);
    PlatformControllerEndLocked(gpio);
    usched_mtx_unlock(&bus->Lock);
    requests = g_fakeController.GpioRequests;
    CHECK(__Stage(powered, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.GpioRequests == requests && gpio->Controller == NULL);
    CHECK(__Stage(gpio, DeviceStop) == OS_EOK);
}

static void
__SupplySynthetic(struct PlatformBus* bus)
{
    struct PlatformEntry*        powered = __EntryNamed(bus, "powered@5000");
    struct PlatformEntry*        shared = __EntryNamed(bus, "shared@5100");
    struct PlatformEntry*        stuck = __EntryNamed(bus, "stuck@5200");
    struct PlatformEntry*        gpio = __EntryNamed(bus, "gpio@3000");
    struct PlatformEntry*        mailbox = __EntryNamed(bus, "mailbox@2000");
    struct FdtPlatformDependency vdd = { .Kind = FdtPlatformSupply, .NodeOffset = __Phandle(bus, 220) };
    struct FdtPlatformDependency vaux = { .Kind = FdtPlatformSupply, .NodeOffset = __Phandle(bus, 222) };

    memset(&g_fakeController, 0, sizeof(g_fakeController));
    __SupplyDecoding(bus);
    __SupplyLogging(bus);
    __SupplyValidation(powered);

    // Regulators with lines wait for their GPIO controller. The one without a
    // line works at once. The device behind an unsupported one waits forever.
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformDependencyFindOwner(bus, &vdd) == NULL && PlatformDependencyFindOwner(bus, &vaux) != NULL);
    usched_mtx_unlock(&bus->Lock);
    CHECK(powered->Waiting && shared->Waiting && stuck->Waiting);

    CHECK(__Stage(mailbox, DevicePrepare) == OS_EOK);
    __ControllerJobsReset();
    CHECK(DeviceControllerEnable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(powered->Waiting && g_controllerJobCount == 0);
    CHECK(__Stage(gpio, DevicePrepare) == OS_EOK);
    CHECK(DeviceControllerEnable(gpio->DeviceId, DEVICE_CONTROLLER_GPIO) == OS_EOK);
    CHECK(!powered->Waiting && !shared->Waiting && stuck->Waiting && g_controllerJobCount == 2);
    __ControllerJobsRun();
    CHECK(DmDeviceIsBindable(powered->DeviceId) && !DmDeviceIsBindable(stuck->DeviceId));
    usched_mtx_lock(&bus->Lock);
    CHECK(PlatformDependencyFindOwner(bus, &vdd) != NULL);
    usched_mtx_unlock(&bus->Lock);

    // Registration did not touch a single line.
    CHECK(g_fakeController.GpioRequests == 0);

    __SupplyPrepare(bus, powered, gpio);
    __SupplyVoltages(powered);
    __SupplySwitching(powered, shared);
    __SupplyStopping(bus, powered, gpio);

    CHECK(DeviceControllerDisable(mailbox->DeviceId, DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(__Stage(mailbox, DeviceStop) == OS_EOK);
}

// Raspberry Pi 4: the SD host waits for its two regulators until the mailbox
// driver enables its GPIO expander, and for cprman clock 51, which the clock
// route of the firmware driver's manifest sends to firmware clock 12. A fake SD driver then runs both supplies
// itself, as the SDHCI driver will.
static void
__SupplyPi4(struct PlatformBus* bus)
{
    struct PlatformEntry*          emmc = __FindCompatible(bus, "brcm,bcm2711-emmc2");
    struct PlatformEntry*          mailbox = __FindCompatible(bus, "brcm,bcm2835-mbox");
    DeviceFirmwareNode_t           node;
    SDFirmwareConfiguration_t       sd;
    SDFirmwareConfiguration_t       saved;
    struct FdtPlatformDependencies plan;
    struct timespec                start;
    unsigned int                   requests;
    unsigned int                   notices;
    uint32_t                       microvolts;
    uint64_t                       hz;

    memset(&g_fakeController, 0, sizeof(g_fakeController));
    CHECK(emmc->Waiting && !DmDeviceIsBindable(emmc->DeviceId));
    CHECK(FdtPlatformDependencies(bus->Tree, emmc->Firmware.FirmwareNode, &plan) == OS_EOK);
    CHECK(plan.Count == 3 && !strcmp(plan.Entries[0].Property, "vqmmc-supply"));
    CHECK(!strcmp(plan.Entries[1].Property, "vmmc-supply") && plan.Entries[2].Arguments[0] == 51);
    CHECK(__RegulatorNamed(bus, "regulator-sd-io-1v8") != NULL && __RegulatorNamed(bus, "regulator-sd-vcc") != NULL);

    // A known SD driver makes the waiting SD host a NOTICE line.
    g_knownDriver = "brcm,bcm2711-emmc2";
    notices = g_logCounts[OSSYSLOGLEVEL_TRACE];
    PlatformWaitingReport(bus);
    CHECK(g_logCounts[OSSYSLOGLEVEL_TRACE] == notices + 2);
    g_knownDriver = "test,stuck";
    PlatformWaitingReport(bus);
    CHECK(g_logCounts[OSSYSLOGLEVEL_TRACE] == notices + 3);

    // The firmware node reads before preparation, and the SDHCI driver's
    // decoder finds the slot in it.
    CHECK(DeviceFirmwareNodeRead(emmc->DeviceId, &node) == OS_EOK);
    CHECK(SDFirmwareDecode(&node, &sd) == OS_EOK);
    DeviceFirmwareNodeDestroy(&node);
    CHECK(sd.Role == SDFirmwareRoleMemory && sd.BusWidth == 4 && sd.MaxFrequency == 0);
    CHECK(sd.Removable == 1 && sd.CardDetect == SDFirmwareCardDetectPolling);
    CHECK(sd.WriteProtect == SDFirmwareWriteProtectHost && sd.Timings == SD_FIRMWARE_TIMING_MMC_DDR_3V3);
    CHECK(sd.Supplies == (SD_FIRMWARE_SUPPLY_VMMC | SD_FIRMWARE_SUPPLY_VQMMC));
    CHECK(sd.Voltages == (SD_FIRMWARE_SIGNAL_3V3 | SD_FIRMWARE_SIGNAL_1V8));
    CHECK(sd.HostRegister == 0 && sd.ConfigRegister == SD_FIRMWARE_REGISTER_NONE);
    saved = sd;
    g_ioctlAnswer = __AnswerBadNode;
    CHECK(DeviceFirmwareNodeRead(emmc->DeviceId, &node) == OS_EPROTOCOL);
    g_ioctlAnswer = NULL;
    CHECK(!memcmp(&sd, &saved, sizeof(sd)));

    // The mailbox driver enables its expander and firmware clocks. Without
    // the clock route of its manifest, cprman clock 51 has no owner, so the
    // SD host keeps waiting.
    CHECK(__Stage(mailbox, DevicePrepare) == OS_EOK);
    __ControllerJobsReset();
    memset(&g_manifest, 0, sizeof(g_manifest));
    CHECK(__ControllerRequest(mailbox, OSIOCTLREQUEST_CONTROLLER_ENABLE,
        DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(emmc->Waiting && mailbox->Controller->ClockNodeCount == 1);
    CHECK(__ControllerRequest(mailbox, OSIOCTLREQUEST_CONTROLLER_DISABLE,
        DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);

    // The manifest of modules/platform/rpi-firmware/rpi-firmware.yaml: its
    // route gives the cprman node an owner. That releases the SD host.
    g_manifest.CompatibleCount = 2;
    strcpy(g_manifest.Compatibles[0], "raspberrypi,firmware-gpio");
    strcpy(g_manifest.Compatibles[1], "raspberrypi,firmware-clocks");
    strcpy(g_manifest.ClockRoutes[0].Compatible, "brcm,bcm2711-cprman");
    g_manifest.ClockRoutes[0].Clock = 51;
    g_manifest.ClockRoutes[0].ControllerClock = 12;
    g_manifest.ClockRouteCount = 1;
    CHECK(__ControllerRequest(mailbox, OSIOCTLREQUEST_CONTROLLER_ENABLE,
        DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(!emmc->Waiting && mailbox->Controller->ClockNodeCount == 2);
    __ControllerJobsRun();
    CHECK(DmDeviceIsBindable(emmc->DeviceId));

    // The SD driver runs both supplies itself. DevicePrepare sends no GPIO
    // request, and starts firmware clock 12.
    CHECK(DeviceSupplyManage(emmc->DeviceId, "vmmc") == OS_EOK);
    CHECK(DeviceSupplyManage(emmc->DeviceId, "vqmmc") == OS_EOK);
    requests = g_fakeController.GpioRequests;
    g_fakeController.Rate = 100000000;
    CHECK(__Stage(emmc, DevicePrepare) == OS_EOK);
    CHECK(g_fakeController.GpioRequests == requests);
    CHECK(g_fakeController.Clocks[12] == 1 && g_fakeController.LastDevice == mailbox->DeviceId);

    CHECK(DeviceClockGetRate(emmc->DeviceId, 0, &hz) == OS_EOK && hz == 100000000);
    CHECK(g_fakeController.LastClock == 12);

    // The second board run: the controller driver failed the GPIO request
    // with OS_ENOTSUPPORTED. That is a device fault for the SD driver, never
    // a regulator that cannot be switched. Other codes pass unchanged, and
    // no line changes.
    requests = g_fakeController.GpioRequests;
    g_fakeController.Result = OS_ENOTSUPPORTED;
    CHECK(DeviceSupplyGetVoltage(emmc->DeviceId, "vqmmc", &microvolts) == OS_EDEVFAULT);
    CHECK(DeviceSupplySetVoltage(emmc->DeviceId, "vqmmc", 3300000) == OS_EDEVFAULT);
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 0) == OS_EDEVFAULT);
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 1) == OS_EDEVFAULT);
    CHECK(DeviceClockGetRate(emmc->DeviceId, 0, &hz) == OS_EDEVFAULT);
    CHECK(g_fakeController.GpioRequests == requests + 4);

    // The always-on I/O regulator sends no request and still says it cannot
    // be switched off.
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vqmmc", 0) == OS_ENOTSUPPORTED);
    CHECK(g_fakeController.GpioRequests == requests + 4);
    g_fakeController.Result = OS_ETIMEOUT;
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 1) == OS_ETIMEOUT);
    CHECK(DeviceSupplySetVoltage(emmc->DeviceId, "vqmmc", 3300000) == OS_ETIMEOUT);
    g_fakeController.Result = OS_EOK;
    CHECK(g_fakeController.Directions[4] == 0 && g_fakeController.Directions[6] == 0);

    // 3.3 V signaling drives expander line 4 low, then waits 5 ms.
    start = g_fakeNow;
    CHECK(DeviceSupplySetVoltage(emmc->DeviceId, "vqmmc", 3300000) == OS_EOK);
    CHECK(g_fakeController.Directions[4] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[4] == 0);
    CHECK(__ElapsedUs(&start) == 5000);
    CHECK(DeviceSupplyGetVoltage(emmc->DeviceId, "vqmmc", &microvolts) == OS_EOK && microvolts == 3300000);
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vqmmc", 0) == OS_ENOTSUPPORTED);

    // Card power: line 6 high. A power cycle has no delays in this tree.
    start = g_fakeNow;
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 1) == OS_EOK);
    CHECK(g_fakeController.Directions[6] == PLATFORM_GPIO_OUTPUT && g_fakeController.Lines[6] == 1);
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 0) == OS_EOK && g_fakeController.Lines[6] == 0);
    CHECK(DeviceSupplySetState(emmc->DeviceId, "vmmc", 1) == OS_EOK && g_fakeController.Lines[6] == 1);
    CHECK(__ElapsedUs(&start) == 0);
    CHECK(DeviceSupplyGetVoltage(emmc->DeviceId, "vmmc", &microvolts) == OS_EOK && microvolts == 3300000);

    // DeviceStop switches card power off and stops the clock. The I/O
    // voltage regulator is always on and keeps its selection.
    CHECK(__Stage(emmc, DeviceStop) == OS_EOK);
    CHECK(g_fakeController.Lines[6] == 0 && g_fakeController.Lines[4] == 0 && g_fakeController.Clocks[12] == 0);
    CHECK(mailbox->Controller->GpioUsers == 0);
    CHECK(__ControllerRequest(mailbox, OSIOCTLREQUEST_CONTROLLER_DISABLE,
        DEVICE_CONTROLLER_GPIO | DEVICE_CONTROLLER_CLOCK) == OS_EOK);
    CHECK(mailbox->Controller == NULL && __Stage(mailbox, DeviceStop) == OS_EOK);
    memset(&g_manifest, 0, sizeof(g_manifest));
}
