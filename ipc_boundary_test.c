/** Exercise syscall range validation around kernel-only IPC storage. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <machine.h>
#include <memoryspace.h>
#include <shm.h>
#include <futex.h>
#include <threading.h>

oserr_t ScSHMMap(SHMHandle_t*, size_t, size_t, unsigned int);
oserr_t ScFutexWait(OSAsyncContext_t*, OSFutexParameters_t*);
oserr_t ScFutexWake(OSFutexParameters_t*);

static SystemMachine_t g_machine;
static int             g_maps;
static int             g_waits;
static int             g_wakes;

static OSTimestamp_t   g_deadline;

SystemMachine_t* GetMachine(void) { return &g_machine; }

oserr_t
MemorySpaceCopyUser(void* user, void* kernel, size_t length, bool toUser)
{
    if (user == NULL) {
        return OS_EINVALPARAMS;
    }
    memcpy(toUser ? user : kernel, toUser ? kernel : user, length);
    return OS_EOK;
}

oserr_t
SHMMap(SHMHandle_t* handle, size_t offset, size_t length, unsigned int flags)
{
    (void)handle; (void)offset; (void)length; (void)flags;
    g_maps++;
    return OS_EOK;
}



oserr_t
FutexWait(OSAsyncContext_t* async, atomic_int* futex, int expected, int flags,
          atomic_int* second, int count, int operation, OSTimestamp_t* deadline)
{
    (void)futex; (void)expected; (void)flags; (void)second; (void)count; (void)operation;
    (void)async;
    assert(deadline == &g_deadline);
    g_waits++;
    return OS_ETIMEOUT;
}

oserr_t
FutexWake(atomic_int* futex, int count, int flags)
{
    (void)futex; (void)count; (void)flags;
    g_wakes++;
    return OS_EOK;
}

oserr_t
FutexWakeOperation(atomic_int* futex, int count, atomic_int* second,
                   int secondCount, int operation, int flags)
{
    (void)second; (void)secondCount; (void)operation;
    return FutexWake(futex, count, flags);
}

int
main(void)
{
    SHMHandle_t         handle = {.Buffer = (void*)0x1fff, .Length = 1, .Offset = 0};
    OSFutexParameters_t parameters = {.Futex0 = (atomic_int*)0x1000, .Deadline = &g_deadline};
    OSAsyncContext_t    async = {0};

    g_machine.MemoryMap.UserCode = (SystemMemoryRange_t){.Start = 0x1000, .Length = 0x1000};
    g_machine.MemoryMap.UserHeap = (SystemMemoryRange_t){.Start = 0x4000, .Length = 0x2000};
    g_machine.MemoryMap.ThreadLocal = (SystemMemoryRange_t){.Start = UINTPTR_MAX - 0xfff, .Length = 0xfff};
    assert(__UserMemoryRangeValid(0x1000, 0x1000));
    assert(__UserMemoryRangeValid(0x1fff, 1));
    assert(!__UserMemoryRangeValid(0, 1));
    assert(!__UserMemoryRangeValid(0x1000, 0));
    assert(!__UserMemoryRangeValid(0x1fff, 2));
    assert(!__UserMemoryRangeValid(0x3000, 1));
    assert(!__UserMemoryRangeValid(UINTPTR_MAX - 1, 4));
    assert(ScSHMMap(&handle, 0, 2, 0) == OS_EPERMISSIONS && g_maps == 0);
    assert(ScSHMMap(&handle, 0, 1, 0) == OS_EOK && g_maps == 1);
    handle.Buffer = (void*)(UINTPTR_MAX - 1);
    assert(ScSHMMap(&handle, 0, 4, 0) == OS_EPERMISSIONS && g_maps == 1);
    handle.Buffer = (void*)0x3000;
    assert(ScSHMMap(&handle, 1, 1, 0) == OS_EPERMISSIONS && g_maps == 1);

    parameters.Flags = FUTEX_FLAG_WAKE | FUTEX_FLAG_OP;
    parameters.Futex1 = (atomic_int*)0x3000;
    assert(ScFutexWake(&parameters) == OS_EPERMISSIONS && g_wakes == 0);
    parameters.Futex1 = (atomic_int*)0x4000;
    assert(ScFutexWake(&parameters) == OS_EOK && g_wakes == 1);
    parameters.Futex0 = (atomic_int*)0x1fff;
    assert(ScFutexWake(&parameters) == OS_EPERMISSIONS && g_wakes == 1);
    assert(ScFutexWait(&async, &parameters) == OS_EPERMISSIONS && g_waits == 0);
    parameters.Futex0 = (atomic_int*)0x1000;
    parameters.Flags = FUTEX_FLAG_WAIT;
    assert(ScFutexWait(&async, &parameters) == OS_ETIMEOUT && g_waits == 1);
    assert(ScFutexWait(NULL, &parameters) == OS_ETIMEOUT && g_waits == 2);
    puts("IPC syscall boundaries: remap spans, wrapping, futex targets passed");
    return 0;
}
