#include "../../services/deviced/core/publication.c"
#include "../../services/deviced/bus/pci/bars.c"
#include "../../services/deviced/bus/pci/resources.c"
#include "../../librt/libfdt/parser.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../services/deviced/firmware/dma.c"
#include "../../services/deviced/firmware/gic.c"
#include "../../services/deviced/firmware/pci.c"
#include "../../services/deviced/bus/pci/hosts/broadcom/firmware.c"
#include "../../services/deviced/bus/rp1/firmware.c"
#include "../../services/deviced/firmware/fdt.c"
#include "../../services/deviced/bus/rp1/rp1.c"
#include "../../services/deviced/bus/rp1/dma.c"
#include "../../services/deviced/bus/pci/dma.c"
#include "../../services/deviced/bus/rp1/publish.c"
#include "../../services/deviced/bus/pci/functionhandlers.c"
#include "../../services/deviced/bus/legacy/fixed.c"
static void* __PciCalloc(size_t count, size_t size);
#define calloc __PciCalloc
#include "../../services/deviced/bus/pci/host.c"
#include "../../services/deviced/bus/pci/enumerate.c"
#include "../../services/deviced/bus/pci/discovery.c"
#undef calloc
#include "../../services/deviced/bus/pci/device.c"
#include "../../services/deviced/bus/pci/interrupts.c"
#include "../../services/deviced/bus/pci/io.c"
#include "../../services/deviced/bus/pci/hosts/ecam.c"
#include "../../services/deviced/bus/pci/hosts/legacy.c"
#include "../../services/deviced/bus/pci/hosts/broadcom/bcm.c"
#include "../../services/deviced/bus/pci/hosts/broadcom/bcm2711.c"
#include "../../services/deviced/bus/pci/hosts/broadcom/bcm2712.c"
#include "../../services/deviced/bus/pci/helpers.c"
#include "../../librt/libds/list.c"
#include <stdio.h>
#include <stdlib.h>

#define FDT_ALIGN(x) (((x) + 3U) & ~3U)
#define FDT_MAGIC 0xD00DFEEDU

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static int g_failPciAllocation;

static void*
__PciCalloc(size_t count, size_t size)
{
    if (g_failPciAllocation) {
        g_failPciAllocation = 0;
        return NULL;
    }
    return calloc(count, size);
}

struct Fixture {
    uint8_t Bytes[16384];
    uint8_t Strings[4096];
    uint32_t Used;
    uint32_t StringsUsed;
};

static struct FdtPciHost g_host;
static int g_count;
static DeviceIo_t* g_selectedIo;
static uintptr_t g_resourceBase;
static size_t g_ioOffset;
static int g_resources;
static uint32_t g_bcmRegisters[0xA000 / 4];
static DeviceIo_t* g_bcmIo;
static DeviceIo_t* g_bcmSecondIo;
static uint32_t g_bcmSecondRegisters[0xA000 / 4];
static struct usched_mtx* g_lockStack[8];
static unsigned int g_lockDepth;
static unsigned int g_firmwareUnmaps;
static size_t g_defaultConfigValue = 0x1234;
static struct usched_mtx* g_lockedMutex;
static long g_delayMilliseconds;
static size_t g_failedWrite = SIZE_MAX;
static unsigned int g_bcmConfigAccesses;
static unsigned int g_bcmWrites;
static unsigned int g_interruptDeliveries;
static int g_deliveredLine;
static uint32_t g_dmaBuffer[1024];
static int g_sleepFailure = 0;
static DeviceIo_t* g_resetIo;
static uint32_t g_resetRegisters[3];
static unsigned int g_releases;
static unsigned int g_destroys;
static unsigned int g_driverRegistrations;
static uint64_t g_bridgeResetBase;
static DeviceIo_t* g_bridgeResetIo;
static uint64_t g_rescalBase;
static uint32_t g_bridgeResetRegisters[12];
static unsigned int g_bridgeWrites;
static unsigned int g_rescalStarts;
static int g_bridgeStuck;
static int g_rescalComplete;
static int g_mdioComplete;
static int g_model2712;
static unsigned int g_mdioWrites;
static unsigned int g_unassignedBars;
static unsigned int g_outsideBars;
static uint32_t g_barMasks[6];
static long g_linkUpAt;
static unsigned int g_firmwarePrimary;
static size_t g_mcfgLength;
static uintptr_t g_mappingFailureBase;
static struct {
    ACPI_TABLE_MCFG Table;
    ACPI_MCFG_ALLOCATION Entries[2];
} g_mcfg;

oserr_t
AcpiQueryStatus(AcpiDescriptor_t* descriptor)
{
    memset(descriptor, 0, sizeof(*descriptor));
    descriptor->Version = ACPI_VERSION_6_0;
    descriptor->BootFlags = ACPI_IA_LEGACY_DEVICES;
    return OS_EOK;
}

oserr_t
AcpiQueryTable(const char* signature, ACPI_TABLE_HEADER** tableOut)
{
    CHECK(strcmp(signature, ACPI_SIG_MCFG) == 0);
    if (g_mcfgLength == 0) {
        return OS_ENOENT;
    }
    *tableOut = malloc(g_mcfgLength);
    CHECK(*tableOut != NULL);
    memcpy(*tableOut, &g_mcfg, g_mcfgLength);
    (*tableOut)->Length = g_mcfgLength;
    return OS_EOK;
}

oserr_t
FirmwareQuery(OSFirmwareInfo_t* firmware)
{
    memset(firmware, 0, sizeof(*firmware));
    firmware->Primary = g_firmwarePrimary;
    return OS_EOK;
}

oserr_t
FirmwareTableMap(const OSFirmwareTableKey_t* key, const void** blobOut, size_t* lengthOut)
{
    CHECK(key->Source == OSFIRMWARE_DEVICETREE);
    *blobOut = g_host.Blob;
    *lengthOut = g_host.BlobLength;
    return OS_EOK;
}

void
spinlock_init(spinlock_t* lock)
{
    lock->next = 0;
    lock->current = 0;
}

void
spinlock_acquire(spinlock_t* lock)
{
    CHECK(lock->next == lock->current);
    lock->next++;
}

void
spinlock_release(spinlock_t* lock)
{
    lock->current++;
}

const char*
PciToString(uint8_t class, uint8_t subclass, uint8_t interface)
{
    (void)class; (void)subclass; (void)interface;
    return "modeled PCI device";
}

static Device_t* g_published[1024];
static struct DmDeviceProvider g_providers[1024];
static unsigned int g_publicationCount;
static unsigned int g_bindingCount;
static unsigned int g_nextPublication = 1;
static unsigned int g_failPublication;
static int g_failBinding;
static unsigned int g_expectedPublicationCount;
static int g_failIoCreation;
static int g_failIoAcquisition;

oserr_t
DmDeviceCreate(Device_t* device, unsigned int flags, uuid_t* id)
{
    if (flags & DEVICE_REGISTER_FLAG_LOADDRIVER) {
        g_driverRegistrations++;
        free(device->Identification.Description);
        free(device);
        return OS_EOK;
    }
    if (g_failPublication && --g_failPublication == 0) {
        return OS_EOOM;
    }
    CHECK(g_nextPublication < 1024);
    *id = device->Id = g_nextPublication++;
    g_published[*id] = device;
    g_publicationCount++;
    return OS_EOK;
}

oserr_t
DmDeviceCreateWithProvider(
    const struct DmDeviceRegistration* registration,
    unsigned int flags,
    uuid_t* id)
{
    oserr_t status;

    *id = UUID_INVALID;
    if (registration->Provider.Operations != NULL) {
        status = registration->Provider.Operations->Retain(registration->Provider.Context);
        CHECK(status == OS_EOK);
    }
    status = DmDeviceCreate(registration->Description, flags, id);
    if (status != OS_EOK) {
        if (registration->Provider.Operations != NULL) {
            registration->Provider.Operations->Release(registration->Provider.Context);
        }
        return status;
    }
    g_providers[*id] = registration->Provider;
    return OS_EOK;
}

oserr_t
DmDeviceEnableDriverBinding(uuid_t id)
{
    // During the RP1 test, every bind must see the complete host tree.
    if (g_expectedPublicationCount != 0) {
        CHECK(g_publicationCount == g_expectedPublicationCount);
    }
    // The countdown allows an earlier child to bind before a later one fails.
    if (g_failBinding && --g_failBinding == 0) {
        return OS_EOOM;
    }
    CHECK(id < 1024 && g_published[id]);
    if (g_published[id]->Length == sizeof(PlatformDevice_t)) {
        CHECK(PlatformDeviceValidate((PlatformDevice_t*)g_published[id]));
    } else {
        CHECK(g_published[id]->Length == sizeof(BusDevice_t));
        g_driverRegistrations++;
    }
    g_bindingCount++;
    return OS_EOK;
}

oserr_t
DmDeviceDestroy(uuid_t id)
{
    CHECK(id < 1024 && g_published[id]);
    for (unsigned int i = 1; i < g_nextPublication; i++) {
        if (g_published[i] && g_published[i]->ParentId == id) {
            return OS_EBUSY;
        }
    }
    if (g_providers[id].Operations != NULL) {
        g_providers[id].Operations->Release(g_providers[id].Context);
        memset(&g_providers[id], 0, sizeof(g_providers[id]));
    }
    free(g_published[id]->Identification.Description);
    free(g_published[id]);
    g_published[id] = NULL;
    g_publicationCount--;
    return OS_EOK;
}

static void __BcmAcceptance(const struct FdtPciHost* description);
static void __Bcm2712Acceptance(const struct FdtPciHost* description);

#ifndef __OSCONFIG_HAS_LEGACY_PCI
static void
__Bcm2712Discovery(void* blob, size_t length);
#endif

void
usched_mtx_init(struct usched_mtx* mutex, int type)
{
    (void)type;
    memset(mutex, 0, sizeof(*mutex));
}

void
usched_mtx_lock(struct usched_mtx* mutex)
{
    CHECK(g_lockDepth < 8);
    for (unsigned int index = 0; index < g_lockDepth; index++) {
        CHECK(g_lockStack[index] != mutex);
    }
    g_lockStack[g_lockDepth++] = mutex;
    g_lockedMutex = mutex;
}

void
usched_mtx_unlock(struct usched_mtx* mutex)
{
    CHECK(g_lockedMutex == mutex);
    CHECK(g_lockDepth != 0);
    g_lockDepth--;
    g_lockedMutex = g_lockDepth != 0 ? g_lockStack[g_lockDepth - 1] : NULL;
}

int*
__errno(void)
{
    static int error;

    return &error;
}

void
timespec_add(const struct timespec* first, const struct timespec* second,
    struct timespec* result)
{
    g_delayMilliseconds += second->tv_sec * 1000 + second->tv_nsec / 1000000;
    result->tv_sec = first->tv_sec + second->tv_sec;
    result->tv_nsec = first->tv_nsec + second->tv_nsec;
}

int
usched_job_sleep(const struct timespec* until)
{
    (void)until;
    return g_sleepFailure;
}

oserr_t
FirmwareTableUnmap(const void* blob, size_t length)
{
    CHECK(blob != NULL && length != 0);
    g_firmwareUnmaps++;
    return OS_EOK;
}

oserr_t
AcquireDeviceIo(DeviceIo_t* io)
{
    if (g_failIoAcquisition) {
        return OS_EOOM;
    }
    if (io->Type == DeviceIoMemoryBased && g_mappingFailureBase != 0 &&
        io->Access.Memory.PhysicalBase == g_mappingFailureBase) {
        return OS_EUNKNOWN;
    }
    return OS_EOK;
}

oserr_t
ReleaseDeviceIo(DeviceIo_t* io)
{
    (void)io;
    g_releases++;
    return OS_EOK;
}

oserr_t
DestroyDeviceIo(DeviceIo_t* io)
{
    g_destroys++;
    if (io == g_bridgeResetIo) {
        g_bridgeResetIo = NULL;
    }
    if (io == g_resetIo) {
        g_resetIo = NULL;
    }
    if (io == g_bcmIo) {
        g_bcmIo = NULL;
    }
    if (io == g_bcmSecondIo) {
        g_bcmSecondIo = NULL;
    }
    return OS_EOK;
}

oserr_t
AcpiQueryInterrupt(unsigned int bus, unsigned int device, int pin,
    int* line, unsigned int* flags)
{
    (void)bus; (void)device; (void)pin; (void)line; (void)flags;
    return OS_ENOENT;
}

size_t
ReadDeviceIo(DeviceIo_t* io, size_t offset, size_t width)
{
    uint32_t value;
    uint32_t* registers = io == g_bcmSecondIo ? g_bcmSecondRegisters : g_bcmRegisters;

    if (g_bridgeResetBase && io->Access.Memory.PhysicalBase == g_bridgeResetBase) {
        CHECK(width == 4 && offset < sizeof(g_bridgeResetRegisters));
        return g_bridgeResetRegisters[offset / 4];
    }
    if (io == g_resetIo) {
        CHECK(width == 4 && offset <= 8);
        return g_resetRegisters[offset / 4];
    }
    if (io == g_bcmIo || io == g_bcmSecondIo) {
        if (offset == 0x4068 && g_linkUpAt && g_delayMilliseconds >= g_linkUpAt) {
            registers[offset / 4] = 0xB0;
        }
        CHECK(offset + width <= sizeof(g_bcmRegisters));
        if (offset >= 0x8000 && offset < 0x9000) {
            CHECK(g_lockedMutex == &((struct BcmPciHost*)
                ((PciHost_t*)io)->OpContext)->ConfigLock);
            g_bcmConfigAccesses++;
        }
        value = registers[offset / 4];
        if (g_model2712 && offset >= 0x8010 && offset < 0x8028 && value == UINT32_MAX) {
            CHECK(!(registers[0x8004 / 4] & (PCI_COMMAND_MMIO | PCI_COMMAND_PORTIO)));
            value = g_barMasks[(offset - 0x8010) / 4];
        }
        return (value >> ((offset & 3) * 8)) & (UINT32_MAX >> ((4 - width) * 8));
    }
    g_selectedIo = io;
    g_ioOffset = offset;
    return g_defaultConfigValue;
}

