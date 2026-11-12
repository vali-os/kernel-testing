/** Public DMA signatures must stand alone without provider or syscall headers. */
#include <ddk/dma.h>
#include <os/shm.h>

#ifdef DMA_MAX_RANGES
#error Provider configuration must not leak through the driver-facing DMA header
#endif

_Static_assert(sizeof(struct DMASegment) == 16, "Device addresses remain 64-bit on every target");

// Function pointer assignments verify the exact ownership types without running
// hardware. In particular, a buffer cannot be passed to the streaming handoffs.
static oserr_t (*const Open)(uuid_t, uint64_t, struct DMAContext**) = DMAOpen;
static oserr_t (*const Close)(struct DMAContext**) = DMAClose;
static oserr_t (*const Allocate)(struct DMAContext*, const struct DMARequirements*, struct DMABuffer**) = DMAAllocate;
static oserr_t (*const Free)(struct DMABuffer**) = DMAFree;
static oserr_t (*const Map)(struct DMAContext*, const OSHandle_t*, size_t, size_t,
    enum DMADirection, const struct DMAMapRequirements*, struct DMAMapping**) = DMAMapSHM;
static oserr_t (*const MapDirect)(struct DMAContext*, const OSHandle_t*, size_t, size_t,
    enum DMADirection, const struct DMAMapRequirements*, struct DMAMapping**) = DMAMapSHMDirect;
static oserr_t (*const MapPersistent)(struct DMAContext*, const OSHandle_t*, size_t, size_t,
    enum DMADirection, const struct DMAMapRequirements*, struct DMAMapping**) = DMAMapSHMPersistent;
static oserr_t (*const BeginUse)(struct DMAMapping*, struct DMAUse*, enum SHMWaitMode, const OSTimestamp_t*) = DMABeginUse;
static oserr_t (*const EndUse)(struct DMAUse*) = DMAEndUse;
static oserr_t (*const UnmapPersistent)(struct DMAMapping**) = DMAUnmap;
static oserr_t (*const BeginCpu)(struct SHMAccessSession*, enum SHMCpuAccess, enum SHMWaitMode, const OSTimestamp_t*) = SHMAccessBeginCpu;
static oserr_t (*const EndCpu)(struct SHMAccessSession*) = SHMAccessEndCpu;
static oserr_t (*const FenceWait)(OSFence_t*, const OSTimestamp_t*, oserr_t*) = OSFenceWait;
static oserr_t (*const BeginFenced)(struct DMAMapping*, struct DMAFencedUse*, OSFence_t*, enum SHMWaitMode, const OSTimestamp_t*) = DMABeginUseFenced;
static oserr_t (*const EndFenced)(struct DMAFencedUse*, oserr_t) = DMAEndUseFenced;
static oserr_t (*const Begin)(struct DMAMapping*, size_t, size_t, enum SHMWaitMode, const OSTimestamp_t*) = DMABeginTransfer;
static oserr_t (*const End)(struct DMAMapping*, size_t) = DMAEndTransfer;
static oserr_t (*const Unmap)(struct DMAMapping**) = DMAUnmap;

int
DmaApiCompile(void)
{
    return Open != 0 && Close != 0 && Allocate != 0 && Free != 0 &&
        BeginCpu != 0 && EndCpu != 0 && FenceWait != 0 && BeginFenced != 0 && EndFenced != 0 &&
        MapPersistent != 0 && BeginUse != 0 && EndUse != 0 && UnmapPersistent != 0 &&
        Map != 0 && MapDirect != 0 && Begin != 0 && End != 0 && Unmap != 0;
}
