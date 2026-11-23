// Exercise write() and read() on an event descriptor, with the real libc
// event operations, the real write() and the real OSEventLock and
// OSEventUnlock against a fake futex. A write that adds a unit posts once to
// the I/O sets. A write that finds the counter at its maximum adds nothing
// and posts nothing, but still succeeds, because a reader will take a unit
// and see the work behind it. write() returns sizeof(unsigned int) for both
// event types.
#ifndef LIBC_EVENT_SOURCE
#define LIBC_EVENT_SOURCE "../librt/libc/compat/event.c"
#endif
#include LIBC_EVENT_SOURCE
#include <internal/_syscalls.h>
#include <internal/_tls.h>
#include <os/futex.h>
#include <stdio.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

// Matches struct Event in librt/libos/event.c, which is compiled on its own
// because the libc event structure has the same name.
struct __OSEvent {
    _Atomic(int)* SyncAddress;
    unsigned int  MaxValue;
};

static _Atomic(int)          g_counter;
static struct __OSEvent      g_osEvent = { .SyncAddress = &g_counter, .MaxValue = 1 };
static struct Event          g_event;
static stdio_handle_t        g_handle;
static struct thread_storage g_threadStorage;
static int                   g_errno;
static unsigned int          g_posts;
static unsigned int          g_postFlags;
static oserr_t               g_postResult;
static unsigned int          g_wakes;
static unsigned int          g_errors;

// Functions called by librt/libos/event.c.

oserr_t
OSFutex(
    _In_ OSFutexParameters_t* parameters,
    _In_ OSAsyncContext_t*    asyncContext)
{
    (void)asyncContext;
    CHECK(parameters->Futex0 == &g_counter);

    // The tests never read from an empty counter, so only wakes reach here.
    CHECK(FUTEX_FLAG_ACTION(parameters->Flags) == FUTEX_FLAG_WAKE);
    g_wakes++;
    return OS_ENOENT;
}

struct thread_storage*
__tls_current(void)
{
    return &g_threadStorage;
}

SCTYPE
syscall1(SCTYPE function, SCTYPE arg0)
{
    (void)function;
    (void)arg0;
    abort();
}

SCTYPE
syscall4(SCTYPE function, SCTYPE arg0, SCTYPE arg1, SCTYPE arg2, SCTYPE arg3)
{
    (void)function;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
    abort();
}

oserr_t
OSHandleWrap(
    _In_ uuid_t            id,
    _In_ enum OSHandleType type,
    _In_ void*             payload,
    _In_ bool              ownership,
    _In_ struct OSHandle*  handle)
{
    (void)id;
    (void)type;
    (void)payload;
    (void)ownership;
    (void)handle;
    abort();
}

// Functions called by the libc event operations, write() and OsErrToErrNo.

void
OSSysLogWrite(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format, ...)
{
    (void)format;
    if (level == OSSYSLOGLEVEL_ERROR) {
        g_errors++;
    }
}

errno_t*
__errno(void)
{
    return &g_errno;
}

oserr_t
OSNotificationQueuePost(
    _In_ OSHandle_t*  handle,
    _In_ unsigned int flags)
{
    CHECK(handle == &g_handle.OSHandle);
    g_posts++;
    g_postFlags = flags;
    return g_postResult;
}

stdio_handle_t*
stdio_handle_get(
    _In_ int iod)
{
    CHECK(iod == g_handle.IOD);
    return &g_handle;
}

long
lseek(
    _In_ int  fd,
    _In_ long offset,
    _In_ int  origin)
{
    (void)fd;
    (void)offset;
    (void)origin;
    abort();
}

int
stdio_handle_create(
    _In_  int              iod,
    _In_  int              ioFlags,
    _In_  unsigned int     xtFlags,
    _In_  unsigned int     signature,
    _In_  void*            opsContext,
    _Out_ stdio_handle_t** handleOut)
{
    (void)iod;
    (void)ioFlags;
    (void)xtFlags;
    (void)signature;
    (void)opsContext;
    (void)handleOut;
    abort();
}

int
stdio_handle_set_handle(
    _In_ stdio_handle_t* handle,
    _In_ OSHandle_t*     osHandle)
{
    (void)handle;
    (void)osHandle;
    abort();
}

int
stdio_handle_iod(
    _In_ stdio_handle_t* handle)
{
    (void)handle;
    abort();
}

