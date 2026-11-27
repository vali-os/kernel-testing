/**
 * Tests for the PCI host that carries the kernel's log port in a
 * VALI_RPI5_EARLY_SERIAL build (services/deviced/bus/pci/discovery-dt.c).
 *
 * With __OSCONFIG_RPI5_EARLY_SERIAL the host whose node path leads to the
 * node of the alias serial0 must never be started. On the bundled BCM2712
 * trees that is /axi/pcie@1000120000, the host of RP1. Its register range must
 * never be mapped, its bridge reset must never be written, nothing may be
 * published for it, and one WARNING must name it. The other hosts start as
 * before. Without the define every host starts as before and nothing is
 * logged about the alias.
 *
 * Run through pci_host_test.c --early-serial, see serial-host-test.sh.
 */

// Bridge reset bit of the RP1 host (reset line 44, bit 12 of its bank) and of
// the external host pcie@1000110000 (reset line 43, bit 11).
#define SERIAL_TEST_RP1_BRIDGE_BIT      (1U << 12)
#define SERIAL_TEST_EXTERNAL_BRIDGE_BIT (1U << 11)

// The WARNING that deviced logs for the RP1 host in an early serial build.
#define SERIAL_TEST_RP1_WARNING \
    "not starting PCI host /axi/pcie@1000120000, the kernel log port /axi/pcie@1000120000/rp1/serial@30000"

// The parsed trees point into their bytes until __FreeFixtures runs at exit,
// so the bytes live in static storage that is never freed. Each real tree
// gets one slot, and each synthetic tree one fixture.
#define SERIAL_TEST_TREE_SLOTS    8
#define SERIAL_TEST_TREE_SIZE     (128 * 1024)
#define SERIAL_TEST_FIXTURE_SLOTS 16

static uint8_t        g_serialTrees[SERIAL_TEST_TREE_SLOTS][SERIAL_TEST_TREE_SIZE];
static unsigned int   g_serialTreesUsed;
static struct Fixture g_serialFixtures[SERIAL_TEST_FIXTURE_SLOTS];
static unsigned int   g_serialFixturesUsed;

/**
 * Reads a whole device tree file into the next free slot of g_serialTrees.
 */
static uint8_t*
__SerialLoadTree(
        _In_  const char* path,
        _Out_ size_t*     lengthOut)
{
    FILE*    file = fopen(path, "rb");
    uint8_t* bytes;
    long     length;

    CHECK(file != NULL);
    CHECK(g_serialTreesUsed < SERIAL_TEST_TREE_SLOTS);
    bytes = g_serialTrees[g_serialTreesUsed++];
    CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && (size_t)length <= SERIAL_TEST_TREE_SIZE);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    CHECK(fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    *lengthOut = (size_t)length;
    return bytes;
}

/**
 * Sets status = "okay" on pcie@1000110000, the way the firmware does when an
 * external PCIe device is enabled in config.txt. The bundled trees keep that
 * host disabled. The property keeps its place, the unused word becomes FDT_NOP.
 */
static void
__SerialEnableExternalHost(
        _InOut_ uint8_t* bytes,
        _In_    size_t   length)
{
    size_t node;
    size_t status;

    for (node = 0; node + 16 < length; node++) {
        if (!memcmp(bytes + node, "pcie@1000110000", 16)) {
            break;
        }
    }
    CHECK(node + 16 < length);
    for (status = node; status + 9 < length; status++) {
        if (!memcmp(bytes + status, "disabled", 9)) {
            break;
        }
    }
    CHECK(status + 9 < length);
    __Word(bytes + status - 8, 5);
    memset(bytes + status, 0, 8);
    memcpy(bytes + status, "okay", 5);
    __Word(bytes + status + 8, 4);
}

/**
 * Runs device tree discovery on a bundled BCM2712 tree with the external host
 * enabled. Discovery runs twice, as it does after a failed pass. The second
 * pass must neither start anything nor log the WARNING again.
 */
