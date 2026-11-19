#include "../../services/deviced/core/io.c"
/** Exercise real DT decoding, registry publication, matching and provider lifetime. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <firmware/tree.h>

static unsigned int g_structureParses;
static oserr_t __CountedParseStructure(const void* structure, uint32_t structureLength,
    const char* strings, uint32_t stringsLength, struct FdtParser* parser);

static void* __TestMalloc(size_t size);
static void* __TestRealloc(void* allocation, size_t size);
static void* __TestCalloc(size_t count, size_t size);
static char* __TestStrdup(const char* text);
static void __TestFree(void* allocation);
#define realloc __TestRealloc
#define malloc __TestMalloc
#define calloc __TestCalloc
#define strdup __TestStrdup
#define free __TestFree
#include "../../services/deviced/core/devices.c"
#include "../../services/deviced/core/dma-device.c"
// Kernel registration is exercised by dma_service_test. This fixture keeps
// publication/registry failure injection independent of the manager transport.
static oserr_t __TestPublicationDma(uuid_t device) { (void)device; return OS_EOK; }
#define DmDmaRegisterDevice __TestPublicationDma
#include "../../services/deviced/core/publication.c"
#undef DmDmaRegisterDevice
#include "../../services/deviced/core/match.c"
#define FdtParseStructure __CountedParseStructure
#include "../../services/deviced/firmware/fdt.c"
#undef FdtParseStructure
#include "../../services/deviced/firmware/mapping.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../services/deviced/firmware/gic.c"
#include "../../services/deviced/firmware/platform.c"
#ifndef FDT_DMA_SOURCE
#define FDT_DMA_SOURCE "../../librt/libfdt/dma.c"
#endif
#include FDT_DMA_SOURCE
#ifndef FDT_BCM2712_SOURCE
#define FDT_BCM2712_SOURCE "../../librt/libfdt/bcm2712.c"
#endif
#include "../../librt/libfdt/board.c"
#include FDT_BCM2712_SOURCE
#include "../../services/deviced/firmware/dma.c"
#include "../../services/deviced/firmware/ethernet.c"
#ifndef BOARD_DMA_SOURCE
#define BOARD_DMA_SOURCE "../../services/deviced/firmware/board-dma.c"
#endif
#include BOARD_DMA_SOURCE
#include "../../services/deviced/firmware/platform-runtime.c"
#ifndef ACTIVATION_SOURCE
#define ACTIVATION_SOURCE "../../services/deviced/bus/platform/activation.c"
#endif
#include ACTIVATION_SOURCE
#include "../../services/deviced/bus/platform/dma.c"
#include "../../services/deviced/bus/platform/interrupts.c"
#include "../../services/deviced/bus/platform/ownership.c"
#include "../../services/deviced/bus/platform/enumerate.c"
#include "../../services/deviced/bus/platform/platform.c"
#include "../../services/deviced/bus/platform/discovery.c"
#ifndef BOARD_CLOCKS_SOURCE
#define BOARD_CLOCKS_SOURCE "../../services/deviced/firmware/board-clocks.c"
#endif
#include BOARD_CLOCKS_SOURCE
#include "../../services/deviced/firmware/controllers.c"
#ifndef SD_SOURCE
#define SD_SOURCE "../../services/deviced/firmware/sd.c"
#endif
#include SD_SOURCE
#ifndef REGULATORS_SOURCE
#define REGULATORS_SOURCE "../../services/deviced/firmware/regulators.c"
#endif
#include REGULATORS_SOURCE
#ifndef SUPPLY_SOURCE
#define SUPPLY_SOURCE "../../services/deviced/bus/platform/supply.c"
#endif
#include SUPPLY_SOURCE
#ifndef WAITING_SOURCE
#define WAITING_SOURCE "../../services/deviced/bus/platform/waiting.c"
#endif
#include WAITING_SOURCE
#ifndef CONTROLLER_SOURCE
#define CONTROLLER_SOURCE "../../services/deviced/bus/platform/controller.c"
#endif
#include CONTROLLER_SOURCE
// The request code reads the clock and sleeps through these two names. The
// test replaces both with a fake clock, so deadlines run without waiting.
int __TestTimespecGet(struct timespec* time, int base);
int __TestJobSleep(const struct timespec* until);
#define timespec_get __TestTimespecGet
#define usched_job_sleep __TestJobSleep
#ifndef CONTROLLER_CALLS_SOURCE
#define CONTROLLER_CALLS_SOURCE "../../services/deviced/bus/platform/controller-calls.c"
#endif
#include CONTROLLER_CALLS_SOURCE
#ifndef REGULATOR_SOURCE
#define REGULATOR_SOURCE "../../services/deviced/bus/platform/regulator.c"
#endif
#include REGULATOR_SOURCE
#undef timespec_get
#undef usched_job_sleep
#include "../../librt/libddk/controller.c"
#include "../../librt/libddk/supply.c"
#include "../../librt/libddk/clock.c"
#include "../../librt/libddk/sd.c"
#include "../../librt/libds/list.c"
#undef realloc
#undef malloc
#undef calloc
#undef strdup
#undef free

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

#include "platform_resources_test.inc"
#include "platform_controller_test.inc"
#include "platform_supply_test.inc"
#include "platform_runtime_test.inc"
#include "bcm2712_dma_test.inc"

static oserr_t
__CountedParseStructure(const void* structure, uint32_t structureLength,
    const char* strings, uint32_t stringsLength, struct FdtParser* parser)
{
    g_structureParses++;
    return FdtParseStructure(structure, structureLength, strings, stringsLength, parser);
}

struct __TreeCheck {
    struct FdtTree* Tree;
    unsigned int   Count;
    uint32_t       Previous;
    uint32_t       Subtree;
    int            SubtreeDepth;
};

static void
__CheckCachedNode(const struct FdtResources* nodes, int depth, void* context)
{
    struct __TreeCheck* check = context;
    const struct FdtResources* cached = FdtTreeNode(check->Tree, nodes[depth].NodeOffset);
    int i;

    CHECK(cached != NULL && !memcmp(cached, &nodes[depth], sizeof(*cached)));
    if (check->Count) {
        CHECK(check->Previous < cached->NodeOffset);
    }
    for (i = 0; i < depth; i++) {
        CHECK(nodes[i].NodeOffset < nodes[i + 1].NodeOffset);
    }
    if (check->Subtree != UINT32_MAX) {
        CHECK(depth >= check->SubtreeDepth);
        CHECK(nodes[check->SubtreeDepth].NodeOffset == check->Subtree);
    }
    check->Previous = cached->NodeOffset;
    check->Count++;
}

static void
__CheckCachedTree(struct FdtTree* tree, int synthetic)
{
    struct __TreeCheck check = { .Tree = tree, .Subtree = UINT32_MAX };
    struct FdtResources provider;
    unsigned int parses = g_structureParses;
    unsigned int i;

    CHECK(FdtWalkResources(tree, __CheckCachedNode, &check) == OS_EOK);
    CHECK(check.Count > 64 && FdtTreeNode(tree, UINT32_MAX) == NULL);
    if (synthetic) {
        CHECK(FdtWalkResources(tree, __CheckPlatformDecoder, tree) == OS_EOK);
        for (i = 10; i < 80; i++) {
            CHECK(FdtFindResources(tree, i, &provider) == OS_EOK);
            CHECK(provider.Phandle == i);
            CHECK(FdtTreeNode(tree, provider.NodeOffset)->Phandle == i);
            check.Subtree = provider.NodeOffset;
            check.SubtreeDepth = 2;
            check.Count = 0;
            CHECK(FdtVisitSubtree(tree, provider.NodeOffset, __CheckCachedNode, &check) == OS_EOK);
            CHECK(check.Count == 1);
        }
        CHECK(FdtTreeConsole(tree) != NULL);
        CHECK(!strcmp(FdtTreeConsole(tree)->Name, "serial@1000"));
    }
    CHECK(FdtVisitSubtree(tree, UINT32_MAX, __CheckCachedNode, &check) == OS_ENOENT);
    CHECK(g_structureParses == parses);
}

static int g_allocations;
static int g_failAt;
static int g_liveAllocations;
static int g_treeAllocations;
static unsigned int g_bindings;
static unsigned int g_expectedDevices;
static unsigned int g_matches;
static unsigned int g_ioCreated;
static unsigned int g_ioDestroyed;
static unsigned int g_failIo;
static int g_failDestroy;
static struct usched_mtx* g_locks[8];
static unsigned int g_lockCount;
static const void* g_blob;
static size_t g_blobLength;
static unsigned int g_maps;
static unsigned int g_unmaps;
static oserr_t g_failUnmap;

static void*
__TestMalloc(size_t size)
{
    void* allocation;

    if (++g_allocations == g_failAt) {
        return NULL;
    }
    allocation = malloc(size);
    if (allocation != NULL) {
        g_liveAllocations++;
    }
    return allocation;
}

static void*
__TestRealloc(void* allocation, size_t size)
{
    void* result;

    if (++g_allocations == g_failAt) {
        return NULL;
    }
    result = realloc(allocation, size);
    if (result != NULL && allocation == NULL) {
        g_liveAllocations++;
    }
    return result;
}

static void*
__TestCalloc(size_t count, size_t size)
{
    void* allocation = __TestMalloc(count * size);

    if (allocation != NULL) {
        memset(allocation, 0, count * size);
    }
    return allocation;
}

static char*
__TestStrdup(const char* text)
{
    size_t length = strlen(text) + 1;
    char*  copy = __TestMalloc(length);

    if (copy != NULL) {
        memcpy(copy, text, length);
    }
    return copy;
}

static void
__TestFree(void* allocation)
{
    if (allocation != NULL) {
        CHECK(g_liveAllocations > 0);
        g_liveAllocations--;
        free(allocation);
    }
}

// Log lines per level, and the last line, so tests can check log levels and
// quiet decoding. Set g_logEcho to print every line while debugging a test.
static unsigned int g_logCounts[8];
static char         g_logLast[256];
static int          g_logEcho;

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vsnprintf(g_logLast, sizeof(g_logLast), format, arguments);
    va_end(arguments);
    if ((unsigned int)level < 8) {
        g_logCounts[level]++;
    }
    if (g_logEcho) {
        printf("log %u: %s\n", (unsigned int)level, g_logLast);
    }
}

static unsigned int
__LogProblems(void)
{
    return g_logCounts[OSSYSLOGLEVEL_ERROR] + g_logCounts[OSSYSLOGLEVEL_WARNING];
}

void
usched_mtx_init(struct usched_mtx* mutex, int type)
{
    memset(mutex, 0, sizeof(*mutex));
    mutex->type = type;
}

void
usched_mtx_lock(struct usched_mtx* mutex)
{
    unsigned int i;

    for (i = 0; i < g_lockCount; i++) {
        CHECK(g_locks[i] != mutex);
    }
    CHECK(g_lockCount < 8);
    g_locks[g_lockCount++] = mutex;
}

void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    CHECK(g_lockCount && g_locks[--g_lockCount] == mutex);
}

struct usched_mtx g_dmDmaLock;
void DmDiscoverForgetDevice(uuid_t id) { CHECK(DmDeviceFindLocked(id) == NULL); }

void spinlock_init(spinlock_t* lock) { lock->next = 0; lock->current = 0; }
void spinlock_acquire(spinlock_t* lock) { CHECK(lock->next == lock->current); lock->next++; }
void spinlock_release(spinlock_t* lock) { CHECK(lock->next == lock->current + 1); lock->current++; }

oserr_t
OSDeviceIOCtl2(
    _In_    uuid_t              device,
    _In_    uuid_t              driver,
    _In_    enum OSIOCtlRequest request,
    _InOut_ void*               buffer,
    _In_    size_t              length)
{
    (void)device;
    (void)driver;
    (void)request;
    (void)buffer;
    (void)length;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

// This suite isolates platform callbacks. The native service suite exercises
// kernel binding checks. Keep assignment validation at the control boundary.
oserr_t DmDmaCheckControlLocked(uuid_t id, uint64_t principal, bool enableDma)
{
    DmDevice_t* device = DmDeviceFindLocked(id);
    (void)enableDma;
    if (device == NULL) return OS_ENOENT;
    if (device->Removing) return OS_EBUSY;
    return device->DriverPrincipal == principal && device->DriverId != UUID_INVALID
        ? OS_EOK : OS_EPERMISSIONS;
}
oserr_t DmDmaCheckAssignmentLocked(uuid_t id, uint64_t principal)
{ (void)id; (void)principal; return OS_EOK; }
oserr_t DmDmaDrainEventsLocked(void) { return OS_EOK; }

oserr_t
DmDiscoverFindDriver(uuid_t id, struct DriverIdentification* identification)
{
    struct DriverConfiguration configuration = { 0 };
    struct DriverCompatible    compatible = { .Name = "test,generic" };

    CHECK(g_lockCount == 0 && DmDeviceIsBindable(id));
    CHECK(g_devices.count == g_expectedDevices);
    CHECK(identification->IsPlatform);
    list_construct(&configuration.Compatibles);
    ELEMENT_INIT(&compatible.ListHeader, 0, &compatible);
    list_append(&configuration.Compatibles, &compatible.ListHeader);
    if (DmDriverMatchScore(&configuration, identification) == 2) {
        g_matches++;
    }
    g_bindings++;
    return OS_ENOENT;
}

// Driver configurations the waiting report checks against. Only the
// compatible named here has a "known" driver.
static const char* g_knownDriver = "test,stuck";

int
DmDiscoverHasDriver(const struct DriverIdentification* identification)
{
    CHECK(g_lockCount == 0 && identification->IsPlatform);
    return FdtStringListContains((const uint8_t*)identification->Compatibles,
        identification->CompatibleLength, g_knownDriver);
}

oserr_t
CreateDeviceMemoryIo(DeviceIo_t* io, uintptr_t physical, size_t length)
{
    CHECK(g_lockCount == 2 && g_locks[0] == &g_dmDmaLock && g_locks[1] != &g_dmDevicesLock);
    if (++g_ioCreated == g_failIo) {
        return OS_EOOM;
    }
    io->Id = g_ioCreated;
    io->Type = DeviceIoMemoryBased;
    io->Access.Memory.PhysicalBase = physical;
    io->Access.Memory.Length = length;
    return OS_EOK;
}

oserr_t
DestroyDeviceIo(DeviceIo_t* io)
{
    CHECK(io->Id != UUID_INVALID);
    if (g_failDestroy) {
        g_failDestroy = 0;
        return OS_EBUSY;
    }
    g_ioDestroyed++;
    return OS_EOK;
}

oserr_t
ReleaseDeviceIo(
    _In_ DeviceIo_t* io)
{
    (void)io;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

oserr_t
FirmwareTableMap(const OSFirmwareTableKey_t* key, const void** blob, size_t* length)
{
    CHECK(key->Source == OSFIRMWARE_DEVICETREE);
    *blob = g_blob;
    *length = g_blobLength;
    g_maps++;
    return OS_EOK;
}

oserr_t
FirmwareTableUnmap(const void* blob, size_t length)
{
    oserr_t status;

    CHECK(blob == g_blob && length == g_blobLength);
    if (g_failUnmap != OS_EOK) {
        status = g_failUnmap;
        g_failUnmap = OS_EOK;
        return status;
    }
    g_unmaps++;
    return OS_EOK;
}

static struct PlatformEntry*
__FindCompatible(struct PlatformBus* bus, const char* compatible)
{
    struct PlatformEntry* entry;

    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        if (FdtStringListContains((const uint8_t*)entry->Firmware.Compatibles,
            entry->Firmware.CompatibleLength, compatible)) {
            return entry;
        }
    }
    return NULL;
}

static unsigned int
__InventoryCount(struct PlatformBus* bus)
{
    struct PlatformEntry* entry;
    unsigned int         count = 1;

    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        count++;
    }
    return count;
}

static void
__CheckEmpty(void)
{
    CHECK(g_devices.count == 0 && g_liveAllocations == g_treeAllocations && g_lockCount == 0);
}

static void
__Exercise(struct PlatformBus* bus, int synthetic)
{
    struct PlatformEntry* entry;
    DmDevice_t*      registered;
    struct DmDmaLease*    lease = NULL;
    struct DeviceResources resources;
    struct DeviceResources before;
    unsigned int          bindings;
    unsigned int          created;
    enum DeviceActivation activation = DevicePrepare;
    struct DeviceEthernetConfiguration ethernet;
    uint32_t reset = 0;

    g_expectedDevices = __InventoryCount(bus);
    CHECK(PlatformBusPublish(bus) == OS_EOK);
    bindings = g_bindings;
    CHECK(PlatformBusPublish(bus) == OS_EOK && g_bindings == bindings);
    for (entry = bus->Entries; entry != NULL; entry = entry->Next) {
        registered = DmDeviceFindLocked(entry->DeviceId);
        CHECK(registered != NULL);
        registered->DriverId = 99;
        registered->DriverPrincipal = 1;
        CHECK(DmDeviceFindLocked(registered->Device->ParentId) != NULL);
        // Devices waiting for a dependency owner are published, not bindable.
        CHECK(DmDeviceIsBindable(entry->DeviceId) == (!entry->IsBus && !entry->Waiting));
    }
    __ControllerMatchedDevices(bus);
    entry = __FindCompatible(bus, "brcm,bcm2711-genet-v5");
    if (entry != NULL) {
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_ETHERNET_CONFIG,
            &ethernet, sizeof(ethernet)) == OS_EOK);
        CHECK(ethernet.Mode == DeviceEthernetRgmiiRxId && ethernet.PhyAddress == 1);
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
            &activation, sizeof(activation)) == OS_EOK);
        CHECK(DmHandleIoctl(2, entry->DeviceId, OSIOCTLREQUEST_ETHERNET_PHY_RESET,
            &reset, sizeof(reset)) == OS_EPERMISSIONS);
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_ETHERNET_PHY_RESET,
            &reset, sizeof(reset)) == OS_EOK);
        activation = DeviceStop;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
            &activation, sizeof(activation)) == OS_EOK);
        activation = DevicePrepare;
    }
    entry = __FindCompatible(bus, "brcm,bcm2712-sdhci");
    if (entry != NULL) {
        __Pi5Dma(bus, entry);
    }
    if (__FindCompatible(bus, "brcm,bcm2711-emmc2") != NULL) {
        __SupplyPi4(bus);
    }
    if (synthetic) {
        entry = __FindCompatible(bus, "test,device");
        CHECK(entry != NULL && g_matches == 1);
        CHECK(entry->Firmware.RegisterCount == 2);
        CHECK(entry->Firmware.Registers[0].Base == 0x100000120ULL);
        CHECK(entry->Firmware.InterruptCount == 2);
        CHECK(entry->Firmware.Interrupts[0].Number == 72);
        CHECK(entry->Firmware.Interrupts[1].Number == 73);
        CHECK(entry->Firmware.Pending == (PLATFORM_DEVICE_PENDING_ACTIVATION |
            PLATFORM_DEVICE_PENDING_DMA | PLATFORM_DEVICE_PENDING_INTERRUPTS));
        CHECK(__FindCompatible(bus, "test,excluded") == NULL);
        CHECK(__FindCompatible(bus, "arm,pl011") == NULL);
        CHECK(__FindCompatible(bus, "arm,gic-400") == NULL);
        CHECK(__FindCompatible(bus, "brcm,bcm2712-pcie") == NULL);

        memset(&resources, 0xa5, sizeof(resources));
        before = resources;
        g_failIo = g_ioCreated + 2;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_RESOURCES,
            &resources, sizeof(resources)) == OS_EOOM);
        CHECK(memcmp(&resources, &before, sizeof(resources)) == 0);
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_RESOURCES,
            &resources, sizeof(resources)) == OS_EOK);
        CHECK(resources.Count == 2 && resources.Registers[0].Access.Memory.PhysicalBase == 0x100000120ULL);
        created = g_ioCreated;
        CHECK(DmHandleIoctl(1, entry->DeviceId, OSIOCTLREQUEST_DEVICE_RESOURCES,
            &resources, sizeof(resources)) == OS_EOK && g_ioCreated == created);
        __NativeRuntime(bus, entry);
        __NativeDependencies(bus, __FindCompatible(bus, "test,dependent"),
            __FindCompatible(bus, "test,dependency-owner")->Firmware.FirmwareNode);
        __ControllerSynthetic(bus);
        __SupplySynthetic(bus);
        registered = DmDeviceFindLocked(entry->DeviceId);
        registered->ActiveRequests++;
        CHECK(PlatformBusDestroy(bus) == OS_EBUSY);
        CHECK(PlatformBusPublish(bus) == OS_EBUSY);
        registered->ActiveRequests--;
        g_failDestroy = 1;
        CHECK(PlatformBusDestroy(bus) == OS_EBUSY);
    }
    CHECK(PlatformBusDestroy(bus) == OS_EOK);
    __CheckEmpty();
}

static void
__AllocationFailures(struct FdtTree* tree)
{
    struct PlatformBus* bus;
    oserr_t             status;
    int                 limit;
    int                 index;

    g_allocations = 0;
    CHECK(PlatformBusCreate(tree, &bus) == OS_EOK);
    limit = g_allocations;
    CHECK(PlatformBusDestroy(bus) == OS_EOK);
    for (index = 1; index <= limit; index++) {
        g_allocations = 0;
        g_failAt = index;
        bus = NULL;
        status = PlatformBusCreate(tree, &bus);
        CHECK(status == OS_EOOM && bus == NULL);
        __CheckEmpty();
    }
    g_failAt = 0;
    CHECK(PlatformBusCreate(tree, &bus) == OS_EOK);
    g_allocations = 0;
    g_expectedDevices = __InventoryCount(bus);
    CHECK(PlatformBusPublish(bus) == OS_EOK);
    limit = g_allocations;
    CHECK(PlatformBusDestroy(bus) == OS_EOK);
    for (index = 1; index <= limit; index++) {
        CHECK(PlatformBusCreate(tree, &bus) == OS_EOK);
        g_allocations = 0;
        g_bindings = 0;
        g_failAt = index;
        CHECK(PlatformBusPublish(bus) == OS_EOOM);
        CHECK(g_devices.count == 0 && g_bindings == 0);
        g_failAt = 0;
        CHECK(PlatformBusPublish(bus) == OS_EOK);
        CHECK(PlatformBusDestroy(bus) == OS_EOK);
        __CheckEmpty();
    }
}

int
main(int argc, char** argv)
{
    struct PlatformBus* bus = NULL;
    struct FdtTree* tree = NULL;
    struct FdtTree* failed = NULL;
    struct DmStartupResult result;
    int allocationCount;
    int index;
    FILE*               file;
    long                length;
    void*               blob;
    oserr_t             status;
    int                 expected;
    int                 synthetic;

    CHECK(argc == 4);
    file = fopen(argv[1], "rb");
    CHECK(file != NULL && fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    blob = malloc((size_t)length);
    CHECK(blob != NULL && fread(blob, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    DmDevicesInitialize();
    expected = !strcmp(argv[2], "invalid") ? OS_EINVALPARAMS : atoi(argv[2]);
    synthetic = !strcmp(argv[3], "synthetic");
    g_allocations = 0;
    status = FdtTreeCreate(blob, (size_t)length, &tree);
    allocationCount = g_allocations;
    if (status == OS_EOK) {
        CHECK(g_structureParses == 1);
        __CheckCachedTree(tree, synthetic);
        if (!strcmp(argv[3], "missing-alias")) {
            CHECK(FdtTreeConsole(tree) == NULL);
        }
        g_treeAllocations = g_liveAllocations;
        status = PlatformBusCreate(tree, &bus);
    }
    CHECK(status == expected);
    if (status == OS_EOK) {
        if (!strcmp(argv[3], "pi4")) {
            CHECK(__FindCompatible(bus, "brcm,bcm2711-genet-v5") != NULL);
            CHECK(__FindCompatible(bus, "brcm,bcm2711-emmc2") != NULL);
        } else if (!strcmp(argv[3], "pi5")) {
            CHECK(__FindCompatible(bus, "brcm,bcm2712-sdhci") != NULL);
            CHECK(__FindCompatible(bus, "brcm,bcm2712c0-pinctrl") != NULL);
            CHECK(__FindCompatible(bus, "snps,dwc3") == NULL);
            CHECK(__FindCompatible(bus, "raspberrypi,rp1-gem") == NULL);
        }
        __Exercise(bus, synthetic);
        CHECK(g_structureParses == 1);
        if (synthetic) {
            __AllocationFailures(tree);
            FdtTreeRelease(tree);
            tree = NULL;
            g_treeAllocations = 0;
            __CheckEmpty();
            // Every construction allocation must unwind without publishing a tree.
            for (index = 1; index <= allocationCount; index++) {
                g_allocations = 0;
                g_failAt = index;
                CHECK(FdtTreeCreate(blob, (size_t)length, &failed) == OS_EOOM);
                CHECK(failed == NULL);
                __CheckEmpty();
            }
            g_failAt = 0;
            g_blob = blob;
            g_blobLength = (size_t)length;
            FirmwareInitialize(&result);
            CHECK(result.Status == OS_EOK);
            FirmwareInitialize(&result);
            CHECK(result.Status == OS_EOK && g_maps == 1);
            PlatformEnumerate(&result);
            CHECK(result.Status == OS_EOK);
            PlatformEnumerate(&result);
            CHECK(result.Status == OS_EOK && g_maps == 1 && g_unmaps == 0);
            CHECK(g_platformBus->Tree == FirmwareGetTree());
            CHECK(FirmwareShutdown() == OS_EBUSY);
            CHECK(PlatformBusDestroy(g_platformBus) == OS_EOK);
            g_platformBus = NULL;
            g_failUnmap = OS_EUNKNOWN;
            CHECK(FirmwareShutdown() == OS_EUNKNOWN && FirmwareGetTree() != NULL);
            CHECK(FirmwareShutdown() == OS_EOK && FirmwareGetTree() == NULL);
            CHECK(g_unmaps == 1);
            // A bad tail must never publish a partial tree. Failed cleanup is retried.
            g_blobLength--;
            g_failUnmap = OS_ENOTSUPPORTED;
            FirmwareInitialize(&result);
            CHECK(result.Outcome == DmStartupRetry && FirmwareGetTree() == NULL);
            FirmwareGetStartupResult(&result);
            CHECK(result.Outcome == DmStartupRetry);
            CHECK(g_maps == 2 && g_unmaps == 1);
            FirmwareInitialize(&result);
            CHECK(result.Outcome == DmStartupFailed);
            CHECK(g_maps == 2 && g_unmaps == 2);
            allocationCount = g_structureParses;
            FirmwareInitialize(&result);
            CHECK(result.Outcome == DmStartupFailed);
            CHECK(g_structureParses == allocationCount && g_maps == 2);
            PlatformEnumerate(&result);
            CHECK(result.Outcome == DmStartupFailed);
            CHECK(FirmwareShutdown() == OS_EOK && g_unmaps == 2);
            g_blobLength++;
            FirmwareInitialize(&result);
            CHECK(result.Status == OS_EOK && g_maps == 3);
            CHECK(FirmwareShutdown() == OS_EOK && g_unmaps == 3);
        }
    } else {
        CHECK(bus == NULL);
        if (tree != NULL) {
            // A malformed platform inventory fails once, without reparsing on retries.
            g_blob = blob;
            g_blobLength = (size_t)length;
            FirmwareInitialize(&result);
            CHECK(result.Outcome == DmStartupComplete);
            PlatformEnumerate(&result);
            CHECK(result.Outcome == DmStartupFailed);
            allocationCount = g_allocations;
            PlatformEnumerate(&result);
            CHECK(result.Outcome == DmStartupFailed);
            CHECK(g_allocations == allocationCount && g_platformBus == NULL);
            CHECK(FirmwareShutdown() == OS_EOK);
        }
    }
    FdtTreeRelease(tree);
    g_treeAllocations = 0;
    free(blob);
    __CheckEmpty();
    printf("DT platform: %s passed\n", argv[3]);
    return 0;
}

oserr_t DmDmaRemoveDevice(uuid_t device) { (void)device; return OS_EOK; }
