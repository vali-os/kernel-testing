#ifndef NET_TEST_SHM_H
#define NET_TEST_SHM_H
#include <os/osdefs.h>
struct NetTestShmState { unsigned int Live, Calls, FailCall, DeviceCalls, TokensLive; };
extern struct NetTestShmState g_netShm;
/** CPU query result and failure controls used when sizing packet pools. */
struct NetTestCpuState {
    size_t       CacheLineSize;
    size_t       QueryBytes;
    oserr_t      QueryStatus;
};
extern struct NetTestCpuState g_netCpu;
/** Fake driver's mapping of a registered pool. NULL if the handle was freed. */
void* NetTestShmData(uuid_t handle);
void* NetTestTokenData(uuid_t token);
#endif