oserr_t
WriteDeviceIo(DeviceIo_t* io, size_t offset, size_t value, size_t width)
{
    uint32_t mask;
    uint32_t* registers = io == g_bcmSecondIo ? g_bcmSecondRegisters : g_bcmRegisters;

    if (g_bridgeResetBase && io->Access.Memory.PhysicalBase == g_bridgeResetBase) {
        CHECK(width == 4 && (offset == 0x18 || offset == 0x1C));
        CHECK(value == (1U << 11) || value == (1U << 12));
        g_bridgeWrites++;
        if (!g_bridgeStuck) {
            if (offset == 0x18) {
                g_bridgeResetRegisters[0x20 / 4] |= value;
            } else {
                g_bridgeResetRegisters[0x20 / 4] &= ~value;
            }
        }
        return OS_EOK;
    }
    if (io == g_resetIo) {
        CHECK(width == 4 && offset <= 8);
        g_resetRegisters[offset / 4] = (uint32_t)value;
        if (offset == 0 && (value & 1)) {
            g_rescalStarts++;
            if (g_rescalComplete) {
                g_resetRegisters[2] = 1;
            }
        }
        return OS_EOK;
    }
    if (io == g_bcmIo || io == g_bcmSecondIo) {
        g_bcmWrites++;
        if (offset == g_failedWrite) {
            return OS_EUNKNOWN;
        }
        CHECK(offset + width <= sizeof(g_bcmRegisters));
        if (offset >= 0x8000 && offset <= 0x9000) {
            CHECK(g_lockedMutex == &((struct BcmPciHost*)
                ((PciHost_t*)io)->OpContext)->ConfigLock);
            g_bcmConfigAccesses++;
        }
        mask = (UINT32_MAX >> ((4 - width) * 8)) << ((offset & 3) * 8);
        registers[offset / 4] = (registers[offset / 4] & ~mask) |
                (((uint32_t)value << ((offset & 3) * 8)) & mask);
        if (g_model2712 && offset == 0x1104) {
            g_mdioWrites++;
            if (g_mdioComplete) {
                registers[offset / 4] &= ~0x80000000U;
            }
        }
        return OS_EOK;
    }
    g_selectedIo = io;
    g_ioOffset = offset;
    return OS_EOK;
}

oserr_t
CreateDeviceMemoryIo(DeviceIo_t* resource, uintptr_t base, size_t length)
{
    if (g_failIoCreation) {
        return OS_EOOM;
    }
    CHECK(length != 0);
    // Match the kernel's exclusive registration rule for the shared reset range.
    // Without this check, two hosts can appear to work with separate mappings.
    if (g_bridgeResetBase && base == g_bridgeResetBase) {
        if (g_bridgeResetIo != NULL) {
            return OS_EBUSY;
        }
        g_bridgeResetIo = resource;
    }
    memset(resource, 0, sizeof(*resource));
    resource->Type = DeviceIoMemoryBased;
    resource->Access.Memory.PhysicalBase = base;
    resource->Access.Memory.Length = length;
    if (base == 0x80003000 || (g_rescalBase && base == g_rescalBase)) {
        g_resetIo = resource;
    }
    if (g_model2712 && base == 0x1000120000ULL) {
        g_bcmIo = resource;
    }
    if (g_model2712 && base == 0x1000110000ULL) {
        g_bcmSecondIo = resource;
    }
    g_resourceBase = base;
    g_resources++;
    return OS_EOK;
}

oserr_t
CreateDevicePortIo(DeviceIo_t* resource, uint16_t base, size_t length)
{
    if (g_failIoCreation) {
        return OS_EOOM;
    }
    CHECK(length != 0);
    memset(resource, 0, sizeof(*resource));
    resource->Type = DeviceIoPortBased;
    resource->Access.Port.Base = base;
    resource->Access.Port.Length = length;
    g_resourceBase = base;
    g_resources++;
    return OS_EOK;
}

static void
__TestBarResource(PciHost_t* host, DeviceIo_t* resource, uint32_t space, uint64_t address, uint64_t size)
{
    struct PciBar bar;

    PciDescribeBar(host, space, 0, address, size, &bar);
    __CreateBarResource(host, resource, &bar);
}

static void
__PciIntegration(void)
{
    PciHost_t first = { .Identification = { .Segment = 1, .BusEnd = 1 }, .IsExtended = 1,
            .Operations = &g_pciAcpiEcamOperations };
    PciHost_t second = { .Identification = { .Segment = 2, .BusEnd = 1 }, .IsExtended = 1,
            .Operations = &g_pciDtEcamOperations, .OpContext = &g_host,
            .IoResourcePolicy = PciIoResourceMemory };
    PciDevice_t firstDevice = { .Host = &first, .Slot = 3 };
    PciDevice_t secondDevice = { .Host = &second, .Slot = 3 };
    struct OSIOCtlBusControl control = { .Flags = __DEVICEMANAGER_IOCTL_ENABLE };
    PciDevice_t root = { .Host = &second };
    PciDevice_t bridge = { .Parent = &root, .Host = &second, .Slot = 3 };
    PciNativeHeader_t header = { .InterruptPin = 4, .InterruptLine = 200 };
    PciDevice_t child = { .Parent = &bridge, .Host = &second, .Slot = 1, .Header = &header };
    DeviceIo_t resource;
    size_t value;

    firstDevice.list_header.value = &firstDevice;
    firstDevice.list_header.next = &secondDevice.list_header;
    secondDevice.list_header.value = &secondDevice;
    g_pciDevices.head = &firstDevice.list_header;
    CHECK(__PciProviderControl(&secondDevice, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_EOK && g_selectedIo == &second.IoSpace);
    CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_READ, 0x10, &value, 4) == OS_EOK);
    CHECK(value == 0x1234 && g_selectedIo == &second.IoSpace);
    CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_WRITE, 0x10, &value, 4) == OS_EOK);
    CHECK(g_selectedIo == &second.IoSpace);
    second.DriversBlocked = 1;
    g_selectedIo = NULL;
    CHECK(__PciProviderControl(&secondDevice, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_ENOTSUPPORTED && g_selectedIo == NULL);
        CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_WRITE,
            0x04, &value, 4) == OS_ENOTSUPPORTED && g_selectedIo == NULL);
    second.DriversBlocked = 0;
    secondDevice.Handler = &g_rp1PciHandler;
    CHECK(secondDevice.Attachment == NULL);
    CHECK(__PciProviderControl(&secondDevice, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_ENOTSUPPORTED && g_selectedIo == NULL);
    CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        0x04, &value, 4) == OS_ENOTSUPPORTED && g_selectedIo == NULL);
    CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_READ,
        0x10, &value, 4) == OS_EOK && value == 0x1234);
    secondDevice.Handler = NULL;
    // Requests use the retained function directly, even without a lookup list.
    g_pciDevices.head = NULL;
    CHECK(__PciProviderControl(&secondDevice, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    CHECK(__PciProviderAccessRegister(&secondDevice, __DEVICEMANAGER_IOCTL_EXT_READ,
        0x10, &value, 4) == OS_EOK);
    g_selectedIo = NULL;
    CHECK(PciRead32(&second, 2, 0, 0, 0) == UINT32_MAX && g_selectedIo == NULL);
    CHECK(PciRead32(&second, 0, 0, 0, 4096) == UINT32_MAX && g_selectedIo == NULL);
    CHECK(PciRead32(&second, 0, 0, 0, 3) == UINT32_MAX && g_selectedIo == NULL);
    CHECK(PciRead32(&second, 1, 3, 7, 0x10) == 0x1234);
    CHECK(g_ioOffset == ((1 << 20) | (3 << 15) | (7 << 12) | 0x10));
    __TestBarResource(&second, &resource, 2, 0x40000100, 0x100);
    CHECK(g_resources == 1 && g_resourceBase == 0x81000100);
    __TestBarResource(&second, &resource, 2, 0x400fff00, 0x200);
    CHECK(g_resources == 1);
    root.Host->RootDevice = &root;
    memset(root.Host->ScannedBuses, 0, sizeof(root.Host->ScannedBuses));
    PciResolveInterruptLineAndPin(&bridge, 1, 1, 0, &child);
    CHECK(child.InterruptLine == 332 && header.InterruptLine == 200);
    header.InterruptPin = 0;
    PciResolveInterruptLineAndPin(&bridge, 1, 1, 0, &child);
    CHECK(child.InterruptLine == INTERRUPT_NONE);
    g_pciDevices.head = NULL;
    root.Host->RootDevice = NULL;
}

static void
__RuntimePciHosts(void)
{
    PciHost_t* first;
    PciHost_t* second;
    PciHost_t resourceHost = { .Operations = &g_pciAcpiEcamOperations,
        .IoResourcePolicy = PciIoResourcePorts };
    DeviceIo_t resource;
    int resources;
    unsigned int unmaps = g_firmwareUnmaps;
    unsigned int releases;
    unsigned int destroys;
    ACPI_MCFG_ALLOCATION savedAllocation;

    g_defaultConfigValue = UINT32_MAX;
    g_firmwarePrimary = OSFIRMWARE_ACPI;
    g_mcfgLength = sizeof(g_mcfg);
    g_mcfg.Entries[0] = (ACPI_MCFG_ALLOCATION) {
        .Address = 0x80000000, .PciSegment = 3, .StartBusNumber = 2, .EndBusNumber = 3 };
    g_mcfg.Entries[1] = (ACPI_MCFG_ALLOCATION) {
        .Address = 0xA0000000, .PciSegment = 7, .StartBusNumber = 5, .EndBusNumber = 5 };
    BusEnumerate();
    CHECK(g_pciRoots.count == 2 && g_pciDevices.count == 0);
    first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    second = ((PciDevice_t*)g_pciRoots.tail->value)->Host;
    CHECK(first->Operations == &g_pciAcpiEcamOperations && second->Operations == &g_pciAcpiEcamOperations);
    CHECK(first->Identification.Segment == 3 && first->Identification.BusStart == 2 && first->Identification.BusEnd == 3);
    CHECK(second->Identification.Segment == 7 && second->Identification.BusStart == 5 && second->Identification.BusEnd == 5);
    CHECK(first->IoSpace.Access.Memory.PhysicalBase == 0x80200000);
    CHECK(first->IoSpace.Access.Memory.Length == 2 * 1024 * 1024);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    CHECK(first->IoResourcePolicy == PciIoResourcePorts);
#else
    CHECK(first->IoResourcePolicy == PciIoResourceMemory);
#endif
    CHECK(PciRead32(first, 2, 3, 7, 0x100) == UINT32_MAX);
    CHECK(g_ioOffset == ((3 << 15) | (7 << 12) | 0x100));
    CHECK(PciRead32(second, 5, 1, 0, 0xFFC) == UINT32_MAX);
    CHECK(g_selectedIo == &second->IoSpace && g_ioOffset == ((1 << 15) | 0xFFC));
    PciHostDestroy(first);
    PciHostDestroy(second);

    // Discovery owns cleanup when registration rejects an overlapping window.
    savedAllocation = g_mcfg.Entries[1];
    g_mcfg.Entries[1].PciSegment = 3;
    g_mcfg.Entries[1].StartBusNumber = 3;
    g_mcfg.Entries[1].EndBusNumber = 4;
    releases = g_releases;
    destroys = g_destroys;
    BusEnumerate();
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 0);
    CHECK(g_releases == releases + 1 && g_destroys == destroys + 1);
    first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    CHECK(first->Identification.Segment == 3 && first->Identification.BusEnd == 3);
    CHECK(PciRead32(first, 2, 0, 0, 0) == UINT32_MAX && g_selectedIo == &first->IoSpace);
    PciHostDestroy(first);
    g_mcfg.Entries[1] = savedAllocation;

    g_firmwarePrimary = OSFIRMWARE_DEVICETREE;
    BusEnumerate();
    CHECK(g_pciRoots.count == 1);
    first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    CHECK(first->Operations == &g_pciDtEcamOperations);
    CHECK(first->IoResourcePolicy == PciIoResourceMemory);
    CHECK(first->Firmware != NULL && first->FirmwareMapping != NULL);
    CHECK(g_firmwareUnmaps == unmaps);
    PciHostDestroy(first);
    CHECK(g_firmwareUnmaps == unmaps + 1);
    g_firmwarePrimary = OSFIRMWARE_ACPI;

    g_mappingFailureBase = 0xA0500000;
    BusEnumerate();
    CHECK(g_pciRoots.count == 1);
    first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    CHECK(first->Identification.Segment == 3 && first->Operations == &g_pciAcpiEcamOperations);
    PciHostDestroy(first);
    g_mappingFailureBase = 0;

    for (unsigned int failure = 0; failure < 5; failure++) {
        g_firmwarePrimary = failure == 0 ? OSFIRMWARE_NONE : OSFIRMWARE_ACPI;
        g_mcfgLength = failure == 1 ? 0 : sizeof(g_mcfg);
        if (failure == 2) {
            g_mcfgLength = sizeof(ACPI_TABLE_HEADER);
        }
        if (failure == 3) {
            g_mcfg.Entries[0].Address = UINT64_MAX;
            g_mcfg.Entries[1].EndBusNumber = 4;
        }
        if (failure == 4) {
            g_mcfg.Entries[0].Address = 0x80000000;
            g_mappingFailureBase = 0x80200000;
        }
        BusEnumerate();
#ifdef __OSCONFIG_HAS_LEGACY_PCI
        CHECK(g_pciRoots.count == 1);
        first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
        CHECK(first->Operations == &g_pciLegacyOperations);
        CHECK(first->Identification.Segment == 0 && first->Identification.BusStart == 0 && first->Identification.BusEnd == 255);
        CHECK(first->IoResourcePolicy == PciIoResourcePorts && !first->IsExtended);
        CHECK(PciRead8(first, 0, 0, 0, 0x3D) == UINT8_MAX && g_ioOffset == 5);
        CHECK(PciRead16(first, 0, 0, 0, 0x3E) == UINT16_MAX && g_ioOffset == 6);
        PciWrite8(first, 0, 0, 0, 0x3F, 0);
        CHECK(g_ioOffset == 7);
        g_selectedIo = NULL;
        CHECK(PciRead32(first, 0, 0, 0, 0x100) == UINT32_MAX && g_selectedIo == NULL);
        PciHostDestroy(first);
#else
        CHECK(g_pciRoots.count == 0);
#endif
    }
    g_mappingFailureBase = 0;
    g_mcfgLength = 0;
    g_defaultConfigValue = 0x1234;

    __TestBarResource(&resourceHost, &resource, 1, 0x1000, 0x100);
    CHECK(resource.Type == DeviceIoPortBased);
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    resourceHost.Operations = &g_pciLegacyOperations;
    __TestBarResource(&resourceHost, &resource, 1, 0x1000, 0x100);
    CHECK(resource.Type == DeviceIoPortBased);
#endif
    resourceHost.IoResourcePolicy = PciIoResourceMemory;
    __TestBarResource(&resourceHost, &resource, 1, 0x1000, 0x100);
    CHECK(resource.Type == DeviceIoMemoryBased);
    resourceHost.IoResourcePolicy = PciIoResourcePorts;
    __TestBarResource(&resourceHost, &resource, 2, 0x1000, 0x100);
    CHECK(resource.Type == DeviceIoMemoryBased);
    resources = g_resources;
    __TestBarResource(&resourceHost, &resource, 1, 0x10000, 4);
    __TestBarResource(&resourceHost, &resource, 1, 0xFFFC, 8);
    CHECK(g_resources == resources);
    puts("PCI runtime hosts: MCFG segments, mapping failures, legacy fallback and BAR policy passed");
}

