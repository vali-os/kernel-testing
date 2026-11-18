// Checks the busy retry in __StorageTransfer (librt/libddk/storage.c). The
// production source is compiled into this file. Doubles replace the grant
// token calls, the ctt_storage transfer IPC, the gracht wait and the usched
// sleep. The sleep moves a fake clock, so the total wait can be checked
// without waiting.
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// The host time.h has no timespec_add. It comes from the Vali libc and is
// replaced by a double below.
void timespec_add(const struct timespec* a, const struct timespec* b, struct timespec* result);

#ifndef DDK_STORAGE_SOURCE
#define DDK_STORAGE_SOURCE "../librt/libddk/storage.c"
#endif
#include DDK_STORAGE_SOURCE

// Upper bound for the attempts one test case can make.
#define TEST_MAX_ATTEMPTS 64

// The values must match STORAGE_BUSY_RETRY_DELAY_MS and
// STORAGE_BUSY_RETRY_COUNT. They are repeated here so a changed limit fails
// the test instead of silently changing what is checked.
#define TEST_RETRY_DELAY_MS 20
#define TEST_RETRY_COUNT    50

#define TEST_DRIVER 7
#define TEST_DEVICE 3
#define TEST_BUFFER 9

// What the driver answers on each attempt. g_answers[i] is used for attempt
// i, and the last entry repeats once the list runs out.
static oserr_t g_answers[TEST_MAX_ATTEMPTS];
static size_t  g_answerSectors[TEST_MAX_ATTEMPTS];
static int     g_answerCount;
static bool    g_sendFails;
static bool    g_awaitFails;
static oserr_t g_tokenResult = OS_EOK;

// Grant tokens. Token IDs start at 100 and grow by one per created token.
// g_tokenRevoked[i] counts the revokes of token 100 + i.
static uuid_t g_nextToken = 100;
static int    g_tokensCreated;
static int    g_tokenRevoked[TEST_MAX_ATTEMPTS];

// Requests sent to the driver, and the token each one carried.
static int    g_attempts;
static uuid_t g_attemptToken[TEST_MAX_ATTEMPTS];

// The fake clock, in nanoseconds since the epoch.
static long long g_clockNs = 1000LL * NSEC_PER_SEC + 123;
static int       g_sleeps;
static long long g_sleptNs;

size_t MemoryPageSize(void) { return 4096; }
gracht_client_t* GetGrachtClient(void) { return NULL; }

oserr_t
OSGrantTokenCreate(
    _In_  uuid_t                     handleID,
    _In_  const GrantTokenOptions_t* options,
    _Out_ GrantToken_t*              tokenOut)
{
    assert(handleID == TEST_BUFFER);
    if (g_tokenResult != OS_EOK) {
        return g_tokenResult;
    }
    assert(g_tokensCreated < TEST_MAX_ATTEMPTS);
    memset(tokenOut, 0, sizeof(GrantToken_t));
    tokenOut->ControlHandleID = g_nextToken;
    tokenOut->ExportGlobalHandleID = g_nextToken;
    tokenOut->Version = GRANT_TOKEN_VERSION;
    tokenOut->Options = *options;
    g_nextToken++;
    g_tokensCreated++;
    return OS_EOK;
}

oserr_t
OSGrantTokenRevoke(
    _In_ uuid_t controlHandleID)
{
    assert(controlHandleID >= 100 && controlHandleID < g_nextToken);
    g_tokenRevoked[controlHandleID - 100]++;
    return OS_EOK;
}

// The request must carry the newest token, and that token must still be live.
int
ctt_storage_transfer(
    _In_ gracht_client_t*                client,
    _In_ struct gracht_message_context*  context,
    _In_ const uuid_t                    deviceId,
    _In_ const enum sys_transfer_direction direction,
    _In_ const unsigned int              sectorLow,
    _In_ const unsigned int              sectorHigh,
    _In_ const struct sys_shm_transfer*  buffer,
    _In_ const size_t                    count)
{
    uuid_t token = buffer->token.export_id;

    assert(deviceId == TEST_DEVICE && sectorLow == 16 && sectorHigh == 0 && count == 4);
    assert(token == g_nextToken - 1 && g_tokenRevoked[token - 100] == 0);
    assert(g_attempts < TEST_MAX_ATTEMPTS);
    g_attemptToken[g_attempts++] = token;
    return g_sendFails ? -1 : 0;
}

