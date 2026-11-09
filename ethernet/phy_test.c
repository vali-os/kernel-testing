#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../modules/eth/common/phy.h"

struct __PhyModel {
    uint16_t Registers[32];
    uint16_t Shadows[32];
    uint16_t Aux[8];
    unsigned Shadow;
    unsigned AuxSelect;
    unsigned Transactions;
    unsigned FailAt;
    unsigned ResetReads;
    unsigned BmsrReads;
    unsigned Delay;
    int StuckReset;
    int BadDelay;
    int LatchedLow;
};

static oserr_t
__Read(
    _In_ void* context,
    _In_ uint8_t address,
    _In_ uint8_t reg,
    _In_ uint16_t* value)
{
    struct __PhyModel* model = context;

    assert(address == 1);
    if (++model->Transactions == model->FailAt) {
        *value = 0xffff;
        return OS_EDEVFAULT;
    }
    if (reg == 0 && (model->Registers[0] & 0x8000)) {
        model->ResetReads++;
        if (!model->StuckReset && model->ResetReads == 2) {
            model->Registers[0] = 0;
        }
    }
    if (reg == 1) {
        model->BmsrReads++;
        if (model->LatchedLow) {
            model->LatchedLow = 0;
            *value = model->Registers[1] & ~4U;
            return OS_EOK;
        }
    }
    if (reg == 0x18) {
        *value = model->Aux[model->AuxSelect];
        if (model->BadDelay) {
            *value &= ~0x100U;
        }
    } else if (reg == 0x1c) {
        *value = model->Shadows[model->Shadow] | (model->Shadow << 10);
    } else {
        *value = model->Registers[reg];
    }
    return OS_EOK;
}

static oserr_t
__Write(
    _In_ void* context,
    _In_ uint8_t address,
    _In_ uint8_t reg,
    _In_ uint16_t value)
{
    struct __PhyModel* model = context;

    assert(address == 1);
    if (++model->Transactions == model->FailAt) {
        return OS_EDEVFAULT;
    }
    if (reg == 0x1c) {
        model->Shadow = (value >> 10) & 31;
        if (value & 0x8000) {
            model->Shadows[model->Shadow] = value & 0x3ff;
        }
    } else if (reg == 0x18) {
        if (!(value & 0x8000) && (value & 0xfff) == 7) {
            model->AuxSelect = (value >> 12) & 7;
        } else {
            model->AuxSelect = value & 7;
            model->Aux[model->AuxSelect] = value;
        }
    } else {
        model->Registers[reg] = value;
    }
    return OS_EOK;
}

static oserr_t
__Delay(
    _In_ void* context,
    _In_ uint32_t microseconds)
{
    struct __PhyModel* model = context;

    model->Delay += microseconds;
    return OS_EOK;
}

static const struct MdioOperations g_operations = { __Read, __Write, __Delay };

static void
__Initialize(
    _In_ struct __PhyModel* model,
    _In_ MdioBus_t* bus)
{
    memset(model, 0, sizeof(*model));
    model->Registers[2] = 0x600d;
    model->Registers[3] = 0x84a2;
    model->Registers[1] = 0x7908;
    model->Registers[15] = 0x3000;
    model->Registers[9] = 0x1f00;
    model->Shadows[3] = 0x2a5;
    model->Shadows[10] = 0x20;
    model->Aux[7] = 0x37;
    assert(MdioInitialize(bus, &g_operations, model, 1) == OS_EOK);
}

