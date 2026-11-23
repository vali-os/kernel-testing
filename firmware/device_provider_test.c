#include "../../services/deviced/core/io.c"
#include <stdlib.h>

static void* __RegistrationCalloc(size_t count, size_t size);
#define calloc __RegistrationCalloc
#define __OSCONFIG_NODRIVERS
static oserr_t __TestRemoveDeviceDma(uuid_t device);
#define DmDmaRemoveDevice __TestRemoveDeviceDma
#ifndef DMA_REGISTRY_SOURCE
#define DMA_REGISTRY_SOURCE "../../services/deviced/core/devices.c"
#endif
#include DMA_REGISTRY_SOURCE
#undef DmDmaRemoveDevice
// The mutation run substitutes only the DMA registry implementation.
#ifndef DMA_DEVICE_SOURCE
#define DMA_DEVICE_SOURCE "../../services/deviced/core/dma-device.c"
#endif
#include DMA_DEVICE_SOURCE
#include "../../services/deviced/core/dma.c"
#undef calloc
static void* __PublicationCalloc(size_t count, size_t size);
static oserr_t __PublicationBind(uuid_t id);
#define calloc __PublicationCalloc
#define DmDeviceEnableDriverBinding __PublicationBind
// Kernel registration is exercised by dma_service_test. This fixture keeps
// publication/registry failure injection independent of the manager transport.
static unsigned int g_publicationDmaCalls;
static unsigned int g_failPublicationDmaAt;
static oserr_t __TestPublicationDma(uuid_t device)
{
    (void)device;
    g_publicationDmaCalls++;
    return g_publicationDmaCalls == g_failPublicationDmaAt ? OS_EOOM : OS_EOK;
}
#define DmDmaRegisterDevice __TestPublicationDma
#include "../../services/deviced/core/publication.c"
#undef DmDmaRegisterDevice
#undef DmDeviceEnableDriverBinding
#undef calloc
#ifndef PCI_DEVICE_SOURCE
#define PCI_DEVICE_SOURCE "../../services/deviced/bus/pci/device.c"
#endif
#include PCI_DEVICE_SOURCE
#include "../../services/deviced/bus/pci/io.c"
#include "../../services/deviced/bus/pci/dma.c"
#include "../../services/deviced/bus/pci/msi.c"
#include "../../services/deviced/bus/pci/msix.c"
#include "../../services/deviced/bus/rp1/rp1.c"
#include "../../services/deviced/bus/rp1/publish.c"
#include "../../services/deviced/bus/rp1/resources.c"
#include "../../services/deviced/firmware/resources.c"
#include "../../librt/libds/list.c"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static void __AssignDriver(uuid_t device, uint64_t principal)
{
    DmDevice_t* entry = DmDeviceFindLocked(device);
    CHECK(entry != NULL);
    entry->DriverId = 99;
    entry->DriverPrincipal = principal;
}

static void __CheckRp1RuntimeLock(struct Rp1Bus* bus, int needsPci);
#include "rp1_runtime_stub.inc"

static bool g_pauseStopRequest;
static DmDevice_t* g_pausedStopDevice;

static oserr_t __TestRemoveDeviceDma(uuid_t deviceId)
{
    oserr_t status = DmDmaRemoveDevice(deviceId);
    if (status != OS_EOK) return status;
    if (g_pauseStopRequest) {
        // Model a StopDma callback admitted after removal dropped the registry
        // mutex. Keep its counted request until the test resumes cleanup.
        usched_mtx_lock(&g_dmDevicesLock);
        g_pausedStopDevice = DmDeviceFindLocked(deviceId);
        g_pausedStopDevice->ActiveRequests++;
        usched_mtx_unlock(&g_dmDevicesLock);
    }
    return OS_EOK;
}

static unsigned int g_registryLocks;
static unsigned int g_pciLocks;
static struct usched_mtx* g_rp1Locks[8];
static unsigned int g_rp1LockDepth;
static unsigned int g_rp1LockEntries;
static unsigned int g_forgotten;
static int g_failAllocation;

static void
__CheckRp1RuntimeLock(
    _In_ struct Rp1Bus* bus,
    _In_ int            needsPci)
{
    if (needsPci) {
        CHECK(g_pciLocks == 1);
    }
    CHECK(g_rp1LockDepth != 0);
    CHECK(g_rp1Locks[g_rp1LockDepth - 1] == &bus->Lock);
}

