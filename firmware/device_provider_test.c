#include <stdlib.h>

static void* __RegistrationCalloc(size_t count, size_t size);
#define calloc __RegistrationCalloc
#define __OSCONFIG_NODRIVERS
#include "../../services/deviced/core/devices.c"
#undef calloc
static void* __PublicationCalloc(size_t count, size_t size);
static oserr_t __PublicationBind(uuid_t id);
#define calloc __PublicationCalloc
#define DmDeviceEnableDriverBinding __PublicationBind
#include "../../services/deviced/core/publication.c"
#undef DmDeviceEnableDriverBinding
#undef calloc
#include "../../services/deviced/bus/pci/device.c"
#include "../../services/deviced/bus/pci/io.c"
#include "../../services/deviced/bus/rp1/rp1.c"
#include "../../services/deviced/bus/rp1/publish.c"
#include "../../librt/libds/list.c"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static unsigned int g_registryLocks;
static unsigned int g_pciLocks;
static unsigned int g_forgotten;
static int g_failAllocation;
static int g_removeDuringDriverRequest;

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
    CHECK(mutex == &g_devicesLock && g_registryLocks == 0);
    g_registryLocks++;
}

void
usched_mtx_unlock(
    struct usched_mtx* mutex)
{
    CHECK(mutex == &g_devicesLock && g_registryLocks == 1);
    g_registryLocks--;
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
    CHECK(g_pciLocks == 0 && g_registryLocks == 0);
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
    CHECK(__GetDeviceUnsafe(deviceId) == NULL);
    g_forgotten++;
}

oserr_t
OSDeviceIOCtl2(
    uuid_t deviceId,
    uuid_t driverId,
    enum OSIOCtlRequest request,
    void* buffer,
    size_t length)
{
    (void)buffer;
    (void)length;
    CHECK(g_registryLocks == 0 && driverId == 17);
    CHECK(request == OSIOCTLREQUEST_IO_REQUIREMENTS);
    if (g_removeDuringDriverRequest) {
        CHECK(DmDeviceDestroy(deviceId) == OS_EBUSY);
    }
    return OS_EOK;
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
    struct OSIOCtlBusControl control = { 0 };
    size_t value = 0;
    unsigned int forgotten = g_forgotten;

    // Group tests must remove earlier siblings too while this request is active.
    if (provider->Group != NULL) {
        CHECK(DmPublicationRemove(provider->Group) == OS_EBUSY);
        CHECK(provider->Group->State == DmPublicationRemoving);
        CHECK(DmPublicationEnableBinding(provider->Group) == OS_EBUSY);
        CHECK(DmPublicationReset(provider->Group) == OS_EBUSY);
        // A sibling was removed before reaching the busy provider.
        forgotten++;
    } else {
        CHECK(DmDeviceDestroy(provider->DeviceId) == OS_EBUSY);
    }
    CHECK(provider->References == 1 && provider->Releases == 0);
    CHECK(g_forgotten == forgotten);
    CHECK(!DmDeviceIsBindable(provider->DeviceId));
    CHECK(DmDeviceEnableDriverBinding(provider->DeviceId) == OS_EBUSY);
    CHECK(DmHandleIoctl(provider->DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EBUSY);
    CHECK(DmHandleIoctl2(provider->DeviceId, 0, 0, 0, 4, &value) == OS_EBUSY);
}

static oserr_t
__FirstControl(
    _In_ void* context,
    _In_ enum OSIOCtlRequest request,
    _In_ void* buffer,
    _In_ size_t length)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References == 1);
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
    _In_ enum OSIOCtlRequest request,
    _In_ void* buffer,
    _In_ size_t length)
{
    struct __TestProvider* provider = context;

    CHECK(g_registryLocks == 0 && provider->References == 1);
    (void)request;
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
    if (direction == __DEVICEMANAGER_IOCTL_EXT_READ) {
        *value = provider->Value;
    } else {
        provider->Value = *value;
    }
    return OS_EOK;
}

static const struct DmDeviceProviderOperations g_firstOperations = {
    .Retain = __ProviderRetain,
    .Release = __ProviderRelease,
    .Control = __FirstControl,
    .AccessRegister = __ProviderAccessRegister
};

