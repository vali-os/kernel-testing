#include "../../librt/libddk/interrupt.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

int
main(void)
{
    BusDevice_t device = {
        .IsPci = 1, .Segment = 2, .Bus = 5, .Slot = 3, .Function = 7,
        .InterruptLine = INTERRUPT_NONE
    };
    DeviceInterrupt_t interrupt;

    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(interrupt.IsPci && interrupt.Segment == 2 && interrupt.Bus == 5);
    CHECK(interrupt.Slot == 3 && interrupt.Function == 7 && interrupt.Line == INTERRUPT_NONE);
    CHECK(interrupt.Vectors[0] == INTERRUPT_NONE);
    interrupt.MsiAddress = 0xfffffff000ULL;
    CHECK(interrupt.MsiAddress == 0xfffffff000ULL);
    device.Segment = 0;
    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(interrupt.IsPci && interrupt.Segment == 0 && interrupt.MsiAddress == 0);
    memset(&device, 0, sizeof(device));
    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(!interrupt.IsPci);
    puts("Interrupt descriptors: PCI host/requester identity and 64-bit MSI address passed");
    return 0;
}
