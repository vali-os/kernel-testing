#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <linux/futex.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include <os/fence.h>
#undef FUTEX_OP
#include <os/futex.h>
#include <internal/_tls.h>

static struct thread_storage g_tls;
static int                   g_waitReady = -1;
static unsigned int          g_wakeCalls;

struct thread_storage*
__tls_current(void)
{
    return &g_tls;
}

uuid_t
usched_notification_handle(void)
{
    assert(0);
    return UUID_INVALID;
}

oserr_t
OSFutex(
    _In_ OSFutexParameters_t* parameters,
    _In_ OSAsyncContext_t*    asyncContext)
{
    struct timespec timeout = { .tv_sec = 3 };
    int             operation;
    long            result;
    char            ready = 1;

    assert(asyncContext == NULL);
    assert(parameters->Deadline == NULL);
    assert(!(parameters->Flags & FUTEX_FLAG_PRIVATE));
    if (FUTEX_FLAG_ACTION(parameters->Flags) == FUTEX_FLAG_WAIT) {
        operation = FUTEX_WAIT;
        if (g_waitReady != -1) {
            assert(write(g_waitReady, &ready, sizeof(ready)) == sizeof(ready));
            g_waitReady = -1;
        }
    } else {
        assert(FUTEX_FLAG_ACTION(parameters->Flags) == FUTEX_FLAG_WAKE);
        operation = FUTEX_WAKE;
        g_wakeCalls++;
    }
    result = syscall(SYS_futex, parameters->Futex0, operation,
        parameters->Expected0, &timeout, NULL, 0);
    if (result >= 0) {
        return OS_EOK;
    }
    if (errno == ETIMEDOUT) {
        return OS_ETIMEOUT;
    }
    assert(errno == EAGAIN || errno == EINTR);
    return OS_EINTERRUPTED;
}

/** Shared data includes two completions to check independent offsets. */
struct __SharedJob {
    struct OSFenceStorage Completion[2];
    int                   Result;
};

static void
__TestGenericCompletion(void)
{
    struct OSFence* fence = NULL;
    oserr_t         result = OS_EUNKNOWN;

    assert(OSFenceCreate(&fence) == OS_EOK);
    assert(OSFencePrepare(NULL) == OS_EINVALPARAMS);
    assert(OSFencePrepare(fence) == OS_EOK);
    assert(OSFencePrepare(fence) == OS_EBUSY);
    assert(OSFencePoll(fence, &result) == OS_EINVALPARAMS);
    OSFenceCancel(fence);
    assert(OSFencePrepare(fence) == OS_EOK);
    OSFenceSubmit(fence);
    assert(OSFencePoll(fence, &result) == OS_EINPROGRESS);
    assert(OSFenceSignal(fence, OS_ECANCELLED) == OS_EOK);
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK);
    assert(result == OS_ECANCELLED);
    assert(OSFenceReset(fence) == OS_EOK);
    OSFenceRelease(&fence);
    assert(fence == NULL);
}

static void
__WaitInPeer(
    _In_ struct __SharedJob* parentMapping,
    _In_ struct __SharedJob* peerMapping,
    _In_ int                ready,
    _In_ int                completed)
{
    struct OSFence* fence = NULL;
    struct OSFence* other = NULL;
    oserr_t         result = OS_EUNKNOWN;
    char            message;

    assert(munmap(parentMapping, sizeof(*parentMapping)) == 0);
    assert(OSFenceAttachShared(&peerMapping->Completion[1], &fence) == OS_EOK);
    assert(OSFenceAttachShared(&peerMapping->Completion[0], &other) == OS_EOK);
    assert(OSFenceReset(fence) == OS_ENOTSUPPORTED);
    g_waitReady = ready;
    assert(OSFenceWait(fence, NULL, &result) == OS_EOK);
    assert(result == OS_EDEVFAULT && peerMapping->Result == 42);
    assert(OSFencePoll(other, &result) == OS_EINPROGRESS);
    // The producer has released its reference and unmapped its address before
    // this peer reads the result again through its independent mapping.
    assert(read(completed, &message, sizeof(message)) == sizeof(message));
    assert(OSFencePoll(fence, &result) == OS_EOK && result == OS_EDEVFAULT);
    OSFenceRelease(&fence);
    OSFenceRelease(&other);
    assert(munmap(peerMapping, sizeof(*peerMapping)) == 0);
    _exit(0);
}