int
PciIsMsiAvailable(void)
{
    // This fixture exercises bus control, not interrupt transport discovery.
    CHECK(0);
    return 0;
}

oserr_t
PciMsixDeviceProbe(
    _In_ struct PciDevice* device)
{
    (void)device;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

oserr_t
PciMsixDeviceControl(
    _In_ struct PciDevice* device,
    _In_ const struct PciInterruptRequest* request)
{
    (void)device;
    (void)request;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}

// Capture only the operation being asserted; unrelated fixtures may also warn.
static struct {
    enum OSSysLogLevel Level;
    char Text[256];
} g_diagnostics[32];
static unsigned int g_diagnosticCount;
static int g_captureDiagnostics;

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char* format,
    ...)
{
    va_list arguments;

    if (!g_captureDiagnostics ||
        (level != OSSYSLOGLEVEL_WARNING && level != OSSYSLOGLEVEL_ERROR)) {
        return;
    }
    CHECK(g_diagnosticCount < sizeof(g_diagnostics) / sizeof(g_diagnostics[0]));
    g_diagnostics[g_diagnosticCount].Level = level;
    va_start(arguments, format);
    vsnprintf(g_diagnostics[g_diagnosticCount].Text,
        sizeof(g_diagnostics[g_diagnosticCount].Text), format, arguments);
    va_end(arguments);
    g_diagnosticCount++;
}

static void
__CaptureDiagnostics(void)
{
    g_diagnosticCount = 0;
    g_captureDiagnostics = 1;
}

static int
__HasDiagnostic(
    _In_ enum OSSysLogLevel level,
    _In_ const char* text)
{
    unsigned int index;

    for (index = 0; index < g_diagnosticCount; index++) {
        if (g_diagnostics[index].Level == level &&
            strstr(g_diagnostics[index].Text, text) != NULL) {
            return 1;
        }
    }
    return 0;
}

/** Records which object receives a request and when its reference is released. */
struct __TestProvider {
    uuid_t DeviceId;
    struct DmPublicationGroup* Group;
    unsigned int References;
    unsigned int Retains;
    unsigned int Releases;
    unsigned int Controls;
    unsigned int RegisterRequests;
    int FailRetain;
    int RemoveDuringRequest;
    size_t Value;
};

static void*
__RegistrationCalloc(
    size_t count,
    size_t size)
{
    if (g_failAllocation) {
        return NULL;
    }
    return calloc(count, size);
}

void
usched_mtx_init(
    struct usched_mtx* mutex,
    int type)
{
    (void)type;
    memset(mutex, 0, sizeof(*mutex));
}

void
usched_mtx_lock(
    struct usched_mtx* mutex)
{
    unsigned int index;

    if (mutex == &g_dmDmaLock) {
        CHECK(mutex->references == 0 && g_registryLocks == 0 && g_rp1LockDepth == 0);
        mutex->references = 1;
        return;
    }
    if (mutex == &g_dmDevicesLock) {
        CHECK(g_registryLocks == 0 && g_rp1LockDepth == 0);
        g_registryLocks++;
        return;
    }
    CHECK(g_registryLocks == 0 && g_rp1LockDepth < 8);
    for (index = 0; index < g_rp1LockDepth; index++) {
        CHECK(g_rp1Locks[index] != mutex);
    }
    g_rp1Locks[g_rp1LockDepth++] = mutex;
    g_rp1LockEntries++;
}

void
usched_mtx_unlock(
    struct usched_mtx* mutex)
{
    if (mutex == &g_dmDmaLock) {
        CHECK(mutex->references == 1 && g_registryLocks == 0 && g_rp1LockDepth == 0);
        mutex->references = 0;
        return;
    }
    if (mutex == &g_dmDevicesLock) {
        CHECK(g_registryLocks == 1 && g_rp1LockDepth == 0);
        g_registryLocks--;
        return;
    }
    CHECK(g_rp1LockDepth != 0 && g_rp1Locks[g_rp1LockDepth - 1] == mutex);
    g_rp1LockDepth--;
}

void
spinlock_init(
    _In_ spinlock_t* lock)
{
    memset(lock, 0, sizeof(*lock));
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    CHECK(lock->next == lock->current);
    lock->next++;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    lock->current++;
}

