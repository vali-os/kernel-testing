/** Public DMA signatures must stand alone without provider or syscall headers. */
#include <ddk/dma.h>

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
    enum DMADirection, struct DMAMapping**) = DMAMapSHM;
static oserr_t (*const Begin)(struct DMAMapping*, size_t, size_t) = DMABeginTransfer;
static oserr_t (*const End)(struct DMAMapping*, size_t) = DMAEndTransfer;
static oserr_t (*const Unmap)(struct DMAMapping**) = DMAUnmap;

int
DmaApiCompile(void)
{
    return Open != 0 && Close != 0 && Allocate != 0 && Free != 0 &&
        Map != 0 && Begin != 0 && End != 0 && Unmap != 0;
}