static void
__SerialRealTree(
        _In_ const char* path)
{
    struct FdtPciDependencies dependencies;
    struct FdtTree*           tree;
    PciHost_t*                external;
    PciDevice_t*              endpoint;
    unsigned int              starts;
    unsigned int              writes;
    unsigned int              resources;
    uint8_t*                  bytes;
    size_t                    length;

    bytes = __SerialLoadTree(path, &length);
    __SerialEnableExternalHost(bytes, length);

    // The last host the tree reports is the RP1 host. Its reset controllers
    // tell the register model which ranges are the shared reset registers.
    g_count = 0;
    CHECK(__EnumerateFixture(bytes, length, __Host, NULL) == OS_EOK);
    CHECK(g_count == 2 && g_host.EcamBase == 0x1000120000ULL);
    CHECK(__FixtureDependencies(&g_host, &dependencies) == OS_EOK);
    g_bridgeResetBase = dependencies.BridgeResetBase;
    g_rescalBase = dependencies.ResetBase;

    __Bcm2712Model();
#if defined(__OSCONFIG_RPI5_EARLY_SERIAL)
    g_serialHostSearch = __SerialHostNotSearched;
#endif
    list_construct(&g_pciRoots);
    list_construct(&g_pciDevices);
    g_bridgeWriteBits = 0;
    g_warningNeedle = "__FindSerialConsoleHost";
    g_warningMatches = 0;
    starts = g_rescalStarts;
    CHECK(__ParseFixture(bytes, length, &tree) == OS_EOK);
    CHECK(FdtEnumeratePciHosts(tree, __OnFdtPciHost, tree) == OS_EOK);

    writes = g_bcmWrites + g_bridgeWrites;
    resources = g_resources;
    CHECK(FdtEnumeratePciHosts(tree, __OnFdtPciHost, tree) == OS_EOK);
    CHECK(g_bcmWrites + g_bridgeWrites == writes && g_resources == resources);

    // The external host starts in both builds, with its endpoint behind it.
    external = (PciHost_t*)g_bcmSecondIo;
    CHECK(external != NULL && external->Identification.Segment == 1);
    endpoint = ((PciDevice_t*)external->RootDevice->Children.head->value)->Children.head->value;
    CHECK(endpoint->Header->VendorId == 0x1234);
    CHECK(g_bridgeWriteBits & SERIAL_TEST_EXTERNAL_BRIDGE_BIT);
    CHECK(g_rescalStarts == starts + 1);

#if defined(__OSCONFIG_RPI5_EARLY_SERIAL)
    // The register range of the RP1 host was never mapped, so nothing can
    // have read or written it, and its bridge reset line was never written.
    CHECK(g_bcmIo == NULL);
    CHECK(!(g_bridgeWriteBits & SERIAL_TEST_RP1_BRIDGE_BIT));
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 2);
    CHECK(g_warningMatches == 1);
    g_warningNeedle = SERIAL_TEST_RP1_WARNING;
    g_warningMatches = 0;
    g_serialHostSearch = __SerialHostNotSearched;
    CHECK(FdtEnumeratePciHosts(tree, __OnFdtPciHost, tree) == OS_EOK);
    CHECK(g_warningMatches == 1 && g_bcmIo == NULL);
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 2);
#else
    // Without the define the RP1 host starts as it always did.
    CHECK(g_bcmIo != NULL);
    CHECK(g_bridgeWriteBits & SERIAL_TEST_RP1_BRIDGE_BIT);
    CHECK(g_pciRoots.count == 2 && g_pciDevices.count == 4);
    CHECK(g_warningMatches == 0);
    endpoint = ((PciDevice_t*)((PciHost_t*)g_bcmIo)->RootDevice->Children.head->value)->Children.head->value;
    CHECK(endpoint->Header->VendorId == 0x1DE4 && endpoint->Header->DeviceId == 1);
    CHECK(PciHostDestroy((PciHost_t*)g_bcmIo) == OS_EOK);
    g_bcmIo = NULL;
#endif

    CHECK(PciHostDestroy(external) == OS_EOK);
    g_bcmSecondIo = NULL;
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetMappings.count == 0);
    g_model2712 = 0;
    g_bridgeResetBase = 0;
    g_rescalBase = 0;
    g_warningNeedle = NULL;
    printf("Early serial: %s passed\n", path);
}

#if defined(__OSCONFIG_RPI5_EARLY_SERIAL)

// What a synthetic tree holds at /aliases.
enum __SerialAliasCase {
    // No /aliases node at all.
    __SerialNoAliases,
    // An /aliases node without serial0.
    __SerialNoAlias,
    // serial0 holds the given bytes.
    __SerialAlias
};

/**
 * Builds a small tree with two PCI hosts and one UART outside them:
 *
 *   /aliases                          (by aliasCase)
 *   /axi/pcie@100012                  PCI host
 *   /axi/pcie@1000120000              PCI host
 *   /axi/pcie@1000120000/rp1/serial@30000
 *   /axi/serial@7d001000              outside every host
 *
 * pcie@100012 is a plain string prefix of pcie@1000120000, which is what the
 * separator rule must tell apart.
 */
