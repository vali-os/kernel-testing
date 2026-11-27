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
    bus->State = Rp1BusRunning;
    return OS_EOK;
}

oserr_t
Rp1RuntimeStop(struct Rp1Bus* bus)
{
    __CheckRp1RuntimeLock(bus, bus->Runtime != NULL);
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    bus->Runtime = NULL;
    bus->State = Rp1BusStopped;
    return OS_EOK;
}

oserr_t
Rp1RuntimePrepareRemoval(struct Rp1Bus* bus)
{
    __CheckRp1RuntimeLock(bus, 1);
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    return g_busyRp1Removal ? OS_EBUSY : OS_EOK;
}

// Same answer as the real function in runtime-source.c, which these fixtures
// do not link.
int
Rp1RuntimeChildHoldsSource(const struct Rp1Child* child)
{
    if (child->Bus->Runtime == NULL) {
        return 0;
    }
    for (unsigned int i = 0; i < FDT_RP1_INTERRUPT_COUNT; i++) {
        if (child->Bus->Runtime->Sources[i].Owner == child) {
            return 1;
        }
    }
    return 0;
}

oserr_t
Rp1RuntimeControl(struct Rp1Child* child, uuid_t principal, struct DeviceInterruptSourceRequest* request)
{
    __CheckRp1RuntimeLock(child->Bus, 0);
    (void)principal;
    (void)request;
    return OS_ENOTSUPPORTED;
}
