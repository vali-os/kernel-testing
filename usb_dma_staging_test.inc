/** Exercise private staging across a large client ring and failed handoffs. */
static void
__TestStagingBounded(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferOffset = 31, .BufferLength = 1024 * 1024 }
    };
    struct DMAMapping* mapping = NULL;
    uint8_t* staging;
    uint8_t value;
    size_t offset;
    uint32_t index;
    unsigned int cycle;
    int maps = g_mapCalls;
    int creates = g_stagingCreates;
    int counts = g_elementCounts;
    int fills = g_elementFills;

    memset(g_source, 0xa5, sizeof(g_source));
    for (cycle = 0; cycle < 2; cycle++) {
        for (index = 0; index < transfer.Dma.BufferLength; index += 8) {
            transfer.TData.Periodic.CurrentDataIndex = index;
            offset = transfer.Dma.BufferOffset + index;
            CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
            if (mapping == NULL) {
                mapping = transfer.Dma.Mapping;
            }
            CHECK(transfer.Dma.Mapping == mapping);
            CHECK(transfer.Dma.Mappings == NULL && transfer.Dma.MappingCount == 0);
            CHECK(g_maps == 1 && g_stagingBuffers == 1 && g_stagingBytes == 4096);
            CHECK(g_mapCalls == maps + 1 && g_stagingCreates == creates + 1);
            CHECK(g_elementCounts == counts + 1 && g_elementFills == fills + 1);

            CHECK(DMASyncForDevice(mapping, 0, transfer.Dma.Length) == OS_EOK);
            transfer.Dma.DeviceOwned = true;
            transfer.Dma.HardwareActive = true;
            CHECK(USBTransferDmaPrepare(&transfer) == OS_EBUSY);
            CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);

            // Model a device write and its completion, with a short last byte.
            staging = SHMBuffer(&transfer.Dma.Staging.Buffer);
            value = (uint8_t)(index / 8 + cycle);
            memset(staging, value, 8);
            transfer.Dma.HardwareActive = false;
            CHECK(USBTransferDmaComplete(&transfer, 7) == OS_EOK);
            CHECK(!transfer.Dma.DeviceOwned && !mapping->Owned);
            CHECK(g_source[offset] == value && g_source[offset + 6] == value);
            CHECK(g_source[offset + 7] == 0xa5);
        }
    }
    CHECK(g_source[30] == 0xa5);
    CHECK(g_source[31 + transfer.Dma.BufferLength] == 0xa5);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(g_maps == 0 && g_stagingBuffers == 0 && g_stagingBytes == 0);
    free(transfer.Elements);
}

static void
__TestStagingHandoffs(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 8,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferOffset = 31, .BufferLength = 19 }
    };
    struct DMAMapping* mapping;
    uint8_t* staging;
    unsigned int index;

    memset(g_source, 0xa5, sizeof(g_source));
    transfer.TData.Periodic.CurrentDataIndex = 16;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(transfer.Dma.Length == 3 && transfer.Dma.Staging.ClientOffset == 47);
    mapping = transfer.Dma.Mapping;
    staging = SHMBuffer(&transfer.Dma.Staging.Buffer);
    CHECK(DMASyncForDevice(mapping, 0, 3) == OS_EOK);
    transfer.Dma.DeviceOwned = true;
    transfer.Dma.HardwareActive = true;
    memset(staging, 0x42, 8);
    g_failSync = 1;
    CHECK(USBTransferDmaComplete(&transfer, 2) == OS_EUNKNOWN);
    CHECK(transfer.Dma.DeviceOwned && transfer.Dma.HardwareActive && mapping->Owned);
    CHECK(g_source[47] == 0xa5 && g_source[48] == 0xa5);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);
    g_failSync = 0;
    CHECK(USBTransferDmaComplete(&transfer, 4) == OS_EINVALPARAMS);
    CHECK(transfer.Dma.DeviceOwned && mapping->Owned);
    CHECK(USBTransferDmaComplete(&transfer, 2) == OS_EOK);
    CHECK(g_source[47] == 0x42 && g_source[48] == 0x42 && g_source[49] == 0xa5);

    // A duplicate completion must not publish a second copy from stale storage.
    memset(staging, 0x99, 8);
    CHECK(USBTransferDmaComplete(&transfer, 2) == OS_EOK);
    CHECK(g_source[47] == 0x42);

    // Cancellation reclaims ownership without copying any device bytes.
    transfer.TData.Periodic.CurrentDataIndex = 0;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(DMASyncForDevice(mapping, 0, 8) == OS_EOK);
    transfer.Dma.DeviceOwned = true;
    CHECK(USBTransferDmaComplete(&transfer, 0) == OS_EOK);
    CHECK(g_source[31] == 0xa5 && g_source[38] == 0xa5);

    g_failUnmap = 1;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);
    CHECK(transfer.Dma.Mapping == mapping && g_stagingBytes == 4096);
    CHECK(SHMBuffer(&transfer.Dma.Staging.Buffer) == staging);
    g_failUnmap = 0;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    free(transfer.Elements);

    // OUT packets refresh the same staging slot from the current client slot.
    transfer = (UsbManagerTransfer_t) {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_OUT,
        .MaxPacketSize = 8,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferOffset = 31, .BufferLength = 19 }
    };
    for (index = 0; index < 19; index += 8) {
        transfer.TData.Periodic.CurrentDataIndex = index;
        memset(g_source + 31 + index, 0x50 + index, index == 16 ? 3 : 8);
        CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
        staging = SHMBuffer(&transfer.Dma.Staging.Buffer);
        CHECK(memcmp(staging, g_source + 31 + index, transfer.Dma.Length) == 0);
        CHECK(DMASyncForDevice(transfer.Dma.Mapping, 0, transfer.Dma.Length) == OS_EOK);
        transfer.Dma.DeviceOwned = true;
        CHECK(USBTransferDmaComplete(&transfer, transfer.Dma.Length) == OS_EOK);
        CHECK(g_source[31 + index] == 0x50 + index);
    }
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(g_maps == 0 && g_stagingBuffers == 0 && g_stagingBytes == 0);
    free(transfer.Elements);
}

