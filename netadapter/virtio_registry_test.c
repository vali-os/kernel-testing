#include "virtio-net.h"
#include <assert.h>
#include <stdio.h>

#define __DDK_CONVERT_H__
#define __INTERNAL_UTILS__

struct sys_device;
static void* from_sys_device(const struct sys_device* description);
uuid_t GetNativeHandle(int descriptor);
int read(int descriptor, void* data, unsigned int length);

#define IOSETSYN 1
struct ioset_event {
    unsigned int events;
    union {
        void* context;
    } data;
};

#include "../../modules/virtio/net/main.c"

static bool         g_failDestroy;
static unsigned int g_destroyAttempts[4];

static void*
from_sys_device(
    _In_ const struct sys_device* description)
{
    (void)description;
    return NULL;
}

void
usched_mtx_lock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}

void
usched_mtx_unlock(
    _In_ struct usched_mtx* mutex)
{
    (void)mutex;
}

void
spinlock_acquire(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

void
spinlock_release(
    _In_ spinlock_t* lock)
{
    (void)lock;
}

oserr_t
VirtioNetDeviceDestroy(
    _In_ VirtioNetDevice_t* device)
{
    uintptr_t id = (uintptr_t)device->Header.key;

    assert(id < 4);
    g_destroyAttempts[id]++;
    return g_failDestroy ? OS_EDEVFAULT : OS_EOK;
}

int
main(
    void)
{
    VirtioNetDevice_t incomplete = {0};
    VirtioNetDevice_t detached = {0};
    VirtioNetDevice_t active = {0};
    VirtioNetClosedSession_t closed = {
        .Owner = 7,
        .Identity = {.id = 8, .generation = 9}
    };
    struct gracht_message message = {.client = 7};
    struct ctt_netadapter_session identity = {.id = 10, .generation = 11};
    Device_t descriptor = {.Id = 2};
    oserr_t status;

    ELEMENT_INIT(&incomplete.Header, (void*)(uintptr_t)1, &incomplete);
    ELEMENT_INIT(&detached.Header, (void*)(uintptr_t)2, &detached);
    ELEMENT_INIT(&active.Header, (void*)(uintptr_t)3, &active);
    incomplete.Metadata.ID = UUID_INVALID;
    incomplete.EventDescriptor = -1;
    incomplete.Session.Active = true;
    incomplete.Session.Owner = message.client;
    incomplete.Session.Identity = identity;
    incomplete.Closed = &closed;
    incomplete.ClosedCount = 1;

    VirtioNetRetainDevice(&incomplete);
    assert(VirtioNetFindDevice(1) == NULL);
    assert(VirtioNetFindSession(&message, &identity) == NULL);
    assert(!VirtioNetWasClosed(&message, &closed.Identity));
    assert(list_count(&g_cleanupDevices) == 1);

    detached.Session = incomplete.Session;
    detached.Closed = &closed;
    detached.ClosedCount = 1;
    list_append(&g_devices, &detached.Header);
    assert(VirtioNetFindDevice(2) == &detached);
    assert(VirtioNetFindSession(&message, &identity) == &detached);
    assert(VirtioNetWasClosed(&message, &closed.Identity));

    g_failDestroy = true;
    status = OnUnregister(&descriptor);
    assert(status == OS_EDEVFAULT);
    assert(VirtioNetFindDevice(2) == NULL);
    assert(VirtioNetFindSession(&message, &identity) == NULL);
    assert(!VirtioNetWasClosed(&message, &closed.Identity));
    assert(list_count(&g_devices) == 0);
    assert(list_count(&g_cleanupDevices) == 2);
    status = OnUnregister(&descriptor);
    assert(status == OS_ENOENT);
    assert(g_destroyAttempts[2] == 1);

    list_append(&g_devices, &active.Header);
    OnUnload();
    assert(g_destroyAttempts[1] == 1);
    assert(g_destroyAttempts[2] == 2);
    assert(g_destroyAttempts[3] == 1);
    assert(list_count(&g_devices) == 0);
    assert(list_count(&g_cleanupDevices) == 3);
    assert(VirtioNetFindDevice(3) == NULL);

    g_failDestroy = false;
    OnUnload();
    assert(g_destroyAttempts[1] == 2);
    assert(g_destroyAttempts[2] == 3);
    assert(g_destroyAttempts[3] == 2);
    assert(list_count(&g_devices) == 0);
    assert(list_count(&g_cleanupDevices) == 0);

    puts("virtio-net registry: failed cleanup stays private and unload retries both lists");
    return 0;
}