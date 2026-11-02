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

void
FirmwareInitialize(
    _Out_ struct DmStartupResult* result)
{
    g_firmwareAttempts++;
    *result = g_firmwareResult;
}

void
PlatformEnumerate(
    _Out_ struct DmStartupResult* result)
{
    g_platformAttempts++;
    *result = g_platformResult;
}

const struct PciFunctionHandler g_rp1PciHandler = { 0 };

void
PciInitialize(
    _In_ const struct PciFunctionHandler* const* handlers,
    _In_ size_t                                handlerCount)
{
    CHECK(handlerCount == 1 && handlers[0] == &g_rp1PciHandler);
}

void
BusRecoverAndRepublish(
    _Out_ struct DmStartupResult* result)
{
    g_reconciliations++;
    if (g_failures != 0) {
        g_failures--;
        __DmStartupRetryResult(OS_EBUSY, result);
        return;
    }
    *result = g_pciResult;
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

static void
__TestStartupResultStorage(void)
{
    const oserr_t failures[] = {
        OS_EINVALPARAMS,
        OS_ENOTSUPPORTED,
        OS_EEXISTS,
        OS_EOVERFLOW,
        OS_ENOENT
    };
    struct DmStartupResult result = {
        .Status = OS_EUNKNOWN,
        .Outcome = DmStartupRetry
    };
    struct DmStartupResult next;
    size_t index;

    __DmStartupFromStatus(OS_EOK, &result);
    CHECK(result.Status == OS_EOK && result.Outcome == DmStartupComplete);
    for (index = 0; index < sizeof(failures) / sizeof(failures[0]); index++) {
        __DmStartupFromStatus(failures[index], &result);
        CHECK(result.Status == failures[index] && result.Outcome == DmStartupFailed);
        __DmStartupRetryResult(failures[index], &result);
        CHECK(result.Status == failures[index] && result.Outcome == DmStartupRetry);
    }
    __DmStartupFromStatus(OS_EOOM, &result);
    CHECK(result.Status == OS_EOOM && result.Outcome == DmStartupRetry);
    __DmStartupRetryResult(OS_EOK, &result);
    CHECK(result.Status == OS_EOK && result.Outcome == DmStartupComplete);

    __DmStartupFromStatus(OS_EINVALPARAMS, &next);
    __DmStartupMerge(&result, &next);
    CHECK(result.Status == OS_EINVALPARAMS && result.Outcome == DmStartupFailed);
    CHECK(next.Status == OS_EINVALPARAMS && next.Outcome == DmStartupFailed);
    __DmStartupRetryResult(OS_EBUSY, &next);
    __DmStartupMerge(&result, &next);
    CHECK(result.Status == OS_EINVALPARAMS && result.Outcome == DmStartupRetry);
    CHECK(next.Status == OS_EBUSY && next.Outcome == DmStartupRetry);
    __DmStartupFromStatus(OS_EOK, &next);
    __DmStartupMerge(&result, &next);
    CHECK(result.Status == OS_EINVALPARAMS && result.Outcome == DmStartupRetry);
}

int
main(void)
{
    struct ServiceStartupOptions options = { 0 };
    struct DmStartupResult result;
    unsigned int attempts;

    __TestStartupResultStorage();
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
    __DmStartupFromStatus(OS_EINVALPARAMS, &g_firmwareResult);
    __DmStartupRetryResult(OS_EOOM, &g_platformResult);
    ServiceInitialize(&options);
    CHECK(g_job != NULL);
    __DmStartupFromStatus(OS_EOK, &g_platformResult);
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1);
    __RecoverDevices(&result);
    CHECK(result.Outcome == DmStartupFailed);

    // A terminal result never schedules a timer just because its status is nonzero.
    g_job = NULL;
    ServiceInitialize(&options);
    CHECK(g_job == NULL);
    g_firmwareResult = (struct DmStartupResult){ .Status = OS_ENOENT };
    __DmStartupFromStatus(OS_ENOTSUPPORTED, &g_pciResult);
    ServiceInitialize(&options);
    CHECK(g_job == NULL);

    // The owner can request a retry even for an error normally treated as terminal.
    __DmStartupRetryResult(OS_ENOTSUPPORTED, &g_pciResult);
    g_sleeps = 0;
    ServiceInitialize(&options);
    CHECK(g_job != NULL);
    __DmStartupFromStatus(OS_EOK, &g_pciResult);
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1);
    __RecoverDevices(&result);
    CHECK(result.Outcome == DmStartupComplete);

    // Cancellation during sleep must not publish anything after waking.
    attempts = g_reconciliations;
    g_sleeps = 0;
    g_cancelOnSleep = 1;
    g_job(NULL, &g_cancelled);
    CHECK(g_sleeps == 1 && g_reconciliations == attempts);
    puts("Device startup: explicit outcomes, mixed failures, handler selection, capped retry and cancellation passed");
    return 0;
}
