// Exercise the actual MFS partial-sector path through AHCI's DMA request setup.
#define main __AhciRegressionMain
#include "ahci_dma_test.c"
#undef main
#include "../modules/filesystems/mfs/file_operations.c"

static AhciDevice_t* g_mfsDevice;
static uint8_t g_mfsTransferData[512];
static unsigned int g_mfsReads;
static unsigned int g_mfsWrites;

void*
SHMBuffer(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 43);
    return g_mfsTransferData;
}

size_t
SHMBufferLength(
    _In_ OSHandle_t* handle)
{
    CHECK(handle->ID == 43);
    return sizeof(g_mfsTransferData);
}

oserr_t
MfsEnsureRecordSpace(
    _In_ FileSystemMFS_t* mfs,
    _In_ MFSEntry_t*      entry,
    _In_ uint64_t         spaceRequired)
{
    CHECK(spaceRequired <= entry->AllocatedSize);
    return OS_EOK;
}

oserr_t
MFSAdvanceToNextBucket(
    _In_ FileSystemMFS_t* mfs,
    _In_ MFSEntry_t*      entry)
{
    CHECK(false);
    return OS_ENOENT;
}

static oserr_t
__TestStorageTransfer(
    _In_  int           direction,
    _In_  uuid_t        buffer,
    _In_  size_t        offset,
    _In_  UInteger64_t* sector,
    _In_  size_t        count,
    _Out_ size_t*       completed)
{
    struct gracht_message message = {0};
    AhciTransaction_t* transaction;
    oserr_t status;

    status = AhciTransactionStorageCreate(
        g_mfsDevice, &message, direction, sector->QuadPart, buffer, offset, count
    );
    if (status != OS_EOK) {
        *completed = 0;
        return status;
    }
    transaction = g_mfsDevice->Port->Transactions.tail->value;
    __Complete(g_mfsDevice, transaction, count * 512);
    *completed = g_responseSectors;
    return g_responseStatus;
}

oserr_t
FSStorageRead(
    _In_  struct VFSStorageParameters* storageParameters,
    _In_  uuid_t                       buffer,
    _In_  size_t                       offset,
    _In_  UInteger64_t*                sector,
    _In_  size_t                       count,
    _Out_ size_t*                      read)
{
    g_mfsReads++;
    return __TestStorageTransfer(__STORAGE_OPERATION_READ, buffer, offset, sector, count, read);
}

oserr_t
FSStorageWrite(
    _In_  struct VFSStorageParameters* storageParameters,
    _In_  uuid_t                       buffer,
    _In_  size_t                       offset,
    _In_  UInteger64_t*                sector,
    _In_  size_t                       count,
    _Out_ size_t*                      written)
{
    g_mfsWrites++;
    return __TestStorageTransfer(__STORAGE_OPERATION_WRITE, buffer, offset, sector, count, written);
}

int
main(void)
{
    FileSystemMFS_t mfs = {0};
    MFSEntry_t entry = {0};
    AhciController_t* controller;
    AhciDevice_t device;
    uint8_t buffer[1024] = {0};
    size_t completed;
    oserr_t status;

    controller = __CreateController(true, &status);
    CHECK(status == OS_EOK);
    device = __Device(controller);
    g_mfsDevice = &device;
    mfs.SectorSize = 512;
    mfs.SectorsPerBucket = 16;
    mfs.TransferBuffer.ID = 43;
    entry.Position = 1;
    entry.ActualSize = 8192;
    entry.AllocatedSize = 8192;
    entry.DataBucketPosition = 1;
    entry.DataBucketLength = 1;

    // The first 511 bytes use the sector buffer. The next 512 bytes reach AHCI
    // with an odd client offset, although the original client offset was zero.
    CHECK(FsWriteToFile(&mfs, &entry, 42, buffer, 0, 1023, &completed) == OS_EOK);
    CHECK(completed == 1023 && g_mfsReads == 1 && g_mfsWrites == 2);
    CHECK(g_lastMapOffset == 511 && g_lastMapLength == 512 && g_lastDirection == DMAToDevice);
    entry.Position = 1;
    CHECK(FsReadFromFile(&mfs, &entry, 42, buffer, 0, 1023, &completed) == OS_EOK);
    CHECK(completed == 1023 && g_mfsReads == 3 && g_mfsWrites == 2);
    CHECK(g_lastMapOffset == 511 && g_lastMapLength == 512 && g_lastDirection == DMAFromDevice);
    CHECK(AhciControllerDestroy(controller) == OS_EOK);
    __ExpectEmpty();
    puts("AHCI/MFS: partial-sector reads and writes accept odd client offsets");
    return 0;
}
