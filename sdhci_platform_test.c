/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 * Host test for the SDHCI device tree slot (platform.c) with the BCM2711
 * controller variant and the real engine, with the fakes of
 * sdhci_fake_host.inc. The firmware node comes through the real libddk
 * wrapper (DeviceFirmwareNodeRead) and the SD layer's decoder
 * (SDFirmwareDecode). This file answers the wrapper's device request with a
 * node description built from the case's board values, and adds fakes of
 * the other libddk calls a platform host makes: DeviceSupplyManage,
 * DeviceSupplySetState, DeviceSupplySetVoltage and DeviceClockGetRate. Each
 * one is recorded in the shared event list, so a case can check the order of
 * these calls against DevicePrepare, the register reads and the power
 * control writes.
 *
 * The host is the Raspberry Pi 4 EMMC2 with the register values of a real
 * board, whose capability register reports an embedded slot.
 */

#include "sdhci_fake_host.inc"
#include <ddk/devices/clock.h>
#include <ddk/devices/platform.h>
#include <ddk/devices/firmware-node.h>
#include <ddk/devices/supply.h>
#include <os/device.h>
#include "sdhci/variants/bcm2711.h"
#include "sdhci/platform.h"

#define EVENT_DESCRIPTOR 9

// Register values read from a Raspberry Pi 4 EMMC2 host: a 100 MHz base
// clock, ADMA2, high speed, 3.3 V and 1.8 V, eight data lines, 2048-byte
// blocks and an embedded slot type. The host version register reports 3.00.
#define BOARD_CAPABILITIES  0x45EE6432U
#define BOARD_CAPABILITIES1 0x0000A525U
#define BOARD_BASE_CLOCK_HZ 100000000U

// Values of the Offset field of the supply events.
#define SUPPLY_VMMC  1U
#define SUPPLY_VQMMC 2U

#define CLOCK_400K   400000U
#define CLOCK_50M    50000000U
#define CLOCK_200M   200000000U

// The libddk calls a case can make fail.
enum PlatformFail {
    PlatformFailNone,
    PlatformFailConfiguration,
    PlatformFailManageVmmc,
    PlatformFailManageVqmmc,
    PlatformFailClock,
    PlatformFailVoltage,
    PlatformFailVmmcOn,
    PlatformFailVmmcOff,
    // Switching vmmc off fails with OS_EDEVFAULT, which is what deviced now
    // gives for a failed GPIO line, as on the second board run.
    PlatformFailVmmcOffFault
};

static SdhciController_t            g_controller;
static SdhciPlatform_t              g_platform;
static PlatformDevice_t             g_platformDevice;
// The board values of the case. The node description is built from them.
static SDFirmwareConfiguration_t     g_configuration;
static enum PlatformFail            g_platformFail;
static uint64_t                     g_clockRateHz;
// The fake "vmmc" is always on: switching it off answers OS_ENOTSUPPORTED.
static bool                         g_vmmcAlwaysOn;

// ---------------------------------------------------------------------------
// libddk fakes
// ---------------------------------------------------------------------------

static uint32_t
__SupplyId(
    _In_ const char* name)
{
    if (strcmp(name, "vmmc") == 0) {
        return SUPPLY_VMMC;
    }
    assert(strcmp(name, "vqmmc") == 0);
    return SUPPLY_VQMMC;
}

/**
 * @brief The node description of the case, as deviced would pack it.
 */
static struct {
    uint8_t  Data[512];
    uint32_t Length;
    uint32_t PropertyCount;
    uint32_t ChildCount;
} g_node;

static void
__NodeU32(
    _In_ uint32_t value)
{
    memcpy(&g_node.Data[g_node.Length], &value, sizeof(value));
    g_node.Length += sizeof(value);
}

static void
__NodeBytes(
    _In_ const void* data,
    _In_ uint32_t    length)
{
    uint32_t padded = (length + 3U) & ~3U;

    assert(g_node.Length + padded <= sizeof(g_node.Data));
    memset(&g_node.Data[g_node.Length], 0, padded);
    if (length != 0) {
        memcpy(&g_node.Data[g_node.Length], data, length);
    }
    g_node.Length += padded;
}

static void
__NodeProperty(
    _In_ const char* name,
    _In_ const void* value,
    _In_ uint32_t    length)
{
    __NodeU32((uint32_t)strlen(name) + 1);
    __NodeU32(length);
    __NodeBytes(name, (uint32_t)strlen(name) + 1);
    __NodeBytes(value, length);
    g_node.PropertyCount++;
}

