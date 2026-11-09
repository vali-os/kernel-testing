/** Exercise syscall argument forwarding and supported SHM guard checks. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <shm/shm.h>
#include <futex.h>

oserr_t ScSHMMap(SHMHandle_t*, size_t, size_t, unsigned int);
oserr_t ScFutexWait(OSAsyncContext_t*, OSFutexParameters_t*);
oserr_t ScFutexWake(OSFutexParameters_t*);

static SHMHandle_t g_handle;
static OSFutexParameters_t g_parameters;
static OSAsyncContext_t g_async;
static unsigned int g_maps, g_waits, g_wakes;

oserr_t
MemorySpaceCopyUser(
    _In_ void*  userBuffer,
    _In_ void*  kernelBuffer,
    _In_ size_t length,
    _In_ bool   toUser)
{
    if (userBuffer == NULL) {
        return OS_EINVALPARAMS;
    }
    if (toUser) {
        memcpy(userBuffer, kernelBuffer, length);
    } else {
        memcpy(kernelBuffer, userBuffer, length);
    }
    return OS_EOK;
}

oserr_t
SHMUnmap(
    _In_ SHMHandle_t* handle,
    _In_ vaddr_t      address,
    _In_ size_t       length)
{
    (void)handle;
    (void)address;
    (void)length;
    assert(!"Failed mapping must not need publication rollback");
    return OS_EUNKNOWN;
}

oserr_t
SHMMap(
    _In_ SHMHandle_t* handle,
    _In_ size_t offset,
    _In_ size_t length,
    _In_ unsigned int flags)
{
    assert(memcmp(handle, &g_handle, sizeof(g_handle)) == 0);
    assert(offset == 4096 && length == 8192 && flags == SHM_ACCESS_READ);
    g_maps++;
    return OS_EBUSY;
}

oserr_t
FutexWait(
    _In_ OSAsyncContext_t* async,
    _In_ atomic_int* futex,
    _In_ int expected,
    _In_ int flags,
    _In_ atomic_int* second,
    _In_ int count,
    _In_ int operation,
    _In_ OSTimestamp_t* deadline)
{
    assert(async == &g_async || async == NULL);
    assert(futex == g_parameters.Futex0 && second == g_parameters.Futex1);
    assert(expected == g_parameters.Expected0 && flags == g_parameters.Flags);
    assert(count == g_parameters.Count && operation == g_parameters.Op);
    assert(deadline == g_parameters.Deadline);
    g_waits++;
    return OS_ETIMEOUT;
}

oserr_t
FutexWake(
    _In_ atomic_int* futex,
    _In_ int count,
    _In_ int flags)
{
    assert(futex == g_parameters.Futex0 && count == g_parameters.Expected0);
    assert(flags == g_parameters.Flags);
    g_wakes++;
    return OS_EBUSY;
}

oserr_t
FutexWakeOperation(
    _In_ atomic_int* futex,
    _In_ int count,
    _In_ atomic_int* second,
    _In_ int secondCount,
    _In_ int operation,
    _In_ int flags)
{
    assert(second == g_parameters.Futex1 && secondCount == g_parameters.Count);
    assert(operation == g_parameters.Op);
    return FutexWake(futex, count, flags);
}

int
main(void)
{
    atomic_int first = 0;
    atomic_int second = 0;
    OSTimestamp_t deadline = {0};

    assert(ScSHMMap(&g_handle, 4096, 8192, SHM_ACCESS_READ) == OS_EBUSY);
    assert(g_maps == 1);

    g_parameters.Futex0 = &first;
    g_parameters.Futex1 = &second;
    g_parameters.Expected0 = 3;
    g_parameters.Count = 4;
    g_parameters.Op = 5;
    g_parameters.Deadline = &deadline;
    g_parameters.Flags = FUTEX_FLAG_WAIT;
    assert(ScFutexWait(&g_async, &g_parameters) == OS_ETIMEOUT);
    assert(ScFutexWait(NULL, &g_parameters) == OS_ETIMEOUT && g_waits == 2);
    g_parameters.Flags = FUTEX_FLAG_WAKE;
    assert(ScFutexWake(&g_parameters) == OS_EBUSY && g_wakes == 1);
    g_parameters.Flags |= FUTEX_FLAG_OP;
    assert(ScFutexWake(&g_parameters) == OS_EBUSY && g_wakes == 2);
    puts("IPC syscall boundaries: SHM guards, map/futex argument forwarding and backend errors passed");
    return 0;
}