void
OSHandleDestroy(
    _In_ struct OSHandle* handle)
{
    (void)handle;
    abort();
}

// Prepares an event descriptor of the given type with the counter at counter.
static void
__Reset(
    _In_ unsigned int flags,
    _In_ int          counter,
    _In_ unsigned int maxValue)
{
    atomic_store(&g_counter, counter);
    g_osEvent.MaxValue = maxValue;
    g_event = (struct Event) { .InitialValue = maxValue, .Flags = flags };
    g_handle = (stdio_handle_t) {
        .IOD = 7,
        .OSHandle = { .ID = 9, .Payload = &g_osEvent },
        .Ops = &g_evtOps,
        .OpsContext = &g_event
    };
    g_errno = 0;
    g_posts = 0;
    g_postFlags = 0;
    g_postResult = OS_EOK;
    g_wakes = 0;
    g_errors = 0;
}

// Reads one unit through the event operations and returns the value read.
static unsigned int
__ReadUnit(void)
{
    unsigned int value = 0;
    size_t       bytesRead = 0;

    CHECK(g_evtOps.read(&g_handle, &value, sizeof(value), &bytesRead) == OS_EOK);
    CHECK(bytesRead == sizeof(value));
    return value;
}

static void
__TestResetEvent(void)
{
    unsigned int value;

    // A write to an empty counter adds the unit, wakes a reader and posts once.
    __Reset(EVT_RESET_EVENT, 0, 1);
    value = 5;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(atomic_load(&g_counter) == 1 && g_wakes == 1);
    CHECK(g_posts == 1 && g_postFlags == IOSETSYN);

    // A second write merges. It used to fail with OS_EINCOMPLETE, so write()
    // returned -1 with errno ECANCELED. Now it succeeds without a wake or a
    // post, and the reader gets the latest value.
    value = 6;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(g_errno == 0);
    CHECK(atomic_load(&g_counter) == 1 && g_wakes == 1 && g_posts == 1);

    // One read takes the unit that covers both writes.
    CHECK(__ReadUnit() == 6);
    CHECK(atomic_load(&g_counter) == 0);

    // The next write after the read adds a unit and posts again.
    value = 7;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(atomic_load(&g_counter) == 1 && g_wakes == 2 && g_posts == 2);
    puts("reset event: added, merged and read writes passed");
}

static void
__TestSemaphoreEvent(void)
{
    unsigned int value;

    // Units are added up to the maximum of 3, with one post per write that
    // added any. The byte count is the size of the value, not of size_t.
    __Reset(EVT_SEM_EVENT, 0, 3);
    value = 2;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(atomic_load(&g_counter) == 2 && g_posts == 1);
    value = 5;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(atomic_load(&g_counter) == 3 && g_posts == 2);

    // At the maximum the write merges, like a reset event, and posts nothing.
    value = 1;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(g_errno == 0 && atomic_load(&g_counter) == 3 && g_posts == 2);

    // Each read takes one unit and reads 1.
    CHECK(__ReadUnit() == 1);
    CHECK(atomic_load(&g_counter) == 2);
    puts("semaphore event: added, capped and merged writes passed");
}

static void
__TestFailures(void)
{
    unsigned short shortValue = 1;
    unsigned int   value = 1;

    // A value shorter than an unsigned int is refused and changes nothing.
    __Reset(EVT_RESET_EVENT, 0, 1);
    CHECK(write(7, &shortValue, sizeof(shortValue)) == -1 && g_errno == EACCES);
    CHECK(atomic_load(&g_counter) == 0 && g_posts == 0);

    // When no I/O set watches the event, the post returns OS_ENOENT. That is
    // normal and is not logged.
    __Reset(EVT_RESET_EVENT, 0, 1);
    g_postResult = OS_ENOENT;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(g_posts == 1 && g_errors == 0);

    // Any other failed post is logged. The unit is added, so the write
    // still succeeds.
    __Reset(EVT_RESET_EVENT, 0, 1);
    g_postResult = OS_EUNKNOWN;
    CHECK(write(7, &value, sizeof(value)) == (int)sizeof(value));
    CHECK(g_posts == 1 && g_errors == 1 && atomic_load(&g_counter) == 1);
    puts("event writes: short buffer and failed posts passed");
}

int
main(void)
{
    __TestResetEvent();
    __TestSemaphoreEvent();
    __TestFailures();
    return 0;
}