static size_t
__SerialFixture(
        _Out_ struct Fixture*        fixture,
        _In_  enum __SerialAliasCase aliasCase,
        _In_  const char*            alias,
        _In_  uint32_t               aliasLength)
{
    const uint32_t two[] = { 2 };
    const uint32_t three[] = { 3 };
    uint32_t       structLength;
    uint32_t       stringsOffset;
    uint32_t       total;

    memset(fixture, 0, sizeof(*fixture));
    fixture->Used = 56;
    __Node(fixture, "");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    if (aliasCase != __SerialNoAliases) {
        __Node(fixture, "aliases");
        if (aliasCase == __SerialAlias) {
            __Property(fixture, "serial0", alias, aliasLength);
        }
        __Property(fixture, "serial1", "/axi/serial@7d001000", sizeof("/axi/serial@7d001000"));
        __Token(fixture, FDT_END_NODE);
    }
    __Node(fixture, "axi");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Node(fixture, "pcie@100012");
    __Property(fixture, "compatible", "brcm,bcm2712-pcie", sizeof("brcm,bcm2712-pcie"));
    __Property(fixture, "device_type", "pci", sizeof("pci"));
    __Cells(fixture, "#address-cells", three, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "pcie@1000120000");
    __Property(fixture, "compatible", "brcm,bcm2712-pcie", sizeof("brcm,bcm2712-pcie"));
    __Property(fixture, "device_type", "pci", sizeof("pci"));
    __Cells(fixture, "#address-cells", three, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Node(fixture, "rp1");
    __Property(fixture, "compatible", "simple-bus", sizeof("simple-bus"));
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Node(fixture, "serial@30000");
    __Property(fixture, "compatible", "arm,pl011-axi", sizeof("arm,pl011-axi"));
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "serial@7d001000");
    __Property(fixture, "compatible", "arm,pl011", sizeof("arm,pl011"));
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END);
    structLength = fixture->Used - 56;
    stringsOffset = fixture->Used;
    memcpy(fixture->Bytes + stringsOffset, fixture->Strings, fixture->StringsUsed);
    total = stringsOffset + fixture->StringsUsed;
    __Word(fixture->Bytes, FDT_MAGIC);
    __Word(fixture->Bytes + 4, total);
    __Word(fixture->Bytes + 8, 56);
    __Word(fixture->Bytes + 12, stringsOffset);
    __Word(fixture->Bytes + 16, 40);
    __Word(fixture->Bytes + 20, 17);
    __Word(fixture->Bytes + 24, 16);
    __Word(fixture->Bytes + 32, fixture->StringsUsed);
    __Word(fixture->Bytes + 36, structLength);
    return total;
}

// The start of each WARNING that __FindSerialConsoleHost can log.
#define SERIAL_TEST_FOUND        "__FindSerialConsoleHost: not starting PCI host"
#define SERIAL_TEST_NOT_BELOW    "which is not below a PCI host"
#define SERIAL_TEST_NO_ALIAS     "__FindSerialConsoleHost: the tree has no alias serial0"
#define SERIAL_TEST_NO_ALIASES   "__FindSerialConsoleHost: the tree has no /aliases node"
#define SERIAL_TEST_MALFORMED    "is not a full node path"

/**
 * Runs the search on one synthetic tree. expectedHost is the path of the host
 * that must be found, or NULL when no host may be found. Every case must log
 * exactly one WARNING from the search, and it must be the one that names the
 * reason (expectedWarning).
 */
static void
__SerialSearchCase(
        _In_ enum __SerialAliasCase aliasCase,
        _In_ const char*            alias,
        _In_ uint32_t               aliasLength,
        _In_ const char*            expectedHost,
        _In_ const char*            expectedWarning)
{
    const struct FdtResources* host;
    struct Fixture*            fixture;
    struct FdtTree*            tree;
    size_t                     length;

    CHECK(g_serialFixturesUsed < SERIAL_TEST_FIXTURE_SLOTS);
    fixture = &g_serialFixtures[g_serialFixturesUsed++];
    length = __SerialFixture(fixture, aliasCase, alias, aliasLength);
    CHECK(__ParseFixture(fixture->Bytes, length, &tree) == OS_EOK);

    g_serialHostSearch = __SerialHostNotSearched;
    g_serialHostNode = UINT32_MAX;
    g_warningNeedle = "__FindSerialConsoleHost";
    g_warningMatches = 0;
    __FindSerialConsoleHost(tree);
    CHECK(g_warningMatches == 1);

    // Search again for the reason. The search runs on every call, so the
    // result must also be the same the second time.
    g_warningNeedle = expectedWarning;
    g_warningMatches = 0;
    __FindSerialConsoleHost(tree);
    CHECK(g_warningMatches == 1);
    g_warningNeedle = NULL;

    if (expectedHost == NULL) {
        CHECK(g_serialHostSearch == __SerialHostNone);
        CHECK(g_serialHostNode == UINT32_MAX);
        return;
    }
    host = FdtTreeFindPath(tree, expectedHost, strlen(expectedHost));
    CHECK(host != NULL);
    CHECK(g_serialHostSearch == __SerialHostFound);
    CHECK(g_serialHostNode == host->NodeOffset);
}

