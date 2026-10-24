// Publication fixtures test the registry, not PCI interrupt delivery. The real
// runtime and completion callback are exercised by rp1_runtime_test.c.
#include <bus/rp1/runtime-private.h>

static struct Rp1Runtime g_stubRp1Runtime;
static int g_failRp1Runtime;
static int g_busyRp1Removal;

oserr_t
Rp1RuntimeStart(struct Rp1Bus* bus)
{
    if (g_failRp1Runtime) {
        return OS_ENOTSUPPORTED;
    }
    bus->Runtime = &g_stubRp1Runtime;
    bus->Ready = 1;
    return OS_EOK;
}

oserr_t
Rp1RuntimeStop(struct Rp1Bus* bus)
{
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    bus->Runtime = NULL;
    bus->Ready = 0;
    return OS_EOK;
}

oserr_t
Rp1RuntimePrepareRemoval(struct Rp1Bus* bus)
{
    CHECK(bus->Runtime == NULL || bus->Runtime == &g_stubRp1Runtime);
    return g_busyRp1Removal ? OS_EBUSY : OS_EOK;
}

oserr_t
Rp1RuntimeControl(struct Rp1Child* child, uuid_t principal, struct PciInterruptRequest* request)
{
    (void)child;
    (void)principal;
    (void)request;
    CHECK(0);
    return OS_ENOTSUPPORTED;
}