static void
__TestStagingSetupFailures(void)
{
    UsbManagerTransfer_t transfer;
    unsigned int failure;
    oserr_t status;

    for (failure = 0; failure < 5; failure++) {
        transfer = (UsbManagerTransfer_t) {
            .Type = USBTRANSFER_TYPE_INTERRUPT,
            .Direction = USBTRANSFER_DIRECTION_IN,
            .MaxPacketSize = 8,
            .SHMHandle = { .ID = 17 },
            .Dma = { .Context = 42, .BufferLength = 1024 }
        };
        g_failClientMap = failure == 0;
        g_failCreate = failure == 1;
        g_failMap = failure == 2;
        g_failSegments = failure == 3;
        g_failElementGrowth = failure == 4;
        status = USBTransferDmaPrepare(&transfer);
        CHECK(status != OS_EOK);
        CHECK(g_stagingBuffers == (failure >= 2 ? 1 : 0));
        CHECK(g_maps == (failure >= 3 ? 1 : 0));
        g_failClientMap = 0;
        g_failCreate = 0;
        g_failMap = 0;
        g_failSegments = 0;
        g_failElementGrowth = 0;

        // Partial initialization leaves its owners available for retry/cleanup.
        if (failure >= 3) {
            g_failUnmap = 1;
            CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);
            CHECK(g_maps == 1 && g_stagingBytes == 4096);
            g_failUnmap = 0;
        }
        CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
        CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
        CHECK(g_maps == 0 && g_stagingBuffers == 0 && g_stagingBytes == 0);
        free(transfer.Elements);
    }
}

static void
__TestLargePeriodicCleanup(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_IN,
        .MaxPacketSize = 4097,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferOffset = 31, .BufferLength = 3 * 4097 }
    };
    unsigned int index;

    // Larger periodic transfers keep direct mappings, including retryable
    // cleanup when earlier slots have already been successfully unmapped.
    for (index = 0; index < 3; index++) {
        transfer.TData.Periodic.CurrentDataIndex = index * 4097;
        CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
        CHECK(g_mappedOffset == 31 + index * 4097);
        CHECK(transfer.Dma.Staging.Buffer.ID == UUID_INVALID);
        CHECK(transfer.Elements[0].Data.Address == transfer.Dma.Segments[0].Address);
    }
    CHECK(g_maps == 3 && g_stagingBytes == 0);
    g_failUnmapAfter = 1;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EBUSY);
    CHECK(g_maps == 2 && transfer.Dma.Mappings[0] == NULL);
    g_failUnmapAfter = -1;
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(g_maps == 0 && transfer.Dma.Mappings == NULL && transfer.Dma.Mapping == NULL);
    free(transfer.Elements);
}

static void
__TestPeriodicLayoutCache(void)
{
    UsbManagerTransfer_t transfer = {
        .Type = USBTRANSFER_TYPE_INTERRUPT,
        .Direction = USBTRANSFER_DIRECTION_OUT,
        .MaxPacketSize = 8,
        .SHMHandle = { .ID = 17 },
        .Dma = { .Context = 42, .BufferLength = 19 }
    };
    int counts = g_elementCounts;
    int fills = g_elementFills;

    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(transfer.ElementCount == 2 && transfer.Elements[0].Length == 8);
    CHECK(g_elementCounts == counts + 1 && g_elementFills == fills + 1);
    transfer.TData.Periodic.CurrentDataIndex = 8;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(g_elementCounts == counts + 1 && g_elementFills == fills + 1);

    // A short tail and then a full slot must each rebuild lengths and ZLPs.
    transfer.TData.Periodic.CurrentDataIndex = 16;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(transfer.ElementCount == 1 && transfer.Elements[0].Length == 3);
    CHECK(g_elementCounts == counts + 2 && g_elementFills == fills + 2);
    transfer.TData.Periodic.CurrentDataIndex = 0;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(transfer.ElementCount == 2 && transfer.Elements[0].Length == 8);
    CHECK(g_elementCounts == counts + 3 && g_elementFills == fills + 3);

    // Equal byte lengths do not imply equal packet/ZLP geometry.
    transfer.MaxPacketSize = 4;
    transfer.TData.Periodic.Bandwith = 1;
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(transfer.ElementCount == 1 && transfer.Elements[0].Length == 8);
    CHECK(g_elementCounts == counts + 4 && g_elementFills == fills + 4);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    CHECK(transfer.Dma.Layout.Mapping == NULL);
    CHECK(USBTransferDmaPrepare(&transfer) == OS_EOK);
    CHECK(g_elementCounts == counts + 5 && g_elementFills == fills + 5);
    CHECK(USBTransferDmaDestroy(&transfer) == OS_EOK);
    free(transfer.Elements);
    puts("USB periodic layout cache: reuse, short tails, wrap, geometry and lifetime passed");
}