static void
__NodeCells(
    _In_ const char* name,
    _In_ uint32_t    count,
    _In_ uint32_t    first,
    _In_ uint32_t    second)
{
    uint32_t values[2] = { first, second };
    uint8_t  bytes[8];

    // Firmware cells are big-endian.
    for (uint32_t i = 0; i < count; i++) {
        bytes[i * 4] = (uint8_t)(values[i] >> 24);
        bytes[i * 4 + 1] = (uint8_t)(values[i] >> 16);
        bytes[i * 4 + 2] = (uint8_t)(values[i] >> 8);
        bytes[i * 4 + 3] = (uint8_t)values[i];
    }
    __NodeProperty(name, bytes, count * 4);
}

// Builds the EMMC2 node of the bundled Raspberry Pi 4 tree, changed by the
// case's board values. One cell per address and size, as below "emmc2bus".
static void
__BuildNode(void)
{
    static const char hostNames[] = "cfg\0host";

    memset(&g_node, 0, sizeof(g_node));
    g_node.Length = sizeof(struct DeviceFirmwareNodeHeader);
    __NodeProperty("compatible", "brcm,bcm2711-emmc2", sizeof("brcm,bcm2711-emmc2"));
    if (g_configuration.HostRegister == 1) {
        uint8_t reg[16] = { 0x7e, 0x34, 0x00, 0x00, 0, 0, 1, 0, 0x7e, 0x34, 0x01, 0x00, 0, 0, 1, 0 };
        __NodeProperty("reg", reg, sizeof(reg));
        __NodeProperty("reg-names", hostNames, sizeof(hostNames));
    } else {
        __NodeCells("reg", 2, 0x7e340000U, 0x100);
    }
    __NodeCells("bus-width", 1, g_configuration.BusWidth, 0);
    if (g_configuration.MaxFrequency != 0) {
        __NodeCells("max-frequency", 1, g_configuration.MaxFrequency, 0);
    }
    if (g_configuration.Removable == 0) {
        __NodeProperty("non-removable", NULL, 0);
    }
    if (g_configuration.CardDetect == SDFirmwareCardDetectPolling) {
        __NodeProperty("broken-cd", NULL, 0);
    }
    if (g_configuration.Timings & SD_FIRMWARE_TIMING_MMC_DDR_3V3) {
        __NodeProperty("mmc-ddr-3_3v", NULL, 0);
    }
    if (g_configuration.Supplies & SD_FIRMWARE_SUPPLY_VQMMC) {
        __NodeCells("vqmmc-supply", 1, 0x24, 0);
    }
    if (g_configuration.Supplies & SD_FIRMWARE_SUPPLY_VMMC) {
        __NodeCells("vmmc-supply", 1, 0x25, 0);
    }

    // An SDIO function is an enabled child with "reg".
    if (g_configuration.Role == SDFirmwareRoleSdio) {
        __NodeU32(sizeof("wifi@1"));
        __NodeU32(DEVICE_FIRMWARE_CHILD_ENABLED | DEVICE_FIRMWARE_CHILD_HAS_REG);
        __NodeBytes("wifi@1", sizeof("wifi@1"));
        g_node.ChildCount++;
    }

    ((struct DeviceFirmwareNodeHeader*)g_node.Data)[0] = (struct DeviceFirmwareNodeHeader) {
        .Version = DEVICE_FIRMWARE_NODE_VERSION,
        .Length = g_node.Length,
        .ParentAddressCells = 1,
        .ParentSizeCells = 1,
        .PropertyCount = g_node.PropertyCount,
        .ChildCount = g_node.ChildCount
    };
}

// The libddk firmware node wrapper reaches deviced through this call.
oserr_t
OSDeviceIOCtl(
    _In_ uuid_t              device,
    _In_ enum OSIOCtlRequest request,
    _In_ void*               buffer,
    _In_ size_t              length)
{
    struct DeviceFirmwareNodeRequest* body = buffer;

    assert(device == DEVICE_ID);
    assert(request == OSIOCTLREQUEST_FIRMWARE_NODE && length == sizeof(*body));
    // Firmware data only: nothing is prepared yet when it is read.
    assert(!g_prepared);
    __Record(EventSdConfiguration, 0, 0, 0);
    if (g_platformFail == PlatformFailConfiguration) {
        return OS_EPROTOCOL;
    }

    __BuildNode();
    assert(body->Offset == 0 && body->Reserved == 0);
    body->Length = g_node.Length;
    body->Count = g_node.Length;
    memcpy(&body->Data[0], &g_node.Data[0], g_node.Length);
    return OS_EOK;
}