static void
__TestSharedCompletion(void)
{
    struct __SharedJob* parentMapping;
    struct __SharedJob* peerMapping;
    struct OSFence*     fence = NULL;
    struct OSFence*     other = NULL;
    struct OSFence*     alias;
    FILE*               file;
    pid_t               child;
    int                 ready[2];
    int                 completed[2];
    int                 status;
    char                message = 1;

    file = tmpfile();
    assert(file != NULL);
    assert(ftruncate(fileno(file), sizeof(*parentMapping)) == 0);
    parentMapping = mmap(NULL, sizeof(*parentMapping), PROT_READ | PROT_WRITE,
        MAP_SHARED, fileno(file), 0);
    peerMapping = mmap(NULL, sizeof(*peerMapping), PROT_READ | PROT_WRITE,
        MAP_SHARED, fileno(file), 0);
    assert(parentMapping != MAP_FAILED && peerMapping != MAP_FAILED);
    assert(parentMapping != peerMapping);
    assert(fclose(file) == 0);
    assert(OSFenceInitializeShared(NULL) == OS_EINVALPARAMS);
    assert(OSFenceInitializeShared(&parentMapping->Completion[0]) == OS_EOK);
    assert(OSFenceInitializeShared(&parentMapping->Completion[1]) == OS_EOK);
    assert(OSFenceAttachShared(NULL, &fence) == OS_EINVALPARAMS);
    assert(OSFenceAttachShared(&parentMapping->Completion[1], NULL) == OS_EINVALPARAMS);
    assert(OSFenceAttachShared(&parentMapping->Completion[1], &fence) == OS_EOK);
    assert(OSFenceAttachShared(&parentMapping->Completion[1], &fence) == OS_EBUSY);
    assert(OSFenceAttachShared(&parentMapping->Completion[0], &other) == OS_EOK);
    assert(OSFenceReset(fence) == OS_ENOTSUPPORTED);
    assert(OSFencePrepare(fence) == OS_EOK);
    OSFenceCancel(fence);
    assert(OSFencePrepare(fence) == OS_EOK);
    OSFenceSubmit(fence);
    assert(OSFencePrepare(other) == OS_EOK);
    OSFenceSubmit(other);
    assert(OSFenceAcquire(fence) == OS_EOK);
    alias = fence;
    OSFenceRelease(&alias);
    assert(alias == NULL);
    assert(pipe(ready) == 0 && pipe(completed) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready[0]);
        close(completed[1]);
        __WaitInPeer(parentMapping, peerMapping, ready[1], completed[0]);
    }
    close(ready[1]);
    close(completed[0]);
    assert(munmap(peerMapping, sizeof(*peerMapping)) == 0);
    assert(read(ready[0], &message, sizeof(message)) == sizeof(message));
    parentMapping->Result = 42;
    assert(OSFenceSignal(fence, OS_EDEVFAULT) == OS_EOK);
    assert(g_wakeCalls == 1);
    assert(OSFenceReset(fence) == OS_ENOTSUPPORTED);
    OSFenceRelease(&fence);
    OSFenceRelease(&other);
    assert(munmap(parentMapping, sizeof(*parentMapping)) == 0);
    assert(write(completed[1], &message, sizeof(message)) == sizeof(message));
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(ready[0]);
    close(completed[1]);
}

int
main(void)
{
    __TestGenericCompletion();
    __TestSharedCompletion();
    puts("OS fences: generic completion and cross-process shared mappings passed");
    return 0;
}