static const struct DmDeviceProviderOperations g_secondOperations = {
    .Retain = __ProviderRetain,
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
        .Provider = { .Operations = &g_firstOperations, .Context = &first }
    };
    struct OSIOCtlBusControl control = { 0 };
    size_t value = 0;
    unsigned int mode;

    CHECK(DmDeviceCreateWithProvider(&registration, 0, &first.DeviceId) == OS_EOK);
    registration.Description = __NewDescription();
    registration.Provider.Operations = &g_secondOperations;
    registration.Provider.Context = &second;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &second.DeviceId) == OS_EOK);
    CHECK(first.References == 1 && second.References == 1);
    CHECK(DmHandleIoctl(first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    CHECK(DmHandleIoctl(second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(first.Controls == 1 && second.Controls == 10);
    CHECK(DmHandleIoctl(first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control) - 1) == OS_EINVALPARAMS);
    CHECK(first.Controls == 1);
    CHECK(DmHandleIoctl2(first.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        12, 0, 4, &value) == OS_EOK && value == first.Value);
    CHECK(DmHandleIoctl2(first.DeviceId, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        12, 99, 4, &value) == OS_EOK && first.Value == 99);
    CHECK(DmHandleIoctl2(second.DeviceId, 0, 0, 55, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(value == 55 && second.RegisterRequests == 0);
    CHECK(DmDeviceDestroy(second.DeviceId) == OS_EOK);
    CHECK(second.References == 0 && second.Releases == 1);
    CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK);
    CHECK(first.References == 0 && first.Releases == 1);

    for (mode = 0; mode < 2; mode++) {
        memset(&first, 0, sizeof(first));
        first.RemoveDuringRequest = 1;
        registration.Description = __NewDescription();
        registration.Provider.Operations = &g_firstOperations;
        registration.Provider.Context = &first;
        CHECK(DmDeviceCreateWithProvider(&registration, DEVICE_REGISTER_FLAG_LOADDRIVER,
            &first.DeviceId) == OS_EOK);
        if (mode == 0) {
            CHECK(DmHandleIoctl(first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
                &control, sizeof(control)) == OS_EOK);
        } else {
            CHECK(DmHandleIoctl2(first.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
                12, 0, 4, &value) == OS_EOK);
        }
        CHECK(first.References == 1 && first.Releases == 0);
        CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK);
        CHECK(first.References == 0 && first.Releases == 1);
        CHECK(DmHandleIoctl2(first.DeviceId, 0, 0, 0, 4, &value) == OS_ENOENT);
    }
}

static void
__TestRegistrationFailures(void)
{
    struct __TestProvider provider = { 0 };
    struct DmDeviceRegistration registration = {
        .Description = __NewDescription(),
        .Kind = DmDeviceDescriptionBus,
        .Provider = { .Operations = &g_firstOperations, .Context = &provider }
    };
    struct DmDeviceProviderOperations incomplete = g_firstOperations;
    uuid_t id = 123;
    uuid_t parentId;
    uuid_t childId;
    Device_t* child;
    Device_t* rejected;

    CHECK(DmDeviceCreateWithProvider(&registration, 0, &id) == OS_EINVALPARAMS);
    CHECK(id == UUID_INVALID && provider.Retains == 0);
    registration.Kind = DmDeviceDescriptionGeneric;
    incomplete.Release = NULL;
    registration.Provider.Operations = &incomplete;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &id) == OS_EINVALPARAMS);
    CHECK(provider.Retains == 0);
    registration.Provider.Operations = &g_firstOperations;
    g_failAllocation = 1;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &id) == OS_EOOM);
    CHECK(provider.Retains == 0 && id == UUID_INVALID);
    g_failAllocation = 0;
    provider.FailRetain = 1;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &id) == OS_EUNKNOWN);
    CHECK(provider.Retains == 1 && provider.Releases == 0 && provider.References == 0);
    provider.FailRetain = 0;
    registration.Description->ParentId = 0x7fffffff;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &id) == OS_ENOENT);
    CHECK(provider.Retains == 2 && provider.Releases == 1 && provider.References == 0);
    registration.Description->ParentId = UUID_INVALID;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &parentId) == OS_EOK);
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
    bus->IsPci = 1;
    CHECK(DmDeviceCreate(&bus->Base, 0, &id) == OS_EOK);
    CHECK(__GetDeviceUnsafe(id)->Kind == DmDeviceDescriptionBus);
    CHECK(DmHandleIoctl(id, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(id, 0, 0, 0, 4, &value) == OS_ENOTSUPPORTED);
    __GetDeviceUnsafe(id)->driver_id = 17;
    g_removeDuringDriverRequest = 1;
    CHECK(DmHandleIoctl(id, OSIOCTLREQUEST_IO_REQUIREMENTS, &value, sizeof(value)) == OS_EOK);
    CHECK(DmDeviceDestroy(id) == OS_EOK);
    g_removeDuringDriverRequest = 0;
    usb->Base.Length = sizeof(*usb);
    CHECK(DmDeviceCreate(&usb->Base, 0, &id) == OS_EOK);
    CHECK(__GetDeviceUnsafe(id)->Kind == DmDeviceDescriptionUsb);
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

static void
__TestPciProvider(void)
{
    static const struct PciHostOperations operations = {
        .Read = __ReadPciRegister,
        .Write = __WritePciRegister
    };
    static const struct PciFunctionHandler handler = { .BlockActivation = 1 };
    struct __PciRegisters firstRegisters = { .Value = 0x11223344 };
    struct __PciRegisters secondRegisters = { .Value = 0x55667788 };
    PciHost_t firstHost = {
        .Identification = { .Segment = 1, .BusEnd = 1 },
        .Operations = &operations,
        .OpContext = &firstRegisters,
        .IsExtended = 1
    };
    PciHost_t secondHost = {
        .Identification = { .Segment = 2, .BusEnd = 1 },
        .Operations = &operations,
        .OpContext = &secondRegisters
    };
    PciDevice_t first = { .Host = &firstHost, .Bus = 1, .Slot = 2 };
    PciDevice_t second = { .Host = &secondHost, .Bus = 1, .Slot = 2 };
    struct DmDeviceRegistration registration = {
        .Kind = DmDeviceDescriptionBus,
        .Provider = { .Operations = &g_pciDeviceProviderOperations, .Context = &first }
    };
    struct OSIOCtlBusControl control = { .Flags = __DEVICEMANAGER_IOCTL_ENABLE };
    BusDevice_t* description;
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
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &first.DeviceId) == OS_EOK);
    description = calloc(1, sizeof(*description));
    CHECK(description != NULL);
    description->Base.Length = sizeof(*description);
    registration.Description = &description->Base;
    registration.Provider.Context = &second;
    CHECK(DmDeviceCreateWithProvider(&registration, 0, &second.DeviceId) == OS_EOK);
    CHECK(first.ProviderReferences == 1 && second.ProviderReferences == 1);
    CHECK(DmHandleIoctl2(first.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0x100, 0, 4, &value) == OS_EOK && value == 0x11223344);
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0x10, 0, 4, &value) == OS_EOK && value == 0x55667788);
    reads = secondRegisters.Reads;
    writes = secondRegisters.Writes;
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0x100, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        3, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0, 0, 3, &value) == OS_EINVALPARAMS);
    CHECK(DmHandleIoctl2(second.DeviceId, 123,
        0, 0, 4, &value) == OS_EINVALPARAMS);
    CHECK(secondRegisters.Reads == reads && secondRegisters.Writes == writes);
    secondHost.DriversBlocked = 1;
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        0, 0, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl(second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    secondHost.DriversBlocked = 0;
    second.Handler = &handler;
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        0, 0, 4, &value) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl(second.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_ENOTSUPPORTED);
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_READ,
        0, 0, 4, &value) == OS_EOK);
    second.Handler = NULL;
    CHECK(DmHandleIoctl2(second.DeviceId, __DEVICEMANAGER_IOCTL_EXT_WRITE,
        0x10, 42, 4, &value) == OS_EOK && secondRegisters.Value == 42);
    CHECK(DmHandleIoctl(first.DeviceId, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control)) == OS_EOK);
    CHECK(DmDeviceDestroy(first.DeviceId) == OS_EOK && first.ProviderReferences == 0);
    CHECK(second.ProviderReferences == 1);
    CHECK(DmDeviceDestroy(second.DeviceId) == OS_EOK && second.ProviderReferences == 0);
}

#include "publication_test.inc"
#include "rp1_provider_test.inc"
#include "dma_lease_test.inc"

int
main(void)
{
    DmDevicesInitialize();
    __TestProviders();
    __TestRegistrationFailures();
    __TestDescriptionsWithoutProviders();
    __TestPciProvider();
    __TestPublicationFailures();
    __TestPublicationLifecycle();
    __TestRp1Providers();
    __TestDmaLeases();
    __TestRp1DmaLease();
    CHECK(g_devices.count == 0 && g_registryLocks == 0 && g_pciLocks == 0);
    puts("Device providers: dispatch, ownership, removal during requests, description kinds and PCI access passed");
    return 0;
}