oserr_t
DeviceSupplyManage(
    _In_ uuid_t      device,
    _In_ const char* name)
{
    uint32_t supply = __SupplyId(name);

    assert(device == DEVICE_ID);
    // deviced answers OS_EBUSY on a prepared device. The glue must never ask
    // that late.
    assert(!g_prepared);
    __Record(EventSupplyManage, supply, 0, 0);
    if ((supply == SUPPLY_VMMC && g_platformFail == PlatformFailManageVmmc) ||
        (supply == SUPPLY_VQMMC && g_platformFail == PlatformFailManageVqmmc)) {
        return OS_EPERMISSIONS;
    }
    return OS_EOK;
}

oserr_t
DeviceSupplySetState(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ int         on)
{
    uint32_t supply = __SupplyId(name);

    assert(device == DEVICE_ID);
    assert(g_prepared);
    // The glue only ever switches card power.
    assert(supply == SUPPLY_VMMC);
    __Record(EventSupplyState, supply, 0, on ? 1U : 0U);
    if (on && g_platformFail == PlatformFailVmmcOn) {
        return OS_ETIMEOUT;
    }
    if (!on && g_vmmcAlwaysOn) {
        return OS_ENOTSUPPORTED;
    }
    if (!on && g_platformFail == PlatformFailVmmcOff) {
        return OS_ENOTCONNECTED;
    }
    if (!on && g_platformFail == PlatformFailVmmcOffFault) {
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

oserr_t
DeviceSupplySetVoltage(
    _In_ uuid_t      device,
    _In_ const char* name,
    _In_ uint32_t    microvolts)
{
    uint32_t supply = __SupplyId(name);

    assert(device == DEVICE_ID);
    assert(g_prepared);
    assert(supply == SUPPLY_VQMMC);
    __Record(EventSupplyVoltage, supply, 0, microvolts);
    if (g_platformFail == PlatformFailVoltage) {
        return OS_EDEVFAULT;
    }
    return OS_EOK;
}

oserr_t
DeviceSupplyGetVoltage(
    _In_  uuid_t      device,
    _In_  const char* name,
    _Out_ uint32_t*   microvolts)
{
    // The glue never reads a voltage back.
    (void)device;
    (void)name;
    (void)microvolts;
    assert(false);
    return OS_ENOTSUPPORTED;
}

oserr_t
DeviceClockGetRate(
    _In_  uuid_t    device,
    _In_  uint32_t  index,
    _Out_ uint64_t* hz)
{
    assert(device == DEVICE_ID);
    assert(g_prepared);
    __Record(EventClockRate, index, 0, (uint32_t)g_clockRateHz);
    if (g_platformFail == PlatformFailClock) {
        return OS_ETIMEOUT;
    }
    *hz = g_clockRateHz;
    return OS_EOK;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void
__SetCompatibles(
    _In_ const char* list,
    _In_ size_t      length)
{
    memset(g_platformDevice.Compatibles, 0, sizeof(g_platformDevice.Compatibles));
    memcpy(g_platformDevice.Compatibles, list, length);
    g_platformDevice.CompatibleLength = (uint32_t)length;
}

// The Raspberry Pi 4 slot as deviced describes it with the bundled tree.
static void
__ConfigureBoard(
    _In_ uint32_t capabilities)
{
    static const char compatible[] = "brcm,bcm2711-emmc2";

    __Reset();
    __AddBar(0, 0x100, true);
    __SetHost(SDHCI_SPEC_300, capabilities);
    __StoreRegister(SDHCI_CAPABILITIES1, 4, BOARD_CAPABILITIES1);
    g_wordAccessOnly = true;
    g_lineResetBusyReads = 0;

    memset(&g_platformDevice, 0, sizeof(g_platformDevice));
    g_platformDevice.Base.Id = DEVICE_ID;
    g_platformDevice.Base.Length = sizeof(PlatformDevice_t);
    g_platformDevice.Version = PLATFORM_DEVICE_VERSION;
    __SetCompatibles(compatible, sizeof(compatible));
    g_platformDevice.RegisterCount = 1;
    g_platformDevice.Registers[0].Base = 0xFE340000ULL;
    g_platformDevice.Registers[0].Length = 0x100;
    g_platformDevice.InterruptCount = 1;
    g_platformDevice.Interrupts[0].Controller = 1;
    g_platformDevice.Interrupts[0].Number = 158;
    g_platformDevice.Interrupts[0].Type = 4;

    memset(&g_configuration, 0, sizeof(g_configuration));
    g_configuration.Role = SDFirmwareRoleMemory;
    g_configuration.BusWidth = 4;
    g_configuration.MaxFrequency = 0;
    g_configuration.Removable = 1;
    g_configuration.CardDetect = SDFirmwareCardDetectPolling;
    g_configuration.WriteProtect = SDFirmwareWriteProtectHost;
    g_configuration.Voltages = SD_FIRMWARE_SIGNAL_3V3 | SD_FIRMWARE_SIGNAL_1V8;
    g_configuration.Timings = SD_FIRMWARE_TIMING_MMC_DDR_3V3;
    g_configuration.Supplies = SD_FIRMWARE_SUPPLY_VMMC | SD_FIRMWARE_SUPPLY_VQMMC;
    g_configuration.HostRegister = 0;
    g_configuration.ConfigRegister = SD_FIRMWARE_REGISTER_NONE;

    g_platformFail = PlatformFailNone;
    g_clockRateHz = BOARD_BASE_CLOCK_HZ;
    g_vmmcAlwaysOn = false;
    memset(&g_platform, 0, sizeof(g_platform));
    memset(&g_controller, 0, sizeof(g_controller));
}

// SdhciPlatformSetup, then SdhciControllerPrepare with its variant and
// slot, as main.c runs them.
static oserr_t
__SetupPlatform(void)
{
    const SdhciVariant_t* variant = NULL;
    const SdhciSlot_t*    slot = NULL;
    oserr_t               status;

    status = SdhciPlatformSetup(&g_platform, &g_platformDevice.Base, &g_controller, &variant, &slot);
    if (status != OS_EOK) {
        assert(variant == NULL && slot == NULL);
        return status;
    }
    assert(variant == &g_sdhciBcm2711Variant && slot == &g_sdhciPlatformSlot);
    assert(g_controller.SlotContext == &g_platform);
    return SdhciControllerPrepare(&g_controller, &g_platformDevice.Base, variant, slot);
}

static uint32_t
__Count(
    _In_ enum EventKind kind,
    _In_ uint32_t       start)
{
    uint32_t count = 0;
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind) {
            count++;
        }
    }
    return count;
}

static int
__FindSupply(
    _In_ enum EventKind kind,
    _In_ uint32_t       supply,
    _In_ uint32_t       value,
    _In_ uint32_t       start)
{
    uint32_t i;

    for (i = start; i < g_eventCount; i++) {
        if (g_events[i].Kind == kind && g_events[i].Offset == supply &&
            (value == ANY_VALUE || g_events[i].Value == value)) {
            return (int)i;
        }
    }
    return -1;
}

static int
__FindPowerWrite(
    _In_ bool     busOn,
    _In_ uint32_t start)
{
    uint32_t i;

    // The power control byte lives in the word at 0x28, bits 15:8, and this
    // host takes 32-bit writes only.
    for (i = start; i < g_eventCount; i++) {
        uint32_t power;
        if (g_events[i].Kind != EventWrite || g_events[i].Offset != (SDHCI_POWER_CONTROL & ~3U)) {
            continue;
        }
        power = (g_events[i].Value >> ((SDHCI_POWER_CONTROL & 3U) * 8U)) & 0xFFU;
        if (((power & SDHCI_POWER_BUS_ON) != 0) == busOn) {
            return (int)i;
        }
    }
    return -1;
}

static oserr_t
__SetPower(
    _In_ enum SdBusPower power)
{
    SdBusSettings_t settings = { CLOCK_400K, SdBusWidth1, SdBusTimingDefault, power };
    uint32_t        clockHz = 0;

    return SdhciControllerSetBus(&g_controller, &settings, &clockHz);
}

static void
__Destroy(void)
{
    assert(SdhciControllerDestroy(&g_controller) == OS_EOK);
    assert(g_liveBuffers == 0 && g_liveContexts == 0);
    assert(!g_acquired && !g_prepared);
}

// ---------------------------------------------------------------------------
// Cases
// ---------------------------------------------------------------------------

static void
__TestCallOrder(void)
{
    int configuration;
    int manageVmmc;
    int manageVqmmc;
    int prepare;
    int resources;
    int acquire;
    int clock;
    int voltage;
    int vmmcOn;
    int busOn;
    int busOff;
    int vmmcOff;
    int teardownOff;
    int stop;
    uint32_t mark;

    // Configuration, both supplies taken over, DevicePrepare, the register
    // window at the host register position, the input clock while the
    // capabilities are decoded.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    configuration = __Find(EventSdConfiguration, 0, ANY_VALUE, 0);
    manageVmmc = __FindSupply(EventSupplyManage, SUPPLY_VMMC, ANY_VALUE, 0);
    manageVqmmc = __FindSupply(EventSupplyManage, SUPPLY_VQMMC, ANY_VALUE, 0);
    prepare = __Find(EventPrepare, 0, ANY_VALUE, 0);
    resources = __Find(EventResources, 0, ANY_VALUE, 0);
    acquire = __Find(EventAcquire, 0, BAR_ID_BASE, 0);
    clock = __Find(EventClockRate, 0, ANY_VALUE, 0);
    assert(configuration == 0);
    assert(configuration < manageVmmc && manageVmmc < manageVqmmc && manageVqmmc < prepare);
    assert(prepare < resources && resources < acquire && acquire < clock);
    assert(g_events[clock].Offset == SDHCI_PLATFORM_INPUT_CLOCK);
    assert(__Count(EventClockRate, 0) == 1);
    // DevicePrepare does not switch anything: no supply call before card
    // setup asks for power.
    assert(__Count(EventSupplyState, 0) == 0 && __Count(EventSupplyVoltage, 0) == 0);
    assert(strstr(g_log, "SD config: memory slot, 4 lines, max none, removable, cd polling, wp host, "
        "supplies vmmc vqmmc, host reg 0") != NULL);

    // Interrupt setup selects firmware source 0 before the allocation.
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    assert(__Find(EventInterrupt, InterruptCallSelectSource, 0, 0) >= 0);
    {
        int select = -1;
        int allocate = -1;
        uint32_t i;
        for (i = 0; i < g_eventCount; i++) {
            if (g_events[i].Kind == EventInterrupt && g_events[i].Offset == InterruptCallSelectSource) {
                select = (int)i;
                assert(g_events[i].Value == SDHCI_PLATFORM_INTERRUPT_SOURCE);
            }
            if (g_events[i].Kind == EventInterrupt && g_events[i].Offset == InterruptCallAllocate) {
                allocate = (int)i;
            }
        }
        assert(select >= 0 && select < allocate);
    }
    assert(SdhciControllerStart(&g_controller) == OS_EOK);

    // Power on: 3.3 V on vqmmc, then vmmc on, then the power control
    // register (voltage select, then bus power).
    mark = g_eventCount;
    assert(__SetPower(SdBusPowerOn) == OS_EOK);
    voltage = __FindSupply(EventSupplyVoltage, SUPPLY_VQMMC, SDHCI_PLATFORM_SIGNAL_3V3_UV, mark);
    vmmcOn = __FindSupply(EventSupplyState, SUPPLY_VMMC, 1, mark);
    busOn = __FindPowerWrite(true, mark);
    assert(voltage >= 0 && voltage < vmmcOn && vmmcOn < busOn);

    // Power off: bus power cleared first, then vmmc off.
    mark = g_eventCount;
    assert(__SetPower(SdBusPowerOff) == OS_EOK);
    busOff = __FindPowerWrite(false, mark);
    vmmcOff = __FindSupply(EventSupplyState, SUPPLY_VMMC, 0, mark);
    assert(busOff >= 0 && busOff < vmmcOff);
    assert(__Count(EventSupplyVoltage, mark) == 0);

    // Teardown: the host reset, then vmmc off once more, then DeviceStop.
    mark = g_eventCount;
    __Destroy();
    teardownOff = __FindSupply(EventSupplyState, SUPPLY_VMMC, 0, mark);
    stop = __Find(EventStop, 0, ANY_VALUE, mark);
    assert(teardownOff >= 0 && teardownOff < stop);
    assert(__Count(EventSupplyState, mark) == 1);
    printf("Platform call order passed\n");
}

static void
__TestLeftAlone(void)
{
    // An SDIO host: only the configuration is read. No supply, no prepare,
    // no register access.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.Role = SDFirmwareRoleSdio;
    assert(__SetupPlatform() == OS_ENOTSUPPORTED);
    assert(g_eventCount == 1 && g_events[0].Kind == EventSdConfiguration);
    assert(strstr(g_log, "left alone") != NULL);
    __Destroy();
    assert(g_eventCount == 1);

    // An unknown compatible: nothing at all is called.
    __ConfigureBoard(BOARD_CAPABILITIES);
    __SetCompatibles("brcm,bcm2712-sdhci\0brcm,sdhci-brcmstb", sizeof("brcm,bcm2712-sdhci\0brcm,sdhci-brcmstb"));
    assert(SdhciPlatformFindVariant(&g_platformDevice.Base) == NULL);
    assert(__SetupPlatform() == OS_ENOTSUPPORTED);
    assert(g_eventCount == 0);
    assert(strstr(g_log, "no controller variant for platform device \"brcm,bcm2712-sdhci\"") != NULL);

    // A later compatible in the list still matches.
    __ConfigureBoard(BOARD_CAPABILITIES);
    __SetCompatibles("vendor,unknown\0brcm,bcm2711-emmc2", sizeof("vendor,unknown\0brcm,bcm2711-emmc2"));
    assert(SdhciPlatformIsDevice(&g_platformDevice.Base));
    assert(SdhciPlatformFindVariant(&g_platformDevice.Base) == &g_sdhciBcm2711Variant);

    // A bus device is not a platform device.
    assert(!SdhciPlatformIsDevice(&g_device.Base));
    printf("Platform hosts left alone passed\n");
}

static void
__TestSupplies(void)
{
    uint32_t mark;

    // Without vqmmc: only vmmc is taken over, and power on switches vmmc
    // without a voltage selection.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.Supplies = SD_FIRMWARE_SUPPLY_VMMC;
    assert(__SetupPlatform() == OS_EOK);
    assert(__Count(EventSupplyManage, 0) == 1);
    assert(__FindSupply(EventSupplyManage, SUPPLY_VMMC, ANY_VALUE, 0) >= 0);
    assert(strstr(g_log, "supplies vmmc, host reg 0") != NULL);
    mark = g_eventCount;
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOn) == OS_EOK);
    assert(__Count(EventSupplyVoltage, mark) == 0);
    assert(__FindSupply(EventSupplyState, SUPPLY_VMMC, 1, mark) >= 0);
    __Destroy();

    // Without any supply: nothing is taken over or switched.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.Supplies = 0;
    assert(__SetupPlatform() == OS_EOK);
    assert(__Count(EventSupplyManage, 0) == 0);
    assert(strstr(g_log, "supplies none") != NULL);
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOn) == OS_EOK);
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOff) == OS_EOK);
    assert(__Count(EventSupplyState, 0) == 0 && __Count(EventSupplyVoltage, 0) == 0);
    __Destroy();

    // An always-on vmmc: power off is "no power cycle", logged once, and
    // the call succeeds.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_vmmcAlwaysOn = true;
    assert(__SetupPlatform() == OS_EOK);
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOff) == OS_EOK);
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOff) == OS_EOK);
    assert(__Count(EventSupplyState, 0) == 2);
    {
        const char* first = strstr(g_log, "cannot be switched off");
        assert(first != NULL && strstr(first + 1, "cannot be switched off") == NULL);
    }
    __Destroy();
    printf("Platform supplies passed\n");
}