void
PciCriticalSectionEnter(void)
{
    CHECK(g_pciLocks == 0 && g_registryLocks == 0 && g_rp1LockDepth == 0);
    g_pciLocks++;
}

void
PciCriticalSectionLeave(void)
{
    CHECK(g_pciLocks == 1);
    g_pciLocks--;
}

void
DmDiscoverForgetDevice(
    uuid_t deviceId)
{
    CHECK(g_registryLocks == 0);
    CHECK(DmDeviceFindLocked(deviceId) == NULL);
    g_forgotten++;
}

static oserr_t
__ProviderRetain(
    _In_ void* context)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0);
    provider->Retains++;
    if (provider->FailRetain) {
        return OS_EUNKNOWN;
    }
    provider->References++;
    return OS_EOK;
}

static void
__ProviderRelease(
    _In_ void* context)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References != 0);
    provider->References--;
    provider->Releases++;
}

static void
__RemoveDuringRequest(
    _In_ struct __TestProvider* provider)
{
    unsigned int forgotten = g_forgotten;
    char diagnostic[256];

    // Group tests must remove earlier siblings too while this request is active.
    if (provider->Group != NULL) {
        __CaptureDiagnostics();
        CHECK(DmPublicationRemove(provider->Group) == OS_EBUSY);
        g_captureDiagnostics = 0;
        snprintf(diagnostic, sizeof(diagnostic),
            "DmPublicationRemove: device %u removal failed: %u; keeping remaining entries",
            provider->DeviceId, OS_EBUSY);
        CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
        CHECK(provider->Group->State == DmPublicationRemoving);
        CHECK(DmPublicationEnableBinding(provider->Group) == OS_EBUSY);
        CHECK(DmPublicationFinish(provider->Group) == OS_EBUSY);
        // A sibling was removed before reaching the busy provider.
        forgotten++;
    } else {
        CHECK(DmDeviceDestroy(provider->DeviceId) == OS_EBUSY);
    }
    CHECK(provider->References == 1 && provider->Releases == 0);
    CHECK(g_forgotten == forgotten);
    CHECK(!DmDeviceIsBindable(provider->DeviceId));
    CHECK(DmDeviceEnableDriverBinding(provider->DeviceId) == OS_EBUSY);
}

static oserr_t
__FirstControl(
    _In_ void* context,
    _In_ uuid_t principal,
    _In_ enum OSIOCtlRequest request,
    _In_ void* buffer,
    _In_ size_t length)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References == 1);
    CHECK(principal == 700);
    CHECK(request == OSIOCTLREQUEST_BUS_CONTROL);
    CHECK(buffer != NULL && length == sizeof(struct OSIOCtlBusControl));
    provider->Controls++;
    if (provider->RemoveDuringRequest) {
        __RemoveDuringRequest(provider);
    }
    return OS_EOK;
}

static oserr_t
__SecondControl(
    _In_ void* context,
    _In_ uuid_t principal,
    _In_ enum OSIOCtlRequest request,
    _In_ void* buffer,
    _In_ size_t length)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References == 1);
    (void)request;
    CHECK(principal == 700);
    (void)buffer;
    (void)length;
    provider->Controls += 10;
    return OS_ENOTSUPPORTED;
}

static oserr_t
__ProviderAccessRegister(
    _In_ void* context,
    _In_ int direction,
    _In_ unsigned int reg,
    _InOut_ size_t* value,
    _In_ size_t width)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References == 1);
    CHECK(reg == 12 && width == 4);
    provider->RegisterRequests++;
    if (provider->RemoveDuringRequest) {
        __RemoveDuringRequest(provider);
    }
    if (direction == DEVICEMANAGER_IOCTL_EXT_READ) {
        *value = provider->Value;
    } else {
        provider->Value = *value;
    }
    return OS_EOK;
}

static const struct DmDeviceBusOperations g_firstOperations = {
    .AddReference = __ProviderRetain,
    .Release = __ProviderRelease,
    .Control = __FirstControl,
    .AccessRegister = __ProviderAccessRegister
};

static const struct DmDeviceBusOperations g_secondOperations = {
    .AddReference = __ProviderRetain,
    .Release = __ProviderRelease,
    .Control = __SecondControl
};

