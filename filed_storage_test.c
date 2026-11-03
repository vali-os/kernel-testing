// Exercise the real GPT/MBR fallback and detection with a two-sector disk.
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <ddk/utils.h>
#include <os/handle.h>
#include <os/shm.h>
#include <vfs/mbr.h>

static unsigned char g_disk[2][512];
static unsigned char g_buffer[512];
static int           g_reads;
static int           g_failRead;
static int           g_registrations;
static int           g_releases;
static int           g_unknownWarnings;
static int           g_active;
static int           g_failCreate;
static uint64_t      g_expectedSector;

void
SystemDebug(
    _In_ enum OSSysLogLevel level,
    _In_ const char*        format,
    ...)
{
    if (!strcmp(format, "Unknown filesystem detected")) {
        g_unknownWarnings++;
    }
}

oserr_t
SHMCreate(
    _In_  SHM_t*      request,
    _Out_ OSHandle_t* handle)
{
    assert(request->Size == sizeof(g_buffer));
    assert(!g_active);
    if (g_failCreate) {
        return OS_EOOM;
    }
    g_active = 1;
    *handle = (OSHandle_t){.ID = 7, .Payload = g_buffer};
    return OS_EOK;
}

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    assert(g_active && handle->ID == 7);
    return handle->Payload;
}

void
OSHandleDestroy(
    _In_ OSHandle_t* handle)
{
    assert(g_active && handle->ID == 7);
    g_active = 0;
    g_releases++;
}

static oserr_t
__Read(
    _In_  struct VFSStorage* storage,
    _In_  uuid_t             handle,
    _In_  size_t             offset,
    _In_  UInteger64_t*      sector,
    _In_  size_t             count,
    _Out_ size_t*            sectorsRead)
{
    assert(g_active && handle == 7 && offset == 0 && count == 1);
    assert(sector != NULL && sector->QuadPart < 2);
    // GPT reads LBA 1; MBR and whole-disk detection each read LBA 0.
    assert(sector->QuadPart == (g_reads == 0 ? 1 : 0));
    g_reads++;
    *sectorsRead = 0;
    if (g_reads == g_failRead) {
        return OS_EDEVFAULT;
    }
    memcpy(g_buffer, g_disk[sector->QuadPart], sizeof(g_buffer));
    *sectorsRead = 1;
    return OS_EOK;
}

oserr_t
VFSStorageRegisterAndSetupPartition(
    _In_ struct VFSStorage* storage,
    _In_ int                partitionIndex,
    _In_ UInteger64_t*      sector,
    _In_ guid_t*            guid,
    _In_ const char*        typeHint,
    _In_ guid_t*            typeGuid,
    _In_ uuid_t             interfaceDriverID,
    _In_ mstring_t*         mountPoint)
{
    assert(g_active && partitionIndex == 0);
    assert(sector != NULL && sector->QuadPart == g_expectedSector);
    assert(!strcmp(typeHint, "mfs"));
    g_registrations++;
    return OS_EOK;
}

static void
__Reset(void)
{
    assert(!g_active);
    memset(g_disk, 0, sizeof(g_disk));
    memset(g_buffer, 0, sizeof(g_buffer));
    g_reads = g_failRead = g_registrations = g_releases = 0;
    g_unknownWarnings = g_failCreate = 0;
    g_expectedSector = 0;
}

int
main(void)
{
    struct VFSStorage storage = {0};
    MasterBootRecord_t mbr = {0};

    storage.Stats.SectorSize = sizeof(g_buffer);
    storage.Operations.Read = __Read;

    __Reset();
    assert(VFSStorageParse(&storage) == OS_EUNKNOWN);
    assert(g_reads == 3 && g_registrations == 0 && g_unknownWarnings == 1);
    assert(g_releases == 1 && !g_active);

    // A disk without a partition table can still contain a filesystem at LBA 0.
    __Reset();
    memcpy(&g_disk[0][3], "MFS1", 4);
    assert(VFSStorageParse(&storage) == OS_EOK);
    assert(g_reads == 3 && g_registrations == 1 && g_unknownWarnings == 0);
    assert(g_releases == 1 && !g_active);

    // A valid partition must keep its start sector and skip whole-disk detection.
    __Reset();
    mbr.Partitions[0].Type = 0x61;
    mbr.Partitions[0].LbaSector = 32;
    mbr.Partitions[0].LbaSize = 64;
    mbr.BootSignature[0] = 0x55;
    mbr.BootSignature[1] = 0xAA;
    memcpy(g_disk[0], &mbr, sizeof(mbr));
    g_expectedSector = 32;
    assert(VFSStorageParse(&storage) == OS_EOK);
    assert(g_reads == 2 && g_registrations == 1);
    assert(g_releases == 1 && !g_active);

    __Reset();
    g_failRead = 3;
    assert(VFSStorageParse(&storage) == OS_EDEVFAULT);
    assert(g_reads == 3 && g_registrations == 0);
    assert(g_releases == 1 && !g_active);

    __Reset();
    g_failCreate = 1;
    assert(VFSStorageParse(&storage) == OS_EOOM);
    assert(g_reads == 0 && g_releases == 0 && !g_active);
    puts("filed storage: blank disk, whole-disk filesystem, MBR and cleanup passed");
    return 0;
}