static void
__TestFailures(void)
{
    uint32_t mark;

    // Configuration: nothing else is called.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_platformFail = PlatformFailConfiguration;
    assert(__SetupPlatform() == OS_EPROTOCOL);
    assert(g_eventCount == 1);
    assert(strstr(g_log, "reading the firmware node failed") != NULL);

    // Each supply take-over stops setup before DevicePrepare.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_platformFail = PlatformFailManageVmmc;
    assert(__SetupPlatform() == OS_EPERMISSIONS);
    assert(__Count(EventSupplyManage, 0) == 1 && __Count(EventPrepare, 0) == 0);
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_platformFail = PlatformFailManageVqmmc;
    assert(__SetupPlatform() == OS_EPERMISSIONS);
    assert(__Count(EventSupplyManage, 0) == 2 && __Count(EventPrepare, 0) == 0);
    assert(strstr(g_log, "taking over vqmmc") != NULL);

    // DevicePrepare: teardown releases nothing that was not set up.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_failPoint = FailPrepare;
    assert(__SetupPlatform() == OS_EPERMISSIONS);
    __Destroy();
    assert(__Count(EventStop, 0) == 0);

    // A host register position outside the resources.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.HostRegister = 1;
    assert(__SetupPlatform() == OS_ENOENT);
    assert(__Count(EventAcquire, 0) == 0);
    assert(strstr(g_log, "host register range 1 is not among the 1 device resources") != NULL);
    __Destroy();

    // The input clock fails while the host reports its own base clock: a
    // warning, and the capability value is used.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_platformFail = PlatformFailClock;
    assert(__SetupPlatform() == OS_EOK);
    assert(g_controller.Capabilities.BaseClockHz == BOARD_BASE_CLOCK_HZ);
    assert(strstr(g_log, "no platform input clock") != NULL);
    __Destroy();

    // The input clock fails and the capabilities give none: setup fails.
    __ConfigureBoard(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_platformFail = PlatformFailClock;
    assert(__SetupPlatform() == OS_ETIMEOUT);
    assert(strstr(g_log, "reading the rate of input clock 0 failed") != NULL);
    __Destroy();

    // The voltage selection fails: vmmc stays off and power on fails.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    g_platformFail = PlatformFailVoltage;
    mark = g_eventCount;
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOn) == OS_EDEVFAULT);
    assert(__Count(EventSupplyState, mark) == 0);
    assert(strstr(g_log, "selecting 3300000 uV on vqmmc failed") != NULL);
    __Destroy();

    // vmmc does not come on.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    g_platformFail = PlatformFailVmmcOn;
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOn) == OS_ETIMEOUT);
    assert(strstr(g_log, "switching vmmc on failed") != NULL);
    __Destroy();

    // vmmc does not go off: power off fails with that error. Teardown only
    // logs it, because DeviceStop releases the supply too.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    assert(SdhciInterruptSetup(&g_controller, EVENT_DESCRIPTOR) == OS_EOK);
    assert(SdhciControllerStart(&g_controller) == OS_EOK);
    assert(__SetPower(SdBusPowerOn) == OS_EOK);
    g_platformFail = PlatformFailVmmcOff;
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOff) == OS_ENOTCONNECTED);
    assert(strstr(g_log, "switching vmmc off failed") != NULL);
    mark = g_eventCount;
    __Destroy();
    assert(__Count(EventSupplyState, mark) == 1 && __Count(EventStop, mark) == 1);
    assert(strstr(g_log, "switching the card supply off failed") != NULL);

    // A device fault at power off fails the power cycle. It is not read as
    // an always-on vmmc.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    g_platformFail = PlatformFailVmmcOffFault;
    assert(g_sdhciPlatformSlot.SetCardPower(&g_controller, SdBusPowerOff) == OS_EDEVFAULT);
    assert(strstr(g_log, "switching vmmc off failed") != NULL);
    assert(strstr(g_log, "cannot be switched off") == NULL && !g_platform.PowerCycleLogged);
    g_platformFail = PlatformFailNone;
    __Destroy();
    printf("Platform failures passed\n");
}

