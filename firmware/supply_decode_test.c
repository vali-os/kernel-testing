/*
 * Regulator and SD host decoders against the bundled Raspberry Pi trees and
 * a synthetic fixture (supply_fixtures.py).
 *
 * The SD decoder is the SDHCI module's binding decoder
 * (modules/storage/sd/firmware.c). It reads each host node the way the driver
 * does: deviced's packed node description (firmware/tree/node.c), read in chunks
 * through the libddk wrapper (librt/libddk/devices/firmware-node.c), whose request is
 * answered here straight from the tree.
 *
 * bundled <dtb>: prints what every enabled regulator and SD host decodes to,
 * one line each, and the controller nodes behind every mailbox, for the
 * runner to compare with the expected files.
 * synthetic <dtb>: checks every fixture node by name, the plan rule for GPIO
 * lines a driver only reads (cd-gpios and wp-gpios), and quiet decoding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../services/deviced/firmware/tree/decode-log.c"
#include "../../services/deviced/firmware/tree/reader.c"
#include "../../services/deviced/firmware/tree/tree.c"
#include "../../services/deviced/firmware/tree/resources.c"
#include "../../services/deviced/firmware/tree/shared-tree.c"
#include "../../services/deviced/firmware/bindings/ethernet.c"
#include "../../services/deviced/firmware/bindings/board-dma.c"
#ifndef DEPENDENCIES_SOURCE
#define DEPENDENCIES_SOURCE "../../services/deviced/firmware/bindings/dependencies.c"
#endif
#include DEPENDENCIES_SOURCE
#include "../../services/deviced/firmware/bindings/controllers.c"
#include "../../services/deviced/firmware/bindings/gpio.c"
#include "../../services/deviced/firmware/tree/node.c"
#include "../../librt/libddk/devices/firmware-node.c"
#ifndef BINDING_SOURCE
#define BINDING_SOURCE "../../modules/storage/sd/firmware.c"
#endif
#include BINDING_SOURCE
#ifndef REGULATORS_SOURCE
#define REGULATORS_SOURCE "../../services/deviced/firmware/bindings/regulators.c"
#endif
#include REGULATORS_SOURCE

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned int    g_logLines;
static struct FdtTree* g_tree;

// The libddk wrapper reaches deviced through this call. Answer it from the
// tree, with the device ID standing for the node offset.
oserr_t
OSDeviceIOCtl(
    _In_ uuid_t              device,
    _In_ enum OSIOCtlRequest request,
    _In_ void*               buffer,
    _In_ size_t              length)
{
    struct DeviceFirmwareNodeRequest* body = buffer;

    CHECK(request == OSIOCTLREQUEST_FIRMWARE_NODE && length == sizeof(*body) && body->Reserved == 0);
    return FdtNodeDescriptionRead(g_tree, device, body->Offset, &body->Data[0], sizeof(body->Data),
                                  &body->Length, &body->Count);
}

// Read a node as the SDHCI driver does and decode its SD slot description.
static oserr_t
__DecodeSd(struct FdtTree* tree, uint32_t node, SDFirmwareConfiguration_t* sd)
{
    DeviceFirmwareNode_t description;
    oserr_t              status;

    g_tree = tree;
    status = DeviceFirmwareNodeRead(node, &description);
    if (status != OS_EOK) {
        return status;
    }
    status = SDFirmwareDecode(&description, sd);
    DeviceFirmwareNodeDestroy(&description);
    return status;
}

// Which bundled nodes get an "sd" line. Only this test still selects SD hosts
// by name, so the expected files stay comparable: "mmc", "mmcnr", "sdhci",
// "emmc" and "emmc2" before the "@".
static int
__IsSdHostName(const char* name)
{
    static const char* const names[] = { "mmc", "mmcnr", "sdhci", "emmc", "emmc2" };
    const char*              at = strchr(name, '@');
    size_t                   length = at != NULL ? (size_t)(at - name) : strlen(name);

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (strlen(names[i]) == length && !strncmp(name, names[i], length)) {
            return 1;
        }
    }
    return 0;
}

// The controller keys of modules/platform/rpi-firmware/rpi-firmware.yaml.
// supply-test.sh checks that the manifest still declares the same values.
static const char* const g_firmwareCompatibles[] = {
    "raspberrypi,firmware-gpio",
    "raspberrypi,firmware-clocks"
};
static const struct FdtClockRouteRule g_firmwareRoutes[] = {
    { "brcm,bcm2711-cprman", 51, 12 }
};
static const struct FdtControllerRules g_firmwareRules = {
    .Compatibles = g_firmwareCompatibles,
    .CompatibleCount = sizeof(g_firmwareCompatibles) / sizeof(g_firmwareCompatibles[0]),
    .Routes = g_firmwareRoutes,
    .RouteCount = sizeof(g_firmwareRoutes) / sizeof(g_firmwareRoutes[0])
};

static const char*
__PhandleName(struct FdtTree* tree, uint32_t phandle)
{
    struct FdtResources resources;

    return FdtFindResources(tree, phandle, &resources) == OS_EOK ? resources.Name : "?";
}

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    (void)level;
    (void)format;
    g_logLines++;
}

struct __Search {
    const char* Name;
    uint32_t    Node;
};

static void
__FindNamed(const struct FdtResources* nodes, int depth, void* context)
{
    struct __Search* search = context;

    if (!strcmp(nodes[depth].Name, search->Name)) {
        search->Node = nodes[depth].NodeOffset;
    }
}

static uint32_t
__Node(struct FdtTree* tree, const char* name)
{
    struct __Search search = { .Name = name, .Node = UINT32_MAX };

    CHECK(FdtWalkResources(tree, __FindNamed, &search) == OS_EOK);
    if (search.Node == UINT32_MAX) {
        printf("missing node %s\n", name);
    }
    CHECK(search.Node != UINT32_MAX);
    return search.Node;
}

static const char*
__NodeName(struct FdtTree* tree, uint32_t node)
{
    const struct FdtResources* resource = FdtTreeNode(tree, node);

    return resource != NULL ? resource->Name : "?";
}

static void
__PrintLine(struct FdtTree* tree, const char* label, const struct FdtRegulatorLine* line)
{
    if (!line->Present) {
        printf(" %s=none", label);
        return;
    }
    printf(" %s=%s:%u/%s", label, __NodeName(tree, line->Line.Controller), line->Line.Line,
           line->ActiveLow ? "low" : "high");
}

static void
__PrintBundled(const struct FdtResources* nodes, int depth, void* context)
{
    struct FdtTree*                tree = context;
    const struct FdtResources*     node = &nodes[depth];
    struct FdtRegulator            regulator;
    SDFirmwareConfiguration_t       sd;
    struct FdtPlatformDependencies plan;
    oserr_t                        status;

    if (node->Disabled || node->ParentDisabled) {
        return;
    }

    // What the firmware driver's DeviceControllerEnable would find behind
    // each mailbox, with the controller keys of its manifest.
    if (FdtCompatible(&node->View, "brcm,bcm2835-mbox")) {
        struct FdtControllerNodes nodes;

        printf("controller %s:", node->Name);
        status = FdtFindControllerNodes(tree, node->NodeOffset, DEVICE_CONTROLLER_GPIO, &g_firmwareRules, &nodes);
        if (status == OS_EOK) {
            printf(" gpio=%s", __NodeName(tree, nodes.GpioNode));
        } else {
            printf(" gpio=none(%u)", status);
        }
        status = FdtFindControllerNodes(tree, node->NodeOffset, DEVICE_CONTROLLER_CLOCK, &g_firmwareRules, &nodes);
        if (status != OS_EOK) {
            printf(" clock=none(%u)\n", status);
            return;
        }
        printf(" clock=%s", nodes.ClockNode == FDT_CONTROLLER_NONE ? "none" : __NodeName(tree, nodes.ClockNode));
        for (uint32_t i = 0; i < nodes.RouteCount; i++) {
            printf(" route=%s:%u->%u", __NodeName(tree, nodes.Routes[i].Node), nodes.Routes[i].SourceClock,
                   nodes.Routes[i].ControllerClock);
        }
        printf("\n");
        return;
    }

    if (FdtRegulatorIsKnown(node)) {
        status = FdtRegulatorDescribe(tree, node->NodeOffset, &regulator);
        if (status != OS_EOK) {
            printf("regulator %s: rejected (%u)\n", node->Name, status);
            return;
        }
        printf("regulator %s: %s %u-%u always=%d boot=%d", node->Name,
               regulator.Type == FdtRegulatorFixed ? "fixed" : "gpio",
               regulator.MinimumMicrovolts, regulator.MaximumMicrovolts, regulator.AlwaysOn, regulator.BootOn);
        __PrintLine(tree, "enable", &regulator.Enable);
        printf(" startup=%u offon=%u settling=%u", regulator.StartupDelayUs, regulator.OffOnDelayUs,
               regulator.SettlingTimeUs);
        if (regulator.Type == FdtRegulatorGpio) {
            __PrintLine(tree, "selector", &regulator.Selector);
            printf(" states=");
            for (uint32_t i = 0; i < regulator.StateCount; i++) {
                printf("%s%u:%u", i ? "," : "", regulator.States[i].Microvolts, regulator.States[i].Value);
            }
        }
        printf("\n");
        return;
    }

    if (node->Compatible == NULL || !__IsSdHostName(node->Name)) {
        return;
    }

    status = __DecodeSd(tree, node->NodeOffset, &sd);
    if (status != OS_EOK) {
        printf("sd %s: rejected (%u)\n", node->Name, status);
        return;
    }
    printf("sd %s: role=%s width=%u max=%u removable=%u", node->Name,
           sd.Role == SDFirmwareRoleMemory ? "memory" : "sdio", sd.BusWidth, sd.MaxFrequency, sd.Removable);
    printf(" cd=%s", sd.CardDetect == SDFirmwareCardDetectHost ? "host" :
                     sd.CardDetect == SDFirmwareCardDetectPolling ? "polling" : "gpio");
    if (sd.CardDetect == SDFirmwareCardDetectGpio) {
        printf("(%s:%u/%s)", __PhandleName(tree, sd.CardDetectLine.Controller), sd.CardDetectLine.Line,
               sd.CardDetectLine.ActiveLow ? "low" : "high");
    }
    printf(" cd-inverted=%u wp=%s wp-inverted=%u", sd.CardDetectInverted,
           sd.WriteProtect == SDFirmwareWriteProtectHost ? "host" :
           sd.WriteProtect == SDFirmwareWriteProtectNone ? "none" : "gpio", sd.WriteProtectInverted);
    printf(" signal=%s timings=0x%x supplies=%s%s host=%u cfg=", (sd.Voltages & SD_FIRMWARE_SIGNAL_1V8) ? "3v3+1v8" : "3v3",
           sd.Timings, (sd.Supplies & SD_FIRMWARE_SUPPLY_VMMC) ? "vmmc" : "-",
           (sd.Supplies & SD_FIRMWARE_SUPPLY_VQMMC) ? "+vqmmc" : "", sd.HostRegister);
    if (sd.ConfigRegister == SD_FIRMWARE_REGISTER_NONE) {
        printf("none");
    } else {
        printf("%u", sd.ConfigRegister);
    }

    status = FdtPlatformDependencies(tree, node->NodeOffset, &plan);
    if (status == OS_EOK) {
        printf(" plan=%u\n", plan.Count);
    } else {
        printf(" plan=rejected(%u)\n", status);
    }
}

static void
__Regulators(struct FdtTree* tree)
{
    static const struct {
        const char* Name;
        oserr_t     Status;
    } expected[] = {
        { "r-ok-fixed", OS_EOK }, { "r-fixed-novolt", OS_EOK }, { "r-fixed-onevolt", OS_EINVALPARAMS },
        { "r-fixed-diff", OS_EINVALPARAMS }, { "r-fixed-both-gpio", OS_EINVALPARAMS },
        { "r-fixed-two-lines", OS_ENOTSUPPORTED }, { "r-fixed-vin", OS_ENOTSUPPORTED },
        { "r-fixed-pinctrl", OS_ENOTSUPPORTED }, { "r-fixed-badbool", OS_EINVALPARAMS },
        { "r-fixed-baddelay", OS_EINVALPARAMS }, { "r-fixed-3cells", OS_ENOTSUPPORTED },
        { "r-fixed-flagged", OS_EOK }, { "r-disabled", OS_ENOENT }, { "r-other", OS_ENOENT },
        { "r-ok-gpio", OS_EOK }, { "r-gpio-noline", OS_EINVALPARAMS }, { "r-gpio-twolines", OS_ENOTSUPPORTED },
        { "r-gpio-nostates", OS_EINVALPARAMS }, { "r-gpio-badstates", OS_EINVALPARAMS },
        { "r-gpio-value2", OS_EINVALPARAMS }, { "r-gpio-dupvolt", OS_EINVALPARAMS },
        { "r-gpio-dupvalue", OS_EINVALPARAMS }, { "r-gpio-outside", OS_EINVALPARAMS },
        { "r-gpio-offon", OS_ENOTSUPPORTED }, { "r-gpio-badhint", OS_EINVALPARAMS },
        { "r-gpio-toomany", OS_ENOTSUPPORTED }, { "r-gpio-enable-low", OS_EOK }
    };
    struct FdtRegulator regulator;
    struct FdtRegulator saved;
    oserr_t             status;

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        memset(&regulator, 0xa5, sizeof(regulator));
        saved = regulator;
        status = FdtRegulatorDescribe(tree, __Node(tree, expected[i].Name), &regulator);
        if (status != expected[i].Status) {
            printf("regulator %s: got %u, expected %u\n", expected[i].Name, status, expected[i].Status);
        }
        CHECK(status == expected[i].Status);
        if (status != OS_EOK) {
            CHECK(!memcmp(&regulator, &saved, sizeof(regulator)));
        }
    }

    // Every property of a fixed regulator.
    CHECK(FdtRegulatorDescribe(tree, __Node(tree, "r-ok-fixed"), &regulator) == OS_EOK);
    CHECK(regulator.Type == FdtRegulatorFixed && regulator.MinimumMicrovolts == 3300000);
    CHECK(regulator.MaximumMicrovolts == 3300000 && regulator.AlwaysOn && regulator.BootOn);
    CHECK(regulator.Enable.Present && regulator.Enable.Line.Line == 3 && regulator.Enable.ActiveLow == 0);
    CHECK(regulator.StartupDelayUs == 100 && regulator.OffOnDelayUs == 200 && !regulator.Selector.Present);

    // Without a line or voltages, a fixed regulator is always on with no voltage.
    CHECK(FdtRegulatorDescribe(tree, __Node(tree, "r-fixed-novolt"), &regulator) == OS_EOK);
    CHECK(!regulator.Enable.Present && regulator.MinimumMicrovolts == 0 && regulator.MaximumMicrovolts == 0);

    // The flags cell says active low. enable-active-high wins, as in Linux.
    CHECK(FdtRegulatorDescribe(tree, __Node(tree, "r-fixed-flagged"), &regulator) == OS_EOK);
    CHECK(regulator.Enable.Line.Flags == 1 && regulator.Enable.ActiveLow == 0);

    // Every property of a GPIO regulator. The selector keeps its own flags,
    // and enable-gpios without enable-active-high is active low.
    CHECK(FdtRegulatorDescribe(tree, __Node(tree, "r-ok-gpio"), &regulator) == OS_EOK);
    CHECK(regulator.Type == FdtRegulatorGpio && regulator.StateCount == 2);
    CHECK(regulator.States[0].Microvolts == 1800000 && regulator.States[0].Value == 1);
    CHECK(regulator.States[1].Microvolts == 3300000 && regulator.States[1].Value == 0);
    CHECK(regulator.Selector.Line.Line == 5 && regulator.Selector.ActiveLow == 1);
    CHECK(regulator.Enable.Line.Line == 6 && regulator.Enable.ActiveLow == 1);
    CHECK(regulator.SettlingTimeUs == 5000 && regulator.StartupDelayUs == 10);
    CHECK(regulator.MinimumMicrovolts == 1800000 && regulator.MaximumMicrovolts == 3300000);
    CHECK(FdtRegulatorDescribe(tree, __Node(tree, "r-gpio-enable-low"), &regulator) == OS_EOK);
    CHECK(regulator.Enable.ActiveLow == 1 && regulator.Selector.ActiveLow == 0);
}

static void
__SdHosts(struct FdtTree* tree)
{
    // The driver cannot see a GPIO controller's cell count, so a line on a
    // three-cell controller (mmc@1300) is refused as malformed. deviced's
    // plan refuses the same node first, with OS_ENOTSUPPORTED (see __Plans).
    static const struct {
        const char* Name;
        oserr_t     Status;
    } expected[] = {
        { "mmc@100", OS_EOK }, { "mmc@200", OS_EOK }, { "mmc@300", OS_EOK }, { "mmc@400", OS_EOK },
        { "mmc@500", OS_EINVALPARAMS }, { "mmc@600", OS_EINVALPARAMS }, { "mmc@700", OS_EINVALPARAMS },
        { "mmc@800", OS_EINVALPARAMS }, { "mmc@900", OS_EINVALPARAMS }, { "mmc@a00", OS_EINVALPARAMS },
        { "mmc@b00", OS_EINVALPARAMS }, { "mmc@c00", OS_EINVALPARAMS }, { "mmc@d00", OS_EINVALPARAMS },
        { "mmc@e00", OS_ENOTSUPPORTED }, { "mmc@f00", OS_EINVALPARAMS }, { "mmc@1000", OS_EINVALPARAMS },
        { "mmc@1200", OS_EINVALPARAMS }, { "mmc@1300", OS_EINVALPARAMS }, { "mmc@1400", OS_EOK },
        { "mmc@1500", OS_EINVALPARAMS }, { "mmc@1600", OS_EINVALPARAMS }, { "mmc@1700", OS_EOK },
        { "mmc@1800", OS_EOK }, { "serial@1100", OS_EOK }
    };
    SDFirmwareConfiguration_t sd;
    SDFirmwareConfiguration_t saved;
    oserr_t                  status;

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        memset(&sd, 0xa5, sizeof(sd));
        saved = sd;
        status = __DecodeSd(tree, __Node(tree, expected[i].Name), &sd);
        if (status != expected[i].Status) {
            printf("sd %s: got %u, expected %u\n", expected[i].Name, status, expected[i].Status);
        }
        CHECK(status == expected[i].Status);
        if (status != OS_EOK) {
            CHECK(!memcmp(&sd, &saved, sizeof(sd)));
        }
    }

    // A memory slot without bus-width has four lines and default policies.
    CHECK(__DecodeSd(tree, __Node(tree, "mmc@100"), &sd) == OS_EOK);
    CHECK(sd.Role == SDFirmwareRoleMemory && sd.BusWidth == 4);
    CHECK(sd.MaxFrequency == 0 && sd.Removable == 1 && sd.CardDetect == SDFirmwareCardDetectHost);
    CHECK(sd.WriteProtect == SDFirmwareWriteProtectHost && sd.Voltages == (SD_FIRMWARE_SIGNAL_3V3 | SD_FIRMWARE_SIGNAL_1V8));
    CHECK(sd.Timings == 0 && sd.Supplies == 0 && sd.HostRegister == 0 && sd.ConfigRegister == SD_FIRMWARE_REGISTER_NONE);
    CHECK(sd.CardDetectLine.Controller == 0 && sd.WriteProtectLine.Controller == 0);

    // Every flag and value at once.
    CHECK(__DecodeSd(tree, __Node(tree, "mmc@200"), &sd) == OS_EOK);
    CHECK(sd.BusWidth == 8 && sd.MaxFrequency == 50000000 && sd.Removable == 0);
    CHECK(sd.WriteProtect == SDFirmwareWriteProtectNone && sd.Voltages == SD_FIRMWARE_SIGNAL_3V3);
    CHECK(sd.Timings == SD_FIRMWARE_TIMING_KNOWN);
    CHECK(sd.Supplies == (SD_FIRMWARE_SUPPLY_VMMC | SD_FIRMWARE_SUPPLY_VQMMC));
    CHECK(sd.HostRegister == 0 && sd.ConfigRegister == 1);

    // GPIO policies with their polarity inputs, and reg-names in either order.
    // The controller is reported by its firmware handle.
    CHECK(__DecodeSd(tree, __Node(tree, "mmc@300"), &sd) == OS_EOK);
    CHECK(sd.CardDetect == SDFirmwareCardDetectGpio && sd.CardDetectLine.Line == 7);
    CHECK(sd.CardDetectLine.ActiveLow == 1 && sd.CardDetectInverted == 1);
    CHECK(!strcmp(__PhandleName(tree, sd.CardDetectLine.Controller), "gpio"));
    CHECK(sd.WriteProtect == SDFirmwareWriteProtectGpio && sd.WriteProtectLine.Line == 8);
    CHECK(sd.WriteProtectLine.ActiveLow == 0 && sd.WriteProtectInverted == 1);
    CHECK(sd.HostRegister == 1 && sd.ConfigRegister == 0);

    // A described function below the host makes it an SDIO host with one
    // line. A disabled one does not.
    CHECK(__DecodeSd(tree, __Node(tree, "mmc@400"), &sd) == OS_EOK);
    CHECK(sd.Role == SDFirmwareRoleSdio && sd.BusWidth == 1 && sd.Removable == 0);
    CHECK(__DecodeSd(tree, __Node(tree, "mmc@1800"), &sd) == OS_EOK);
    CHECK(sd.Role == SDFirmwareRoleMemory && sd.BusWidth == 4);

    CHECK(__DecodeSd(tree, __Node(tree, "mmc@1400"), &sd) == OS_EOK);
    CHECK(sd.CardDetect == SDFirmwareCardDetectPolling && sd.Timings == SD_FIRMWARE_TIMING_MMC_DDR_3V3);
    CHECK(sd.Supplies == SD_FIRMWARE_SUPPLY_VMMC);

    // The decoder does not care how a node is named.
    CHECK(__DecodeSd(tree, __Node(tree, "serial@1100"), &sd) == OS_EOK);
    CHECK(sd.CardDetect == SDFirmwareCardDetectGpio && sd.CardDetectLine.Line == 7);
}

static void
__Plans(struct FdtTree* tree)
{
    struct FdtPlatformDependencies plan;
    unsigned int                   lines;

    // The plan accepts cd-gpios and wp-gpios on any node whose references
    // decode, and acquires nothing for them. It does not decode the SD
    // binding, so contradictions and unsupported flags are left to the driver.
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@300"), &plan) == OS_EOK && plan.Count == 0);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1400"), &plan) == OS_EOK && plan.Count == 1);
    CHECK(plan.Entries[0].Kind == FdtPlatformSupply && !strcmp(plan.Entries[0].Property, "vmmc-supply"));
    CHECK(plan.Entries[0].Consumer == __Node(tree, "mmc@1400"));
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "serial@1100"), &plan) == OS_EOK && plan.Count == 0);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@600"), &plan) == OS_EOK && plan.Count == 0);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@e00"), &plan) == OS_EOK && plan.Count == 0);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@d00"), &plan) == OS_EOK && plan.Count == 0);

    // A reference that does not decode is refused: an unsupported controller,
    // a truncated entry, and an empty property.
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1300"), &plan) == OS_ENOTSUPPORTED);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1500"), &plan) == OS_EINVALPARAMS);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1600"), &plan) == OS_EINVALPARAMS);

    // Every other GPIO line property still needs a driver that sets it up.
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1700"), &plan) == OS_ENOTSUPPORTED);

    // Quiet decoding logs nothing for the same failures, and nests.
    lines = g_logLines;
    FdtDecodeQuietBegin();
    FdtDecodeQuietBegin();
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1500"), &plan) == OS_EINVALPARAMS);
    FdtDecodeQuietEnd();
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1700"), &plan) == OS_ENOTSUPPORTED);
    CHECK(g_logLines == lines);
    FdtDecodeQuietEnd();
    CHECK(!FdtDecodeIsQuiet());
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1500"), &plan) == OS_EINVALPARAMS);
    CHECK(g_logLines > lines);

    // An unmatched end is logged and leaves logging on.
    lines = g_logLines;
    FdtDecodeQuietEnd();
    CHECK(g_logLines == lines + 1 && !FdtDecodeIsQuiet());
}

int
main(int argc, char** argv)
{
    struct FdtTree* tree;
    FILE*           file;
    long            length;
    void*           blob;

    CHECK(argc == 3);
    file = fopen(argv[2], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    blob = malloc((size_t)length);
    CHECK(blob != NULL && fread(blob, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    CHECK(FdtTreeCreate(blob, (size_t)length, &tree) == OS_EOK);

    if (!strcmp(argv[1], "bundled")) {
        CHECK(FdtWalkResources(tree, __PrintBundled, tree) == OS_EOK);
    } else {
        CHECK(!strcmp(argv[1], "synthetic"));
        __Regulators(tree);
        __SdHosts(tree);
        __Plans(tree);
        printf("Regulator and SD decoding: synthetic passed\n");
    }

    FdtTreeRelease(tree);
    free(blob);
    return 0;
}
