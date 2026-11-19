/*
 * Regulator and SD host decoders against the bundled Raspberry Pi trees and
 * a synthetic fixture (supply_fixtures.py).
 *
 * bundled <dtb>: prints what every enabled regulator and SD host decodes to,
 * one line each, for the runner to compare with the expected files.
 * synthetic <dtb>: checks every fixture node by name, the plan carve-out for
 * cd-gpios and wp-gpios, and quiet decoding.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "../../services/deviced/firmware/fdt.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../services/deviced/firmware/mapping.c"
#include "../../services/deviced/firmware/ethernet.c"
#include "../../services/deviced/firmware/board-dma.c"
#include "../../services/deviced/firmware/platform-runtime.c"
#include "../../services/deviced/firmware/board-clocks.c"
#include "../../services/deviced/firmware/controllers.c"
#ifndef SD_SOURCE
#define SD_SOURCE "../../services/deviced/firmware/sd.c"
#endif
#include SD_SOURCE
#ifndef REGULATORS_SOURCE
#define REGULATORS_SOURCE "../../services/deviced/firmware/regulators.c"
#endif
#include REGULATORS_SOURCE

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static unsigned int g_logLines;

void
SystemDebug(
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
    struct DeviceSdConfiguration   sd;
    struct FdtPlatformDependencies plan;
    oserr_t                        status;

    if (node->Disabled || node->ParentDisabled) {
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

    if (node->Compatible == NULL || !FdtSdIsHost(node)) {
        return;
    }

    status = FdtSdConfiguration(tree, node->NodeOffset, &sd);
    if (status != OS_EOK) {
        printf("sd %s: rejected (%u)\n", node->Name, status);
        return;
    }
    printf("sd %s: role=%s width=%u max=%u removable=%u", node->Name,
           sd.Role == DeviceSdRoleMemory ? "memory" : "sdio", sd.BusWidth, sd.MaxFrequency, sd.Removable);
    printf(" cd=%s", sd.CardDetect == DeviceSdCardDetectHost ? "host" :
                     sd.CardDetect == DeviceSdCardDetectPolling ? "polling" : "gpio");
    if (sd.CardDetect == DeviceSdCardDetectGpio) {
        printf("(%s:%u/%s)", __NodeName(tree, sd.CardDetectLine.Controller), sd.CardDetectLine.Line,
               sd.CardDetectLine.ActiveLow ? "low" : "high");
    }
    printf(" cd-inverted=%u wp=%s wp-inverted=%u", sd.CardDetectInverted,
           sd.WriteProtect == DeviceSdWriteProtectHost ? "host" :
           sd.WriteProtect == DeviceSdWriteProtectNone ? "none" : "gpio", sd.WriteProtectInverted);
    printf(" signal=%s timings=0x%x supplies=%s%s host=%u cfg=", (sd.Voltages & DEVICE_SD_SIGNAL_1V8) ? "3v3+1v8" : "3v3",
           sd.Timings, (sd.Supplies & DEVICE_SD_SUPPLY_VMMC) ? "vmmc" : "-",
           (sd.Supplies & DEVICE_SD_SUPPLY_VQMMC) ? "+vqmmc" : "", sd.HostRegister);
    if (sd.ConfigRegister == DEVICE_SD_REGISTER_NONE) {
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
    static const struct {
        const char* Name;
        oserr_t     Status;
    } expected[] = {
        { "mmc@100", OS_EOK }, { "mmc@200", OS_EOK }, { "mmc@300", OS_EOK }, { "mmc@400", OS_EOK },
        { "mmc@500", OS_EINVALPARAMS }, { "mmc@600", OS_EINVALPARAMS }, { "mmc@700", OS_EINVALPARAMS },
        { "mmc@800", OS_EINVALPARAMS }, { "mmc@900", OS_EINVALPARAMS }, { "mmc@a00", OS_EINVALPARAMS },
        { "mmc@b00", OS_EINVALPARAMS }, { "mmc@c00", OS_EINVALPARAMS }, { "mmc@d00", OS_EINVALPARAMS },
        { "mmc@e00", OS_ENOTSUPPORTED }, { "mmc@f00", OS_EINVALPARAMS }, { "mmc@1000", OS_EINVALPARAMS },
        { "mmc@1200", OS_EINVALPARAMS }, { "mmc@1300", OS_ENOTSUPPORTED }, { "mmc@1400", OS_EOK },
        { "serial@1100", OS_ENOTSUPPORTED }
    };
    struct DeviceSdConfiguration sd;
    struct DeviceSdConfiguration saved;
    oserr_t                      status;

    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        memset(&sd, 0xa5, sizeof(sd));
        saved = sd;
        status = FdtSdConfiguration(tree, __Node(tree, expected[i].Name), &sd);
        if (status != expected[i].Status) {
            printf("sd %s: got %u, expected %u\n", expected[i].Name, status, expected[i].Status);
        }
        CHECK(status == expected[i].Status);
        if (status != OS_EOK) {
            CHECK(!memcmp(&sd, &saved, sizeof(sd)));
        }
    }

    // A memory slot without bus-width has four lines and default policies.
    CHECK(FdtSdConfiguration(tree, __Node(tree, "mmc@100"), &sd) == OS_EOK);
    CHECK(sd.Version == DEVICE_SD_CONFIGURATION_VERSION && sd.Role == DeviceSdRoleMemory && sd.BusWidth == 4);
    CHECK(sd.MaxFrequency == 0 && sd.Removable == 1 && sd.CardDetect == DeviceSdCardDetectHost);
    CHECK(sd.WriteProtect == DeviceSdWriteProtectHost && sd.Voltages == (DEVICE_SD_SIGNAL_3V3 | DEVICE_SD_SIGNAL_1V8));
    CHECK(sd.Timings == 0 && sd.Supplies == 0 && sd.HostRegister == 0 && sd.ConfigRegister == DEVICE_SD_REGISTER_NONE);
    CHECK(sd.CardDetectLine.Controller == 0 && sd.WriteProtectLine.Controller == 0);

    // Every flag and value at once.
    CHECK(FdtSdConfiguration(tree, __Node(tree, "mmc@200"), &sd) == OS_EOK);
    CHECK(sd.BusWidth == 8 && sd.MaxFrequency == 50000000 && sd.Removable == 0);
    CHECK(sd.WriteProtect == DeviceSdWriteProtectNone && sd.Voltages == DEVICE_SD_SIGNAL_3V3);
    CHECK(sd.Timings == DEVICE_SD_TIMING_KNOWN);
    CHECK(sd.Supplies == (DEVICE_SD_SUPPLY_VMMC | DEVICE_SD_SUPPLY_VQMMC));
    CHECK(sd.HostRegister == 0 && sd.ConfigRegister == 1);

    // GPIO policies with their polarity inputs, and reg-names in either order.
    CHECK(FdtSdConfiguration(tree, __Node(tree, "mmc@300"), &sd) == OS_EOK);
    CHECK(sd.CardDetect == DeviceSdCardDetectGpio && sd.CardDetectLine.Line == 7);
    CHECK(sd.CardDetectLine.ActiveLow == 1 && sd.CardDetectInverted == 1);
    CHECK(sd.CardDetectLine.Controller == __Node(tree, "gpio"));
    CHECK(sd.WriteProtect == DeviceSdWriteProtectGpio && sd.WriteProtectLine.Line == 8);
    CHECK(sd.WriteProtectLine.ActiveLow == 0 && sd.WriteProtectInverted == 1);
    CHECK(sd.HostRegister == 1 && sd.ConfigRegister == 0);

    // A described function below the host makes it an SDIO host with one line.
    CHECK(FdtSdConfiguration(tree, __Node(tree, "mmc@400"), &sd) == OS_EOK);
    CHECK(sd.Role == DeviceSdRoleSdio && sd.BusWidth == 1 && sd.Removable == 0);

    CHECK(FdtSdConfiguration(tree, __Node(tree, "mmc@1400"), &sd) == OS_EOK);
    CHECK(sd.CardDetect == DeviceSdCardDetectPolling && sd.Timings == DEVICE_SD_TIMING_MMC_DDR_3V3);
    CHECK(sd.Supplies == DEVICE_SD_SUPPLY_VMMC);
}

static void
__Plans(struct FdtTree* tree)
{
    struct FdtPlatformDependencies plan;
    unsigned int                   lines;

    // The plan accepts cd-gpios and wp-gpios on an SD host that decodes, and
    // acquires nothing for them.
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@300"), &plan) == OS_EOK && plan.Count == 0);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@1400"), &plan) == OS_EOK && plan.Count == 1);
    CHECK(plan.Entries[0].Kind == FdtPlatformSupply && !strcmp(plan.Entries[0].Property, "vmmc-supply"));
    CHECK(plan.Entries[0].Consumer == __Node(tree, "mmc@1400"));

    // Not on other nodes, and not on an SD host whose description is wrong.
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "serial@1100"), &plan) == OS_ENOTSUPPORTED);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@600"), &plan) == OS_EINVALPARAMS);
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@e00"), &plan) == OS_ENOTSUPPORTED);

    // Quiet decoding logs nothing for the same failures, and nests.
    lines = g_logLines;
    FdtDecodeQuietBegin();
    FdtDecodeQuietBegin();
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@600"), &plan) == OS_EINVALPARAMS);
    FdtDecodeQuietEnd();
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "serial@1100"), &plan) == OS_ENOTSUPPORTED);
    CHECK(g_logLines == lines);
    FdtDecodeQuietEnd();
    CHECK(!FdtDecodeIsQuiet());
    CHECK(FdtPlatformDependencies(tree, __Node(tree, "mmc@600"), &plan) == OS_EINVALPARAMS);
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
