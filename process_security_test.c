#include <assert.h>
#include <stdio.h>
#include <process_security.h>
#include <os/types/syslog.h>

void
SystemDebug(enum OSSysLogLevel level, const char* format, ...)
{
    (void)level;
    (void)format;
}

int
main(void)
{
    struct gracht_security_context caller = {42, 0, GRACHT_SECURITY_KERNEL};

    assert(PmAuthorizeSpawn(NULL, 0) == OS_EOK);
    assert(PmAuthorizeSpawn(NULL, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.capabilities = CAPABILITY_DEVICEIO_ACCESS;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.capabilities = CAPABILITY_GRANT_CAPABILITY;
    // Grant authority permits delegating rights the sender does not itself hold.
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EOK);
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_ALL) == OS_EOK);
    assert(PmAuthorizeSpawn(&caller, (Capabilities_t)0x80000000U) == OS_EINVALPARAMS);
    caller.source = GRACHT_SECURITY_NONE;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    caller.source = GRACHT_SECURITY_KERNEL;
    caller.principal = 0;
    assert(PmAuthorizeSpawn(&caller, CAPABILITY_DEVICEIO_ACCESS) == OS_EPERMISSIONS);
    puts("Process authorization: missing, unauthenticated, unprivileged and authorized callers passed");
    return 0;
}
