/**
 * Test deviced's MSI engine with controlled configuration-space failures.
 * Drop a write, or apply it and fail readback so the caller cannot tell whether
 * it succeeded. Retrying cleanup must restore the saved snapshot.
 */
#include "../services/deviced/bus/pci/msi.c"
#include <stdio.h>
#include <stdlib.h>

// The engine logs through the kernel log, which is not available on the host.
void SystemDebug(enum OSSysLogLevel level, const char* format, ...) { (void)level; (void)format; }

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static unsigned char config[256], original[256];
static unsigned writes, failAt, uncertain, badRead;
static uint32_t failedValue;
static struct PciMsiState state;
static struct PciInterruptRequest request;

static uint32_t readConfig(void* context, size_t offset, size_t width)
{
    uint32_t value = 0;
    (void)context;
    CHECK(offset + width <= 256 && (width == 1 || width == 2 || width == 4));
    for (size_t i = 0; i < width; i++) value |= (uint32_t)config[offset + i] << (8 * i);
    if (badRead) { badRead = 0; return failedValue ^ 1; }
    return value;
}
static void put(size_t offset, uint32_t value, size_t width)
{
    for (size_t i = 0; i < width; i++) config[offset + i] = (unsigned char)(value >> (8 * i));
}
static void writeConfig(void* context, size_t offset, uint32_t value, size_t width)
{
    (void)context;
    CHECK(offset != 6); // Never clear W1C PCI status with a command dword write.
    if (++writes == failAt) {
        if (uncertain) put(offset, value, width);
        failedValue = value;
        badRead = 1; // Also detect a dropped idempotent write.
        return;
    }
    // Leave the message payload alone while MSI is enabled.
    if (offset >= 0x44 && offset <= 0x4c)
        CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    put(offset, value, width);
}
static struct PciConfigOperations access = { NULL, readConfig, writeConfig };

static void setup(unsigned layout)
{
    memset(config, 0, sizeof(config));
    memset(&state, 0, sizeof(state));
    writes = failAt = uncertain = badRead = 0;
    put(6, 0x10, 2);
    put(4, 0x06, 2);
    put(0x34, 0x40, 1);
    put(0x40, 5, 1);
    put(0x42, layout | 0x24, 2); // MMC=4 messages, old MME=4, initially disabled.
    put(0x44, 0x11223340, 4);
    if (layout & MSI_64) put(0x48, 0x55667788, 4);
    size_t data = (layout & MSI_64) ? 0x4c : 0x48;
    put(data, 0x1234, 2);
    if (layout & MSI_MASK) put(data + 4, 5, 4);
    memcpy(original, config, sizeof(config));
    request = (struct PciInterruptRequest) {
        .Operation = PciInterruptReserve, .Allocation = 1023,
        .Strategy = INTERRUPT_STRATEGY_MSI, .Count = 1
    };
}
static void
__SelectOperation(
    _In_ unsigned op)
{
    request = (struct PciInterruptRequest) {
        .Operation = op, .Allocation = 1023, .Strategy = INTERRUPT_STRATEGY_MSI
    };
    if (op == PciInterruptReserve) {
        request.Count = 1;
    } else if (op == PciInterruptProgram) {
        request.AddressLow = 0xfee00000;
        request.AddressHigh = (state.Control & MSI_64) ? 1 : 0;
        request.Data = 0x91;
    }
}
static void operation(unsigned op)
{
    __SelectOperation(op);
    CHECK(PciMsiControl(&access, &state, &request) == OS_EOK);
}
static void restored(void)
{
    CHECK(!state.Active);
    CHECK(memcmp(config, original, sizeof(config)) == 0);
}