static Device_t*
__NewDescription(void)
{
    Device_t* description = calloc(1, sizeof(*description));

    CHECK(description != NULL);
    description->Length = sizeof(*description);
    return description;
}

static void
__TestProviders(void)
{
    struct __TestProvider first = { .Value = 0x12345678 };
    struct __TestProvider second = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionGeneric,
        .Bus = { .Operations = &g_firstOperations, .Context = &first }
    };
    struct OSIOCtlBusControl control = { 0 };
    size_t value = 0;
    unsigned int mode;
    char diagnostic[256];

    CHECK(DmDeviceCreateWithBus(&registration, 0, &first.DeviceId) == OS_EOK);
    __AssignDriver(first.DeviceId, 700);
    registration.Description = __NewDescription();
    registration.Bus.Operations = &g_secondOperations;
    registration.Bus.Context = &second;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &second.DeviceId) == OS_EOK);
    __AssignDriver(second.DeviceId, 700);
    CHECK(first.References == 1 && second.References == 1);
    CHECK(DmHandleIoctl(700, first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    __CaptureDiagnostics();
    CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    g_captureDiagnostics = 0;
    snprintf(diagnostic, sizeof(diagnostic),
        "DmHandleIoctl: device %u caller 700 request 0x%x failed: %u",
        second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL, OS_ENOTSUPPORTED);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
    CHECK(first.Controls == 1 && second.Controls == 10);
    CHECK(DmHandleIoctl(700, first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control) - 1) == OS_EINVALPARAMS);
    CHECK(first.Controls == 1);
    CHECK(DmHandleIoctl2(700, first.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        12, 0, 4, &value) == OS_EOK && value == first.Value);
    CHECK(DmHandleIoctl2(700, first.DeviceId, DEVICEMANAGER_IOCTL_EXT_WRITE,
        12, 99, 4, &value) == OS_EOK && first.Value == 99);
    CHECK(DmHandleIoctl2(700, second.DeviceId, 0, 0, 55, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(value == 55 && second.RegisterRequests == 0);
    // Retired DMA ioctl slots now carry Ethernet requests. Both must reach the
    // assigned device's bus callback, whose status is returned unchanged.
    CHECK(OSIOCTLREQUEST_ETHERNET_CONFIG == (7 | __IOCTL_IN));
    CHECK(OSIOCTLREQUEST_ETHERNET_PHY_RESET == 8);
    CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_ETHERNET_CONFIG,
        &value, sizeof(value)) == OS_ENOTSUPPORTED);
    CHECK(second.Controls == 20);
    CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_ETHERNET_PHY_RESET,
        &value, sizeof(value)) == OS_ENOTSUPPORTED);
    CHECK(second.Controls == 30);
    CHECK(DmHandleIoctl(701, second.DeviceId, OSIOCTLREQUEST_ETHERNET_CONFIG,
        &value, sizeof(value)) == OS_EPERMISSIONS);
    CHECK(second.Controls == 30);
    CHECK(DmDeviceDestroy(second.DeviceId) == OS_EOK);
    CHECK(second.References == 0 && second.Releases == 1);
    CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK);
    CHECK(first.References == 0 && first.Releases == 1);

    for (mode = 0; mode < 2; mode++) {
        memset(&first, 0, sizeof(first));
        first.RemoveDuringRequest = 1;
        registration.Description = __NewDescription();
        registration.Bus.Operations = &g_firstOperations;
        registration.Bus.Context = &first;
        CHECK(DmDeviceCreateWithBus(&registration, DEVICE_REGISTER_FLAG_LOADDRIVER,
            &first.DeviceId) == OS_EOK);
        __AssignDriver(first.DeviceId, 700);
        if (mode == 0) {
            CHECK(DmHandleIoctl(700, first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
                &control, sizeof(control)) == OS_EOK);
        } else {
            CHECK(DmHandleIoctl2(700, first.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
                12, 0, 4, &value) == OS_EOK);
        }
        // Lifecycle calls cannot re-enter a bus callback. Check later requests
        // only after the earlier dispatch has released its lifecycle scope.
        CHECK(DmHandleIoctl(700, first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
            &control, sizeof(control)) == OS_EBUSY);
        CHECK(DmHandleIoctl2(700, first.DeviceId, 0, 0, 0, 4, &value) == OS_EBUSY);
        CHECK(first.References == 1 && first.Releases == 0);
        CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK);
        CHECK(first.References == 0 && first.Releases == 1);
        CHECK(DmHandleIoctl2(700, first.DeviceId, 0, 0, 0, 4, &value) == OS_ENOENT);
    }
}

