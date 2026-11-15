static XhciRing_t* g_controlPublishRing;
static unsigned int g_controlLinkPublications;

static void
__CheckControlLinkPublication(
    _In_ volatile void* address,
    _In_ uint32_t       value)
{
    XhciRing_t* ring = g_controlPublishRing;
    uint32_t link;

    if (address != &ring->Trbs[ring->TrbCount - 2].Control) {
        return;
    }
    // Observe memory at the instant the last usable TRB becomes available.
    link = ring->Trbs[ring->TrbCount - 1].Control;
    CHECK((link & XHCI_TRB_CONTROL_CHAIN) == (value & XHCI_TRB_CONTROL_CHAIN));
    CHECK((link & XHCI_TRB_CONTROL_CYCLE) == (value & XHCI_TRB_CONTROL_CYCLE));
    g_controlLinkPublications++;
}

// Check hardware control-stage encoding, including SG and 64 KiB boundaries.
static void
__TestControlStagesCase(
    _In_ enum USBTransferDirection direction,
    _In_ unsigned int             payloadLength,
    _In_ bool                     fragmented)
{
    struct DMASegment segments[2] = {
        { .Address = 0x123400000ULL, .Length = 72 },
        { .Address = 0x567800000ULL, .Length = 16 }
    };
    struct TransferElement elements[8] = { 0 };
    UsbManagerTransfer_t transfer = { .Type = USBTRANSFER_TYPE_CONTROL,
        .Direction = direction, .MaxPacketSize = 64, .Elements = elements,
        .Dma = { .Segments = segments, .SegmentCount = 1 } };
    XhciTrb_t trb;
    XhciTrb_t trbs[8];
    XhciRing_t ring = { .Trbs = trbs, .TrbCount = 8, .DeviceBase = 0xabcdef000ULL };
    uint64_t setupBytes;
    uint32_t expected;
    uint32_t previous;
    uint32_t dataBytes = 0;
    uint16_t index;
    unsigned int start;
    unsigned int cycle;
    int count;
    int i;

    if (fragmented) {
        // Setup consumes eight bytes. The first data segment crosses 64 KiB,
        // then the next SG segment supplies the final sixteen bytes.
        CHECK(payloadLength == 56);
        segments[0].Address = 0x12340fff0ULL;
        segments[0].Length = 48;
        transfer.Dma.SegmentCount = 2;
    }
    transfer.Dma.Setup.Direction = direction == USBTRANSFER_DIRECTION_IN ? USBPACKET_DIRECTION_IN : 0;
    transfer.Dma.Setup.Type = USBPACKET_TYPE_GET_DESC;
    transfer.Dma.Setup.ValueHi = USB_DESCRIPTOR_DEVICE;
    transfer.Dma.Setup.Length = payloadLength;
    memcpy(&setupBytes, &transfer.Dma.Setup, sizeof(setupBytes));
    CHECK(__BuildTransferElements(&transfer, payloadLength + 8, NULL, &count) == OS_EOK);
    CHECK(count == (fragmented ? 5 : (payloadLength ? 3 : 2)));
    transfer.ElementCount = count;
    CHECK(__BuildTransferElements(&transfer, payloadLength + 8, elements, &count) == OS_EOK);

    __BuildTrb(&transfer, 0, &trb);
    // Setup type=2, IDT=1, reserved bits clear; TRT is none, OUT or IN.
    expected = 0x840;
    if (payloadLength != 0) {
        expected |= direction == USBTRANSFER_DIRECTION_IN ? 0x30000 : 0x20000;
    }
    CHECK(trb.Control == expected && trb.Status == 8 && trb.Parameter == setupBytes);
    for (i = 1; i < count - 1; i++) {
        __BuildTrb(&transfer, i, &trb);
        // Data Stage first, then Normal; ISP=1. Normal has no DIR field.
        expected = i == 1 ? 0xc04 : 0x404;
        if (i == 1 && direction == USBTRANSFER_DIRECTION_IN) {
            expected |= 0x10000;
        }
        if (i < count - 2) {
            expected |= 0x10;
        }
        CHECK(trb.Control == expected);
        CHECK(trb.Parameter == elements[i].Data.Address && trb.Parameter > UINT32_MAX);
        CHECK(trb.Status == elements[i].Length && trb.Status != 0);
        CHECK((trb.Parameter & 0xffff) + trb.Status <= 0x10000);
        dataBytes += trb.Status;
    }
    CHECK(dataBytes == payloadLength);
    if (fragmented) {
        CHECK(elements[1].Length == 8 && elements[2].Length == 32 && elements[3].Length == 16);
        CHECK(elements[3].Data.Address == segments[1].Address);
    }
    __BuildTrb(&transfer, count - 1, &trb);
    // Status type=4, IOC=1, no Chain; opposite direction, or IN without data.
    expected = payloadLength && direction == USBTRANSFER_DIRECTION_IN ? 0x1020 : 0x11020;
    CHECK(trb.Control == expected && trb.Parameter == 0 && trb.Status == 0);

    // Move the wrap across each stage boundary and each data fragment. Start
    // with a stale Link Chain bit to check that a new TD boundary clears it.
    g_controlPublishRing = &ring;
    g_beforeVolatileWrite = __CheckControlLinkPublication;
    for (cycle = 0; cycle <= 1; cycle++) {
        for (start = 7 - count; start <= 6; start++) {
            XhciRingReset(&ring);
            ring.EnqueueIndex = start;
            ring.CycleState = cycle;
            g_controlLinkPublications = 0;
            trbs[7].Control |= XHCI_TRB_CONTROL_CHAIN;
            previous = 0;
            for (i = 0; i <= count; i++) {
                if (i == count) {
                    // A following transfer crosses the Link after Status too.
                    __BuildTrb(&transfer, 0, &trb);
                } else {
                    __BuildTrb(&transfer, i, &trb);
                }
                CHECK(XhciRingEnqueue(&ring, &trb, &index) == OS_EOK);
                if (index == 0) {
                    CHECK((trbs[7].Control & XHCI_TRB_CONTROL_CHAIN) ==
                        (previous & XHCI_TRB_CONTROL_CHAIN));
                    CHECK(trbs[7].Control & XHCI_TRB_CONTROL_TOGGLE_CYCLE);
                    CHECK(!!(trbs[7].Control & XHCI_TRB_CONTROL_CYCLE) == cycle);
                    CHECK(!!(trbs[index].Control & XHCI_TRB_CONTROL_CYCLE) == !cycle);
                }
                CHECK((trbs[index].Control & ~XHCI_TRB_CONTROL_CYCLE) == trb.Control);
                previous = trb.Control;
            }
            CHECK(g_controlLinkPublications == 1);
        }
    }
    g_beforeVolatileWrite = NULL;
    g_controlPublishRing = NULL;
}

static void
__TestControlStages(void)
{
    __TestControlStagesCase(USBTRANSFER_DIRECTION_IN, 0, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_OUT, 0, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_IN, 8, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_OUT, 8, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_IN, 64, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_OUT, 64, false);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_IN, 56, true);
    __TestControlStagesCase(USBTRANSFER_DIRECTION_OUT, 56, true);
    puts("xHCI control stages: IN/OUT, no data, single/fragmented data TDs and ring wrap passed");
}
