/** Registry regression tests. Drive worker steps explicitly to cover discovery
 * interleavings without timing assumptions or a running guest. No real DMA is
 * submitted: state changes below model the outcome of remote close barriers.
 */
#include "session.h"
#include "adapters.c"
#include <assert.h>
#include <stdio.h>
#include <threads.h>

// The registry has a single lock, so back the usched mutex with one host mutex.
static mtx_t g_hostLock;
void usched_mtx_init(struct usched_mtx* mutex, int type) { assert(mtx_init(&g_hostLock, mtx_plain) == thrd_success); }
void usched_mtx_lock(struct usched_mtx* mutex) { mtx_lock(&g_hostLock); }
void usched_mtx_unlock(struct usched_mtx* mutex) { mtx_unlock(&g_hostLock); }

gracht_protocol_t ctt_netadapter_client_protocol;
static unsigned clientsCreated, clientsDestroyed;
static bool failAllocation;
int NetAdapterClientCreate(int set, gracht_protocol_t* protocol, gracht_client_t** out)
{
    if (failAllocation) return -1;
    *out = (gracht_client_t*)(uintptr_t)++clientsCreated;
    return 0;
}
void NetAdapterClientDestroy(int set, gracht_client_t* client) { ++clientsDestroyed; }
gracht_conn_t gracht_client_iod(gracht_client_t* client) { return (gracht_conn_t)(uintptr_t)client; }
int write(int fd, const void* data, unsigned int length) { return (int)length; }

/* A callback arriving on another thread must wait for the worker's mutex. */
static int Announce(void* unused)
{
    NetworkAdaptersDiscover(20, 200);
    return 0;
}

int main(void)
{
    usched_mtx_init(&g_lock, USCHED_MUTEX_PLAIN);
    g_initialized = true;
    NetworkAdaptersDiscover(10, 100);
    NetworkAdaptersDiscover(10, 100);
    struct AdapterEntry* entry = __FindPort(10, 0);
    assert(entry && !entry->Adapter && entry->PendingDriver == 100);
    NetAdapterSnapshot_t snapshot;
    memset(&snapshot, 0xff, sizeof(snapshot));
    assert(NetworkAdaptersSnapshot(10, 0, &snapshot) == OS_EOK);
    assert(snapshot.State == NET_ADAPTER_INFO && snapshot.Info.port_count == 0);
    assert(NetworkAdaptersSetRunning(10, 0, true) == OS_EBUSY);

    failAllocation = true;
    AttachPendingPort(entry, 0);
    assert(entry->PendingDriver == 100 && !entry->Adapter);
    failAllocation = false;
    AttachPendingPort(entry, 999);
    assert(!entry->Adapter);
    AttachPendingPort(entry, 1000);
    assert(entry->Adapter && entry->Driver == 100 && clientsCreated == 1);
    NetworkAdaptersDiscover(10, 100);
    assert(!entry->Removed && !entry->PendingDriver);

    // Port expansion belongs to the old driver's advertised capabilities.
    entry->Adapter->Info.port_count = 3;
    __UpdatePort(entry);
    struct AdapterEntry* secondary = __FindPort(10, 1);
    AttachPendingPort(secondary, 1000);
    assert(secondary->Adapter && __FindPort(10, 2));
    NetworkAdaptersDiscover(10, 101);
    assert(entry->Removed && entry->PendingDriver == 101);
    assert(secondary->Removed && !secondary->PendingDriver);
    assert(!__FindPort(10, 2));

    // Neither a second announcement nor the worker may bypass quarantine.
    entry->Adapter->State = NET_ADAPTER_QUARANTINED;
    NetworkAdaptersDiscover(10, 102);
    __UpdatePort(entry);
    AttachPendingPort(entry, 2000);
    assert(entry->Driver == 100 && entry->PendingDriver == 102);
    assert(clientsCreated == 2 && clientsDestroyed == 0);
    // Latest intent can even return to the original driver during close.
    NetworkAdaptersDiscover(10, 100);
    assert(entry->PendingDriver == 100);
    NetworkAdaptersDiscover(10, 102);
    entry->Adapter->State = NET_ADAPTER_CLOSED;
    __UpdatePort(entry);
    assert(!entry->Adapter && entry->PendingDriver == 102);
    AttachPendingPort(entry, 2000);
    assert(entry->Driver == 102 && !entry->Removed);
    assert(clientsCreated == 3 && clientsDestroyed == 1);
    entry->Adapter->Info.port_count = 1;
    secondary->Adapter->State = NET_ADAPTER_CLOSED;
    __UpdatePort(secondary);
    __UpdatePort(entry);
    assert(!__FindPort(10, 1));

    NetworkAdaptersDiscover(10, 103);
    NetworkAdaptersRemove(10);
    assert(!entry->PendingDriver && entry->Removed);
    entry->Adapter->State = NET_ADAPTER_CLOSED;
    __UpdatePort(entry);
    AttachPendingPort(entry, 3000);
    assert(!__FindPort(10, 0) && clientsCreated == clientsDestroyed);
    NetworkAdaptersDiscover(11, 110);
    NetworkAdaptersRemove(11);
    assert(!__FindPort(11, 0));

    usched_mtx_lock(&g_lock);
    thrd_t thread;
    assert(thrd_create(&thread, Announce, NULL) == thrd_success);
    assert(!__FindPort(20, 0));
    usched_mtx_unlock(&g_lock);
    thrd_join(thread, NULL);
    assert(__FindPort(20, 0)->PendingDriver == 200);
    NetworkAdaptersRemove(20);
    mtx_destroy(&g_hostLock);
    puts("adapter registry: PASS (replacement, quarantine, retry, removal, multiport, callback locking)");
    return 0;
}