static PciHost_t*
__RegistrationHost(uint32_t segment, uint8_t start, uint8_t end)
{
    PciHost_t* host = calloc(1, sizeof(*host));

    CHECK(host != NULL);
    host->Identification.Segment = segment;
    host->Identification.BusStart = start;
    host->Identification.BusEnd = end;
    host->Operations = &g_pciAcpiEcamOperations;
    host->IsExtended = 1;
    CHECK(CreateDeviceMemoryIo(&host->IoSpace, 0x80000000, 0x100000) == OS_EOK);
    CHECK(AcquireDeviceIo(&host->IoSpace) == OS_EOK);
    return host;
}

static void
__HostRegistration(void)
{
    PciHost_t* first = __RegistrationHost(7, 2, 3);
    PciHost_t* adjacent = __RegistrationHost(7, 4, 5);
    PciHost_t* other = __RegistrationHost(8, 2, 3);
    PciHost_t* rejected = __RegistrationHost(7, 3, 4);
    PciHost_t snapshot;
    struct PciHostOperations incomplete = { .Read = __EcamRead };
    struct PciFirmwareMapping* mapping = calloc(1, sizeof(*mapping));
    uuid_t firstId;
    uuid_t savedNext;
    unsigned int releases = g_releases;
    unsigned int destroys = g_destroys;
    unsigned int unmaps = g_firmwareUnmaps;

    CHECK(mapping != NULL);
    mapping->Blob = &g_host;
    mapping->Length = sizeof(g_host);
    mapping->References = 1;
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(PciHostRegister(NULL) == OS_EINVALPARAMS);
    g_selectedIo = NULL;
    CHECK(PciHostRegister(first) == OS_EOK);
    firstId = first->Identification.HostId;
    CHECK(firstId != UUID_INVALID && first->RootDevice->Bus == 2);
    CHECK((uintptr_t)first->RootDevice->list_header.key == firstId);
    CHECK(PciHostRegister(adjacent) == OS_EOK);
    CHECK(PciHostRegister(other) == OS_EOK);
    CHECK(adjacent->Identification.HostId != firstId);
    CHECK(other->Identification.HostId != firstId);
    CHECK(other->Identification.HostId != adjacent->Identification.HostId);
    CHECK(g_selectedIo == NULL && g_pciDevices.count == 0);

    snapshot = *first;
    CHECK(PciHostRegister(first) == OS_EEXISTS);
    CHECK(memcmp(first, &snapshot, sizeof(snapshot)) == 0);
    snapshot = *rejected;
    CHECK(PciHostRegister(rejected) == OS_EEXISTS);
    CHECK(memcmp(rejected, &snapshot, sizeof(snapshot)) == 0);
    CHECK(PciHostAttach(rejected, mapping) == OS_EEXISTS);
    CHECK(mapping->References == 1 && g_firmwareUnmaps == unmaps);
    CHECK(memcmp(rejected, &snapshot, sizeof(snapshot)) == 0);
    rejected->Identification.BusStart = 0;
    rejected->Identification.BusEnd = 255;
    CHECK(PciHostRegister(rejected) == OS_EEXISTS);
    rejected->Identification.BusStart = 9;
    rejected->Identification.BusEnd = 8;
    CHECK(PciHostRegister(rejected) == OS_EINVALPARAMS);
    rejected->Identification.BusEnd = 9;
    rejected->Operations = NULL;
    CHECK(PciHostRegister(rejected) == OS_EINVALPARAMS);
    rejected->Operations = &incomplete;
    CHECK(PciHostRegister(rejected) == OS_EINVALPARAMS);
    rejected->Operations = &g_pciAcpiEcamOperations;

    snapshot = *rejected;
    savedNext = g_nextPciHostId;
    g_failPciAllocation = 1;
    CHECK(PciHostAttach(rejected, mapping) == OS_EOOM);
    CHECK(g_nextPciHostId == savedNext && mapping->References == 1);
    CHECK(memcmp(rejected, &snapshot, sizeof(snapshot)) == 0);
    CHECK(g_releases == releases && g_destroys == destroys);
    CHECK(g_pciRoots.count == 3 && rejected->RootDevice == NULL);

    // Removing one range permits its replacement, but never reuses its HostId.
    PciHostDestroy(first);
    rejected->Identification.BusStart = 2;
    rejected->Identification.BusEnd = 3;
    CHECK(PciHostAttach(rejected, mapping) == OS_EOK);
    CHECK(rejected->Identification.HostId != firstId);
    CHECK(mapping->References == 2);
    PciFirmwareRelease(mapping);
    PciHostDestroy(rejected);
    CHECK(g_firmwareUnmaps == unmaps + 1);
    CHECK(g_pciRoots.count == 2);
    CHECK(adjacent->Identification.Segment == 7 && other->Identification.Segment == 8);
    PciHostDestroy(adjacent);
    PciHostDestroy(other);

    // A final valid ID is usable, but wrapping to zero must never recycle IDs.
    first = __RegistrationHost(UINT32_MAX, 255, 255);
    other = __RegistrationHost(8, 0, 255);
    savedNext = g_nextPciHostId;
    g_nextPciHostId = (uuid_t)-1;
    CHECK(PciHostRegister(first) == OS_EOK);
    CHECK(first->Identification.HostId == (uuid_t)-1);
    CHECK(first->Identification.Segment == UINT32_MAX);
    CHECK(PciHostRegister(other) == OS_EOVERFLOW);
    CHECK(other->Identification.HostId == UUID_INVALID && other->RootDevice == NULL);
    PciHostDestroy(first);
    PciHostDestroy(other);
    g_nextPciHostId = savedNext;
    CHECK(g_pciRoots.count == 0);
    puts("PCI host registration: identity, intervals, ownership, allocation failure and ID lifetime passed");
}

void
SystemDebug(
        enum OSSysLogLevel level,
        const char* format,
        ...)
{
    (void)level;
    if (strstr(format, "is unassigned") != NULL) {
        g_unassignedBars++;
    }
    if (strstr(format, "outside host windows") != NULL) {
        g_outsideBars++;
    }
}

static void
__Word(
        _Out_ uint8_t* bytes,
        _In_ uint32_t value)
{
    bytes[0] = value >> 24;
    bytes[1] = value >> 16;
    bytes[2] = value >> 8;
    bytes[3] = value;
}

static void
__Token(
        _InOut_ struct Fixture* fixture,
        _In_ uint32_t value)
{
    __Word(fixture->Bytes + fixture->Used, value);
    fixture->Used += 4;
}

static void
__Node(
        _InOut_ struct Fixture* fixture,
        _In_ const char* name)
{
    size_t length = strlen(name) + 1;

    __Token(fixture, FDT_BEGIN_NODE);
    memcpy(fixture->Bytes + fixture->Used, name, length);
    fixture->Used += FDT_ALIGN(length);
}

static void
__Property(
        _InOut_ struct Fixture* fixture,
        _In_ const char* name,
        _In_ const void* value,
        _In_ uint32_t length)
{
    __Token(fixture, FDT_PROP);
    __Token(fixture, length);
    __Token(fixture, fixture->StringsUsed);
    memcpy(fixture->Strings + fixture->StringsUsed, name, strlen(name) + 1);
    fixture->StringsUsed += (uint32_t)strlen(name) + 1;
    if (length != 0) {
        memcpy(fixture->Bytes + fixture->Used, value, length);
    }
    fixture->Used += FDT_ALIGN(length);
}

static void
__Cells(
        _InOut_ struct Fixture* fixture,
        _In_ const char* name,
        _In_ const uint32_t* values,
        _In_ uint32_t count)
{
    uint8_t bytes[128];
    uint32_t index;

    CHECK(count <= 32);
    for (index = 0; index < count; index++) {
        __Word(bytes + index * 4, values[index]);
    }
    __Property(fixture, name, bytes, count * 4);
}

static size_t
__Fixture(
        _Out_ struct Fixture* fixture,
        _In_ int disabled,
        _In_ int missingRanges,
        _In_ int malformedMap,
        _In_ int dependencies)
{
    const uint32_t two[] = { 2 };
    const uint32_t three[] = { 3 };
    const uint32_t zero[] = { 0 };
    const uint32_t one[] = { 1 };
    const uint32_t ranges[] = { 0, 0, 0, 0x80000000, 0, 0x10000000 };
    const uint32_t dmaRanges[] = { 0, 0, 0, 0x90000000, 0, 0x10000000 };
    const uint32_t reg[] = { 0, 0x100000, 0, 0x200000 };
    const uint32_t window[] = { 0x02000000, 0, 0x40000000, 0, 0x1000000, 0, 0x100000 };
    const uint32_t dmaWindow[] = { 0x02000000, 0, 0, 0, 0, 0, 0x100000 };
    const uint32_t resetPhandle[] = { 3 };
    const uint32_t clockPhandle[] = { 2 };
    const uint32_t resetReg[] = { 0, 0x3000, 0, 12 };
    const uint32_t clockFrequency[] = { 100000000 };
    const uint32_t mask[] = { 0xf800, 0, 0, 7 };
    const uint32_t map[] = { 3 << 11, 0, 0, 1, 1, 0, 300, 4 };
    uint32_t structLength;
    uint32_t stringsOffset;
    uint32_t total;

    memset(fixture, 0, sizeof(*fixture));
    fixture->Used = 56;
    __Node(fixture, "");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Node(fixture, "bus");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    if (disabled) {
        __Property(fixture, "status", "disabled", 9);
    }
    if (!missingRanges) {
        __Cells(fixture, "ranges", ranges, 6);
    }
    __Cells(fixture, "dma-ranges", dmaRanges, 6);
    __Node(fixture, "pci");
    __Property(fixture, "compatible", "pci-host-ecam-generic", 22);
    __Property(fixture, "status", "okay", 5);
    __Cells(fixture, "#address-cells", three, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Cells(fixture, "#interrupt-cells", one, 1);
    __Cells(fixture, "reg", reg, 4);
    __Cells(fixture, "ranges", window, 7);
    __Cells(fixture, "dma-ranges", dmaWindow, 7);
    if (dependencies) {
        __Cells(fixture, "resets", resetPhandle, 1);
        __Property(fixture, "reset-names", "rescal", 7);
        __Cells(fixture, "clocks", clockPhandle, 1);
        __Property(fixture, "clock-names", "sw_pcie", 8);
    }
    __Cells(fixture, "interrupt-map-mask", mask, 4);
    __Cells(fixture, "interrupt-map", map, malformedMap ? 7 : 8);
    __Token(fixture, FDT_END_NODE);
    if (dependencies) {
        __Node(fixture, "rescal");
        __Property(fixture, "compatible", "brcm,bcm7216-pcie-sata-rescal",
            sizeof("brcm,bcm7216-pcie-sata-rescal"));
        __Cells(fixture, "phandle", resetPhandle, 1);
        __Cells(fixture, "#reset-cells", zero, 1);
        __Cells(fixture, "reg", resetReg, 4);
        __Token(fixture, FDT_END_NODE);
    }
    __Token(fixture, FDT_END_NODE);
    if (dependencies) {
        __Node(fixture, "clock");
        __Property(fixture, "compatible", "fixed-clock", 12);
        __Cells(fixture, "phandle", clockPhandle, 1);
        __Cells(fixture, "#clock-cells", zero, 1);
        __Cells(fixture, "clock-frequency", clockFrequency, 1);
        __Token(fixture, FDT_END_NODE);
    }
    __Node(fixture, "gic");
    __Cells(fixture, "phandle", one, 1);
    __Cells(fixture, "#address-cells", zero, 1);
    __Cells(fixture, "#interrupt-cells", three, 1);
    __Property(fixture, "compatible", "arm,gic-v3", 11);
    __Property(fixture, "interrupt-controller", NULL, 0);
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

static void
__Host(
        _In_ const struct FdtPciHost* host,
        _In_ void* context)
{
    (void)context;
    g_host = *host;
    g_count++;
}

/** Captures domains in callback order without depending on hardware access. */
struct __DomainResults {
    uint32_t Domains[6];
    size_t Count;
};

static void
__DomainHost(const struct FdtPciHost* host, void* context)
{
    struct __DomainResults* results = context;

    CHECK(results->Count < 6);
    results->Domains[results->Count++] = host->Segment;
}

static void
__FirmwareDomains(void)
{
    struct Fixture fixture;
    struct __DomainResults results;
    const uint32_t two = 2;
    const uint32_t three = 3;
    const uint32_t domains[] = { 0, 1, 0, 0, UINT32_MAX, 0 };
    const uint32_t expected[][6] = {
        { 2, 1, 0, 3, UINT32_MAX, 4 },
        { 2, UINT32_MAX, 3, 0, 1, 4 },
        { 1, 0, 2, UINT32_MAX, 3, 0 }
    };
    uint32_t reg[] = { 0, 0x80000000, 0, 0x100000 };
    uint32_t structureLength;
    uint32_t stringsOffset;
    uint32_t length;
    uint32_t mode;
    uint32_t index;
    uint32_t node;

    for (mode = 0; mode < 3; mode++) {
        memset(&fixture, 0, sizeof(fixture));
        memset(&results, 0, sizeof(results));
        fixture.Used = 56;
        __Node(&fixture, "");
        __Cells(&fixture, "#address-cells", &two, 1);
        __Cells(&fixture, "#size-cells", &two, 1);
        for (index = 0; index < 6; index++) {
            node = mode == 1 ? 5 - index : index;
            __Node(&fixture, "pci");
            __Property(&fixture, "compatible", "pci-host-ecam-generic",
                sizeof("pci-host-ecam-generic"));
            __Cells(&fixture, "#address-cells", &three, 1);
            __Cells(&fixture, "#size-cells", &two, 1);
            reg[1] = 0x80000000 + (node << 20);
            __Cells(&fixture, "reg", reg, 4);
            __Property(&fixture, "ranges", NULL, 0);
            if (node == 1 || node == 2 || node == 4) {
                __Cells(&fixture, "linux,pci-domain", &domains[node], 1);
            }
            if (mode == 2 && node == 1) {
                __Property(&fixture, "status", "disabled", sizeof("disabled"));
            }
            __Token(&fixture, FDT_END_NODE);
        }
        __Token(&fixture, FDT_END_NODE);
        __Token(&fixture, FDT_END);
        structureLength = fixture.Used - 56;
        stringsOffset = fixture.Used;
        memcpy(fixture.Bytes + stringsOffset, fixture.Strings, fixture.StringsUsed);
        length = stringsOffset + fixture.StringsUsed;
        __Word(fixture.Bytes, FDT_MAGIC);
        __Word(fixture.Bytes + 4, length);
        __Word(fixture.Bytes + 8, 56);
        __Word(fixture.Bytes + 12, stringsOffset);
        __Word(fixture.Bytes + 16, 40);
        __Word(fixture.Bytes + 20, 17);
        __Word(fixture.Bytes + 24, 16);
        __Word(fixture.Bytes + 32, fixture.StringsUsed);
        __Word(fixture.Bytes + 36, structureLength);
        CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __DomainHost, &results) == OS_EOK);
        CHECK(results.Count == (mode == 2 ? 5 : 6));
        CHECK(memcmp(results.Domains, expected[mode], results.Count * sizeof(uint32_t)) == 0);

        // Malformed tails must still fail before exposing a host description.
        memset(&results, 0, sizeof(results));
        __Word(fixture.Bytes + 36, structureLength - 4);
        CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __DomainHost, &results) == OS_EINVALPARAMS);
        CHECK(results.Count == 0);
    }
    puts("PCI firmware domains: explicit reservations, automatic IDs, order and disabled hosts passed");
}

