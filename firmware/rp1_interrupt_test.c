#include "../../services/deviced/bus/rp1/interrupt.c"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(value) do { if (!(value)) { printf("line %d: %s\n", __LINE__, #value); exit(1); } } while (0)

struct Fixture {
    DeviceIo_t Io;
    uint32_t Registers[0x1000 / 4];
    int Masked[FDT_RP1_INTERRUPT_COUNT];
    unsigned int Writes;
    unsigned int Reads;
    unsigned int Handled;
    unsigned int Acknowledged;
    unsigned int LastSource;
    int FailWrite;
};

static struct Fixture*
__Fixture(DeviceIo_t* io)
{
    return (struct Fixture*)io;
}

oserr_t
WriteDeviceIo(DeviceIo_t* io, size_t offset, size_t value, size_t length)
{
    struct Fixture* fixture = __Fixture(io);
    size_t reg = offset & 0x3ff;

    CHECK(length == 4 && offset < 0x1000);
    CHECK(reg >= 8 && reg < 8 + 4 * FDT_RP1_INTERRUPT_COUNT);
    CHECK((offset & 0xc00) == 0x800 || (offset & 0xc00) == 0xc00);
    if (fixture->FailWrite) {
        fixture->FailWrite = 0;
        return OS_EUNKNOWN;
    }
    fixture->Writes++;
    if ((offset & 0xc00) == 0x800) {
        if (value & 4) {
            CHECK(fixture->Handled > fixture->Acknowledged);
            fixture->Acknowledged++;
        }
        fixture->Registers[reg / 4] |= value & ~4U;
    } else {
        fixture->Registers[reg / 4] &= ~value;
    }
    return OS_EOK;
}

size_t
ReadDeviceIo(DeviceIo_t* io, size_t offset, size_t length)
{
    struct Fixture* fixture = __Fixture(io);

    CHECK(length == 4 && offset < 0x1000);
    fixture->Reads++;
    return fixture->Registers[offset / 4];
}

static void
__Mask(void* context, unsigned int source)
{
    struct Fixture* fixture = context;

    CHECK(source < FDT_RP1_INTERRUPT_COUNT);
    fixture->Masked[source] = 1;
}

static void
__Unmask(void* context, unsigned int source)
{
    struct Fixture* fixture = context;

    CHECK(source < FDT_RP1_INTERRUPT_COUNT);
    CHECK(fixture->Registers[RP1_IRQ_CFG(source) / 4] & 1);
    CHECK(fixture->Writes == fixture->Reads);
    fixture->Masked[source] = 0;
}

static void
__Handler(void* context, unsigned int source)
{
    struct Fixture* fixture = context;

    CHECK(!fixture->Masked[source]);
    fixture->Handled++;
    fixture->LastSource = source;
}

