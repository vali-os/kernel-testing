#ifndef NET_TEST_SHM_H
#define NET_TEST_SHM_H
#include <os/osdefs.h>
struct NetTestShmState { unsigned int Live, Calls, FailCall; };
extern struct NetTestShmState g_netShm;
/** Fake driver's mapping of a registered pool. NULL if the handle was freed. */
void* NetTestShmData(uuid_t handle);
#endif
