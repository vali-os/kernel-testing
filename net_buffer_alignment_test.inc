#include <os/types/query.h>

static void
__TestBufferCacheAlignment(void)
{
    // Exercise byte-aligned hardware, larger cache lines and stricter hardware.
    const size_t lines[] = { 64, 128, 64, 128, 128 };
    const uint32_t alignments[] = { 1, 1, 1, 1, 256 };
    const uint32_t strides[] = { 1536, 1536, 1600, 1664, 1792 };
    const size_t invalidLines[] = { 0, 3, 8192 };
    struct ctt_netadapter_info info;
    struct ctt_netadapter_pool pool;
    NetBufferConfig_t          config = Config();
    NetBufferManager_t*        manager = NULL;
    NetBufferLease_t           leases[4];
    NetBufferView_t            view;
    uint64_t                  key;
    uintptr_t                 previousLast;
    uintptr_t                 first;
    unsigned int              calls;
    size_t                    i;
    size_t                    slot;

    for (i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        info = Info();
        info.buffer_alignment = alignments[i];
        info.required_headroom = i < 2 ? 0 : 17;
        info.required_tailroom = i < 2 ? 0 : 7;
        g_netCpu.CacheLineSize = lines[i];
        OK(NetBuffersCreate(&g_session, &info, &config, &manager));
        OK(NetBuffersBeginRegistration(manager, RX, &key, &pool));
        CHECK(pool.slot_size == strides[i]);
        OK(NetBuffersRegistered(manager, &g_session, RX, 100));

        // Actual frame addresses must not share cache lines with their neighbors.
        previousLast = 0;
        for (slot = 0; slot < 4; slot++) {
            OK(NetBuffersAcquire(manager, RX, &leases[slot]));
            OK(NetBuffersView(manager, &leases[slot], &view));
            CHECK(((uintptr_t)view.Data & (alignments[i] - 1)) == 0);
            first = (uintptr_t)view.Data & ~(lines[i] - 1);
            CHECK(slot == 0 || first > previousLast);
            previousLast = ((uintptr_t)view.Data + view.Capacity - 1) | (lines[i] - 1);
        }
        for (slot = 0; slot < 4; slot++) {
            OK(NetBuffersRelease(manager, &leases[slot]));
        }
        CloseDestroy(&manager);
    }

    // Invalid or unavailable CPU information must fail before allocating SHM.
    info = Info();
    calls = g_netShm.Calls;
    for (i = 0; i < sizeof(invalidLines) / sizeof(invalidLines[0]); i++) {
        g_netCpu.CacheLineSize = invalidLines[i];
        CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EINVALPARAMS);
        CHECK(manager == NULL && g_netShm.Calls == calls);
    }
    g_netCpu.CacheLineSize = 64;
    g_netCpu.QueryBytes = 0;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_EINVALPARAMS);
    g_netCpu.QueryBytes = sizeof(OSSystemCPUInfo_t);
    g_netCpu.QueryStatus = OS_ENOTSUPPORTED;
    CHECK(NetBuffersCreate(&g_session, &info, &config, &manager) == OS_ENOTSUPPORTED);
    g_netCpu.QueryStatus = OS_EOK;
    CHECK(manager == NULL && g_netShm.Calls == calls);
    puts("Packet alignment: independent cache lines, hardware alignment and query failures passed");
}