static void
__TestRegistrationFailures(void)
{
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionBus,
        .Bus = { .Operations = &g_firstOperations, .Context = &provider }
    };
    struct DmDeviceBusOperations incomplete = g_firstOperations;
    uuid_t id = 123;
    uuid_t parentId;
    uuid_t childId;
    Device_t* child;
    Device_t* rejected;
    char diagnostic[256];

    CHECK(DmDeviceCreateWithBus(&registration, 0, &id) == OS_EINVALPARAMS);
    CHECK(id == UUID_INVALID && provider.Retains == 0);
    registration.Kind = DmDeviceDescriptionGeneric;
    incomplete.Release = NULL;
    registration.Bus.Operations = &incomplete;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &id) == OS_EINVALPARAMS);
    CHECK(provider.Retains == 0);
    registration.Bus.Operations = &g_firstOperations;
    g_failAllocation = 1;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &id) == OS_EOOM);
    CHECK(provider.Retains == 0 && id == UUID_INVALID);
    g_failAllocation = 0;
    provider.FailRetain = 1;
    __CaptureDiagnostics();
    CHECK(DmDeviceCreateWithBus(&registration, 0, &id) == OS_EUNKNOWN);
    g_captureDiagnostics = 0;
    CHECK(provider.Retains == 1 && provider.Releases == 0 && provider.References == 0);
    snprintf(diagnostic, sizeof(diagnostic),
        "DmDeviceCreateWithBus: failed to add bus reference: %u", OS_EUNKNOWN);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
    provider.FailRetain = 0;
    registration.Description->ParentId = 0x7fffffff;
    __CaptureDiagnostics();
    CHECK(DmDeviceCreateWithBus(&registration, 0, &id) == OS_ENOENT);
    g_captureDiagnostics = 0;
    CHECK(provider.Retains == 2 && provider.Releases == 1 && provider.References == 0);
    snprintf(diagnostic, sizeof(diagnostic),
        "DmDeviceCreateWithBus: parent %u is absent or being removed", 0x7fffffff);
    CHECK(__HasDiagnostic(OSSYSLOGLEVEL_WARNING, diagnostic));
    registration.Description->ParentId = UUID_INVALID;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &parentId) == OS_EOK);
    __AssignDriver(parentId, 700);
    child = __NewDescription();
    child->ParentId = parentId;
    CHECK(DmDeviceCreate(child, 0, &childId) == OS_EOK);
    CHECK(DmDeviceDestroy(parentId) == OS_EBUSY);
    CHECK(provider.References == 1);
    rejected = __NewDescription();
    rejected->ParentId = parentId;
    CHECK(DmDeviceCreate(rejected, 0, &id) == OS_ENOENT && id == UUID_INVALID);
    free(rejected);
    CHECK(DmDeviceDestroy(childId) == OS_EOK);
    CHECK(DmDeviceDestroy(parentId) == OS_EOK && provider.References == 0);
}