int
main(void)
{
    struct Fixture first = { 0 };
    struct Fixture second = { 0 };
    struct Rp1InterruptController a = { 0 };
    struct Rp1InterruptController b = { 0 };
    struct Rp1InterruptParent parent = {
        .Context = &first,
        .VectorCount = 61,
        .MaskAndSynchronize = __Mask,
        .Unmask = __Unmask
    };
    unsigned int source;
    unsigned int writes;

    first.Io.Type = second.Io.Type = DeviceIoMemoryBased;
    first.Io.Access.Memory.Length = second.Io.Access.Memory.Length = 0x1000;
    first.Io.Access.Memory.VirtualBase = (uintptr_t)first.Registers;
    second.Io.Access.Memory.VirtualBase = (uintptr_t)second.Registers;
    parent.VectorCount = 60;
    CHECK(Rp1InterruptInitialize(&a, &first.Io, &parent, __Handler, &first) == OS_ENOTSUPPORTED);
    CHECK(first.Writes == 0 && a.Registers == NULL);
    parent.VectorCount = 61;
    first.Io.Access.Memory.Length--;
    CHECK(Rp1InterruptInitialize(&a, &first.Io, &parent, __Handler, &first) == OS_EINVALPARAMS);
    first.Io.Access.Memory.Length++;
    CHECK(Rp1InterruptInitialize(&a, &first.Io, &parent, __Handler, &first) == OS_EOK);
    CHECK(first.Writes == 61 && first.Reads == 61);
    for (source = 0; source < 61; source++) {
        CHECK(first.Masked[source]);
    }
    CHECK(Rp1InterruptEnable(&a, 0) == OS_EINVALPARAMS);
    CHECK(Rp1InterruptConfigure(&a, 61, 4) == OS_EINVALPARAMS);
    CHECK(Rp1InterruptConfigure(&a, 6, 8) == OS_EINVALPARAMS);
    CHECK(Rp1InterruptConfigure(&a, 6, 4) == OS_EOK);
    CHECK(first.Registers[RP1_IRQ_CFG(6) / 4] == 8);
    CHECK(Rp1InterruptEnable(&a, 6) == OS_EOK);
    CHECK(Rp1InterruptConfigure(&a, 6, 1) == OS_EINVALPARAMS);
    CHECK(Rp1InterruptHandle(&a, 6) == OS_EOK);
    CHECK(first.Handled == 1 && first.Acknowledged == 1 && first.LastSource == 6);
    CHECK(Rp1InterruptConfigure(&a, 31, 1) == OS_EOK);
    CHECK(Rp1InterruptEnable(&a, 31) == OS_EOK);
    writes = first.Writes;
    CHECK(Rp1InterruptHandle(&a, 31) == OS_EOK);
    CHECK(first.Handled == 2 && first.Acknowledged == 1 && first.Writes == writes);
    CHECK(Rp1InterruptConfigure(&a, 60, 4) == OS_EOK);
    CHECK(Rp1InterruptEnable(&a, 60) == OS_EOK);
    CHECK(Rp1InterruptHandle(&a, 60) == OS_EOK);
    CHECK(Rp1InterruptDisable(&a, 6) == OS_EOK && first.Masked[6]);
    CHECK(Rp1InterruptHandle(&a, 6) == OS_ENOENT);
    CHECK(Rp1InterruptConfigure(&a, 6, 1) == OS_EOK);
    CHECK(first.Registers[RP1_IRQ_CFG(6) / 4] == 0);
    first.FailWrite = 1;
    CHECK(Rp1InterruptEnable(&a, 6) == OS_EUNKNOWN && first.Masked[6]);
    CHECK(!a.Enabled[6]);

    parent.Context = &second;
    CHECK(Rp1InterruptInitialize(&b, &second.Io, &parent, __Handler, &second) == OS_EOK);
    CHECK(Rp1InterruptConfigure(&b, 6, 4) == OS_EOK);
    CHECK(Rp1InterruptEnable(&b, 6) == OS_EOK);
    first.FailWrite = 1;
    CHECK(Rp1InterruptDestroy(&a) == OS_EUNKNOWN && a.Registers != NULL);
    for (source = 0; source < 61; source++) {
        CHECK(first.Masked[source]);
    }
    CHECK(Rp1InterruptDestroy(&a) == OS_EOK && a.Registers == NULL);
    CHECK(!second.Masked[6]);
    CHECK(Rp1InterruptHandle(&b, 6) == OS_EOK);
    CHECK(second.Handled == 1 && second.Acknowledged == 1);
    CHECK(Rp1InterruptDestroy(&b) == OS_EOK);
    CHECK(Rp1InterruptDestroy(&b) == OS_EOK);

    parent.Context = &first;
    first.FailWrite = 1;
    CHECK(Rp1InterruptInitialize(&a, &first.Io, &parent, __Handler, &first) == OS_EUNKNOWN);
    CHECK(!a.Ready && a.Registers != NULL);
    CHECK(Rp1InterruptConfigure(&a, 6, 4) == OS_EINVALPARAMS);
    CHECK(Rp1InterruptDestroy(&a) == OS_EOK);
    puts("RP1 interrupts: source modes, ordering, parent masking, isolation and failure cleanup passed");
    return 0;
}