// The shipped Pi 4 template has a zero-sized memory bank. Model the RAM size
// fixup performed by boot firmware before exercising controller initialization.
static void
__FirmwareMemoryFixup(const struct FdtResources* nodes, int depth, void* context)
{
    (void)context;
    if (nodes[depth].IsMemory) {
        CHECK(nodes[depth].RegLength == 12);
        __Word((uint8_t*)nodes[depth].Reg + 8, 0xc0000000);
    }
}

static void __Bcm2712Model(void);
static void __Rp1ConfiguredDma(const struct FdtPciHost* description);

#include "rp1_test.inc"
#include "pci_dma_test.inc"

static void
__RealTree(
        _In_ const char* path,
        _In_ enum FdtPciHostType type)
{
    FILE* file = fopen(path, "rb");
    long length;
    void* bytes;
    int line;
    unsigned int flags;
    struct FdtPciMsi msi;
    struct FdtInterrupt named;
    struct FdtPciDependencies dependencies;

    CHECK(file != NULL);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    length = ftell(file);
    CHECK(length > 0 && fseek(file, 0, SEEK_SET) == 0);
    bytes = malloc((size_t)length);
    CHECK(bytes != NULL && fread(bytes, 1, (size_t)length, file) == (size_t)length);
    fclose(file);
    g_count = 0;
    CHECK(FdtEnumeratePciHosts(bytes, (size_t)length, __Host, NULL) == OS_EOK);
    CHECK(g_count == 1 && g_host.Type == type);
    CHECK(g_host.EcamLength == 0x9310 && g_host.WindowCount != 0);
    CHECK(FdtResolvePciInterrupt(&g_host, 0, 0, 0, 1, &line, &flags) == OS_EOK);
    CHECK(line == (type == FdtPciHostBcm2711 ? 175 : 261));
    CHECK(FdtResolvePciNamedInterrupt(&g_host, "msi", &named) == OS_EOK);
    CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EOK);
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EOK);
    CHECK(msi.Controller != 0);
    if (type == FdtPciHostBcm2712) {
        CHECK(msi.IsMip && msi.RegisterBase == 0x1000130000ULL);
        CHECK(msi.RegisterLength == 0xc0 && msi.DoorbellBase == 0xfffffff000ULL);
        CHECK(msi.Interrupt.Line == 160 && msi.InterruptCount == 64 && msi.Offset == 0);
        CHECK(named.Line == 266 && g_host.Link.MaxSpeed == 2 && g_host.Link.Lanes == 4);
        CHECK(g_host.Link.NoL0s && g_host.Windows[1].Attributes == 0x43000000);
        CHECK(g_host.Windows[1].BusBase == 0x400000000ULL);
        CHECK(dependencies.BridgeResetBase == 0x1001504318ULL && dependencies.BridgeResetId == 44);
        CHECK(g_host.DmaWindowCount == 3);
        CHECK(g_host.DmaWindows[0].Kind == FdtDmaWindowPeer);
        CHECK(g_host.DmaWindows[1].Kind == FdtDmaWindowRam);
        CHECK(g_host.DmaWindows[2].Kind == FdtDmaWindowMsi);
        __Rp1Acceptance(&g_host);
        __Rp1ConfiguredDma(&g_host);
        __Bcm2712Acceptance(&g_host);
#ifndef __OSCONFIG_HAS_LEGACY_PCI
        __Rp1ScannerAcceptance(&g_host);
        __Bcm2712Discovery(bytes, (size_t)length);
#endif
    }
    if (type == FdtPciHostBcm2711) {
        CHECK(!msi.IsMip && msi.Interrupt.Line == named.Line);
        CHECK(g_host.DmaWindows[0].Kind == FdtDmaWindowUnknown);
        CHECK(FdtWalkResources(bytes, (size_t)length,
            __FirmwareMemoryFixup, NULL) == OS_EOK);
        CHECK(FdtEnumeratePciHosts(bytes, (size_t)length, __Host, NULL) == OS_EOK);
        CHECK(g_host.DmaWindows[0].Kind == FdtDmaWindowRam);
        CHECK(g_host.DmaWindowCount == 1 && g_host.DmaWindows[0].PhysicalBase == 0);
        CHECK(g_host.DmaWindows[0].Length == 0xC0000000);
        __BcmAcceptance(&g_host);
    }
    free(bytes);
}

static void
__BcmDmaTransfer(uint64_t address, uint32_t* word, int write)
{
    uint64_t base = ((uint64_t)g_bcmRegisters[0x4038 / 4] << 32) |
            (g_bcmRegisters[0x4034 / 4] & ~31U);
    uint64_t aperture = 1ULL << ((g_bcmRegisters[0x4034 / 4] & 31) + 15);
    uint64_t physical;
    size_t index;

    CHECK(address >= base && address - base < aperture);
    physical = address - base;
    CHECK(physical >= 0x1000 && physical - 0x1000 < sizeof(g_dmaBuffer));
    index = (size_t)(physical - 0x1000) / sizeof(uint32_t);
    if (write) {
        g_dmaBuffer[index] = *word;
    } else {
        *word = g_dmaBuffer[index];
    }
}

static void
__BcmIntxDelivery(PciHost_t* bus, unsigned int pin)
{
    unsigned int flags;
    int line;

    CHECK(bus->Operations->ResolveInterrupt(bus, 1, 0, 0, pin, &line, &flags) == OS_EOK);
    CHECK(line == 174 + (int)pin);
    CHECK(flags & INTERRUPT_ACPICONFORM_TRIGGERMODE);
    CHECK(!(flags & INTERRUPT_ACPICONFORM_POLARITY));
    g_deliveredLine = line;
    g_interruptDeliveries++;
}

static void
__BcmEnumeration(PciHost_t* bus)
{
    PciDevice_t root = { .Host = bus, .IsBridge = 1 };
    PciDevice_t* device;
    element_t* element;
    element_t* next;

    list_construct(&root.children);
    list_construct(&g_pciDevices);
    root.Host->RootDevice = &root;
    memset(root.Host->ScannedBuses, 0, sizeof(root.Host->ScannedBuses));
    g_bcmRegisters[0x08 / 4] = 0x06040000;
    g_bcmRegisters[0x0C / 4] = 0x00010000;
    g_bcmRegisters[0x8008 / 4] = 0x0C033000;
    g_bcmRegisters[0x800C / 4] = 0;
    g_bcmRegisters[0x803C / 4] = 0x00000100;
    g_bcmRegisters[0x8004 / 4] = PCI_COMMAND_BUSMASTER;
    PciCheckBus(&root, bus->Identification.BusStart);
    CHECK(g_pciDevices.count == 2 && root.children.count == 1);
    device = g_pciDevices.tail->value;
    CHECK(device->Bus == 1 && device->Slot == 0 && !device->IsBridge);
    CHECK(device->Header->VendorId == 0x1106 && device->Header->DeviceId == 0x3483);
    if (((struct BcmPciHost*)bus->OpContext)->Firmware.Blob != NULL) {
        CHECK(device->InterruptLine == 175);
    }
    CHECK(!(g_bcmRegisters[0x8004 / 4] & PCI_COMMAND_BUSMASTER));
    CHECK(g_bcmRegisters[0x8004 / 4] & PCI_COMMAND_INTDISABLE);
    CHECK(PciPublishDevice(&root) == OS_EOK);
    CHECK(PciUnpublishDevice(&root) == OS_EOK);
    CHECK(g_driverRegistrations == 0);
    for (element = g_pciDevices.head; element != NULL; element = next) {
        next = element->next;
        device = element->value;
        free(device->Header);
        free(device);
    }
    list_construct(&g_pciDevices);
    root.Host->RootDevice = NULL;
}

static void
__Bcm2711LinkSpeed(void)
{
    struct FdtPciHost firmware = { .BusEnd = 255 };
    PciHost_t bus = {
        .IoSpace = { .Type = DeviceIoMemoryBased, .Access.Memory.Length = 0x9310 }
    };
    uint32_t linkSpeed;
    uint32_t expectedSpeed;

    g_bcmIo = &bus.IoSpace;
    memset(g_bcmRegisters, 0, sizeof(g_bcmRegisters));
    for (linkSpeed = 0; linkSpeed <= 6; linkSpeed++) {
        firmware.Link.MaxSpeed = linkSpeed;
        expectedSpeed = linkSpeed == 1 ? 1 : BCM_PCIE_LINK_SPEED_GEN2;
        g_bcmRegisters[BCM_PCIE_LINK_CAPABILITY / 4] = 0xA5A50C0F;
        g_bcmRegisters[BCM_PCIE_LINK_CONTROL2 / 4] = 0x5A5AA5AF;
        CHECK(__Bcm2711Start(&bus, &firmware) == OS_EOK);
        CHECK(g_bcmRegisters[BCM_PCIE_LINK_CAPABILITY / 4] ==
            ((0xA5A50C0F & ~(BCM_PCIE_LINK_CAP_ASPM_MASK | BCM_PCIE_LINK_CAP_SPEED_MASK)) |
                expectedSpeed));
        CHECK(g_bcmRegisters[BCM_PCIE_LINK_CONTROL2 / 4] ==
            ((0x5A5AA5AF & ~BCM_PCIE_LINK_CONTROL2_SPEED_MASK) | expectedSpeed));
    }
    g_bcmIo = NULL;
}