static void
__TestDescriptionsWithoutProviders(void)
{
    BusDevice_t* bus = calloc(1, sizeof(*bus));
    UsbDevice_t* usb = calloc(1, sizeof(*usb));
    PlatformDevice_t* platform = calloc(1, sizeof(*platform));
    struct OSIOCtlBusControl control = { 0 };
    uuid_t id;
    size_t value = 0;

    CHECK(bus != NULL && usb != NULL && platform != NULL);
    bus->Base.Length = sizeof(*bus);
    bus->Type = BusTypePci;
    CHECK(DmDeviceCreate(&bus->Base, 0, &id) == OS_EOK);
    CHECK(DmDeviceFindLocked(id)->Kind == DmDeviceDescriptionBus);
    __AssignDriver(id, 700);
    CHECK(DmHandleIoctl(700, id, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(700, id, 0, 0, 0, 4, &value) == OS_ENOTSUPPORTED);
    DmDeviceFindLocked(id)->DriverId = 17;
    CHECK(DmDeviceDestroy(id) == OS_EOK);
    usb->Base.Length = sizeof(*usb);
    CHECK(DmDeviceCreate(&usb->Base, 0, &id) == OS_EOK);
    CHECK(DmDeviceFindLocked(id)->Kind == DmDeviceDescriptionUsb);
    CHECK(DmDeviceDestroy(id) == OS_EOK);
    platform->Base.Length = sizeof(*platform);
    CHECK(DmDeviceCreate(&platform->Base, 0, &id) == OS_EINVALPARAMS);
    free(platform);
}

/** Records configuration accesses for two functions with the same PCI address. */
struct __PciRegisters {
    size_t Value;
    unsigned int Reads;
    unsigned int Writes;
};

static size_t
__ReadPciRegister(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t width)
{
    struct __PciRegisters* registers = host->OpContext;

    (void)bus; (void)slot; (void)function; (void)reg; (void)width;
    CHECK(g_pciLocks == 1);
    registers->Reads++;
    return registers->Value;
}

static void
__WritePciRegister(
    _In_ PciHost_t* host,
    _In_ unsigned int bus,
    _In_ unsigned int slot,
    _In_ unsigned int function,
    _In_ size_t reg,
    _In_ size_t value,
    _In_ size_t width)
{
    struct __PciRegisters* registers = host->OpContext;

    (void)bus; (void)slot; (void)function; (void)reg; (void)width;
    CHECK(g_pciLocks == 1);
    registers->Writes++;
    registers->Value = value;
}

static oserr_t g_pciDmaStatus;

static oserr_t
__TestPciDmaRoute(PciHost_t* host, struct PciDmaDescription* description)
{
    (void)host;
    CHECK(g_pciLocks == 1 && g_registryLocks == 0);
    description->CachePolicy = DmDmaCacheCoherent;
    description->Map.Count = 1;
    description->Map.Ranges[0] = (FdtDmaRange_t) {
        .PhysicalBase = 0x4000, .DeviceBase = 0x1000, .Length = 0x2000
    };
    return g_pciDmaStatus;
}

static void
__TestPciProvider(void)
{
    static const struct PciHostOperations operations = {
        .Read = __ReadPciRegister,
        .Write = __WritePciRegister,
        .GetDmaDescription = __TestPciDmaRoute
    };
    static const struct PciFunctionHandler handler = { .BlockActivation = 1 };
    struct __PciRegisters firstRegisters = { .Value = 0x11223344 };
    struct __PciRegisters secondRegisters = { .Value = 0x55667788 };
    PciHost_t firstHost = {
        .Identification = { .HostId = 17, .Segment = 1, .BusEnd = 1 },
        .Operations = &operations,
        .OpContext = &firstRegisters,
        .IsExtended = 1
    };
    PciHost_t secondHost = {
        .Identification = { .Segment = 2, .BusEnd = 1 },
        .Operations = &operations,
        .OpContext = &secondRegisters
    };
    PciDevice_t root = { .DeviceId = 117 };
    PciDevice_t first = { .Host = &firstHost, .Bus = 1, .Slot = 2 };
    PciDevice_t second = { .Host = &secondHost, .Bus = 1, .Slot = 2 };
    struct DmDeviceRegistration registration = {
        .Kind = DmDeviceDescriptionBus,
        .Bus = { .Operations = &g_pciDeviceBusOperations, .Context = &first }
    };
    struct OSIOCtlBusControl control = { .Flags = DEVICEMANAGER_IOCTL_ENABLE };
    BusDevice_t* description;
    DmDmaDescription_t route;
    DmDmaDescription_t savedRoute;
    struct DmDmaLease* lease = NULL;
    size_t value;
    unsigned int reads;
    unsigned int writes;

    // No PCI lookup list exists in this test. A description's address is not
    // consulted when choosing the function that handles the request.
    description = calloc(1, sizeof(*description));
    CHECK(description != NULL);
    description->Base.Length = sizeof(*description);
    description->Segment = 99;
    registration.Description = &description->Base;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &first.DeviceId) == OS_EOK);
    __AssignDriver(first.DeviceId, 700);
    description = calloc(1, sizeof(*description));
    CHECK(description != NULL);
    description->Base.Length = sizeof(*description);
    registration.Description = &description->Base;
    registration.Bus.Context = &second;
    CHECK(DmDeviceCreateWithBus(&registration, 0, &second.DeviceId) == OS_EOK);
    __AssignDriver(second.DeviceId, 700);
    CHECK(first.HostBusReferences == 1 && second.HostBusReferences == 1);
    firstHost.RootDevice = &root;

    // Route queries read the host's configured translation without touching
    // device registers. Active preparation must return the same description.
    CHECK(DmDeviceDescribeDma(first.DeviceId, &route) == OS_EOK);
    CHECK(route.DeviceId == first.DeviceId && route.HostId == 117);
    CHECK(route.AddressLimit == UINT64_MAX && route.CachePolicy == DmDmaCacheCoherent);
    CHECK(route.Map.Count == 1 && route.Map.Ranges[0].PhysicalBase == 0x4000);
    CHECK(route.Map.Ranges[0].DeviceBase == 0x1000 && route.Map.Ranges[0].Length == 0x2000);
    CHECK(firstRegisters.Reads == 0 && firstRegisters.Writes == 0);
    CHECK(DmDevicePrepareDmaForPrincipal(first.DeviceId, 700, &lease) == OS_EOK);
    CHECK(!memcmp(&route, DmDmaLeaseGetDescription(lease), sizeof(route)));
    DmDmaLeaseRelease(&lease);
    CHECK(firstRegisters.Reads == 0 && firstRegisters.Writes == 0);
    savedRoute = route;
    g_pciDmaStatus = OS_EUNKNOWN;
    CHECK(DmDeviceDescribeDma(first.DeviceId, &route) == OS_EUNKNOWN);
    CHECK(!memcmp(&route, &savedRoute, sizeof(route)));
    CHECK(firstRegisters.Reads == 0 && firstRegisters.Writes == 0);
    g_pciDmaStatus = OS_EOK;
    CHECK(DmHandleIoctl2(700, first.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        0x100, 0, 4, &value) == OS_EOK && value == 0x11223344);
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        0x10, 0, 4, &value) == OS_EOK && value == 0x55667788);
    // All valid byte/word/dword accesses to the standard header are mediated.
    // Denial must preserve the supplied value and avoid a configuration write.
    {
        unsigned int width;
        unsigned int offset;
        enum DeviceStage stage = DeviceStart;
        struct OSIOCtlBusControl enable = { .Flags = DEVICEMANAGER_IOCTL_BUSMASTER_ENABLE };
        unsigned int before = secondRegisters.Writes;
        for (width = 1; width <= 4; width *= 2) {
            for (offset = 0; offset < 0x40; offset += width) {
                CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_WRITE,
                    offset, 42, width, &value) == OS_EPERMISSIONS && value == 42);
            }
        }
        CHECK(DmHandleIoctl2((1ULL << 32) | 700, second.DeviceId,
            DEVICEMANAGER_IOCTL_EXT_WRITE, 0x40, 42, 4, &value) == OS_EPERMISSIONS);
        CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
            &enable, sizeof(enable)) == OS_ENOTSUPPORTED);
        CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_DEVICE_ACTIVATE,
            &stage, sizeof(stage)) == OS_ENOTSUPPORTED);
        CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_ETHERNET_CONFIG,
            &value, sizeof(value)) == OS_ENOTSUPPORTED && value == 42);
        CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_ETHERNET_PHY_RESET,
            &value, sizeof(value)) == OS_ENOTSUPPORTED && value == 42);
        CHECK(secondRegisters.Writes == before);
    }
    reads = secondRegisters.Reads;
    writes = secondRegisters.Writes;
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        0x100, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        3, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        0, 0, 3, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(700, second.DeviceId, 123,
        0, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(secondRegisters.Reads == reads && secondRegisters.Writes == writes);
    second.Handler = &handler;
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_WRITE,
        0x40, 0, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl(700, second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_READ,
        0, 0, 4, &value) == OS_EOK);
    second.Handler = NULL;
    CHECK(DmHandleIoctl2(700, second.DeviceId, DEVICEMANAGER_IOCTL_EXT_WRITE,
        0x40, 42, 4, &value) == OS_EOK && secondRegisters.Value == 42);
    CHECK(DmHandleIoctl(700, first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK && first.HostBusReferences == 0);
    CHECK(second.HostBusReferences == 1);
    CHECK(DmDeviceDestroy(second.DeviceId) == OS_EOK && second.HostBusReferences == 0);
}

