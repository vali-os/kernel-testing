// Exercise public interrupt ownership against the existing modeled PCI engines.
#ifdef TEST_MSIX
#define PCI_INTERRUPT_PROGRAM_FIXTURE
#include "pci_msix_ddk_test.c"
#else
#define main __MsiDdkFixtureMain
#include "pci_msi_ddk_test.c"
#undef main
#endif

static void
__TestProgramming(void)
{
    DeviceInterrupt_t        interrupt;
    DeviceInterruptMessage_t message;
    uint32_t                 count;
    uint32_t                 index;

#ifdef TEST_MSIX
    interrupt = __DdkSetup();
    CHECK(DeviceInterruptAllocatePci(&interrupt, 2, 2, INTERRUPT_STRATEGY_MSIX, &count) == OS_EOK);
    CHECK(count == 2);
    for (index = 0; index < count; index++) {
        CHECK(DeviceInterruptGetMessage(&interrupt, index, &message) == OS_EOK);
        CHECK(g_table[index][0] == message.AddressLow);
        CHECK(g_table[index][1] == message.AddressHigh);
        CHECK(g_table[index][2] == message.Data);
        CHECK(g_table[index][3] & MSIX_VECTOR_MASK);
        CHECK(DeviceInterruptSetHandler(&interrupt, index, __Handler) == OS_EOK);
    }
    CHECK(!g_enabled && !g_bound);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(g_enabled && g_bound == 3);
    for (index = 0; index < 4; index++) {
        CHECK(!!(g_table[index][3] & MSIX_VECTOR_MASK) == (index >= count));
    }
    CHECK(__ConfigRead(NULL, 0x42, 2) & MSIX_ENABLE);
    CHECK(!(__ConfigRead(NULL, 0x42, 2) & MSIX_FUNCTION_MASK));
    CHECK(__ConfigRead(NULL, 4, 2) & PCI_COMMAND_INTDISABLE);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(!g_enabled && !g_bound && g_kernelAllocated);
    CHECK(__ConfigRead(NULL, 0x42, 2) & MSIX_FUNCTION_MASK);
    CHECK(!g_state->MayDeliver);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(g_enabled && g_bound == 3);
    __Gone(&interrupt);
#else
    (void)index;
    interrupt = setup();
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_EOK);
    CHECK(count == 1);
    CHECK(DeviceInterruptGetMessage(&interrupt, 0, &message) == OS_EOK);
    CHECK(readConfig(NULL, 0x44, 4) == message.AddressLow);
    CHECK(readConfig(NULL, 0x48, 4) == message.AddressHigh);
    CHECK(readConfig(NULL, 0x4c, 2) == message.Data);
    CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, handler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(enabled && bound && (readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    CHECK(readConfig(NULL, 4, 2) & PCI_COMMAND_INTDISABLE);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(!enabled && !bound && allocated);
    CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(enabled && bound && (readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    gone(&interrupt);
#endif
    CHECK(interrupt.State == NULL && interrupt.Vectors == NULL && interrupt.Count == 0);
}

#ifndef TEST_MSIX
static void
__TestHighMsiTarget(void)
{
    DeviceInterrupt_t interrupt;
    uint32_t count;

    interrupt = setup();
    g_testMessage = (DeviceInterruptMessage_t) {
        .AddressLow = 0xfffffffc,
        .AddressHigh = 0x0000000f,
        .Data = 0x6540
    };
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_EOK);
    CHECK(count == 1);
    CHECK(readConfig(NULL, 0x44, 4) == 0xfffffffc);
    CHECK(readConfig(NULL, 0x48, 4) == 0x0000000f);
    CHECK(readConfig(NULL, 0x4c, 2) == 0x6540);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, handler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(readConfig(NULL, 0x48, 4) == 0x0000000f);
    gone(&interrupt);

    // A 32-bit-only endpoint must reject the target, never truncate it to 4 GiB.
    interrupt = setup();
    writeConfig(NULL, 0x42, 0, 2);
    g_testMessage.AddressLow = 0xfffffffc;
    g_testMessage.AddressHigh = 0xf;
    g_testMessage.Data = 0x6540;
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_ENOTSUPPORTED);
    CHECK(count == 0 && !allocated && !pci.Active);
    CHECK(readConfig(NULL, 0x44, 4) == 0);
    CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    gone(&interrupt);
}

static void
__TestIntxAndValidation(void)
{
    DeviceInterrupt_t interrupt;
    uint32_t          count = 99;

    interrupt = setup();
    allowed = 0;
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_INTx, &count) == OS_EOK);
    CHECK(count == 1 && interrupt.Kind == InterruptSourceLine);
    CHECK(DeviceInterruptGetPciMode(&interrupt) == INTERRUPT_STRATEGY_INTx);
    CHECK(!pci.Active);
    CHECK(DeviceInterruptSetHandler(&interrupt, 0, handler) == OS_EOK);
    CHECK(DeviceInterruptEnable(&interrupt) == OS_EOK);
    CHECK(enabled && bound);
    CHECK(DeviceInterruptDisable(&interrupt) == OS_EOK);
    CHECK(!enabled && !bound && allocated);
    gone(&interrupt);

    // A generic device cannot enter the PCI-specific allocation path.
    DeviceInterruptInitialize(&interrupt, NULL);
    count = 99;
    CHECK(DeviceInterruptAllocatePci(&interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count) == OS_EINVALPARAMS);
    CHECK(count == 0 && !allocated && interrupt.State == NULL);
}
#endif

int
main(void)
{
    __TestProgramming();
#ifndef TEST_MSIX
    __TestHighMsiTarget();
    __TestIntxAndValidation();
    puts("PCI interrupt programming: MSI payload, enable/disable, INTx and device validation passed");
#else
    puts("PCI interrupt programming: MSI-X payloads, unused masks, enable/disable and restoration passed");
#endif
    return 0;
}
