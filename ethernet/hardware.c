/** Copyright 2026, Philip Meulengracht. SPDX-License-Identifier: GPL-3.0-or-later */
#include "adapters/adapters.h"
#include <ddk/utils.h>
#include <stdatomic.h>
#include <string.h>
#include <threads.h>

// Dedicated test image only. EtherType 0x88b5, marker VALIGEN1, operation 1/2,
// big-endian sequence, then a deterministic pattern. No IP stack is required.
static atomic_uint g_device = UUID_INVALID;
static atomic_uint g_expected;
static atomic_uint g_received;
static atomic_uint g_completed;
static atomic_bool g_ready;
static uint8_t g_mac[6];

static void
__Link(
    _In_ uuid_t device,
    _In_ uint32_t port,
    _In_ const struct ctt_netadapter_link* link)
{
    unsigned expected = UUID_INVALID;

    if (port == 0 && link->status == CTT_NETADAPTER_LINK_STATUS_UP) {
        atomic_compare_exchange_strong(&g_device, &expected, device);
    }
}

static void
__Receive(
    _In_ uuid_t device,
    _In_ uint32_t port,
    _In_ const void* data,
    _In_ uint32_t length)
{
    const uint8_t* frame = data;
    uint32_t sequence;
    uint32_t i;

    if (!atomic_load(&g_ready) || device != atomic_load(&g_device) || port != 0) {
        return;
    }
    if (length != 60 && length != 1514) {
        return;
    }
    if (memcmp(frame, g_mac, 6) || frame[12] != 0x88 || frame[13] != 0xb5) {
        return;
    }
    if (memcmp(frame + 14, "VALIGEN1", 8) || frame[22] != 2) {
        return;
    }
    sequence = (uint32_t)frame[23] << 24 | (uint32_t)frame[24] << 16 |
        (uint32_t)frame[25] << 8 | frame[26];
    if (sequence != atomic_load(&g_expected) || length != (sequence & 1 ? 60 : 1514)) {
        return;
    }
    for (i = 27; i < length; i++) {
        if (frame[i] != (uint8_t)(i ^ sequence)) {
            ERROR("ETHERNET HW payload mismatch: seq=%u offset=%u", sequence, i);
            return;
        }
    }
    if (atomic_exchange(&g_received, sequence) != sequence) {
        NOTICE("ETHERNET HW peer reply verified: seq=%u bytes=%u", sequence, length);
    }
}

static void
__Transmitted(
    _In_ uuid_t device,
    _In_ uint32_t port,
    _In_ uint64_t cookie,
    _In_ oserr_t status)
{
    if (device != atomic_load(&g_device) || port != 0) {
        return;
    }
    if (status != OS_EOK) {
        ERROR("ETHERNET HW TX completion failed: seq=%llu status=%u",
            (unsigned long long)cookie, status);
        return;
    }
    atomic_store(&g_completed, (unsigned)cookie);
}

static int
__Run(
    _In_ void* unused)
{
    NetAdapterSnapshot_t snapshot;
    struct ctt_netadapter_mac mac;
    uint8_t frame[1514];
    uint32_t sequence = 1;
    uint32_t length;
    uint32_t i;
    unsigned waiting = 0;
    uuid_t device;
    oserr_t status;
    (void)unused;

    NOTICE("ETHERNET HW test enabled: connect Linux peer; sending at most one frame/second");
    for (;;) {
        thrd_sleep(&(struct timespec){ .tv_sec = 1 }, NULL);
        device = atomic_load(&g_device);
        if (device == UUID_INVALID) {
            if (waiting++ % 5 == 0) {
                NOTICE("ETHERNET HW waiting for adapter link");
            }
            continue;
        }
        status = NetworkAdaptersSnapshot(device, 0, &snapshot);
        if (status != OS_EOK) {
            ERROR("ETHERNET HW adapter unavailable: %u", status);
            return -1;
        }
        if (snapshot.State != NET_ADAPTER_RUNNING ||
            snapshot.Link.status != CTT_NETADAPTER_LINK_STATUS_UP) {
            if (waiting++ % 5 == 0) {
                NOTICE("ETHERNET HW waiting: state=%u link=%u error=%u",
                    snapshot.State, snapshot.Link.status, snapshot.LastError);
            }
            continue;
        }
        if (!atomic_load(&g_ready)) {
            mac = snapshot.Info.current_mac;
            g_mac[0] = mac.octet0;
            g_mac[1] = mac.octet1;
            g_mac[2] = mac.octet2;
            g_mac[3] = mac.octet3;
            g_mac[4] = mac.octet4;
            g_mac[5] = mac.octet5;
            atomic_store(&g_ready, true);
        }
        if (sequence > 1) {
            if (atomic_load(&g_received) == sequence - 1 &&
                atomic_load(&g_completed) == sequence - 1) {
                NOTICE("ETHERNET HW TX/RX PASS seq=%u", sequence - 1);
            } else {
                NOTICE("ETHERNET HW pending/missing: seq=%u last-TX=%u last-RX=%u",
                    sequence - 1, atomic_load(&g_completed), atomic_load(&g_received));
            }
        }
        length = sequence & 1 ? 60 : 1514;
        memset(frame, 0xff, 6);
        memcpy(frame + 6, g_mac, 6);
        frame[12] = 0x88;
        frame[13] = 0xb5;
        memcpy(frame + 14, "VALIGEN1", 8);
        frame[22] = 1;
        frame[23] = sequence >> 24;
        frame[24] = sequence >> 16;
        frame[25] = sequence >> 8;
        frame[26] = sequence;
        for (i = 27; i < length; i++) {
            frame[i] = (uint8_t)(i ^ sequence);
        }
        atomic_store(&g_expected, sequence);
        status = NetworkAdaptersSend(device, 0, frame, length, sequence);
        if (status != OS_EOK) {
            NOTICE("ETHERNET HW send rejected: seq=%u status=%u", sequence, status);
        }
        if (++sequence == 0) {
            return 0;
        }
    }
}

void
NetworkAdaptersRunHardwareTests(void)
{
    NetworkAdapterOps_t callbacks = {
        .Receive = __Receive,
        .Transmitted = __Transmitted,
        .Link = __Link
    };
    thrd_t thread;

    NetworkAdaptersSetHooks(&callbacks);
    if (thrd_create(&thread, __Run, NULL) != thrd_success) {
        ERROR("ETHERNET HW cannot start test thread");
        NetworkAdaptersSetHooks(NULL);
        return;
    }
    thrd_detach(thread);
}