static void
__BcmAcceptance(const struct FdtPciHost* description)
{
    struct FdtPciHost firmware = {
        .Type = FdtPciHostBcm2711, .BusEnd = 255, .WindowCount = 1,
        .Windows = {{ .Space = FdtPciSpaceMemory32, .BusBase = 0xC0000000,
            .PhysicalBase = 0x600000000ULL, .Length = 0x40000000 }},
        .DmaWindowCount = 1,
        .DmaWindows = {{ .Kind = FdtDmaWindowRam, .Space = FdtPciSpaceMemory32,
            .BusBase = 0, .PhysicalBase = 0, .Length = 0xC0000000 }}
    };
    struct BcmPciHost controller;
    PciHost_t bus = { .IsExtended = 1, .Identification = { .BusEnd = 255 },
        .IoSpace = { .Type = DeviceIoMemoryBased, .Access.Memory.Length = 0x9310 } };
    uint64_t physical;
    uint64_t address;
    uint32_t word;
    unsigned int accesses;
    unsigned int pin;
    unsigned int deliveries;

    __Bcm2711LinkSpeed();
    if (description != NULL) {
        firmware = *description;
    }

    memset(g_bcmRegisters, 0, sizeof(g_bcmRegisters));
    g_bcmIo = &bus.IoSpace;
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    g_bcmRegisters[0x8000 / 4] = 0x34831106;
    g_bcmRegisters[0x8010 / 4] = 0xC0000004;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(bus.Operations == &g_pciBcmOperations);
    CHECK(bus.DriversBlocked && controller.Ready && g_lockedMutex == NULL);
    for (enum FdtDmaWindowKind kind = FdtDmaWindowUnknown; kind <= FdtDmaWindowMsi; kind++) {
        if (kind == FdtDmaWindowRam) {
            continue;
        }
        controller.Firmware.DmaWindows[0].Kind = kind;
        CHECK(BcmPciDmaAddress(&controller, 0x1000, 4, &address) == OS_ENOENT);
    }
    controller.Firmware.DmaWindows[0].Kind = FdtDmaWindowRam;
    CHECK(g_delayMilliseconds >= 102);
    CHECK(g_bcmRegisters[0x9210 / 4] == 0);
    CHECK(g_bcmRegisters[0x400C / 4] == 0xC0000000);
    CHECK(g_bcmRegisters[0x4010 / 4] == 0);
    CHECK(g_bcmRegisters[0x4070 / 4] == 0x3FF00000);
    CHECK(g_bcmRegisters[0x4080 / 4] == 6 && g_bcmRegisters[0x4084 / 4] == 6);
    CHECK(g_bcmRegisters[0x4074 / 4] == 0xFFF0);
    CHECK(g_bcmRegisters[0x4034 / 4] == 17);
    CHECK(g_bcmRegisters[0x402C / 4] == 0 && g_bcmRegisters[0x403C / 4] == 0);
    CHECK(g_bcmRegisters[0x4044 / 4] == 0);
    CHECK(g_bcmRegisters[0x18 / 4] == 0xFF0100);
    CHECK(g_bcmRegisters[0x20 / 4] == 0xFFF0C000);
    CHECK(PciRead32(&bus, 1, 0, 0, 0) == 0x34831106);
    CHECK(g_bcmRegisters[0x9000 / 4] == (1U << 20));
    CHECK(bus.Operations->Translate(&bus, 2,
            PciRead32(&bus, 1, 0, 0, 0x10) & ~15U, 0x1000, &physical) == OS_EOK);
    CHECK(physical == 0x600000000ULL);
    g_bcmRegisters[0] = 0x271114E4;
    CHECK(PciRead32(&bus, 0, 0, 0, 0) == 0x271114E4);
    CHECK(PciRead16(&bus, 1, 0, 0, 2) == 0x3483);
    PciWrite16(&bus, 1, 0, 0, 4, 2);
    CHECK(g_bcmRegisters[0x8004 / 4] == 2);
    CHECK(PciRead32(&bus, 2, 3, 7, 0) == 0x34831106);
    CHECK(g_bcmRegisters[0x9000 / 4] == ((2U << 20) | (3U << 15) | (7U << 12)));
    accesses = g_bcmConfigAccesses;
    CHECK(PciRead32(&bus, 0, 1, 0, 0) == UINT32_MAX);
    CHECK(PciRead32(&bus, 0, 0, 1, 0) == UINT32_MAX);
    CHECK(PciRead32(&bus, 1, 1, 0, 0) == UINT32_MAX);
    CHECK(PciRead32(&bus, 256, 0, 0, 0) == UINT32_MAX);
    CHECK(PciRead32(&bus, 1, 0, 0, 4096) == UINT32_MAX);
    CHECK(PciRead32(&bus, 1, 0, 0, 3) == UINT32_MAX);
    CHECK(accesses == g_bcmConfigAccesses);
    CHECK(BcmPciDmaAddress(&controller, 0x1000, sizeof(g_dmaBuffer), &address) == OS_EOK);
    word = 0xA55A1234;
    __BcmDmaTransfer(address, &word, 1);
    word = 0;
    __BcmDmaTransfer(address, &word, 0);
    CHECK(word == 0xA55A1234 && g_dmaBuffer[0] == word);
    CHECK(BcmPciDmaAddress(&controller, 0xBFFFFFFC, 4, &address) == OS_EOK);
    CHECK(BcmPciDmaAddress(&controller, 0xBFFFFFFC, 8, &address) == OS_ENOENT);
    CHECK(BcmPciDmaAddress(&controller, 0xC0000000, 4, &address) == OS_ENOENT);
    CHECK(BcmPciDmaAddress(&controller, UINT64_MAX, 4, &address) == OS_EINVALPARAMS);
    if (description != NULL) {
        deliveries = g_interruptDeliveries;
        for (pin = 1; pin <= 4; pin++) {
            __BcmIntxDelivery(&bus, pin);
            CHECK(g_deliveredLine == 174 + (int)pin);
        }
        CHECK(g_interruptDeliveries == deliveries + 4);
    }
    __BcmEnumeration(&bus);
    g_bcmRegisters[0x4068 / 4] = 0x80;
    accesses = g_bcmConfigAccesses;
    CHECK(PciRead32(&bus, 1, 0, 0, 0) == UINT32_MAX);
    PciWrite32(&bus, 1, 0, 0, 4, 7);
    CHECK(g_bcmConfigAccesses == accesses);
    CHECK(PciRead32(&bus, 0, 0, 0, 0) == 0x271114E4);
    BcmPciDestroy(&bus, &controller);
    CHECK(!controller.Ready && bus.Operations == NULL && bus.OpContext == NULL);
    CHECK((g_bcmRegisters[0x9210 / 4] & 3) == 3);
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(bus.Operations == NULL && g_lockedMutex == NULL);
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    firmware.DmaWindows[0].BusBase = 0x100000000ULL;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(BcmPciDmaAddress(&controller, 0x1000, 4, &address) == OS_EOK);
    CHECK(address == 0x100001000ULL && g_bcmRegisters[0x4038 / 4] == 1);
    word = 0x12345678;
    __BcmDmaTransfer(address, &word, 1);
    CHECK(g_dmaBuffer[0] == word);
    BcmPciDestroy(&bus, &controller);
    g_failedWrite = 0x400C;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(bus.Operations == NULL && bus.OpContext == NULL);
    CHECK((g_bcmRegisters[0x9210 / 4] & 3) == 3);
    g_failedWrite = SIZE_MAX;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(bus.Operations == NULL && bus.OpContext == NULL);
    g_bcmRegisters[0x4068 / 4] = UINT32_MAX;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    firmware.Type = FdtPciHostBcm2712;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware.Type = FdtPciHostBcm2711;
    for (enum FdtDmaWindowKind kind = FdtDmaWindowUnknown; kind <= FdtDmaWindowMsi; kind++) {
        if (kind == FdtDmaWindowRam) {
            continue;
        }
        firmware.DmaWindows[0].Kind = kind;
        CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    }
    firmware.DmaWindows[0].Kind = FdtDmaWindowRam;
    firmware.DmaWindows[0].PhysicalBase = 0x1000;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware.DmaWindows[0].PhysicalBase = 0;
    firmware.DmaWindows[0].BusBase = 0xC0000000;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    g_bcmIo = NULL;
}

// Model the Pi 5 register bank without making reset erase the endpoint BARs:
// enumeration intentionally continues to require firmware-assigned resources.
static void
__Bcm2712Model(void)
{
    memset(g_bcmRegisters, 0, sizeof(g_bcmRegisters));
    memset(g_bcmSecondRegisters, 0, sizeof(g_bcmSecondRegisters));
    memset(g_resetRegisters, 0, sizeof(g_resetRegisters));
    memset(g_bridgeResetRegisters, 0, sizeof(g_bridgeResetRegisters));
    memset(g_barMasks, 0, sizeof(g_barMasks));
    g_model2712 = 1;
    g_mdioComplete = 1;
    g_rescalComplete = 1;
    g_bridgeResetRegisters[0x20 / 4] = 1U << 7; // Unrelated reset must survive.
    g_bcmRegisters[0] = 0x271214E4;
    g_bcmRegisters[0x08 / 4] = 0x06040000;
    g_bcmRegisters[0x0C / 4] = 0x00010000;
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    g_bcmRegisters[0x8000 / 4] = 0x00011DE4; // RP1
    g_bcmRegisters[0x8008 / 4] = 0x02000000;
    g_bcmRegisters[0x8004 / 4] = PCI_COMMAND_MMIO | PCI_COMMAND_BUSMASTER;
    g_bcmRegisters[0x803C / 4] = 0x00000100;
    g_bcmRegisters[0x8010 / 4] = 0x00400000; // Assigned 32-bit BAR
    g_bcmRegisters[0x8014 / 4] = 0x1000000C; // Assigned 64-bit BAR
    g_bcmRegisters[0x8018 / 4] = 4;
    g_barMasks[0] = 0xFFC00000;
    g_barMasks[1] = 0xFFFFF00C;
    g_barMasks[2] = UINT32_MAX;
    g_barMasks[3] = 0xFFFFF000; // Implemented but unassigned
    memcpy(g_bcmSecondRegisters, g_bcmRegisters, sizeof(g_bcmRegisters));
    g_bcmSecondRegisters[0x8000 / 4] = 0x56781234;
}

static void
__Bcm2712Acceptance(const struct FdtPciHost* description)
{
    struct FdtPciHost firmware = *description;
    struct FdtPciDependencies dependencies;
    struct BcmPciHost controller;
    PciHost_t bus = { .IoSpace = { .Type = DeviceIoMemoryBased,
        .Access.Memory.Length = 0x9310 } };
    BusDevice_t device = { .Bus = 1 };
    struct PciBar bars[6];
    uint64_t address;
    uint32_t index;
    unsigned int writes;
    unsigned int starts;
    unsigned int mdio;
    unsigned int warnings;
    unsigned int resources;
    unsigned int releases;
    unsigned int destroys;
    long delay;

    CHECK(FdtResolvePciDependencies(description, &dependencies) == OS_EOK);
    g_bridgeResetBase = dependencies.BridgeResetBase;
    g_rescalBase = dependencies.ResetBase;
    __Bcm2712Model();
    g_bcmIo = &bus.IoSpace;
    // A failed first registration or acquisition must not leave a shared record
    // that makes the next attempt busy or appear to have usable registers.
    g_failIoCreation = 1;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOOM);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetProviders.count == 0);
    g_failIoCreation = 0;
    g_failIoAcquisition = 1;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOOM);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetProviders.count == 0);
    g_failIoAcquisition = 0;
    starts = g_rescalStarts;
    mdio = g_mdioWrites;
    CHECK(firmware.Windows[0].Length == 0xFFFFFFFC);
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(g_rescalStarts == starts + 1 && g_mdioWrites == mdio + 8);
    CHECK(g_bridgeResetRegisters[0x20 / 4] == (1U << 7));
    CHECK(g_bcmRegisters[0x9210 / 4] == 0 && g_bcmRegisters[0x4204 / 4] == 0);
    CHECK((g_bcmRegisters[0x4064 / 4] & 4) == 4);
    CHECK((g_bcmRegisters[0x04DC / 4] & 0x1FF) == 0x42);
    CHECK(g_bcmRegisters[0x4170 / 4] == UINT32_MAX);
    CHECK(g_bcmRegisters[0x405C / 4] == 0x0ABA0000);
    CHECK(g_bcmRegisters[0x400C / 4] == 0 && g_bcmRegisters[0x4010 / 4] == 0);
    CHECK(g_bcmRegisters[0x4018 / 4] == 4); // Wide outbound target
    CHECK(g_bcmRegisters[0x4070 / 4] == 0xFFF00000);
    CHECK(g_bcmRegisters[0x4080 / 4] == 0x1F && g_bcmRegisters[0x4084 / 4] == 0x1F);
    CHECK(g_bcmRegisters[0x20 / 4] == 0xFFF00000);
    CHECK(g_bcmRegisters[0x24 / 4] == 0xFFF10001);
    CHECK(g_bcmRegisters[0x28 / 4] == 4 && g_bcmRegisters[0x2C / 4] == 6);
    // Peer registers, RAM, and MIP have distinct remaps and size encodings.
    CHECK(g_bcmRegisters[0x402C / 4] == 7);
    CHECK(g_bcmRegisters[0x40AC / 4] == 1 && g_bcmRegisters[0x40B0 / 4] == 0x1F);
    CHECK(g_bcmRegisters[0x4034 / 4] == 21 && g_bcmRegisters[0x4038 / 4] == 0x10);
    CHECK(g_bcmRegisters[0x40B4 / 4] == 1 && g_bcmRegisters[0x40B8 / 4] == 0);
    CHECK(g_bcmRegisters[0x403C / 4] == 0xFFFFF01C && g_bcmRegisters[0x4040 / 4] == 0xFF);
    CHECK(g_bcmRegisters[0x40BC / 4] == 0x130001 && g_bcmRegisters[0x40C0 / 4] == 0x10);
    CHECK(g_bcmRegisters[0x4044 / 4] == 0);
    CHECK(controller.Firmware.Windows[0].Length == 0xFFFFFFFC);
    CHECK(bus.Operations->Translate(&bus, 2, 0xFFFFFFFB, 1, &address) == OS_EOK);
    CHECK(address == 0x1FFFFFFFFBULL);
    CHECK(bus.Operations->Translate(&bus, 2, 0xFFFFFFFC, 1, &address) == OS_ENOENT);
    CHECK(bus.Operations->Translate(&bus, 2, 0xFFFFFFF8, 8, &address) == OS_ENOENT);
    CHECK(BcmPciDmaAddress(&controller, 0x1000, 4, &address) == OS_EOK);
    CHECK(address == 0x1000001000ULL);
    CHECK(BcmPciDmaAddress(&controller, 0x1F00000000ULL, 4, &address) == OS_ENOENT);
    CHECK(BcmPciDmaAddress(&controller, 0x1000130000ULL, 4, &address) == OS_ENOENT);
    resources = g_resources;
    warnings = g_unassignedBars;
    PciProbeBars(&bus, &device, 0, bars);
    CHECK(g_resources == resources && g_unassignedBars == warnings);
    CHECK(bars[0].State == PciBarAssigned && bars[0].BusAddress == 0x400000);
    CHECK(bars[0].CpuAddress == 0x1F00400000ULL && bars[0].Size == 0x400000);
    CHECK(bars[1].State == PciBarAssigned && bars[1].Space == 3);
    CHECK(bars[1].BusAddress == 0x410000000ULL && bars[1].CpuAddress == 0x1C10000000ULL);
    CHECK(bars[1].Size == 4096 && bars[1].Attributes == 12);
    CHECK(bars[2].State == PciBarAbsent && bars[2].Size == 0);
    CHECK(bars[3].State == PciBarUnassigned && bars[3].Size == 4096);
    CHECK(bars[3].BusAddress == 0 && bars[3].CpuAddress == 0);
    CHECK(g_bcmRegisters[0x8014 / 4] == 0x1000000C && g_bcmRegisters[0x8018 / 4] == 4);
    CHECK(g_bcmRegisters[0x8004 / 4] == (PCI_COMMAND_MMIO | PCI_COMMAND_BUSMASTER));
    warnings = g_unassignedBars;
    PciReadBars(&bus, &device, 0);
    CHECK(g_unassignedBars == warnings + 1);
    CHECK(device.IoSpaces[0].Access.Memory.PhysicalBase == 0x1F00400000ULL);
    CHECK(device.IoSpaces[1].Access.Memory.PhysicalBase == 0x1C10000000ULL);
    CHECK(device.IoSpaces[1].Access.Memory.Length == 4096);
    CHECK(g_bcmRegisters[0x801C / 4] == 0);
    // A complete BAR must fit: even four bytes of padding invalidate it.
    g_bcmRegisters[0x8010 / 4] = 0xFFFFF000;
    g_barMasks[0] = 0xFFFFF000;
    PciProbeBars(&bus, &device, 0, bars);
    CHECK(bars[0].State == PciBarOutsideWindow && bars[0].BusAddress == 0xFFFFF000);
    CHECK(bars[0].CpuAddress == 0 && bars[0].Size == 4096);
    // A malformed final 64-bit BAR must not probe beyond the BAR register bank.
    g_bcmRegisters[0x8024 / 4] = 4;
    g_bcmRegisters[0x8028 / 4] = 0x12345678;
    PciProbeBars(&bus, &device, 0, bars);
    CHECK(bars[5].State == PciBarInvalid);
    CHECK(g_bcmRegisters[0x8024 / 4] == 4 && g_bcmRegisters[0x8028 / 4] == 0x12345678);
    CHECK(g_bcmRegisters[0x8004 / 4] == (PCI_COMMAND_MMIO | PCI_COMMAND_BUSMASTER));
    g_bcmRegisters[0x8024 / 4] = 0;
    warnings = g_outsideBars;
    resources = g_resources;
    PciReadBars(&bus, &device, 0);
    CHECK(g_outsideBars == warnings + 1 && g_resources == resources + 1);
    BcmPciDestroy(&bus, &controller);
    CHECK(!controller.Ready && !controller.BridgeResetMapped);
    CHECK(!(g_bcmRegisters[0x4064 / 4] & 4));
    CHECK(g_bridgeResetRegisters[0x20 / 4] == (1U << 7));

    // The tenth slot exercises the second register block, not just BAR1..3.
    for (index = 3; index < 10; index++) {
        firmware.DmaWindows[index] = (struct FdtPciWindow) {
            .Space = FdtPciSpaceMemory64, .Kind = FdtDmaWindowPeer,
            .BusBase = 0x2000000000ULL + index * 4096,
            .PhysicalBase = 0x3000000000ULL + index * 4096, .Length = 4096 };
    }
    firmware.DmaWindowCount = 10;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(g_bcmRegisters[0x4104 / 4] == 0x901C && g_bcmRegisters[0x4108 / 4] == 0x20);
    CHECK(g_bcmRegisters[0x413C / 4] == 0x9001 && g_bcmRegisters[0x4140 / 4] == 0x30);
    CHECK(g_rescalStarts == starts + 1); // Reuses the shared calibration.
    BcmPciDestroy(&bus, &controller);
    firmware = *description;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(g_bcmRegisters[0x4104 / 4] == 0 && g_bcmRegisters[0x413C / 4] == 0);
    BcmPciDestroy(&bus, &controller);

    // Unsupported descriptions fail before either controller or reset writes.
    writes = g_bcmWrites + g_bridgeWrites;
    firmware.DmaWindowCount = 11;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware = *description;
    firmware.DmaWindows[1].BusBase = 0;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware = *description;
    firmware.DmaWindows[0].Length--;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware = *description;
    firmware.Windows[1].PhysicalBase = firmware.Windows[0].PhysicalBase + 0xFFF00000;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    firmware = *description;
    firmware.Windows[1].BusBase = UINT64_MAX - 0xFFFFF;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_ENOTSUPPORTED);
    CHECK(g_bcmWrites + g_bridgeWrites == writes);
    firmware = *description;

    // Each wait and error exit is bounded, unpublishes config access and frees
    // both reset mappings. A successful final link poll must also be accepted.
    g_bcmRegisters[0x4068 / 4] = 0x80;
    delay = g_delayMilliseconds;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(g_delayMilliseconds - delay >= 200 && g_delayMilliseconds - delay < 250);
    CHECK(bus.OpContext == NULL && bus.Operations == NULL && !controller.Ready);
    g_linkUpAt = g_delayMilliseconds + 205;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    g_linkUpAt = 0;
    BcmPciDestroy(&bus, &controller);
    g_mdioComplete = 0;
    delay = g_delayMilliseconds;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(g_delayMilliseconds - delay < 50 && !controller.BridgeResetMapped);
    g_mdioComplete = 1;
    g_rescalComplete = 0;
    g_resetRegisters[2] = 0;
    releases = g_releases;
    destroys = g_destroys;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(g_resetRegisters[0] == 0 && g_resetIo == NULL);
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 2);
    g_rescalComplete = 1;
    g_bridgeStuck = 1;
    g_bridgeResetRegisters[0x20 / 4] |= 1U << 12;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(!controller.BridgeResetMapped);
    g_bridgeStuck = 0;
    g_failedWrite = 0x40BC;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(bus.OpContext == NULL && bus.Operations == NULL && !controller.BridgeResetMapped);
    g_failedWrite = SIZE_MAX;
    g_model2712 = 0;
    g_bcmIo = NULL;
    puts("BCM2712: reset, PHY, wide windows, exact bounds, inbound slots and timeouts passed");
}