#include "publication_test.inc"
// The shared RP1 fixture's older fail-on-log stub is not used by this harness.
#define OSSysLogWrite __UnusedRp1SystemDebug
#include "rp1_provider_test.inc"
#undef OSSysLogWrite
#include "dma_lease_test.inc"
#include "device_resource_test.inc"

static void __TestRemovalStopOverlap(void)
{
    struct __TestProvider busState = { 0 };
    DmDeviceRegistration_t registration = {
        .Description = __NewDescription(), .Kind = DmDeviceDescriptionGeneric,
        .Bus = { .Operations = &g_firstOperations, .Context = &busState }
    };
    CHECK(DmDeviceCreateWithBus(&registration, 0, &busState.DeviceId) == OS_EOK);
    __AssignDriver(busState.DeviceId, 700);
    g_pauseStopRequest = true;
    CHECK(DmDeviceDestroy(busState.DeviceId) == OS_EBUSY);
    CHECK(busState.References == 1 && busState.Releases == 0);
    CHECK(g_pausedStopDevice->ActiveRequests == 1 && g_pausedStopDevice->Removing);
    g_pauseStopRequest = false;
    DmDeviceRequestRelease(g_pausedStopDevice);
    CHECK(DmDeviceDestroy(busState.DeviceId) == OS_EOK && busState.Releases == 1);
    g_pausedStopDevice = NULL;
}