static void
__TestProgrammingLifetime(void)
{
    unsigned before;
    unsigned op;

    setup(MSI_64 | MSI_MASK);
    __SelectOperation(PciInterruptProgram);
    CHECK(PciMsiControl(&access, &state, &request) == OS_ENOENT);
    operation(PciInterruptReserve);
    __SelectOperation(PciInterruptProgram);
    failAt = writes + 2;
    uncertain = 1;
    CHECK(PciMsiControl(&access, &state, &request) == OS_EUNKNOWN);
    CHECK(state.Supplied && !state.Programmed);
    failAt = 0;
    before = writes;
    request.Data++;
    CHECK(PciMsiControl(&access, &state, &request) == OS_EINVALPARAMS);
    CHECK(writes == before);
    operation(PciInterruptProgram);
    before = writes;
    operation(PciInterruptProgram);
    CHECK(writes == before);

    // An uncertain enable can already be delivering. Programming stays blocked
    // until Disable confirms both the capability and per-vector mask writes.
    __SelectOperation(PciInterruptEnable);
    failAt = writes + 2;
    CHECK(PciMsiControl(&access, &state, &request) == OS_EUNKNOWN);
    CHECK(state.MayDeliver && (readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    __SelectOperation(PciInterruptProgram);
    CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);
    failAt = 0;
    operation(PciInterruptDisable);
    operation(PciInterruptProgram);
    operation(PciInterruptEnable);
    __SelectOperation(PciInterruptProgram);
    CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);

    // Partial restoration is terminal for this reservation, including when the
    // very first Release write fails. Only cleanup operations remain legal.
    __SelectOperation(PciInterruptRelease);
    failAt = writes + 1;
    CHECK(PciMsiControl(&access, &state, &request) == OS_EUNKNOWN);
    CHECK(state.Releasing);
    before = writes;
    for (op = PciInterruptReserve; op <= PciInterruptEnable; op++) {
        __SelectOperation(op);
        CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);
    }
    CHECK(writes == before);
    failAt = 0;
    operation(PciInterruptDisable);
    operation(PciInterruptRelease);
    restored();
    puts("PCI MSI programming: immutable retries, uncertain delivery and terminal release passed");
}

static void
__TestCapabilities(void)
{
    struct PciInterruptCapabilities capabilities;
    struct PciInterruptCapabilities sentinel = { 1, 2, 3, 4 };
    uint32_t count;
    uint32_t exponent;
    uint16_t offset;

    for (exponent = 0; exponent <= 5; exponent++) {
        setup(0);
        put(0x42, exponent << 1U, 2);
        CHECK(PciInterruptProbe(&access, &capabilities) == OS_EOK);
        CHECK(capabilities.MsiOffset == 0x40 && capabilities.MsiCapacity == (1U << exponent));
        CHECK(capabilities.MsixOffset == 0 && capabilities.MsixCapacity == 0);
        CHECK(writes == 0);
    }
    for (count = 1; count <= 2048; count++) {
        setup(0);
        put(0x40, 0x11, 1);
        put(0x42, 0x4000U | (count - 1), 2);
        memcpy(original, config, sizeof(config));
        CHECK(PciInterruptProbe(&access, &capabilities) == OS_EOK);
        CHECK(capabilities.MsiOffset == 0 && capabilities.MsiCapacity == 0);
        CHECK(capabilities.MsixOffset == 0x40 && capabilities.MsixCapacity == count);
        CHECK(PciMsiProbe(&access, &offset) == OS_ENOTSUPPORTED);
        CHECK(!writes && memcmp(original, config, sizeof(config)) == 0);
    }
    setup(0);
    put(0x41, 0x60, 1);
    put(0x60, 0x11, 1);
    put(0x62, 7, 2);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EOK);
    CHECK(capabilities.MsiCapacity == 4 && capabilities.MsixCapacity == 8);
    CHECK(PciMsiProbe(&access, &offset) == OS_EOK && offset == 0x40);

    // Do not expose partial capacities after finding malformed configuration.
    capabilities = sentinel;
    put(0x61, 0x80, 1);
    put(0x80, 0x11, 1);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EINVALPARAMS);
    put(0x61, 0x64, 1);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EINVALPARAMS);
    put(0x61, 0x60, 1);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EINVALPARAMS);
    put(0x61, 0, 1);
    put(0x62, 0x8007, 2);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EBUSY);
    setup(0);
    put(0x34, 0xf8, 1);
    put(0xf8, 0x11, 1);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EINVALPARAMS);
    setup(0);
    put(0x42, 6U << 1U, 2);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_EINVALPARAMS);
    setup(0);
    put(0x34, 0, 1);
    CHECK(PciInterruptProbe(&access, &capabilities) == OS_ENOTSUPPORTED);
    CHECK(memcmp(&capabilities, &sentinel, sizeof(capabilities)) == 0);
    CHECK(!writes);
    puts("PCI capabilities: MSI counts, all MSI-X sizes, malformed chains and read-only probing passed");
}