#ifndef __OSCONFIG_HAS_LEGACY_PCI
static void
__Bcm2712Discovery(void* blob, size_t length)
{
    struct PciFirmwareMapping* mapping;
    struct FdtPciHost externalFirmware;
    struct BcmPciHost failedController;
    PciHost_t failedHost = { .IoSpace = { .Type = DeviceIoMemoryBased,
        .Access.Memory.Length = 0x9310 } };
    PciHost_t* rp1;
    PciHost_t* external;
    PciDevice_t* endpoint;
    unsigned int starts;
    unsigned int warnings;
    unsigned int unmaps = g_firmwareUnmaps;
    size_t index;
    size_t status;
    uint8_t* bytes = blob;


    // Model firmware enabling pciex1 in its handed-off tree. The bundled file
    // remains untouched; the disabled pcie0 must not acquire a mapping.
    for (index = 0; index + 16 < length; index++) {
        if (!memcmp(bytes + index, "pcie@1000110000", 15)) {
            break;
        }
    }
    CHECK(index + 16 < length);
    for (status = index; status + 9 < length; status++) {
        if (!memcmp(bytes + status, "disabled", 9)) {
            __Word(bytes + status - 8, 5);
            memset(bytes + status, 0, 8);
            memcpy(bytes + status, "okay", 5);
            __Word(bytes + status + 8, 4); // FDT_NOP fills the unused word.
            break;
        }
    }
    CHECK(status + 9 < length);
    g_count = 0;
    CHECK(FdtEnumeratePciHosts(blob, length, __Host, NULL) == OS_EOK);
    CHECK(g_count == 2);
    __Bcm2712Model();
    starts = g_rescalStarts;
    warnings = g_unassignedBars;
    list_construct(&g_pciRoots);
    list_construct(&g_pciDevices);
    mapping = calloc(1, sizeof(*mapping));
    CHECK(mapping != NULL);
    mapping->Blob = blob;
    mapping->Length = length;
    mapping->References = 1;
    CHECK(FdtEnumeratePciHosts(blob, length, __OnFdtPciHost, mapping) == OS_EOK);
    PciFirmwareRelease(mapping);
    CHECK(g_pciRoots.count == 2 && g_pciDevices.count == 4);
    CHECK(g_rescalStarts == starts + 1 && g_unassignedBars == warnings + 2);
    rp1 = (PciHost_t*)g_bcmIo;
    external = (PciHost_t*)g_bcmSecondIo;
    CHECK(rp1->Identification.Segment == 2 && external->Identification.Segment == 1);
    endpoint = ((PciDevice_t*)rp1->RootDevice->children.head->value)->children.head->value;
    CHECK(endpoint->Header->VendorId == 0x1DE4 && endpoint->Header->DeviceId == 1);
    CHECK(endpoint->Bus == 1 && endpoint->Slot == 0 && endpoint->InterruptLine == 261);
    endpoint = ((PciDevice_t*)external->RootDevice->children.head->value)->children.head->value;
    CHECK(endpoint->Header->VendorId == 0x1234 && endpoint->InterruptLine == 251);
    CHECK(rp1->DriversBlocked && external->DriversBlocked);
        CHECK(g_bcm2712ResetProviders.count == 1);
        CHECK(((struct BcmPciHost*)rp1->OpContext)->BridgeReset ==
            ((struct BcmPciHost*)external->OpContext)->BridgeReset);
        CHECK(((struct BcmPciHost*)rp1->OpContext)->BridgeReset->References == 2);
        externalFirmware = ((struct BcmPciHost*)external->OpContext)->Firmware;
    PciHostDestroy(external);
    g_bcmSecondIo = NULL;
    CHECK(g_firmwareUnmaps == unmaps && g_pciDevices.count == 2);
    CHECK(g_bridgeResetIo != NULL);
    CHECK(((struct BcmPciHost*)rp1->OpContext)->BridgeReset->References == 1);
    CHECK(PciRead32(rp1, 1, 0, 0, 0) == 0x00011DE4);
    CHECK(g_resetRegisters[2] == 1 && g_bridgeResetRegisters[0x20 / 4] == (1U << 7));
    // The replacement sibling borrows the live mapping before timing out.
    // Its failure must drop only its own reference, leaving RP1 usable.
    g_bcmSecondIo = &failedHost.IoSpace;
    g_bcmSecondRegisters[0x4068 / 4] = 0x80;
    CHECK(BcmPciInitialize(&failedHost, &failedController, &externalFirmware) != OS_EOK);
    CHECK(failedController.BridgeReset == NULL && failedHost.OpContext == NULL);
    CHECK(g_bridgeResetIo != NULL);
    CHECK(((struct BcmPciHost*)rp1->OpContext)->BridgeReset->References == 1);
    CHECK(PciRead32(rp1, 1, 0, 0, 0) == 0x00011DE4);
    g_bcmSecondIo = NULL;
    PciHostDestroy(rp1);
    g_bcmIo = NULL;
    CHECK(g_firmwareUnmaps == unmaps + 1 && g_pciDevices.count == 0);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetProviders.count == 0);
    // Repeat with the opposite shutdown order. RP1 stopping must not unmap
    // registers still needed by the external host's later reset writes.
    __Bcm2712Model();
    mapping = calloc(1, sizeof(*mapping));
    CHECK(mapping != NULL);
    mapping->Blob = blob;
    mapping->Length = length;
    mapping->References = 1;
    CHECK(FdtEnumeratePciHosts(blob, length, __OnFdtPciHost, mapping) == OS_EOK);
    PciFirmwareRelease(mapping);
    CHECK(g_pciRoots.count == 2);
    rp1 = (PciHost_t*)g_bcmIo;
    external = (PciHost_t*)g_bcmSecondIo;
    PciHostDestroy(rp1);
    g_bcmIo = NULL;
    CHECK(g_bridgeResetIo != NULL);
    CHECK(((struct BcmPciHost*)external->OpContext)->BridgeReset->References == 1);
    CHECK(PciRead32(external, 1, 0, 0, 0) == 0x56781234);
    PciHostDestroy(external);
    g_bcmSecondIo = NULL;
    CHECK(g_firmwareUnmaps == unmaps + 2 && g_pciDevices.count == 0);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetProviders.count == 0);
    // A firmware-enabled but empty external connector must not stop RP1
    // discovery, and the failed host must not retain the shared blob.
    __Bcm2712Model();
    g_bcmSecondRegisters[0x4068 / 4] = 0x80;
    mapping = calloc(1, sizeof(*mapping));
    CHECK(mapping != NULL);
    mapping->Blob = blob;
    mapping->Length = length;
    mapping->References = 1;
    CHECK(FdtEnumeratePciHosts(blob, length, __OnFdtPciHost, mapping) == OS_EOK);
    PciFirmwareRelease(mapping);
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 2 && g_bcmSecondIo == NULL);
    CHECK(mapping->References == 1);
    rp1 = (PciHost_t*)g_bcmIo;
    CHECK(g_bridgeResetIo != NULL);
    CHECK(((struct BcmPciHost*)rp1->OpContext)->BridgeReset->References == 1);
    CHECK(PciRead32(rp1, 1, 0, 0, 0) == 0x00011DE4);
    PciHostDestroy(rp1);
    CHECK(g_firmwareUnmaps == unmaps + 3 && g_pciRoots.count == 0);
    CHECK(g_bridgeResetIo == NULL && g_bcm2712ResetProviders.count == 0);
    g_model2712 = 0;
    g_bridgeResetBase = g_rescalBase = 0;
    puts("Pi 5 tree: generic scanner found RP1 and firmware-enabled external endpoint");
}

#endif

static void
__BcmDmaWindows(void)
{
    struct BcmPciHost controller = { .Ready = 1, .Firmware = {
        .DmaWindowCount = 2,
        .DmaWindows = {
            { .Kind = FdtDmaWindowPeer, .Length = 0x100000 },
            { .Kind = FdtDmaWindowRam, .PhysicalBase = 0x1000,
              .BusBase = 0x100000000ULL, .Length = 0x10000 }
        }
    } };
    uint64_t address = 0;

    CHECK(BcmPciDmaAddress(&controller, 0x1100, 16, &address) == OS_EOK);
    CHECK(address == 0x100000100ULL);
    CHECK(BcmPciDmaAddress(&controller, 0x10FFF, 2, &address) == OS_ENOENT);
    CHECK(BcmPciDmaAddress(&controller, 0, 16, &address) == OS_ENOENT);
    controller.Firmware.DmaWindows[1].BusBase = UINT64_MAX - 0x100;
    CHECK(BcmPciDmaAddress(&controller, 0x1100, 2, &address) == OS_ENOENT);
    CHECK(address == 0x100000100ULL);
}