static void
__Modes(void)
{
    struct DeviceEthernetConfiguration config = { .Version = 1, .PhyAddress = 1 };
    struct __PhyModel model;
    MdioBus_t bus;
    PhyDevice_t phy;
    struct PhyLink link;
    unsigned mode;
    unsigned transactions;
    unsigned failure;
    uint16_t value;

    for (mode = DeviceEthernetRgmii; mode <= DeviceEthernetRgmiiTxId; mode++) {
        __Initialize(&model, &bus);
        config.Mode = mode;
        assert(PhyInitialize(&phy, &bus, &config) == OS_EOK);
        assert(phy.Id == 0x600d84a2 && phy.Ready);
        assert(!!(model.Aux[7] & 0x100) ==
            (mode == DeviceEthernetRgmiiId || mode == DeviceEthernetRgmiiRxId));
        assert(!!(model.Shadows[3] & 0x200) ==
            (mode == DeviceEthernetRgmiiId || mode == DeviceEthernetRgmiiTxId));
        assert((model.Shadows[3] & ~0x200) == 0xa5);
        assert(model.Registers[4] == 0x1e1 && model.Registers[9] == 0x200);
        assert(model.Registers[0] == 0x1200 && model.Registers[14] == 0);
        assert(model.Aux[0] & 0x400);
        assert(!(model.Aux[0] & 0x800));
        assert((model.Registers[0x10] & 0x1800) == 0x1000);
        assert(model.Registers[0x1b] == 0xffff);
        assert(!(model.Shadows[10] & 0x20));
        assert(model.ResetReads == 2 && model.Delay == 20000);
    }
    transactions = model.Transactions;
    config.Mode = DeviceEthernetRgmiiRxId;
    for (failure = 1; failure <= transactions; failure++) {
        __Initialize(&model, &bus);
        model.FailAt = failure;
        assert(PhyInitialize(&phy, &bus, &config) != OS_EOK);
        assert(!phy.Ready);
        memset(&link, 0xff, sizeof(link));
        assert(PhyReadLink(&phy, &link) != OS_EOK && !link.Carrier && !link.Speed);
    }
    __Initialize(&model, &bus);
    model.BadDelay = 1;
    assert(PhyInitialize(&phy, &bus, &config) != OS_EOK && !phy.Ready);
    __Initialize(&model, &bus);
    model.StuckReset = 1;
    assert(PhyInitialize(&phy, &bus, &config) == OS_ETIMEOUT);
    assert(model.Delay == 500000 && !phy.Ready);
    __Initialize(&model, &bus);
    model.Registers[3] = 0x84a1;
    assert(PhyInitialize(&phy, &bus, &config) == OS_ENOTSUPPORTED);
    assert(model.Transactions == 2);
    model.Registers[2] = 0xffff;
    model.Registers[3] = 0xffff;
    assert(PhyInitialize(&phy, &bus, &config) == OS_ENOENT);
    value = 0x1234;
    model.FailAt = model.Transactions + 1;
    assert(MdioRead(&bus, 2, &value) == OS_EDEVFAULT && value == 0x1234);
    assert(MdioRead(&bus, 32, &value) == OS_EINVALPARAMS);
    assert(MdioInitialize(&bus, &g_operations, &model, 32) == OS_EINVALPARAMS);
}

static void
__Negotiation(void)
{
    struct DeviceEthernetConfiguration config = {
        .Version = 1, .PhyAddress = 1, .Mode = DeviceEthernetRgmiiRxId
    };
    struct __PhyModel model;
    MdioBus_t bus;
    PhyDevice_t phy;
    struct PhyLink link;
    unsigned i;
    const uint16_t partner[] = {0x100, 0x80, 0x40, 0x20};
    const unsigned speed[] = {100, 100, 10, 10};

    __Initialize(&model, &bus);
    assert(PhyInitialize(&phy, &bus, &config) == OS_EOK);
    model.Registers[1] |= 0x24;
    model.Registers[5] = 0x41e1;
    model.Registers[10] = 0x800;
    model.LatchedLow = 1;
    assert(PhyReadLink(&phy, &link) == OS_EOK);
    assert(link.Carrier && link.Speed == 1000 && link.FullDuplex);
    model.Registers[10] = 0;
    for (i = 0; i < 4; i++) {
        model.Registers[5] = partner[i] | 0x4001;
        assert(PhyReadLink(&phy, &link) == OS_EOK);
        assert(link.Carrier && link.Speed == speed[i] && link.FullDuplex == !(i & 1));
    }
    model.Registers[1] &= ~0x20;
    assert(PhyReadLink(&phy, &link) == OS_EOK && !link.Carrier);
    model.Registers[1] |= 0x20;
    model.Registers[10] = 0x8800;
    assert(PhyReadLink(&phy, &link) == OS_EOK && !link.Carrier);
    model.Registers[10] = 0;
    model.Registers[1] &= ~4U;
    assert(PhyReadLink(&phy, &link) == OS_EOK && !link.Carrier);
    model.FailAt = model.Transactions + 1;
    assert(PhyReadLink(&phy, &link) == OS_EDEVFAULT && !link.Carrier);
    assert(PhyStop(&phy) == OS_EOK && !phy.Ready && (model.Registers[0] & 0x400));
    __Initialize(&model, &bus);
    config.MaxSpeed = 100;
    assert(PhyInitialize(&phy, &bus, &config) == OS_EOK && model.Registers[9] == 0);
    __Initialize(&model, &bus);
    config.MaxSpeed = 10;
    assert(PhyInitialize(&phy, &bus, &config) == OS_EOK && model.Registers[4] == 0x61);
    __Initialize(&model, &bus);
    config.MaxSpeed = 0;
    config.Mode = DeviceEthernetMii;
    model.Registers[2] = 0x1234;
    model.Registers[3] = 0x5678;
    assert(PhyInitialize(&phy, &bus, &config) == OS_EOK && model.Registers[9] == 0);
    config.MaxSpeed = 2500;
    assert(PhyInitialize(&phy, &bus, &config) == OS_ENOTSUPPORTED);
}

int
main(void)
{
    __Modes();
    __Negotiation();
    puts("Ethernet PHY tests passed");
    return 0;
}
