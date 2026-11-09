// Verify the DDK creation result stays unchanged when the syscall fails.
#include <ddk/memory.h>
#include <os/types/syscall.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static oserr_t g_status;
static unsigned int g_calls;

oserr_t
Syscall_CreateMemorySpace(
    _In_  unsigned int               flags,
    _Out_ MemorySpaceCreateResult_t* resultOut)
{
    assert(flags == 0);
    g_calls++;
    resultOut->Handle = 42;
    resultOut->SecurityGroupID = 71;
    return g_status;
}

int
main(void)
{
    uuid_t handle = 100;
    uuid_t securityGroupID = 200;

    assert(CreateMemorySpace(0, NULL, &securityGroupID) == OS_EUNKNOWN);
    assert(CreateMemorySpace(0, &handle, NULL) == OS_EUNKNOWN);
    assert(CreateMemorySpace(0, NULL, NULL) == OS_EUNKNOWN);
    assert(handle == 100 && securityGroupID == 200);
    assert(g_calls == 0);

    g_status = OS_EPERMISSIONS;
    assert(CreateMemorySpace(0, &handle, &securityGroupID) == OS_EPERMISSIONS);
    assert(handle == 100 && securityGroupID == 200);

    g_status = OS_EOK;
    assert(CreateMemorySpace(0, &handle, &securityGroupID) == OS_EOK);
    assert(handle == 42 && securityGroupID == 71);
    assert(g_calls == 2);
    puts("Memory-space client: result propagation and failure isolation passed");
    return 0;
}