static void
__BcmMultipleHosts(void)
{
    struct FdtPciHost firmware = g_host;
    struct PciFirmwareMapping* mapping;
    PciHost_t* first;
    PciHost_t* second;
    struct BcmPciHost* firstController;
    struct BcmPciHost* secondController;
    PciDevice_t* firstEndpoint;
    PciDevice_t* secondEndpoint;
    uint8_t interruptMap[32];
    uint64_t address;
    uint64_t physical;
    uint32_t savedReset;
    unsigned int writes;
    unsigned int unmaps = g_firmwareUnmaps;
    unsigned int releases = g_releases;
    unsigned int destroys = g_destroys;

    list_construct(&g_pciDevices);
    list_construct(&g_pciRoots);
    mtx_init(&g_pciDevicesLock, mtx_plain);
    first = calloc(1, sizeof(*first) + sizeof(*firstController));
    second = calloc(1, sizeof(*second) + sizeof(*secondController));
    mapping = calloc(1, sizeof(*mapping));
    CHECK(first != NULL && second != NULL && mapping != NULL);
    mapping->Blob = firmware.Blob;
    mapping->Length = firmware.BlobLength;
    mapping->References = 1;
    firstController = (struct BcmPciHost*)(first + 1);
    secondController = (struct BcmPciHost*)(second + 1);
    first->IoSpace.Type = DeviceIoMemoryBased;
    first->IoSpace.Access.Memory.Length = 0x9310;
    second->IoSpace = first->IoSpace;
    g_bcmIo = &first->IoSpace;
    g_bcmSecondIo = &second->IoSpace;
    memset(g_bcmRegisters, 0, sizeof(g_bcmRegisters));
    memset(g_bcmSecondRegisters, 0, sizeof(g_bcmSecondRegisters));
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    g_bcmSecondRegisters[0x4068 / 4] = 0xB0;
    firmware.Type = FdtPciHostBcm2711;
    firmware.Segment = 1;
    firmware.BusEnd = 2;
    firmware.Windows[0] = (struct FdtPciWindow) {
        .Space = FdtPciSpaceMemory32, .BusBase = 0xC0000000,
        .PhysicalBase = 0x600000000ULL,
        .Length = 0x40000000 };
    firmware.DmaWindows[0] = (struct FdtPciWindow) {
        .Kind = FdtDmaWindowRam, .Space = FdtPciSpaceMemory32, .Length = 0xC0000000 };
    memcpy(interruptMap, firmware.InterruptMap, sizeof(interruptMap));
    __Word(interruptMap, 0);
    firmware.InterruptMap = interruptMap;
    CHECK(BcmPciInitialize(first, firstController, &firmware) == OS_EOK);
    CHECK(firstController->Variant == &g_bcm2711PciVariant);
    CHECK(PciHostAttach(first, mapping) == OS_EOK);
    CHECK(BcmPciInitialize(first, firstController, &firmware) == OS_EINVALPARAMS);
    CHECK(firstController->Ready);

    // A Pi 5 description missing its reset provider, and a failed sibling,
    // must leave the live host intact.
    firmware.Type = FdtPciHostBcm2712;
    writes = g_bcmWrites;
    g_bcmSecondRegisters[0x9210 / 4] = 0x12345678;
    CHECK(BcmPciGetVariant(firmware.Type) == &g_bcm2712PciVariant);
    CHECK(BcmPciInitialize(second, secondController, &firmware) == OS_ENOTSUPPORTED);
    CHECK(g_bcmWrites == writes);
    CHECK(g_bcmSecondRegisters[0x9210 / 4] == 0x12345678);
    CHECK(second->OpContext == NULL && second->Operations == NULL);
    firmware.Type = FdtPciHostBcm2711;
    firmware.Segment = 2;
    firmware.Windows[0].PhysicalBase = 0x700000000ULL;
    firmware.DmaWindows[0].BusBase = 0x100000000ULL;
    g_failedWrite = 0x400C;
    CHECK(BcmPciInitialize(second, secondController, &firmware) != OS_EOK);
    CHECK(g_bcmRegisters[0x9210 / 4] == 0 && firstController->Ready);
    CHECK(second->OpContext == NULL && second->Operations == NULL);
    g_failedWrite = SIZE_MAX;
    CHECK(BcmPciInitialize(second, secondController, &firmware) == OS_EOK);
    CHECK(PciHostAttach(second, mapping) == OS_EOK);
    PciFirmwareRelease(mapping);
    CHECK(mapping->References == 2 && g_firmwareUnmaps == unmaps);
    CHECK(first->RootDevice != second->RootDevice && g_pciRoots.count == 2);

    g_bcmRegisters[0] = g_bcmSecondRegisters[0] = 0x271114E4;
    g_bcmRegisters[0x08 / 4] = g_bcmSecondRegisters[0x08 / 4] = 0x06040000;
    g_bcmRegisters[0x0C / 4] = g_bcmSecondRegisters[0x0C / 4] = 0x00010000;
    g_bcmRegisters[0x8000 / 4] = 0x111114E4;
    g_bcmSecondRegisters[0x8000 / 4] = 0x222214E4;
    g_bcmRegisters[0x8008 / 4] = g_bcmSecondRegisters[0x8008 / 4] = 0x0C033000;
    g_bcmRegisters[0x803C / 4] = g_bcmSecondRegisters[0x803C / 4] = 0x00000100;
    PciCheckBus(first->RootDevice, 0);
    PciCheckBus(second->RootDevice, 0);
    CHECK(g_pciDevices.count == 4);
    firstEndpoint = ((PciDevice_t*)first->RootDevice->children.head->value)->children.head->value;
    secondEndpoint = ((PciDevice_t*)second->RootDevice->children.head->value)->children.head->value;
    CHECK(firstEndpoint->Host == first && secondEndpoint->Host == second);
    CHECK(firstEndpoint->InterruptLine == 332 && secondEndpoint->InterruptLine == 332);
    // Route the first host again after attaching the second host's root.
    PciResolveInterruptLineAndPin(firstEndpoint->Parent, 1, 0, 0, firstEndpoint);
    CHECK(firstEndpoint->InterruptLine == 332);
    CHECK(PciRead32(first, 1, 0, 0, 0) == 0x111114E4);
    CHECK(PciRead32(second, 1, 0, 0, 0) == 0x222214E4);
    PciWrite32(first, 2, 3, 0, 0x40, 0xfeed);
    CHECK(g_bcmRegisters[0x9000 / 4] == ((2U << 20) | (3U << 15)));
    CHECK(g_bcmSecondRegisters[0x9000 / 4] == (1U << 20));
    CHECK(g_bcmSecondRegisters[0x8040 / 4] == 0);
    CHECK(first->Operations->Translate(first, 2, 0xC0001000, 4, &physical) == OS_EOK);
    CHECK(physical == 0x600001000ULL);
    CHECK(second->Operations->Translate(second, 2, 0xC0001000, 4, &physical) == OS_EOK);
    CHECK(physical == 0x700001000ULL);
    CHECK(BcmPciDmaAddress(firstController, 0x1000, 4, &address) == OS_EOK && address == 0x1000);
    CHECK(BcmPciDmaAddress(secondController, 0x1000, 4, &address) == OS_EOK && address == 0x100001000ULL);
    first->DriversBlocked = 0;
    CHECK(second->DriversBlocked);

    savedReset = g_bcmSecondRegisters[0x9210 / 4];
    PciHostDestroy(first);
    g_bcmIo = NULL;
    CHECK(g_pciRoots.count == 1 && g_pciDevices.count == 2);
    CHECK(mapping->References == 1 && g_firmwareUnmaps == unmaps);
    CHECK(g_bcmSecondRegisters[0x9210 / 4] == savedReset && secondController->Ready);
    CHECK(PciRead32(second, 1, 0, 0, 0) == 0x222214E4);
    PciResolveInterruptLineAndPin(secondEndpoint->Parent, 1, 0, 0, secondEndpoint);
    CHECK(secondEndpoint->InterruptLine == 332);
    PciHostDestroy(second);
    g_bcmSecondIo = NULL;
    CHECK(g_pciRoots.count == 0 && g_pciDevices.count == 0);
    CHECK(g_firmwareUnmaps == unmaps + 1);
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 2);
    CHECK(g_lockedMutex == NULL);
    puts("Broadcom hosts: independent roots, config locks, routing, failure and teardown passed");
}

static void
__EcamMultipleHosts(void)
{
    struct PciFirmwareMapping* mapping;
    PciHost_t* first;
    PciHost_t* second;
    unsigned int unmaps = g_firmwareUnmaps;

    mapping = calloc(1, sizeof(*mapping));
    CHECK(mapping != NULL);
    mapping->Blob = g_host.Blob;
    mapping->Length = g_host.BlobLength;
    mapping->References = 1;
    g_defaultConfigValue = UINT32_MAX;
    __EnumerateEcamWindow(3, 0, 1, 0x80000000, &g_host, mapping);
    __EnumerateEcamWindow(4, 0, 1, 0x90000000, &g_host, mapping);
    PciFirmwareRelease(mapping);
    CHECK(g_pciRoots.count == 2 && mapping->References == 2);
    first = ((PciDevice_t*)g_pciRoots.head->value)->Host;
    second = ((PciDevice_t*)g_pciRoots.tail->value)->Host;
    CHECK(first != second && first->RootDevice != second->RootDevice);
    CHECK(first->Identification.Segment == 3 && second->Identification.Segment == 4);
    CHECK(first->OpContext != second->OpContext);
    PciHostDestroy(second);
    CHECK(g_pciRoots.count == 1 && mapping->References == 1);
    CHECK(g_firmwareUnmaps == unmaps);
    CHECK(first->RootDevice == g_pciRoots.head->value);
    CHECK(PciRead32(first, 0, 0, 0, 0) == UINT32_MAX);
    CHECK(g_selectedIo == &first->IoSpace);
    PciHostDestroy(first);
    CHECK(g_pciRoots.count == 0 && g_firmwareUnmaps == unmaps + 1);
    g_defaultConfigValue = 0x1234;
    puts("ECAM hosts: separate roots, contexts and firmware lifetime passed");
}

static void
__BcmDependencyAcceptance(const struct FdtPciHost* description)
{
    struct FdtPciHost firmware = *description;
    struct BcmPciHost controller;
    PciHost_t bus = { .IoSpace = {
        .Type = DeviceIoMemoryBased, .Access.Memory.Length = 0x9310 } };
    unsigned int releases = g_releases;
    unsigned int destroys = g_destroys;

    firmware.Type = FdtPciHostBcm2711;
    firmware.BusEnd = 255;
    firmware.Windows[0] = (struct FdtPciWindow) {
        .Space = FdtPciSpaceMemory32, .BusBase = 0xC0000000,
        .PhysicalBase = 0x600000000ULL,
        .Length = 0x40000000 };
    firmware.DmaWindows[0] = (struct FdtPciWindow) {
        .Kind = FdtDmaWindowRam, .Space = FdtPciSpaceMemory32,
        .BusBase = 0, .PhysicalBase = 0, .Length = 0xC0000000 };
    g_bcmIo = &bus.IoSpace;
    g_bcmRegisters[0x4068 / 4] = 0xB0;
    g_resetRegisters[0] = 0;
    g_resetRegisters[2] = 1;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) == OS_EOK);
    CHECK(g_resetRegisters[0] == 0 && g_resetIo == NULL);
    CHECK(g_releases == releases + 1 && g_destroys == destroys + 1);
    BcmPciDestroy(&bus, &controller);
    g_resetRegisters[2] = 0;
    CHECK(BcmPciInitialize(&bus, &controller, &firmware) != OS_EOK);
    CHECK(g_resetIo == NULL && g_resetRegisters[0] == 0);
    CHECK(g_releases == releases + 2 && g_destroys == destroys + 2);
    CHECK(bus.Operations == NULL && bus.OpContext == NULL);
    CHECK((g_bcmRegisters[0x9210 / 4] & 3) == 3);
    g_bcmIo = NULL;
}

/** Independent malformed-provider cases for the synthetic firmware tree. */
enum ResourceCase {
    ResourceValid,
    ResourceDisabledMip,
    ResourceDisabledMipBus,
    ResourceMissingMip,
    ResourceBadMsiCells,
    ResourceShortMsiRange,
    ResourceBadMsiOffset,
    ResourceBadMsiCount,
    ResourceDisabledGic,
    ResourceBadGicCells,
    ResourceDisabledReset,
    ResourceShortReset,
    ResourceBadResetCells,
    ResourceBadNames,
    ResourceExtended,
    ResourceBadLink,
    ResourceBadReg,
    ResourceDuplicateProvider,
    ResourceBadProviderScalar,
    ResourceDisabledClock,
    ResourceBadClockCells,
    ResourceShortMipReg
};

static size_t
__ResourceFixture(struct Fixture* fixture, enum ResourceCase variant)
{
    uint32_t one[] = { 1 };
    uint32_t two[] = { 2 };
    uint32_t three[] = { 3 };
    uint32_t zero[] = { 0 };
    uint32_t reg[] = { 0x10, 0x200000, 0, 0x10000 };
    uint32_t ranges[] = { 0x43000000, 4, 0, 0x20, 0, 1, 0 };
    uint32_t dma[] = {
        0x43000000, 0x10, 0, 0, 0, 0x10, 0,
        0x02000000, 0, 0, 0x20, 0, 0, 0x400000,
        0x03000000, 0xff, 0xfffff000, 0x30, 0x100000, 0, 0x1000
    };
    uint32_t msiParent[] = { variant == ResourceMissingMip ? 99 : 17 };
    uint32_t interrupts[] = { 0, 100, 4, 0, 101, 1 };
    uint32_t extended[] = { 1, 0, 111, 4, 1, 0, 112, 1 };
    uint32_t resets[] = { 19, 44, 18 };
    uint32_t mipReg[] = { 0, 0x100000, 0, 0xc0, 0xff, 0xfffff000, 0, 0x1000 };
    uint32_t mipRanges[] = { 1, 0, 247, 1, variant == ResourceBadMsiCount ? 65 : 8 };
    uint32_t mipOffset[] = { variant == ResourceBadMsiOffset ? 64 : 8 };
    uint32_t parentRanges[] = { 0, 0, 0x30, 0, 0, 0x200000 };
    uint32_t resetReg[] = { 0x40, 0x1234, 0, 0x30 };
    uint32_t memory[] = { 0, 0, 2, 0 };
    uint32_t clock[] = { 20 };
    uint32_t frequency[] = { 100000000 };
    uint32_t handle;
    uint32_t structLength;
    uint32_t stringsOffset;
    uint32_t total;

    memset(fixture, 0, sizeof(*fixture));
    fixture->Used = 56;
    __Node(fixture, "");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Cells(fixture, "interrupt-parent", one, 1);
    __Node(fixture, "pci");
    __Property(fixture, "compatible", "brcm,bcm2712-pcie", sizeof("brcm,bcm2712-pcie"));
    __Cells(fixture, "#address-cells", three, 1);
    __Cells(fixture, "#size-cells", two, 1);
    handle = 5;
    __Cells(fixture, "phandle", &handle, 1);
    __Cells(fixture, "reg", reg, variant == ResourceBadReg ? 3 : 4);
    __Cells(fixture, "ranges", ranges, 7);
    __Cells(fixture, "dma-ranges", dma, 21);
    __Cells(fixture, "msi-parent", msiParent, 1);
    __Cells(fixture, "interrupts", interrupts, 6);
    if (variant == ResourceExtended) {
        __Cells(fixture, "interrupts-extended", extended, 8);
    }
    __Property(fixture, "interrupt-names", "pcie\0msi", variant == ResourceBadNames ? 8 : 9);
    __Cells(fixture, "clocks", clock, 1);
    __Property(fixture, "clock-names", "sw_pcie", 8);
    __Cells(fixture, "resets", resets, variant == ResourceShortReset ? 1 : 3);
    __Property(fixture, "reset-names", "bridge\0rescal", sizeof("bridge\0rescal"));
    __Cells(fixture, "max-link-speed", two, variant == ResourceBadLink ? 0 : 1);
    __Cells(fixture, "num-lanes", one, 1);
    __Property(fixture, "aspm-no-l0s", NULL, 0);
    __Property(fixture, "brcm,enable-ssc", NULL, 0);
    __Property(fixture, "brcm,clkreq-mode", "safe", 5);
    __Token(fixture, FDT_END_NODE);

    __Node(fixture, "mip-bus");
    __Cells(fixture, "#address-cells", two, 1);
    __Cells(fixture, "#size-cells", two, 1);
    __Cells(fixture, "ranges", parentRanges, 6);
    if (variant == ResourceDisabledMipBus) {
        __Property(fixture, "status", "disabled", 9);
    }
    __Node(fixture, "mip");
    handle = 17;
    __Cells(fixture, "phandle", &handle, 1);
    __Property(fixture, "compatible", "brcm,bcm2712-mip", sizeof("brcm,bcm2712-mip"));
    __Property(fixture, "msi-controller", NULL, 0);
    if (variant == ResourceDisabledMip) {
        __Property(fixture, "status", "disabled", 9);
    }
    if (variant == ResourceBadMsiCells) {
        __Cells(fixture, "#msi-cells", one, 1);
    }
    __Cells(fixture, "reg", mipReg, variant == ResourceShortMipReg ? 7 : 8);
    __Cells(fixture, "msi-ranges", mipRanges, variant == ResourceShortMsiRange ? 4 : 5);
    __Cells(fixture, "brcm,msi-offset", mipOffset, variant == ResourceBadProviderScalar ? 0 : 1);
    __Token(fixture, FDT_END_NODE);
    __Token(fixture, FDT_END_NODE);
    if (variant == ResourceDuplicateProvider) {
        __Node(fixture, "duplicate");
        __Cells(fixture, "phandle", &handle, 1);
        __Token(fixture, FDT_END_NODE);
    }
    __Node(fixture, "reset");
    handle = 19;
    __Cells(fixture, "phandle", &handle, 1);
    __Property(fixture, "compatible", "brcm,brcmstb-reset", sizeof("brcm,brcmstb-reset"));
    __Cells(fixture, "#reset-cells", one, variant == ResourceBadResetCells ? 0 : 1);
    __Cells(fixture, "reg", resetReg, 4);
    if (variant == ResourceDisabledReset) {
        __Property(fixture, "status", "disabled", 9);
    }
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "rescal");
    handle = 18;
    __Cells(fixture, "phandle", &handle, 1);
    __Property(fixture, "compatible", "brcm,bcm7216-pcie-sata-rescal", sizeof("brcm,bcm7216-pcie-sata-rescal"));
    __Cells(fixture, "#reset-cells", zero, 1);
    __Cells(fixture, "reg", resetReg, 4);
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "gic");
    __Cells(fixture, "phandle", one, 1);
    __Cells(fixture, "#interrupt-cells", variant == ResourceBadGicCells ? one : three, 1);
    __Property(fixture, "compatible", "arm,gic-400", sizeof("arm,gic-400"));
    __Property(fixture, "interrupt-controller", NULL, 0);
    if (variant == ResourceDisabledGic) {
        __Property(fixture, "status", "disabled", 9);
    }
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "clock");
    __Cells(fixture, "phandle", clock, 1);
    __Property(fixture, "compatible", "fixed-clock", 12);
    __Cells(fixture, "#clock-cells", zero, variant == ResourceBadClockCells ? 0 : 1);
    __Cells(fixture, "clock-frequency", frequency, 1);
    if (variant == ResourceDisabledClock) {
        __Property(fixture, "status", "disabled", 9);
    }
    __Token(fixture, FDT_END_NODE);
    __Node(fixture, "memory");
    __Property(fixture, "device_type", "memory", 7);
    __Cells(fixture, "reg", memory, 4);
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