int
main(void)
{
    DmDevicesInitialize();
    __TestRemovalStopOverlap();
    __TestProviders();
    __TestRegistrationFailures();
    __TestDescriptionsWithoutProviders();
    __TestPciProvider();
    __TestPublicationFailures();
    __TestPublicationLifecycle();
    __TestRp1Providers();
    __TestDmaDescriptions();
    __TestDmaLeases();
    __TestRp1DmaLease();
    __TestRp1ResourcePath();
    CHECK(g_devices.count == 0 && g_registryLocks == 0 && g_pciLocks == 0);
    CHECK(g_rp1LockDepth == 0);
    puts("Device providers: dispatch, ownership, removal during requests, description kinds and PCI access passed");
    return 0;
}

// Ethernet hardware and firmware behavior is covered by ethernet_test.c.
// These registry fixtures have neither a firmware tree nor GEM hardware.
oserr_t FdtEthernetConfiguration(const struct FdtTree* tree, uint32_t node,
    struct DeviceEthernetConfiguration* config)
{
    (void)tree; (void)node; (void)config;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}
oserr_t Rp1EthernetControl(struct Rp1Child* child, uuid_t principal,
    enum OSIOCtlRequest request, const void* buffer, size_t length)
{
    (void)child; (void)principal; (void)request; (void)buffer; (void)length;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}
oserr_t Rp1EthernetRelease(struct Rp1Child* child)
{
    CHECK(child->Ethernet == NULL);
    return OS_EOK;
}
int Rp1EthernetIsDevice(const struct Rp1Child* child)
{
    CHECK(child->Ethernet == NULL);
    return 0;
}

oserr_t Rp1EthernetStatus(const struct Rp1Child* child, uuid_t principal)
{
    (void)principal;
    CHECK(child->Ethernet == NULL);
    return OS_EOK;
}

// These older fixtures create no manager attachments. Catch unexpected kernel
// registration instead of pretending a real attachment was installed.
oserr_t DMAAttachmentRegister(uuid_t manager, const OSDmaAttachmentParameters_t* parameters, OSDmaAttachmentId_t* output)
{ (void)manager; (void)parameters; (void)output; CHECK(0); return OS_EUNKNOWN; }
oserr_t DMAAttachmentRemove(uuid_t manager, const OSDmaAttachmentId_t* id)
{ (void)manager; (void)id; CHECK(0); return OS_EUNKNOWN; }

oserr_t DmDmaDrainEventsLocked(void)
{ CHECK(g_dmDmaState.Devices == NULL); return OS_EOK; }

oserr_t DMAAttachmentBind(uuid_t manager, const OSDmaBindParameters_t* request, OSDmaBindingResult_t* result)
{ (void)manager; (void)request; (void)result; CHECK(0); return OS_ENOTSUPPORTED; }