int
gracht_client_await(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context,
    _In_ unsigned int                   flags)
{
    assert(flags == GRACHT_AWAIT_ASYNC);
    return g_awaitFails ? -1 : 0;
}

int
gracht_client_get_status(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    return GRACHT_MESSAGE_COMPLETED;
}

int
gracht_client_abandon(
    _In_ gracht_client_t*               client,
    _In_ struct gracht_message_context* context)
{
    return 0;
}

int
ctt_storage_transfer_result(
    _In_  gracht_client_t*               client,
    _In_  struct gracht_message_context* context,
    _Out_ oserr_t*                       statusOut,
    _Out_ size_t*                        sectorsOut)
{
    int index = g_attempts - 1 < g_answerCount ? g_attempts - 1 : g_answerCount - 1;

    assert(index >= 0);
    *statusOut = g_answers[index];
    *sectorsOut = g_answerSectors[index];
    return 0;
}

int
timespec_get(
    _Out_ struct timespec* ts,
    _In_  int              base)
{
    assert(base == TIME_UTC);
    ts->tv_sec = (time_t)(g_clockNs / NSEC_PER_SEC);
    ts->tv_nsec = (long)(g_clockNs % NSEC_PER_SEC);
    return base;
}

void
timespec_add(
    _In_  const struct timespec* a,
    _In_  const struct timespec* b,
    _Out_ struct timespec*       result)
{
    result->tv_sec = a->tv_sec + b->tv_sec;
    result->tv_nsec = a->tv_nsec + b->tv_nsec;
    if (result->tv_nsec >= NSEC_PER_SEC) {
        result->tv_sec++;
        result->tv_nsec -= NSEC_PER_SEC;
    }
}

// Moves the fake clock to the wake-up time.
int
usched_job_sleep(
    _In_ const struct timespec* until)
{
    long long untilNs = (long long)until->tv_sec * NSEC_PER_SEC + until->tv_nsec;

    assert(untilNs >= g_clockNs);
    g_sleptNs += untilNs - g_clockNs;
    g_clockNs = untilNs;
    g_sleeps++;
    return 0;
}

static void
__Reset(void)
{
    memset(g_answers, 0, sizeof(g_answers));
    memset(g_answerSectors, 0, sizeof(g_answerSectors));
    memset(g_tokenRevoked, 0, sizeof(g_tokenRevoked));
    memset(g_attemptToken, 0, sizeof(g_attemptToken));
    g_answerCount = 0;
    g_sendFails = false;
    g_awaitFails = false;
    g_tokenResult = OS_EOK;
    g_nextToken = 100;
    g_tokensCreated = 0;
    g_attempts = 0;
    g_sleeps = 0;
    g_sleptNs = 0;
}

static void
__Answer(
    _In_ oserr_t status,
    _In_ size_t  sectors)
{
    assert(g_answerCount < TEST_MAX_ATTEMPTS);
    g_answers[g_answerCount] = status;
    g_answerSectors[g_answerCount] = sectors;
    g_answerCount++;
}

static oserr_t
__Transfer(
    _Out_ size_t* completed)
{
    UInteger64_t sector = { .QuadPart = 16 };

    *completed = 99;
    return __StorageTransfer(TEST_DRIVER, TEST_DEVICE, TEST_BUFFER, 512, &sector, 512, 4, false, completed);
}

// Every attempt used its own token, and every token was revoked once.
static void
__CheckTokens(void)
{
    assert(g_tokensCreated == g_attempts);
    for (int i = 0; i < g_attempts; i++) {
        assert(g_attemptToken[i] == (uuid_t)(100 + i));
        assert(g_tokenRevoked[i] == 1);
    }
}

static void
__TestNoRetryOnSuccess(void)
{
    size_t completed;

    __Reset();
    __Answer(OS_EOK, 4);
    assert(__Transfer(&completed) == OS_EOK && completed == 4);
    assert(g_attempts == 1 && g_sleeps == 0);
    __CheckTokens();
    puts("ddk storage retry: success without retry passed");
}