static void
__ResourceDescriptions(void)
{
    struct Fixture fixture;
    struct FdtPciMsi msi;
    struct FdtInterrupt interrupt;
    struct FdtPciDependencies dependencies;
    struct FdtResources provider;
    size_t length;
    uint64_t physical;
    uint32_t saved;

    for (enum ResourceCase variant = ResourceValid; variant <= ResourceShortMipReg; variant++) {
        length = __ResourceFixture(&fixture, variant);
        g_count = 0;
        CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK);
        if (variant == ResourceBadLink || variant == ResourceBadReg) {
            CHECK(g_count == 0);
            continue;
        }
        CHECK(g_count == 1);
        if (variant == ResourceDisabledMip || variant == ResourceDisabledMipBus ||
            variant == ResourceMissingMip || variant == ResourceDisabledGic) {
            CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_ENOENT);
            CHECK(g_host.DmaWindows[2].Kind == FdtDmaWindowUnknown);
        } else if (variant == ResourceBadMsiCells || variant == ResourceBadGicCells) {
            CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_ENOTSUPPORTED);
        } else if (variant == ResourceShortMsiRange || variant == ResourceBadMsiOffset ||
                   variant == ResourceBadMsiCount || variant == ResourceDuplicateProvider ||
                   variant == ResourceBadProviderScalar || variant == ResourceShortMipReg) {
            CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EINVALPARAMS);
        } else {
            CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EOK);
            CHECK(msi.Controller == 17 && msi.Interrupt.Controller == 1);
            CHECK(msi.RegisterBase == 0x3000100000ULL && msi.DoorbellBase == 0xfffffff000ULL);
            CHECK(msi.Interrupt.Line == 279 && msi.InterruptCount == 8 && msi.Offset == 8);
            CHECK(g_host.DmaWindows[2].Kind == FdtDmaWindowMsi);
        }
        CHECK(g_host.DmaWindows[0].Kind == FdtDmaWindowRam);
        CHECK(g_host.DmaWindows[1].Kind == FdtDmaWindowPeer);
        CHECK(g_host.Windows[0].Attributes & FDT_PCI_PREFETCHABLE);
        CHECK(g_host.Windows[0].BusBase == 0x400000000ULL);
        CHECK(g_host.DmaWindows[0].Length == 0x1000000000ULL);
        CHECK(FdtTranslatePciAddress(&g_host, FdtPciSpaceMemory64,
            0x400001234ULL, 16, &physical) == OS_EOK);
        CHECK(physical == 0x2000001234ULL);
        CHECK(g_host.Link.MaxSpeed == 2 && g_host.Link.Lanes == 1 && g_host.Link.NoL0s);
        CHECK(!strcmp(g_host.Link.ClkreqMode, "safe") && g_host.Link.EnableSsc);
        if (variant == ResourceBadNames) {
            CHECK(FdtResolvePciNamedInterrupt(&g_host, "msi", &interrupt) == OS_EINVALPARAMS);
        } else if (variant == ResourceDisabledGic) {
            CHECK(FdtResolvePciNamedInterrupt(&g_host, "msi", &interrupt) == OS_ENOENT);
        } else if (variant != ResourceBadGicCells) {
            CHECK(FdtResolvePciNamedInterrupt(&g_host, "msi", &interrupt) == OS_EOK);
            CHECK(interrupt.Line == (variant == ResourceExtended ? 144 : 133));
            CHECK(FdtResolvePciNamedInterrupt(&g_host, "missing", &interrupt) == OS_ENOENT);
        }
        if (variant == ResourceDisabledReset || variant == ResourceDisabledClock) {
            CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_ENOENT);
        } else if (variant == ResourceShortReset || variant == ResourceBadResetCells ||
                   variant == ResourceBadClockCells) {
            CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EINVALPARAMS);
        } else {
            CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EOK);
            CHECK(dependencies.ClockFrequency == 100000000);
            CHECK(dependencies.BridgeResetController == 19 && dependencies.BridgeResetId == 44);
            CHECK(dependencies.BridgeResetBase == 0x4000001234ULL && dependencies.BridgeResetLength == 0x30);
            CHECK(dependencies.ResetBase == 0x4000001234ULL);
        }
    }
    length = __ResourceFixture(&fixture, ResourceValid);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK);
    g_host.InterruptsLength -= 4;
    interrupt.Line = -99;
    CHECK(FdtResolvePciNamedInterrupt(&g_host, "pcie", &interrupt) == OS_EINVALPARAMS);
    CHECK(interrupt.Line == -99);
    g_host.InterruptsLength += 4;
    __Word((uint8_t*)g_host.Resets + 4, UINT32_MAX);
    dependencies.ResetBase = 0xfeed;
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EINVALPARAMS);
    CHECK(dependencies.ResetBase == 0xfeed);
    // Two 24-byte banks provide IDs 0..63, not six banks of SET/CLEAR.
    __Word((uint8_t*)g_host.Resets + 4, 64);
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EINVALPARAMS);
    CHECK(dependencies.ResetBase == 0xfeed);
    __Word((uint8_t*)g_host.Resets + 4, 44);
    msi.Controller = 99;
    g_host.MsiParentLength = 0;
    CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EINVALPARAMS && msi.Controller == 99);
    g_host.MsiParentLength = 4;
    CHECK(FdtFindResources(g_host.Blob, g_host.BlobLength, 17, &provider) == OS_EOK);
    __Word((uint8_t*)provider.Reg + 16, UINT32_MAX);
    __Word((uint8_t*)provider.Reg + 20, UINT32_MAX);
    CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EINVALPARAMS);
    __Word((uint8_t*)provider.Reg + 16, 0xff);
    __Word((uint8_t*)provider.Reg + 20, 0xfffff000);
    CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EOK);
    // Provider lookup must validate the tail even when the provider precedes it.
    saved = FdtReadBe32(fixture.Bytes + 36);
    __Word(fixture.Bytes + 36, saved - 4);
    CHECK(FdtResolvePciMsi(&g_host, &msi) == OS_EINVALPARAMS);
    g_count = 0;
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EINVALPARAMS);
    CHECK(g_count == 0);
    puts("Firmware descriptions: MSI, named IRQs, resets, link policy and DMA kinds passed");
}


#include "pci_boundaries_test.inc"

int
main(
        int argc,
        char** argv)
{
    struct Fixture fixture;
    size_t length;
    uint64_t physical;
    int line;
    unsigned int flags;
    size_t index;
    struct FdtPciDependencies dependencies;
    struct FdtNode variant = { 0 };
    enum FdtPciHostType variantType;
    const char compatible[] = "brcm,bcm2711-pcie\0brcm,bcm2712-pcie";

    // Allow the Pi 5 controller checks to run independently of Pi 4 coverage.
    if (argc == 4 && !strcmp(argv[3], "--bcm2712-only")) {
        PciInitialize();
        __RealTree(argv[2], FdtPciHostBcm2712);
        return 0;
    }
    if (argc == 4 && !strcmp(argv[3], "--dma-only")) {
        __PciDmaHosts();
        __Rp1DmaRealTree(argv[2]);
        return 0;
    }
    CHECK(argc == 3);
    memset(&fixture, 0, sizeof(fixture));
    __Property(&fixture, "compatible", compatible, sizeof(compatible));
    variant.Properties = fixture.Bytes;
    variant.PropertiesLength = fixture.Used;
    variant.Strings = (const char*)fixture.Strings;
    variant.StringsLength = fixture.StringsUsed;
    CHECK(FdtPciHostType(&variant, &variantType) && variantType == FdtPciHostBcm2712);
    length = __Fixture(&fixture, 0, 0, 0, 0);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK);
    CHECK(g_count == 1 && g_host.EcamBase == 0x80100000 && g_host.BusEnd == 1);
    CHECK(g_host.DmaWindowCount == 1 && g_host.DmaWindows[0].PhysicalBase == 0x90000000);
    CHECK(FdtTranslatePciAddress(&g_host, FdtPciSpaceMemory32,
        0x40000100, 0x100, &physical) == OS_EOK);
    CHECK(physical == 0x81000100);
    CHECK(FdtTranslatePciAddress(&g_host, FdtPciSpaceMemory32,
        0x400fff00, 0x200, &physical) == OS_ENOENT);
    CHECK(FdtTranslatePciAddress(&g_host, FdtPciSpaceMemory64,
        0x40000100, 0x100, &physical) == OS_EOK);
    CHECK(FdtTranslatePciAddress(&g_host, FdtPciSpaceIo,
        0x40000100, 0x100, &physical) == OS_ENOENT);
    CHECK(FdtResolvePciInterrupt(&g_host, 1, 3, 7, 1, &line, &flags) == OS_EOK);
    CHECK(line == 332 && (flags & INTERRUPT_ACPICONFORM_TRIGGERMODE));
    CHECK(!(flags & INTERRUPT_ACPICONFORM_POLARITY));
    CHECK(FdtResolvePciInterrupt(&g_host, 0, 4, 0, 1, &line, &flags) == OS_ENOENT);
    CHECK(FdtResolvePciInterrupt(&g_host, 0, 3, 0, 0, &line, &flags) == OS_EINVALPARAMS);
    __PciIntegration();
    __HostConstruction();
    __PublicationLifetime();
    __MalformedBridgeRoutes();
#ifdef __OSCONFIG_HAS_LEGACY_PCI
    __LegacyRootScanning();
#endif
    __RuntimePciHosts();
    __HostRegistration();
    __FirmwareDomains();
    __PciDmaHosts();
    __BcmAcceptance(NULL);
    __BcmDmaWindows();
    __BcmMultipleHosts();
    __EcamMultipleHosts();
    for (index = 0; index < length; index++) {
        CHECK(FdtEnumeratePciHosts(fixture.Bytes, index, __Host, NULL) != OS_EOK);
    }
    g_count = 0;
    length = __Fixture(&fixture, 1, 0, 0, 0);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK && g_count == 0);
    length = __Fixture(&fixture, 0, 1, 0, 0);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK && g_count == 0);
    length = __Fixture(&fixture, 0, 0, 1, 0);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK);
    CHECK(FdtResolvePciInterrupt(&g_host, 0, 3, 0, 1, &line, &flags) == OS_EINVALPARAMS);
    length = __Fixture(&fixture, 0, 0, 0, 1);
    CHECK(FdtEnumeratePciHosts(fixture.Bytes, length, __Host, NULL) == OS_EOK);
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_EOK);
    CHECK(dependencies.ResetBase == 0x80003000 && dependencies.ResetLength == 12);
    CHECK(dependencies.ClockFrequency == 100000000);
    __BcmDependencyAcceptance(&g_host);
    g_host.ClockNames = (const uint8_t*)"unknown";
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_ENOTSUPPORTED);
    g_host.ClockNames = (const uint8_t*)"sw_pcie";
    __Word((uint8_t*)g_host.Clocks, 0xFFFF);
    CHECK(FdtResolvePciDependencies(&g_host, &dependencies) == OS_ENOENT);
    __ResourceDescriptions();
    __RealTree(argv[1], FdtPciHostBcm2711);
    __RealTree(argv[2], FdtPciHostBcm2712);
    puts("PCI hosts: translation, BCM2711 ID/BAR, modeled DMA/INTx and activation gate passed");
    return 0;
}
