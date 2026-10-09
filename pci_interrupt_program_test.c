#include "../librt/libddk/pci_interrupt.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

static unsigned char g_config[256];
static uint32_t       g_msixTable[128];

oserr_t
IoctlDeviceEx(
    uuid_t       deviceId,
    int          direction,
    unsigned int offset,
    size_t*      value,
    size_t       width)
{
    CHECK(deviceId == 0x1234);
    CHECK(offset + width <= sizeof(g_config));
    CHECK(width == 1 || width == 2 || width == 4);

    if (direction == __DEVICEMANAGER_IOCTL_EXT_READ) {
        *value = 0;
        for (size_t i = 0; i < width; i++) {
            *value |= (size_t)g_config[offset + i] << (i * 8);
        }
        return OS_EOK;
    }

    for (size_t i = 0; i < width; i++) {
        g_config[offset + i] = (unsigned char)(*value >> (i * 8));
    }
    return OS_EOK;
}

oserr_t
WriteDeviceIo(
    DeviceIo_t* ioSpace,
    size_t      offset,
    size_t      value,
    size_t      width)
{
    CHECK(ioSpace != NULL);
    CHECK(offset + width <= sizeof(g_msixTable));
    CHECK(width == sizeof(uint32_t));
    g_msixTable[offset / sizeof(uint32_t)] = (uint32_t)value;
    return OS_EOK;
}

static void
__WriteConfig(
    unsigned int offset,
    unsigned int width,
    uint32_t     value)
{
    for (unsigned int i = 0; i < width; i++) {
        g_config[offset + i] = (unsigned char)(value >> (i * 8));
    }
}

static uint32_t
__ReadConfig(
    unsigned int offset,
    unsigned int width)
{
    uint32_t value = 0;

    for (unsigned int i = 0; i < width; i++) {
        value |= (uint32_t)g_config[offset + i] << (i * 8);
    }
    return value;
}

static void
__InitializeDevice(
    BusDevice_t* device)
{
    memset(g_config, 0, sizeof(g_config));
    memset(g_msixTable, 0, sizeof(g_msixTable));
    memset(device, 0, sizeof(*device));

    device->Base.Id = 0x1234;
    device->IsPci = 1;
    device->Segment = 0;
    device->Bus = 2;
    device->Slot = 3;
    device->Function = 1;
}

static void
__InitializeVector(
    DeviceInterrupt_t* vector)
{
    memset(vector, 0, sizeof(*vector));
    vector->DeviceId = 0x1234;
    vector->IsPci = 1;
    vector->Segment = 0;
    vector->Bus = 2;
    vector->Slot = 3;
    vector->Function = 1;
    vector->MsiAddress = 0xFEE01000;
    vector->MsiValue = 0x45;
}

static void
__TestMsixProgramming(void)
{
    BusDevice_t       device;
    DeviceInterrupt_t vectors[2];

    __InitializeDevice(&device);
    __InitializeVector(&vectors[0]);
    __InitializeVector(&vectors[1]);
    vectors[1].MsiAddress += 0x10;
    vectors[1].MsiValue++;

    __WriteConfig(0x06, 2, PCI_STATUS_CAPABILITIES_LIST);
    __WriteConfig(PCI_CAPABILITIES_POINTER, 1, 0x50);
    __WriteConfig(0x50, 2, PCI_CAPABILITY_MSIX);
    __WriteConfig(0x52, 2, 3);
    __WriteConfig(0x54, 4, 0x100);
    device.IoSpaces[0].Type = DeviceIoMemoryBased;
    device.IoSpaces[0].Access.Memory.Length = sizeof(g_msixTable);

    CHECK(DeviceInterruptProgram(&device, vectors, 2, INTERRUPT_STRATEGY_MSIX) == OS_EOK);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSIX_CONTROL_ENABLE) != 0);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSIX_CONTROL_FUNCTION_MASK) == 0);
    CHECK((__ReadConfig(0x04, 2) & PCI_COMMAND_INTERRUPT_DISABLE) != 0);
    CHECK(g_msixTable[0x100 / 4] == (uint32_t)vectors[0].MsiAddress);
    CHECK(g_msixTable[0x104 / 4] == (uint32_t)(vectors[0].MsiAddress >> 32));
    CHECK(g_msixTable[0x108 / 4] == vectors[0].MsiValue);
    CHECK(g_msixTable[0x10C / 4] == 0);
    CHECK(g_msixTable[0x110 / 4] == (uint32_t)vectors[1].MsiAddress);
    CHECK(g_msixTable[0x11C / 4] == 0);
    CHECK(g_msixTable[0x12C / 4] == 1);
    CHECK(g_msixTable[0x13C / 4] == 1);

    CHECK(DeviceInterruptUnprogram(&device) == OS_EOK);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSIX_CONTROL_ENABLE) == 0);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSIX_CONTROL_FUNCTION_MASK) != 0);
    CHECK((__ReadConfig(0x04, 2) & PCI_COMMAND_INTERRUPT_DISABLE) != 0);
}

static void
__TestMsiProgramming(void)
{
    BusDevice_t       device;
    DeviceInterrupt_t vector;

    __InitializeDevice(&device);
    __InitializeVector(&vector);
    __WriteConfig(0x06, 2, PCI_STATUS_CAPABILITIES_LIST);
    __WriteConfig(PCI_CAPABILITIES_POINTER, 1, 0x50);
    __WriteConfig(0x50, 2, PCI_CAPABILITY_MSI);
    __WriteConfig(0x52, 2, PCI_MSI_CONTROL_64BIT | PCI_MSI_CONTROL_MASKABLE);

    CHECK(DeviceInterruptProgram(&device, &vector, 1, INTERRUPT_STRATEGY_MSI) == OS_EOK);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSI_CONTROL_ENABLE) != 0);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSI_CONTROL_MME_MASK) == 0);
    CHECK(__ReadConfig(0x54, 4) == (uint32_t)vector.MsiAddress);
    CHECK(__ReadConfig(0x58, 4) == (uint32_t)(vector.MsiAddress >> 32));
    CHECK(__ReadConfig(0x5C, 2) == vector.MsiValue);
    CHECK(__ReadConfig(0x60, 4) == 0);
    CHECK((__ReadConfig(0x04, 2) & PCI_COMMAND_INTERRUPT_DISABLE) != 0);

    CHECK(DeviceInterruptUnprogram(&device) == OS_EOK);
    CHECK((__ReadConfig(0x52, 2) & PCI_MSI_CONTROL_ENABLE) == 0);
}

static void
__TestIntxAndValidation(void)
{
    BusDevice_t       device;
    DeviceInterrupt_t vector;

    __InitializeDevice(&device);
    __InitializeVector(&vector);
    __WriteConfig(0x04, 2, PCI_COMMAND_INTERRUPT_DISABLE);
    CHECK(DeviceInterruptProgram(&device, &vector, 1, INTERRUPT_STRATEGY_INTx) == OS_EOK);
    CHECK((__ReadConfig(0x04, 2) & PCI_COMMAND_INTERRUPT_DISABLE) == 0);

    vector.Bus++;
    CHECK(DeviceInterruptProgram(&device, &vector, 1, INTERRUPT_STRATEGY_INTx) == OS_EINVALPARAMS);
}

int
main(void)
{
    __TestMsixProgramming();
    __TestMsiProgramming();
    __TestIntxAndValidation();
    puts("PCI interrupt programming: MSI, MSI-X masking, INTx, teardown and identity checks passed");
    return 0;
}