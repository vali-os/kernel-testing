// Publication fixtures test the registry, not PCI interrupt delivery. The real
// runtime and completion callback are exercised by rp1_runtime_test.c.
#include <bus/rp1/runtime-private.h>

static struct Rp1Runtime g_stubRp1Runtime;
static oserr_t g_failRp1Runtime;
static int g_busyRp1Removal;
static unsigned int g_rp1RuntimeStarts;

oserr_t
Rp1RuntimeStart(struct Rp1Bus* bus)
{
    __CheckRp1RuntimeLock(bus, 1);
    g_rp1RuntimeStarts++;
    if (g_failRp1Runtime) {
        return g_failRp1Runtime;
    }
    bus->Runtime = &g_stubRp1Runtime;
    bus->Ready = 1;
    return OS_EOK;
}

oserr_t
Rp1RuntimeStop(struct Rp1Bus* bus)
{
    __CheckRp1RuntimeLock(bus, bus->Runtime != NULL);
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    bus->Runtime = NULL;
    bus->Ready = 0;
    return OS_EOK;
}

oserr_t
Rp1RuntimePrepareRemoval(struct Rp1Bus* bus)
{
    __CheckRp1RuntimeLock(bus, 1);
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    return g_busyRp1Removal ? OS_EBUSY : OS_EOK;
}

oserr_t
Rp1RuntimeControl(struct Rp1Child* child, uuid_t principal, struct DeviceInterruptSourceRequest* request)
{
    __CheckRp1RuntimeLock(child->Bus, 0);
    (void)principal;
    (void)request;
    return OS_ENOTSUPPORTED;
}