int main(void)
{
    const unsigned layouts[] = {0, MSI_64, MSI_MASK, MSI_64 | MSI_MASK};
    unsigned cases = 0;
    __TestCapabilities();
    __TestProgrammingLifetime();
    for (unsigned l = 0; l < 4; l++) {
        unsigned totals[5];
        setup(layouts[l]);
        operation(PciInterruptReserve);
        CHECK(state.Prepared && (readConfig(NULL, 4, 2) & PCI_COMMAND_INTDISABLE));
        CHECK(!(readConfig(NULL, 0x42, 2) & (MSI_ENABLE | MSI_MME)));
        totals[0] = writes;
        unsigned before = writes;
        operation(PciInterruptReserve);
        CHECK(writes == before);
        request.Allocation++;
        CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);
        request.Allocation--;
        __SelectOperation(PciInterruptEnable);
        CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);
        writes = 0; operation(PciInterruptProgram); totals[PciInterruptProgram] = writes;
        CHECK(state.Programmed);
        writes = 0; operation(PciInterruptEnable); totals[PciInterruptEnable] = writes;
        CHECK(readConfig(NULL, 0x42, 2) & MSI_ENABLE);
        CHECK(!(readConfig(NULL, 0x42, 2) & MSI_MME));
        writes = 0; operation(PciInterruptDisable); totals[PciInterruptDisable] = writes;
        operation(PciInterruptEnable);
        writes = 0; operation(PciInterruptRelease); totals[PciInterruptRelease] = writes;
        restored();
        operation(PciInterruptRelease); operation(PciInterruptDisable);
        CHECK(PciMsiControl(&access, &state,
            &(struct PciInterruptRequest){.Allocation=1023,.Strategy=INTERRUPT_STRATEGY_MSI,.Operation=PciInterruptEnable}) == OS_ENOENT);
        for (unsigned op = 0; op <= PciInterruptRelease; op++) {
            for (unsigned failure = 1; failure <= totals[op]; failure++) {
                for (unsigned applied = 0; applied < 2; applied++) {
                    setup(layouts[l]);
                    if (op != PciInterruptReserve) operation(PciInterruptReserve);
                    if (op >= PciInterruptEnable) operation(PciInterruptProgram);
                    if (op == PciInterruptDisable || op == PciInterruptRelease) operation(PciInterruptEnable);
                    writes = 0; failAt = failure; uncertain = applied;
                    __SelectOperation(op);
                    CHECK(PciMsiControl(&access, &state, &request) != OS_EOK);
                    CHECK(state.Active && state.Allocation == request.Allocation);
                    failAt = 0;
                    operation(PciInterruptRelease);
                    restored();
                    cases++;
                }
            }
        }
    }
    // Reject malformed capabilities before writing or reserving anything.
    uint16_t cap;
    setup(0); put(0x41, 0x40, 1);
    CHECK(PciMsiProbe(&access, &cap) == OS_EINVALPARAMS);
    setup(0); put(0x34, 0x41, 1);
    CHECK(PciMsiProbe(&access, &cap) == OS_EINVALPARAMS);
    setup(0); put(0x34, 0xfc, 1); put(0xfc, 5, 1);
    CHECK(PciMsiProbe(&access, &cap) == OS_EINVALPARAMS);
    setup(0); put(0x41, 0x60, 1); put(0x60, 5, 1);
    CHECK(PciMsiProbe(&access, &cap) == OS_EINVALPARAMS);
    setup(MSI_64); put(0x41, 0x48, 1); put(0x48, 9, 2);
    CHECK(PciMsiProbe(&access, &cap) == OS_EINVALPARAMS);
    setup(0); put(0x42, 1, 2);
    CHECK(PciMsiProbe(&access, &cap) == OS_EBUSY);
    setup(0); put(0x41, 0x60, 1); put(0x60, 0x11, 1); put(0x62, 0x8000, 2);
    CHECK(PciMsiProbe(&access, &cap) == OS_EBUSY);
    setup(0); operation(PciInterruptReserve); __SelectOperation(PciInterruptProgram);
    unsigned before = writes; request.AddressHigh = 1;
    CHECK(PciMsiControl(&access, &state, &request) == OS_ENOTSUPPORTED);
    CHECK(state.Active && !state.Supplied && writes == before);
    operation(PciInterruptRelease); restored();
    setup(0); operation(PciInterruptReserve); __SelectOperation(PciInterruptProgram);
    before = writes; request.Data = 0x10000;
    CHECK(PciMsiControl(&access, &state, &request) == OS_ENOTSUPPORTED);
    CHECK(state.Active && !state.Supplied && writes == before);
    operation(PciInterruptRelease); restored();
    // Restore only INTx Disable in the command register. Keep unrelated changes.
    setup(MSI_64); operation(PciInterruptReserve);
    put(4, readConfig(NULL, 4, 2) | 0x100, 2);
    operation(PciInterruptRelease);
    CHECK(readConfig(NULL, 4, 2) == 0x106);
    printf("PCI MSI: four layouts, %u injected write/readback failures, retry teardown PASS\n", cases);
    return 0;
}
