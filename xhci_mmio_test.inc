static void
__CheckMmioPair(
    _In_ unsigned int         index,
    _In_ const volatile void* address,
    _In_ uint64_t             value,
    _In_ bool                 write)
{
    CHECK(g_mmioAccesses[index].Address == (uintptr_t)address);
    CHECK(g_mmioAccesses[index + 1].Address == (uintptr_t)address + 4);
    CHECK(g_mmioAccesses[index].Value == (uint32_t)value);
    CHECK(g_mmioAccesses[index + 1].Value == (uint32_t)(value >> 32));
    CHECK(g_mmioAccesses[index].Write == write);
    CHECK(g_mmioAccesses[index + 1].Write == write);
}

static void
__TestMmio64(void)
{
    XhciController_t controller = { 0 };
    XhciOperationalRegisters_t operational = { 0 };
    XhciRuntimeRegisters_t runtime = { 0 };
    XhciInterrupterRegisters_t interrupter = { 0 };
    XhciEventRingSegmentTableEntry_t erst = { 0 };
    XhciTrb_t commands[XHCI_COMMAND_RING_ENTRIES] = { 0 };
    XhciTrb_t events[XHCI_EVENT_RING_ENTRIES] = { 0 };
    uint64_t values[] = { 0, UINT64_MAX, 0x000000047fc21001ULL, 0x89abcdef01234008ULL };
    uint64_t value;
    unsigned int i;

    // Distinct high/low words expose truncation and repeated low-word accesses.
    g_mmioRegisters[0] = &operational.CommandRingControl;
    g_mmioRegisterCount = 1;
    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
        g_mmioAccessCount = 0;
        XhciWrite64(&operational.CommandRingControl, values[i]);
        CHECK(g_mmioAccessCount == 2);
        __CheckMmioPair(0, &operational.CommandRingControl, values[i], true);
        CHECK(operational.CommandRingControl == values[i]);
        g_mmioAccessCount = 0;
        value = XhciRead64(&operational.CommandRingControl);
        CHECK(value == values[i] && g_mmioAccessCount == 2);
        __CheckMmioPair(0, &operational.CommandRingControl, values[i], false);
    }

    controller.OpRegisters = &operational;
    controller.RuntimeRegisters = &runtime;
    controller.InterrupterRegisters = &interrupter;
    controller.Erst = &erst;
    controller.CommandRing.Trbs = commands;
    controller.CommandRing.DeviceBase = 0x47fc21000ULL;
    controller.DCBaaDeviceAddress = 0x47fc00000ULL;
    controller.EventRingDeviceAddress = 0x47fc22000ULL;
    controller.ErstDeviceAddress = 0x47fc23000ULL;
    controller.SlotCount = 32;
    controller.EventRing = events;
    controller.EventRingCycle = 1;
    g_mmioRegisters[0] = &operational.DeviceContextBaseAddressArray;
    g_mmioRegisters[1] = &operational.CommandRingControl;
    g_mmioRegisters[2] = &interrupter.EventRingDequeuePointer;
    g_mmioRegisters[3] = &interrupter.EventRingSegmentTableBaseAddress;
    g_mmioRegisterCount = 4;

    // Execute production startup, not a second implementation of its writes.
    g_mmioAccessCount = 0;
    CHECK(XhciRun(&controller) == OS_EOK);
    CHECK(g_mmioAccessCount == 8);
    __CheckMmioPair(0, g_mmioRegisters[0], controller.DCBaaDeviceAddress, true);
    __CheckMmioPair(2, g_mmioRegisters[1], controller.CommandRing.DeviceBase | 1, true);
    __CheckMmioPair(4, g_mmioRegisters[2], controller.EventRingDeviceAddress | 8, true);
    __CheckMmioPair(6, g_mmioRegisters[3], controller.ErstDeviceAddress, true);
    CHECK(operational.UsbCommand == (XHCI_OP_USBCMD_RUN | XHCI_OP_USBCMD_INTERRUPTER));

    // Event consumption must also update the 64-bit ERDP through two writes.
    controller.EventRingIndex = 7;
    g_mmioAccessCount = 0;
    __ProcessEventRing(&controller);
    CHECK(g_mmioAccessCount == 2);
    __CheckMmioPair(0, &interrupter.EventRingDequeuePointer,
        (controller.EventRingDeviceAddress + 7 * sizeof(XhciTrb_t)) | 8, true);

    // Diagnostic reads must use the same access width and order as setup.
    controller.Commands[0].Type = XHCI_COMMAND_ENABLE_SLOT;
    g_mmioAccessCount = 0;
    XhciDiagnosePendingCommands(&controller);
    CHECK(g_mmioAccessCount == 8);
    for (i = 0; i < g_mmioAccessCount; i += 2) {
        // C may evaluate different register arguments in either order, but each
        // register read must finish its low/high pair before the next call.
        value = *(const volatile uint64_t*)g_mmioAccesses[i].Address;
        __CheckMmioPair(i, (const volatile void*)g_mmioAccesses[i].Address, value, false);
    }
    g_mmioRegisterCount = 0;
    puts("xHCI MMIO: 32-bit low/high access order, full addresses, startup, ERDP and diagnostics passed");
}
