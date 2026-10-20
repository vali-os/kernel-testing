/**
 * Deterministic configuration-space tests of deviced's real MSI engine.
 * A write can be dropped or applied with a failed readback, modeling both
 * definite failure and uncertain completion. Retry must restore the snapshot.
 */
#include "../services/deviced/bus/pci/msi.c"
#include <stdio.h>
#include <stdlib.h>

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
    // Message payload must never be changed while MSI is enabled.
    if (offset >= 0x44 && offset <= 0x4c)
        CHECK(!(readConfig(NULL, 0x42, 2) & MSI_ENABLE));
    put(offset, value, width);
}
static struct PciMsiAccess access = { NULL, readConfig, writeConfig };

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
        .AddressLow = 0xfee00000, .AddressHigh = (layout & MSI_64) ? 1 : 0,
        .Data = 0x91
    };
}
static void operation(unsigned op)
{
    request.Operation = op;
    CHECK(PciMsiControl(&access, &state, &request) == OS_EOK);
}
static void restored(void)
{
    CHECK(!state.Active);
    CHECK(memcmp(config, original, sizeof(config)) == 0);
}

int main(void)
{
    const unsigned layouts[] = {0, MSI_64, MSI_MASK, MSI_64 | MSI_MASK};
    unsigned cases = 0;
    for (unsigned l = 0; l < 4; l++) {
        unsigned totals[4];
        setup(layouts[l]);
        operation(PciInterruptReserve);
        CHECK(state.Prepared && (readConfig(NULL, 4, 2) & INT_DISABLE));
        CHECK(!(readConfig(NULL, 0x42, 2) & (MSI_ENABLE | MSI_MME)));
        totals[0] = writes;
        unsigned before = writes;
        operation(PciInterruptReserve);
        CHECK(writes == before);
        request.Allocation++;
        CHECK(PciMsiControl(&access, &state, &request) == OS_EBUSY);
        request.Allocation--;
        writes = 0; operation(PciInterruptEnable); totals[1] = writes;
        CHECK(readConfig(NULL, 0x42, 2) & MSI_ENABLE);
        CHECK(!(readConfig(NULL, 0x42, 2) & MSI_MME));
        writes = 0; operation(PciInterruptDisable); totals[2] = writes;
        operation(PciInterruptEnable);
        writes = 0; operation(PciInterruptRelease); totals[3] = writes;
        restored();
        operation(PciInterruptRelease); operation(PciInterruptDisable);
        CHECK(PciMsiControl(&access, &state,
            &(struct PciInterruptRequest){.Allocation=1023,.Operation=PciInterruptEnable}) == OS_ENOENT);
        for (unsigned op = 0; op < 4; op++) {
            for (unsigned failure = 1; failure <= totals[op]; failure++) {
                for (unsigned applied = 0; applied < 2; applied++) {
                    setup(layouts[l]);
                    if (op != PciInterruptReserve) operation(PciInterruptReserve);
                    if (op == PciInterruptDisable || op == PciInterruptRelease) operation(PciInterruptEnable);
                    writes = 0; failAt = failure; uncertain = applied;
                    request.Operation = op;
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
    // Malformed capabilities cause no writes or reservations.
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
    setup(0); request.AddressHigh = 1;
    CHECK(PciMsiControl(&access, &state, &request) == OS_ENOTSUPPORTED);
    CHECK(!state.Active && !writes);
    setup(0); request.Data = 0x10000;
    CHECK(PciMsiControl(&access, &state, &request) == OS_ENOTSUPPORTED);
    CHECK(!state.Active && !writes);
    // Command ownership is limited to INTx Disable; preserve unrelated changes.
    setup(MSI_64); operation(PciInterruptReserve);
    put(4, readConfig(NULL, 4, 2) | 0x100, 2);
    operation(PciInterruptRelease);
    CHECK(readConfig(NULL, 4, 2) == 0x106);
    printf("PCI MSI: four layouts, %u injected write/readback failures, retry teardown PASS\n", cases);
    return 0;
}