static void
__SerialSearchRules(void)
{
    const char unterminated[] = { '/', 'a', 'x', 'i' };

    // The alias leads through pcie@1000120000. pcie@100012 is only a string
    // prefix of that host's name and must not be picked.
    __SerialSearchCase(__SerialAlias, "/axi/pcie@1000120000/rp1/serial@30000",
        sizeof("/axi/pcie@1000120000/rp1/serial@30000"), "/axi/pcie@1000120000",
        SERIAL_TEST_FOUND " /axi/pcie@1000120000, the kernel log port /axi/pcie@1000120000/rp1/serial@30000");
    // Only the text decides. A node missing below the host still counts.
    __SerialSearchCase(__SerialAlias, "/axi/pcie@1000120000/missing",
        sizeof("/axi/pcie@1000120000/missing"), "/axi/pcie@1000120000", SERIAL_TEST_FOUND);
    // "/axi/pcie@100012" is a prefix of this path, but without a "/" after it.
    __SerialSearchCase(__SerialAlias, "/axi/pcie@100012000/serial@0",
        sizeof("/axi/pcie@100012000/serial@0"), NULL, SERIAL_TEST_NOT_BELOW);
    // A node that is a host itself is not below a host.
    __SerialSearchCase(__SerialAlias, "/axi/pcie@100012", sizeof("/axi/pcie@100012"), NULL,
        SERIAL_TEST_NOT_BELOW);
    // A UART outside every host.
    __SerialSearchCase(__SerialAlias, "/axi/serial@7d001000", sizeof("/axi/serial@7d001000"), NULL,
        SERIAL_TEST_NOT_BELOW);
    // A path whose first node does not exist.
    __SerialSearchCase(__SerialAlias, "/soc/pcie@1000120000/rp1", sizeof("/soc/pcie@1000120000/rp1"), NULL,
        SERIAL_TEST_NOT_BELOW);
    // No alias, and no /aliases node.
    __SerialSearchCase(__SerialNoAlias, NULL, 0, NULL, SERIAL_TEST_NO_ALIAS);
    __SerialSearchCase(__SerialNoAliases, NULL, 0, NULL, SERIAL_TEST_NO_ALIASES);
    // Values that are not a full path: relative, empty, the root alone,
    // missing the terminating zero, and with a zero inside.
    __SerialSearchCase(__SerialAlias, "axi/pcie@1000120000/rp1", sizeof("axi/pcie@1000120000/rp1"), NULL,
        SERIAL_TEST_MALFORMED);
    __SerialSearchCase(__SerialAlias, "", 0, NULL, SERIAL_TEST_MALFORMED);
    __SerialSearchCase(__SerialAlias, "/", sizeof("/"), NULL, SERIAL_TEST_NOT_BELOW);
    __SerialSearchCase(__SerialAlias, unterminated, sizeof(unterminated), NULL, SERIAL_TEST_MALFORMED);
    __SerialSearchCase(__SerialAlias, "/axi\0/pcie@1000120000/rp1", sizeof("/axi\0/pcie@1000120000/rp1"), NULL,
        SERIAL_TEST_MALFORMED);
    puts("Early serial: alias path rules passed");
}

#endif

/**
 * Entry of the --early-serial mode. Each path is a bundled BCM2712 tree.
 */
static void
__EarlySerialHosts(
        _In_ int          count,
        _In_ char* const* paths)
{
    int index;

    CHECK(count > 0);
    for (index = 0; index < count; index++) {
        __SerialRealTree(paths[index]);
    }
#if defined(__OSCONFIG_RPI5_EARLY_SERIAL)
    __SerialSearchRules();
    g_serialHostSearch = __SerialHostNotSearched;
#endif
}