static void
__TestBoardValues(void)
{
    // The real capability dump reports an embedded slot. The configuration
    // decides: removable.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_REMOVABLE) != 0);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_WRITE_PROTECT) == 0);
    assert(g_controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4));
    assert(g_controller.Host.Limits.MaximumClockHz == BOARD_BASE_CLOCK_HZ);
    // No working detect line: card state stays unknown, the card layer probes.
    {
        uint32_t state;
        assert(SdhciControllerGetCardState(&g_controller, &state) == OS_ENOTSUPPORTED);
    }
    __Destroy();

    // A non-removable slot (soldered eMMC) on a host whose slot type field
    // says removable. The configuration decides again. The binding does not
    // allow "broken-cd" on such a slot, so the node leaves it out.
    __ConfigureBoard((BOARD_CAPABILITIES & ~SDHCI_CAPS_SLOT_TYPE_MASK) |
        (SDHCI_CAPS_SLOT_REMOVABLE << SDHCI_CAPS_SLOT_TYPE_SHIFT));
    g_configuration.Removable = 0;
    g_configuration.CardDetect = SDFirmwareCardDetectHost;
    assert(__SetupPlatform() == OS_EOK);
    assert((g_controller.Host.Limits.Flags & SD_HOST_FLAG_REMOVABLE) == 0);
    assert(strstr(g_log, "non-removable") != NULL);
    __Destroy();

    // Eight wired lines, and a maximum frequency below the base clock.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.BusWidth = 8;
    g_configuration.MaxFrequency = CLOCK_50M;
    assert(__SetupPlatform() == OS_EOK);
    assert(g_controller.Host.Limits.BusWidths == (SD_HOST_BUS_WIDTH_1 | SD_HOST_BUS_WIDTH_4 | SD_HOST_BUS_WIDTH_8));
    assert(g_controller.Host.Limits.MaximumClockHz == CLOCK_50M);
    assert(strstr(g_log, "8 lines, max 50000000 Hz") != NULL);
    __Destroy();

    // One wired line, and a maximum frequency above the base clock changes
    // nothing.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.BusWidth = 1;
    g_configuration.MaxFrequency = CLOCK_200M;
    assert(__SetupPlatform() == OS_EOK);
    assert(g_controller.Host.Limits.BusWidths == SD_HOST_BUS_WIDTH_1);
    assert(g_controller.Host.Limits.MaximumClockHz == BOARD_BASE_CLOCK_HZ);
    __Destroy();

    // A width the binding does not allow is refused by the decoder, before
    // anything is changed.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_configuration.BusWidth = 2;
    assert(__SetupPlatform() == OS_EINVALPARAMS);
    assert(strstr(g_log, "bus-width 2 is not 1, 4 or 8") != NULL);
    assert(strstr(g_log, "decoding the SD configuration failed") != NULL);
    assert(__Count(EventPrepare, 0) == 0 && __Count(EventSupplyManage, 0) == 0);
    printf("Platform board values passed\n");
}

