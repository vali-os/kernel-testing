#include "../../librt/libddk/interrupt.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

int
main(void)
{
    BusDevice_t device = {
        .Type = BusTypePci, .Segment = 2, .Bus = 5, .Slot = 3, .Function = 7,
        .InterruptLine = INTERRUPT_NONE
    };
    DeviceInterrupt_t interrupt;

    // Identity stays with the allocation; raw PCI coordinates are bus metadata.
    device.Base.Id = 42;
    device.InterruptPin = 2;
    device.InterruptAcpiConform = INTERRUPT_ACPICONFORM_PRESENT;
    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(interrupt.IsPci);
    CHECK(interrupt.DeviceId == 42 && interrupt.Line.Line == INTERRUPT_NONE);
    CHECK(interrupt.Line.Pin == 2);
    CHECK(interrupt.Line.AcpiConform == INTERRUPT_ACPICONFORM_PRESENT);
    CHECK(interrupt.Count == 0 && interrupt.Strategy == 0);
    CHECK(interrupt.Vectors == NULL && interrupt.State == NULL);
    device.InterruptLine = 64;
    CHECK(interrupt.Line.Line == INTERRUPT_NONE);
    device.Type = BusTypeGeneric;
    DeviceInterruptInitialize(&interrupt, &device);
    CHECK(!interrupt.IsPci && interrupt.DeviceId == 42);
    DeviceInterruptInitialize(&interrupt, NULL);
    CHECK(!interrupt.IsPci);
    CHECK(interrupt.DeviceId == UUID_INVALID);
    CHECK(interrupt.Line.Line == INTERRUPT_NONE && interrupt.Line.Pin == INTERRUPT_NONE);
    CHECK(interrupt.Vectors == NULL && interrupt.State == NULL);
    puts("Interrupt allocations: copied device identity, line routing and empty initialization passed");
    return 0;
}
