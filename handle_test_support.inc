/**
 * Copyright, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

// Shared handle fixtures without test cases. Define HANDLE_TEST_CUSTOM_SYSCALLS
// when the suite supplies capability checks and user copies. Define
// HANDLE_TEST_REAL_SHM when linking SHM and futex implementations separately.
// The source overrides below allow runners to compile temporary mutations.
#ifndef HANDLES_SOURCE
#define HANDLES_SOURCE "../kernel/handles/handle.c"
#endif
#include HANDLES_SOURCE
#ifndef GRANTS_SOURCE
#define GRANTS_SOURCE "../kernel/handles/grants.c"
#endif
#include GRANTS_SOURCE
#ifndef PROVIDER_SOURCE
#define PROVIDER_SOURCE "../kernel/handles/provider.c"
#endif
#include PROVIDER_SOURCE
#undef __MODULE
#ifndef HANDLE_SET_SOURCE
#define HANDLE_SET_SOURCE "../kernel/handles/set.c"
#endif
#include HANDLE_SET_SOURCE
#undef __MODULE
#ifndef HANDLES_API_SOURCE
#define HANDLES_API_SOURCE "../kernel/api/handles.c"
#endif
#include HANDLES_API_SOURCE
#include <stdio.h>
#include <stdlib.h>

static MSContextSecurity_t* g_current;
static int                  g_failAllocation = -1;
static int                  g_failCopy = -1;
static size_t               g_allocations;
static void __Drain(void);
static bool                 g_allowDestination;
static uuid_t               g_closeSpaceOnCopy = UUID_INVALID;
static uuid_t               g_closeBeforeSetLock = UUID_INVALID;
static uuid_t               g_closeAfterSetUnlock = UUID_INVALID;
static bool                 g_waitContinuation;
static bool                 g_waitFork;
static unsigned int         g_waitCalls;

void
TestBeforeNotificationLock(
    _In_ Spinlock_t* lock)
{
    uuid_t handle;

    if (lock == &g_handleSetsLock && g_closeBeforeSetLock != UUID_INVALID) {
        handle = g_closeBeforeSetLock;
        g_closeBeforeSetLock = UUID_INVALID;
        assert(DestroyHandle(handle) == OS_EINCOMPLETE);
    }
}

void
TestAfterNotificationUnlock(
    _In_ Spinlock_t* lock)
{
    uuid_t handle;

    if (lock == &g_handleSetsLock && g_closeAfterSetUnlock != UUID_INVALID) {
        handle = g_closeAfterSetUnlock;
        g_closeAfterSetUnlock = UUID_INVALID;
        assert(DestroyHandle(handle) == OS_EOK);
    }
}

#ifndef HANDLE_TEST_CUSTOM_SYSCALLS
oserr_t
CapabilityVerify(
    _In_ Capabilities_t capabilities)
{
    assert(capabilities == CAPABILITY_MEMORYSPACE_MANAGEMENT);
    return g_allowDestination ? OS_EOK : OS_EPERMISSIONS;
}

#endif

MSContextSecurity_t*
MemorySpaceGetSecurityContext(void)
{
    return g_current;
}

void*
kmalloc(
    _In_ size_t size)
{
    void* p;
    if (g_failAllocation == 0) {
        return NULL;
    }
    if (g_failAllocation > 0) {
        g_failAllocation--;
    }
    p = malloc(size);
    if (p != NULL) {
        g_allocations++;
    }
    return p;
}

void
kfree(
    _In_ void* p)
{
    if (p != NULL) {
        assert(g_allocations);
        g_allocations--;
        free(p);
    }
}

void*
dsalloc(
    _In_ size_t size)
{
    return malloc(size);
}

void
dsfree(
    _In_ void* p)
{
    free(p);
}

#ifndef HANDLE_TEST_REAL_SHM
oserr_t
FutexWait(
    _In_ OSAsyncContext_t* async,
    _In_ _Atomic(int)*     value,
    _In_ int               expected,
    _In_ int               flags,
    _In_ _Atomic(int)*     other,
    _In_ int               count,
    _In_ int               operation,
    _In_ OSTimestamp_t*    deadline)
{
    if (g_waitFork) {
        assert(async != NULL);
        return OS_EFORKED;
    }
    if (g_waitContinuation) {
        if (g_waitCalls++ == 0) {
            assert(async != NULL);
            return OS_EOK;
        }
        assert(async == NULL);
    }
    (void)async;
    (void)value;
    (void)expected;
    (void)flags;
    (void)other;
    (void)count;
    (void)operation;
    (void)deadline;
    return OS_ETIMEOUT;
}

oserr_t
FutexWake(
    _In_ _Atomic(int)* value,
    _In_ int count,
    _In_ int flags)
{
    (void)value;
    (void)count;
    (void)flags;
    return OS_EOK;
}

// The handle set tries a wait record before a forked wait. These tests model
// the forked wait, so the record path always declines.
oserr_t
FutexWaitAsync(
    _In_ OSAsyncContext_t* async,
    _In_ _Atomic(int)*     value,
    _In_ int               expected,
    _In_ int               flags)
{
    (void)async;
    (void)value;
    (void)expected;
    (void)flags;
    return OS_ENOTSUPPORTED;
}

// No test creates an asynchronous queue, so MarkHandle never signals one.
oserr_t
AsyncQueueSignal(
    _In_ uuid_t handle)
{
    (void)handle;
    abort();
}

#endif

static oserr_t
__CopyProviderUser(
    _In_ void* user,
    _In_ void* kernel,
    _In_ size_t size,
    _In_ bool toUser)
{
    if (g_failCopy == 0) {
        return OS_EINVALPARAMS;
    }
    if (g_failCopy > 0) {
        g_failCopy--;
    }
    if (toUser) {
        memcpy(user, kernel, size);
    } else {
        memcpy(kernel, user, size);
    }
    return OS_EOK;
}

#ifndef HANDLE_TEST_CUSTOM_SYSCALLS
oserr_t
MemorySpaceCopyFromUser(
    _Out_ void*       kernelBuffer,
    _In_  const void* userBuffer,
    _In_  size_t      length)
{
    return __CopyProviderUser((void*)userBuffer, kernelBuffer, length, false);
}

oserr_t
MemorySpaceCopyToUser(
    _Out_ void*       userBuffer,
    _In_  const void* kernelBuffer,
    _In_  size_t      length)
{
    if (g_closeSpaceOnCopy != UUID_INVALID) {
        // Close the selector while the syscall is copying its result. The
        // destination context must still be valid for rollback afterwards.
        assert(DestroyHandle(g_closeSpaceOnCopy) == OS_EOK);
        g_closeSpaceOnCopy = UUID_INVALID;
        __Drain();
    }
    return __CopyProviderUser(userBuffer, (void*)kernelBuffer, length, true);
}

#endif

#ifndef HANDLE_TEST_REAL_SHM
oserr_t
SHMReferenceForExport(
    _In_ uuid_t                     id,
    _In_ const GrantTokenOptions_t* options,
    _In_ HandleType_t*              type)
{
    (void)id;
    (void)options;
    (void)type;
    return OS_ENOTSUPPORTED;
}

#endif

oserr_t
MSContextAcquireSecurity(
    _In_ uint64_t group,
    _In_ MSContextSecurity_t** out)
{
    if (group == 0 || group > UINT32_MAX || (uuid_t)group == UUID_INVALID) {
        return OS_EINVALPARAMS;
    }
    return AcquireGlobalHandle((uuid_t)group, HandleTypeMSContext, (void**)out);
}

static void
__Drain(void)
{
    element_t* entry;
    while ((entry = queue_pop(&g_cleanQueue)) != NULL) {
        __CleanupHandle((struct ResourceHandle*)entry);
    }
}

static void
__NewGroup(
    _In_ MSContextSecurity_t* group)
{
    group->HandleTable = CreateHandleGrantsTable();
    group->GlobalHandleID = CreateGlobalHandle(HandleTypeMSContext, NULL, group);
    assert(group->HandleTable != NULL);
}

static GrantToken_t
__Token(
    _In_ uuid_t reference,
    _In_ uint32_t scope,
    _In_ uuid_t receiver)
{
    GrantTokenOptions_t options = {.ResourceType = GrantTokenResourceTypeProvider,
                                   .TargetType = receiver == UUID_INVALID
                                                     ? GrantTokenTargetTypeAny
                                                     : GrantTokenTargetTypeSpecific,
                                   .TargetGlobalHandleID = receiver,
                                   .Scope = (GrantTokenScope_t)scope};
    GrantToken_t        token;
    assert(GrantTokenCreate(reference, &options, &token) == OS_EOK);
    return token;
}

static void
__ExpectWakeup(
    _In_ uuid_t queue,
    _In_ uuid_t registration)
{
    struct ioset_event event;
    int               count;

    assert(WaitForHandleSet(queue, NULL, &event, 1, 0, NULL, &count) == OS_EOK);
    assert(count == 1 && event.events == IOSETIN);
    assert(event.data.handle == registration);
}

static void
__ExpectNoWakeup(
    _In_ uuid_t queue)
{
    struct ioset_event event;
    int               count;

    assert(WaitForHandleSet(queue, NULL, &event, 1, 0, NULL, &count) == OS_ETIMEOUT);
    assert(atomic_load(&((struct __HandleSet*)LookupHandle(queue, HandleTypeSet))->EventsPending) == 0);
}