static void
__TestSuccessAfterBusy(void)
{
    size_t completed;

    __Reset();
    for (int i = 0; i < 3; i++) {
        __Answer(OS_EBUSY, 0);
    }
    __Answer(OS_EOK, 4);
    assert(__Transfer(&completed) == OS_EOK && completed == 4);
    assert(g_attempts == 4 && g_sleeps == 3);
    assert(g_sleptNs == 3LL * TEST_RETRY_DELAY_MS * NSEC_PER_MSEC);
    __CheckTokens();

    // The last allowed retry can still succeed.
    __Reset();
    for (int i = 0; i < TEST_RETRY_COUNT; i++) {
        __Answer(OS_EBUSY, 0);
    }
    __Answer(OS_EOK, 2);
    assert(__Transfer(&completed) == OS_EOK && completed == 2);
    assert(g_attempts == TEST_RETRY_COUNT + 1 && g_sleeps == TEST_RETRY_COUNT);
    __CheckTokens();
    puts("ddk storage retry: success after busy results passed");
}

static void
__TestBusyLimit(void)
{
    size_t completed;

    // The driver stays busy. One first attempt and 50 retries, 20 ms apart,
    // which is 1 second of waiting in total.
    __Reset();
    __Answer(OS_EBUSY, 0);
    assert(__Transfer(&completed) == OS_EBUSY && completed == 0);
    assert(g_attempts == TEST_RETRY_COUNT + 1);
    assert(g_sleeps == TEST_RETRY_COUNT);
    assert(g_sleptNs == 1LL * NSEC_PER_SEC);
    __CheckTokens();
    puts("ddk storage retry: busy limit passed");
}

static void
__TestNoRetryOnOtherErrors(void)
{
    size_t completed;

    // Other driver results go straight to the caller.
    __Reset();
    __Answer(OS_EDEVFAULT, 1);
    assert(__Transfer(&completed) == OS_EDEVFAULT && completed == 1);
    assert(g_attempts == 1 && g_sleeps == 0);
    __CheckTokens();

    // A busy answer followed by another error stops at that error.
    __Reset();
    __Answer(OS_EBUSY, 0);
    __Answer(OS_EINVALPARAMS, 0);
    assert(__Transfer(&completed) == OS_EINVALPARAMS);
    assert(g_attempts == 2 && g_sleeps == 1);
    __CheckTokens();

    // A request that cannot be sent.
    __Reset();
    __Answer(OS_EBUSY, 0);
    g_sendFails = true;
    assert(__Transfer(&completed) == OS_EUNKNOWN && completed == 0);
    assert(g_attempts == 1 && g_sleeps == 0);
    __CheckTokens();

    // A wait that ends without a reply.
    __Reset();
    __Answer(OS_EBUSY, 0);
    g_awaitFails = true;
    assert(__Transfer(&completed) == OS_EUNKNOWN && completed == 0);
    assert(g_attempts == 1 && g_sleeps == 0);
    __CheckTokens();

    // A token that cannot be created. Nothing is sent or revoked. A local
    // OS_EBUSY is not a driver answer and is not retried either.
    __Reset();
    g_tokenResult = OS_EOOM;
    assert(__Transfer(&completed) == OS_EOOM && completed == 0);
    assert(g_attempts == 0 && g_sleeps == 0);
    __Reset();
    g_tokenResult = OS_EBUSY;
    assert(__Transfer(&completed) == OS_EBUSY && completed == 0);
    assert(g_attempts == 0 && g_sleeps == 0 && g_tokensCreated == 0);

    // More sectors than were asked for is a protocol error.
    __Reset();
    __Answer(OS_EBUSY, 5);
    assert(__Transfer(&completed) == OS_EPROTOCOL && completed == 0);
    assert(g_attempts == 1 && g_sleeps == 0);
    __CheckTokens();
    puts("ddk storage retry: no retry on other errors passed");
}

int
main(void)
{
    __TestNoRetryOnSuccess();
    __TestSuccessAfterBusy();
    __TestBusyLimit();
    __TestNoRetryOnOtherErrors();
    return 0;
}
