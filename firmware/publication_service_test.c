// Exercise the production service retry caller with a deterministic scheduler.
#define __OSCONFIG_DEVICETREE_SUPPORT
#define timespec_get __TestTimeGet
#include "../../services/deviced/main.c"
#undef timespec_get
#include <bus/pci/function.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

gracht_protocol_t sys_device_server_protocol;
gracht_protocol_t ctt_driver_client_protocol;

static usched_task_fn g_job;
static unsigned int  g_reconciliations;
static unsigned int  g_sleeps;
static unsigned int  g_failures;
static int           g_cancelled;
static int           g_cancelOnSleep;
static struct DmStartupResult g_pciResult;
static struct DmStartupResult g_firmwareResult;
static struct DmStartupResult g_platformResult;
static unsigned int g_firmwareAttempts;
static unsigned int g_platformAttempts;
static const int     g_delays[] = { 1, 2, 4, 8, 16, 30, 30, 30 };

struct DmStartupResult
FirmwareInitialize(void)
{
    g_firmwareAttempts++;
    return g_firmwareResult;
}

struct DmStartupResult
PlatformEnumerate(void)
{
    g_platformAttempts++;
    return g_platformResult;
}

const struct PciFunctionHandler g_rp1PciHandler = { 0 };

void
PciInitialize(
    _In_ const struct PciFunctionHandler* const* handlers,
    _In_ size_t                                handlerCount)
{
    CHECK(handlerCount == 1 && handlers[0] == &g_rp1PciHandler);
}

struct DmStartupResult
BusRecoverAndRepublish(void)
{
    g_reconciliations++;
    if (g_failures != 0) {
        g_failures--;
        return __DmStartupRetryResult(OS_EBUSY);
    }
    return g_pciResult;
}

uuid_t
usched_job_queue(
    _In_ usched_task_fn entry,
    _In_ void*         argument)
{
    CHECK(g_job == NULL && argument == NULL);
    g_job = entry;
    return 1;
}

int
__TestTimeGet(
    _Out_ struct timespec* value,
    _In_  int              base)
{
    CHECK(base == TIME_UTC);
    value->tv_sec = 100;
    value->tv_nsec = 0;
    return TIME_UTC;
}

int
usched_job_sleep(
    _In_ const struct timespec* deadline)
{
    CHECK(g_sleeps < sizeof(g_delays) / sizeof(g_delays[0]));
    CHECK(deadline->tv_sec == 100 + g_delays[g_sleeps]);
    g_sleeps++;
    if (g_cancelOnSleep) {
        g_cancelled = 1;
    }
    return 0;
}

bool
usched_is_cancelled(
    _In_ const void* token)
{
    CHECK(token == &g_cancelled);
    return g_cancelled != 0;
}

void
DmDevicesInitialize(void)
{
}

void
DmDiscoverInitialize(void)
{
}

gracht_client_t*
GetGrachtClient(void)
{
    return NULL;
}

int
gracht_client_register_protocol(
    _In_ gracht_client_t*   client,
    _In_ gracht_protocol_t* protocol)
{
    CHECK(protocol == &ctt_driver_client_protocol);
    return 0;
}

int
gracht_server_register_protocol(
    _In_ gracht_server_t*   server,
    _In_ gracht_protocol_t* protocol)
{
    CHECK(protocol == &sys_device_server_protocol);
    return 0;
}

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
}

int
main(void)
{
    struct ServiceStartupOptions options = { 0 };
    unsigned int attempts;

    ServiceInitialize(&options);
    CHECK(g_reconciliations == 1 && g_job == NULL);

    g_failures = 8;
    ServiceInitialize(&options);
    CHECK(g_reconciliations == 2 && g_job != NULL);
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 8 && g_reconciliations == 10);

    // Terminal firmware errors do not hide a temporary failure in another owner.
    g_job = NULL;
    g_sleeps = 0;
    g_firmwareResult = __DmStartupFromStatus(OS_EINVALPARAMS);
    g_platformResult = __DmStartupRetryResult(OS_EOOM);
    ServiceInitialize(&options);
    CHECK(g_job != NULL);
    g_platformResult = __DmStartupFromStatus(OS_EOK);
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1);
    CHECK(__RecoverDevices().Outcome == DmStartupFailed);

    // A terminal result never schedules a timer just because its status is nonzero.
    g_job = NULL;
    ServiceInitialize(&options);
    CHECK(g_job == NULL);
    g_firmwareResult = (struct DmStartupResult){ .Status = OS_ENOENT };
    g_pciResult = __DmStartupFromStatus(OS_ENOTSUPPORTED);
    ServiceInitialize(&options);
    CHECK(g_job == NULL);

    // The owner can request a retry even for an error normally treated as terminal.
    g_pciResult = __DmStartupRetryResult(OS_ENOTSUPPORTED);
    g_sleeps = 0;
    ServiceInitialize(&options);
    CHECK(g_job != NULL);
    g_pciResult = __DmStartupFromStatus(OS_EOK);
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1 && __RecoverDevices().Outcome == DmStartupComplete);

    // Cancellation during sleep must not publish anything after waking.
    attempts = g_reconciliations;
    g_sleeps = 0;
    g_cancelOnSleep = 1;
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1 && g_reconciliations == attempts);
    puts("Device startup: explicit outcomes, mixed failures, handler selection, capped retry and cancellation passed");
    return 0;
}