static void
__TestBaseClock(void)
{
    // Same rate in both places: one line with both values, no warning.
    __ConfigureBoard(BOARD_CAPABILITIES);
    assert(__SetupPlatform() == OS_EOK);
    assert(strstr(g_log, "base clock 100000000 Hz in the capabilities, platform input clock 100000000 Hz") != NULL);
    assert(strstr(g_log, "differ") == NULL);
    assert(g_controller.Capabilities.BaseClockHz == BOARD_BASE_CLOCK_HZ);
    __Destroy();

    // The firmware reports another rate: a warning, and the capability
    // value stays in use. Setup does not fail.
    __ConfigureBoard(BOARD_CAPABILITIES);
    g_clockRateHz = CLOCK_200M;
    assert(__SetupPlatform() == OS_EOK);
    assert(strstr(g_log, "capability base clock 100000000 Hz and platform input clock 200000000 Hz differ") != NULL);
    assert(g_controller.Capabilities.BaseClockHz == BOARD_BASE_CLOCK_HZ);
    __Destroy();

    // No base clock in the capabilities: the firmware rate is used, and the
    // firmware is asked only once.
    __ConfigureBoard(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_clockRateHz = CLOCK_50M;
    assert(__SetupPlatform() == OS_EOK);
    assert(g_controller.Capabilities.BaseClockHz == CLOCK_50M);
    assert(__Count(EventClockRate, 0) == 1);
    assert(strstr(g_log, "differ") == NULL);
    __Destroy();

    // A rate above 32 bits is refused when the engine needs it.
    __ConfigureBoard(BOARD_CAPABILITIES & ~SDHCI_CAPS_BASE_CLOCK_MASK_V3);
    g_clockRateHz = 0x100000000ULL;
    assert(__SetupPlatform() == OS_ENOTSUPPORTED);
    __Destroy();
    printf("Platform base clock passed\n");
}

static void
__TestRejectedHost(void)
{
    uint32_t mark;

    // QEMU's EMMC2 model has no ADMA2. Setup stops after the capabilities,
    // and teardown sends no supply request, because card power was never
    // asked for. The clock was read once, at the capabilities.
    __ConfigureBoard(BOARD_CAPABILITIES & ~SDHCI_CAPS_ADMA2);
    assert(__SetupPlatform() == OS_ENOTSUPPORTED);
    assert(strstr(g_log, "host has no ADMA2") != NULL);
    mark = g_eventCount;
    __Destroy();
    assert(__Count(EventSupplyState, 0) == 0 && __Count(EventSupplyVoltage, 0) == 0);
    assert(__Count(EventStop, mark) == 1 && __Count(EventRelease, mark) == 1);
    printf("Platform rejected host passed\n");
}

int
main(void)
{
    __TestCallOrder();
    __TestLeftAlone();
    __TestSupplies();
    __TestFailures();
    __TestBoardValues();
    __TestBaseClock();
    __TestRejectedHost();
    printf("SDHCI platform tests passed\n");
    return 0;
}
