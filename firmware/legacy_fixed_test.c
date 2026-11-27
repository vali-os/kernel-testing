// The fixed legacy devices of a PC as a step of device startup: when the PS/2
// controller is registered, that it happens once, and what a failure leaves.
#include "../../services/deviced/bus/legacy/fixed.c"
#include <stdio.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static oserr_t      g_acpiStatus;
static uint32_t     g_acpiBootFlags;
static unsigned int g_acpiQueries;
static oserr_t      g_createStatus;
static oserr_t      g_portStatus;
static unsigned int g_created;
static Device_t*    g_registered;

// The step logs what it decides. The test checks results, so lines are dropped.
void
OSSysLogWrite(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

oserr_t
AcpiQueryStatus(AcpiDescriptor_t* descriptor)
{
    g_acpiQueries++;
    descriptor->BootFlags = g_acpiBootFlags;
    return g_acpiStatus;
}

oserr_t
DeviceIoCreatePort(DeviceIo_t* io, uint16_t port, size_t length)
{
    CHECK((port == 0x60 || port == 0x64) && length == 1);
    (void)io;
    return g_portStatus;
}

oserr_t
DmDeviceCreate(Device_t* device, unsigned int flags, uuid_t* idOut)
{
    BusDevice_t* bus = (BusDevice_t*)device;

    CHECK(flags == DEVICE_REGISTER_FLAG_LOADDRIVER);
    CHECK(device->VendorId == PCI_FIXED_VENDORID && device->ProductId == PCI_PS2_DEVICEID);
    CHECK(device->ParentId == UUID_INVALID && device->Length == sizeof(BusDevice_t));
    CHECK(bus->InterruptLine == INTERRUPT_NONE && bus->InterruptPin == INTERRUPT_NONE);
    if (g_createStatus != OS_EOK) {
        *idOut = UUID_INVALID;
        return g_createStatus;
    }

    // The registry owns the description from here on. The test frees it.
    g_created++;
    g_registered = device;
    *idOut = 5;
    return OS_EOK;
}

static void
__FreeRegistered(void)
{
    if (g_registered != NULL) {
        free(g_registered->Identification.Description);
        free(g_registered);
        g_registered = NULL;
    }
}

// Runs the step from a clean state and returns how many controllers it made.
static unsigned int
__Enumerate(oserr_t acpiStatus, uint32_t bootFlags, struct DmStartupResult* result)
{
    unsigned int before = g_created;

    g_legacyStartup = __LegacyStartupPending;
    g_acpiStatus = acpiStatus;
    g_acpiBootFlags = bootFlags;
    LegacyEnumerate(result);
    return g_created - before;
}

int
main(void)
{
    struct DmStartupResult result;
    unsigned int           queries;

    // Without ACPI, with boot flags of zero and with the 8042 flag, the
    // machine is taken to have the controller.
    CHECK(__Enumerate(OS_ENOTSUPPORTED, 0, &result) == 1 && result.Outcome == DmStartupComplete);
    __FreeRegistered();
    CHECK(__Enumerate(OS_EOK, 0, &result) == 1 && result.Outcome == DmStartupComplete);
    __FreeRegistered();
    CHECK(__Enumerate(OS_EOK, ACPI_IA_8042 | ACPI_IA_LEGACY_DEVICES, &result) == 1);
    CHECK(result.Outcome == DmStartupComplete && result.Status == OS_EOK);

    // The step does its work once. A later call asks ACPI nothing.
    queries = g_acpiQueries;
    LegacyEnumerate(&result);
    CHECK(g_created == 3 && g_acpiQueries == queries && result.Outcome == DmStartupComplete);
    __FreeRegistered();

    // Boot flags that are set but lack the 8042 flag mean no controller.
    CHECK(__Enumerate(OS_EOK, ACPI_IA_LEGACY_DEVICES, &result) == 0 && result.Outcome == DmStartupComplete);
    CHECK(g_legacyStartup == __LegacyStartupDone);

    // An ACPI query that may succeed later asks for another attempt and
    // registers nothing. The next attempt does the work.
    CHECK(__Enumerate(OS_EBUSY, 0, &result) == 0);
    CHECK(result.Outcome == DmStartupRetry && result.Status == OS_EBUSY);
    CHECK(g_legacyStartup == __LegacyStartupPending);
    g_acpiStatus = OS_EOK;
    LegacyEnumerate(&result);
    CHECK(g_created == 4 && result.Outcome == DmStartupComplete);
    __FreeRegistered();

    // A controller that cannot be registered does not stop startup and is
    // not tried again. The leak check sees that its description is freed.
    g_createStatus = OS_EOOM;
    CHECK(__Enumerate(OS_EOK, 0, &result) == 0 && result.Outcome == DmStartupComplete);
    CHECK(g_legacyStartup == __LegacyStartupDone);
    g_createStatus = OS_EOK;
    g_portStatus = OS_EDEVFAULT;
    CHECK(__Enumerate(OS_EOK, 0, &result) == 0 && result.Outcome == DmStartupComplete);
    g_portStatus = OS_EOK;

    puts("Legacy fixed devices: PS/2 policy, single run, ACPI retry and failed registration passed");
    return 0;
}
