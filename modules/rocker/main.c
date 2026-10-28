/**
 * Copyright 2026, Philip Meulengracht
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * QEMU Rocker MSI-X delivery test, built only with VALI_ROCKER_SELFTEST.
 * BAR0 TEST_IRQ (0x20) sends the table index written to it. No DMA rings or
 * network ports are needed. Register source:
 * https://github.com/qemu/qemu/blob/master/hw/net/rocker/rocker_hw.h
 */
#include <ddk/interrupts/pci.h>
#include <ddk/devices/bus.h>
#include <ddk/convert.h>
#include <ddk/interrupts/interrupt.h>
#include <ddk/types/pci-interrupt.h>
#include <ddk/utils.h>
#include <os/device.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>
#include <ctt_driver_service_server.h>

extern gracht_server_t* __crt_get_module_server(void);
struct ioset_event;

#define ROCKER_TEST_IRQ 0x20
#define TEST_VECTORS 3

/** @brief Resources retained until interrupt and BAR cleanup have succeeded. */
struct __RockerTest {
    BusDevice_t* Device;
    DeviceInterrupt_t Interrupt;
    _Atomic(unsigned int) Counts[TEST_VECTORS];
    unsigned int Expected[TEST_VECTORS];
    unsigned int Acquired;
};

static struct __RockerTest* g_test;

static irqstatus_t
__Interrupt(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                     context)
{
    InterruptResourceTable_t* resources = context;
    _Atomic(unsigned int)* count = (_Atomic(unsigned int)*)INTERRUPT_RESOURCE(resources, 0);

    (void)functions;
    atomic_fetch_add(count, 1);
    return IRQSTATUS_HANDLED;
}

static void
__Sleep(void)
{
    struct timespec duration = { .tv_nsec = 1000000 };

    thrd_sleep(&duration, NULL);
}

static oserr_t
__WaitCounts(
    _In_ struct __RockerTest* test)
{
    unsigned int attempt;
    unsigned int i;
    int complete;

    for (attempt = 0; attempt < 2000; attempt++) {
        complete = 1;
        for (i = 0; i < TEST_VECTORS; i++) {
            if (atomic_load(&test->Counts[i]) > test->Expected[i]) {
                return OS_EUNKNOWN;
            }
            if (atomic_load(&test->Counts[i]) != test->Expected[i]) {
                complete = 0;
            }
        }
        if (complete) {
            return OS_EOK;
        }
        __Sleep();
    }
    return OS_ETIMEOUT;
}

static oserr_t
__MaskedPending(
    _In_ struct __RockerTest* test)
{
    unsigned int i;
    unsigned int wait;

    for (i = 0; i < TEST_VECTORS; i++) {
        WriteDeviceIo(&test->Device->IoSpaces[0], ROCKER_TEST_IRQ, i, 4);
    }
    for (wait = 0; wait < 10; wait++) {
        __Sleep();
    }
    for (i = 0; i < TEST_VECTORS; i++) {
        if (atomic_load(&test->Counts[i]) != test->Expected[i]) {
            return OS_EUNKNOWN;
        }
    }
    // Rocker stores one pending bit per entry while Function Mask is set.
    if ((ReadDeviceIo(&test->Device->IoSpaces[1], 0x1000, 4) & 7) != 7) {
        return OS_EUNKNOWN;
    }
    for (i = 0; i < TEST_VECTORS; i++) {
        test->Expected[i]++;
    }
    return OS_EOK;
}

static oserr_t
__Run(
    _In_ struct __RockerTest* test)
{
    struct OSIOCtlBusControl control = {
        .Flags = __DEVICEMANAGER_IOCTL_MMIO_ENABLE | __DEVICEMANAGER_IOCTL_BUSMASTER_ENABLE
    };
    DeviceInterruptMessage_t messages[TEST_VECTORS];
    unsigned int cycle;
    unsigned int phase;
    unsigned int round;
    unsigned int i;
    unsigned int j;
    uint32_t count;
    uint32_t original[16];
    size_t command;
    oserr_t status;

    status = OSDeviceIOCtl(test->Device->Base.Id, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control));
    if (status != OS_EOK) {
        return status;
    }
    // Own both BARs first: the provider must map its table without stealing the
    // driver's exclusive BAR1 claim. BAR1 is only read by this test.
    for (i = 0; i < 2; i++) {
        status = AcquireDeviceIo(&test->Device->IoSpaces[i]);
        if (status != OS_EOK) {
            return status;
        }
        test->Acquired++;
    }
    for (i = 0; i < 16; i++) {
        original[i] = ReadDeviceIo(&test->Device->IoSpaces[1], i * 4, 4);
    }

    for (cycle = 0; cycle < 3; cycle++) {
        DeviceInterruptInitialize(&test->Interrupt, &test->Device->Base);
        status = DeviceInterruptAllocatePci(&test->Interrupt, TEST_VECTORS, TEST_VECTORS,
            INTERRUPT_STRATEGY_MSIX, &count);
        if (status != OS_EOK) {
            return status;
        }
        if (count != TEST_VECTORS || DeviceInterruptGetPciMode(&test->Interrupt) != INTERRUPT_STRATEGY_MSIX) {
            return OS_EUNKNOWN;
        }
        for (i = 0; i < count; i++) {
            status = DeviceInterruptGetMessage(&test->Interrupt, i, &messages[i]);
            if (status != OS_EOK) {
                return status;
            }
            for (j = 0; j < i; j++) {
                if (messages[i].Data == messages[j].Data) {
                    return OS_EUNKNOWN;
                }
            }
            if (ReadDeviceIo(&test->Device->IoSpaces[1], i * 16, 4) != messages[i].AddressLow ||
                ReadDeviceIo(&test->Device->IoSpaces[1], i * 16 + 4, 4) != messages[i].AddressHigh ||
                ReadDeviceIo(&test->Device->IoSpaces[1], i * 16 + 8, 4) != messages[i].Data) {
                return OS_EUNKNOWN;
            }
            status = DeviceInterruptSetHandler(&test->Interrupt, i, __Interrupt);
            if (status != OS_EOK) {
                return status;
            }
            status = DeviceInterruptAddMemoryResource(&test->Interrupt, i,
                (uintptr_t)&test->Counts[i], sizeof(test->Counts[i]), 0);
            if (status != OS_EOK) {
                return status;
            }
        }
        // The unallocated fourth entry must stay masked, including while enabled.
        if (!(ReadDeviceIo(&test->Device->IoSpaces[1], 60, 4) & 1)) {
            return OS_EUNKNOWN;
        }
        for (phase = 0; phase < 2; phase++) {
            status = __MaskedPending(test);
            if (status != OS_EOK) {
                return status;
            }
            status = DeviceInterruptEnable(&test->Interrupt);
            if (status != OS_EOK) {
                return status;
            }
            status = __WaitCounts(test);
            if (status != OS_EOK) {
                return status;
            }
            for (round = 0; round < 16; round++) {
                for (i = 0; i < count; i++) {
                    test->Expected[i]++;
                    WriteDeviceIo(&test->Device->IoSpaces[0], ROCKER_TEST_IRQ, i, 4);
                    status = __WaitCounts(test);
                    if (status != OS_EOK) {
                        return status;
                    }
                }
            }
            if (!(ReadDeviceIo(&test->Device->IoSpaces[1], 60, 4) & 1)) {
                return OS_EUNKNOWN;
            }
            status = DeviceInterruptDisable(&test->Interrupt);
            if (status != OS_EOK) {
                return status;
            }
        }
        status = DeviceInterruptDestroy(&test->Interrupt);
        if (status != OS_EOK) {
            return status;
        }
        for (i = 0; i < 16; i++) {
            if (ReadDeviceIo(&test->Device->IoSpaces[1], i * 4, 4) != original[i]) {
                return OS_EUNKNOWN;
            }
        }
        status = IoctlDeviceEx(test->Device->Base.Id, __DEVICEMANAGER_IOCTL_EXT_READ, 4, &command, 2);
        if (status != OS_EOK || !(command & 0x400)) {
            return OS_EUNKNOWN;
        }
    }
    return OS_EOK;
}

void
ctt_driver_register_device_invocation(
    _In_ struct gracht_message*  message,
    _In_ const struct sys_device* device)
{
    DeviceIdentification_t* identification;
    oserr_t status;
    oserr_t cleanup;
    unsigned int i;

    (void)message;
    if (g_test != NULL) {
        return;
    }
    g_test = calloc(1, sizeof(*g_test));
    if (g_test == NULL) {
        ERROR("ROCKER MSI-X self-test: FAILED allocating state");
        return;
    }
    g_test->Device = (BusDevice_t*)from_sys_device(device);
    if (g_test->Device == NULL) {
        ERROR("ROCKER MSI-X self-test: FAILED decoding device");
        free(g_test);
        g_test = NULL;
        return;
    }
    for (i = 0; i < TEST_VECTORS; i++) {
        atomic_init(&g_test->Counts[i], 0);
    }
    WARNING("ROCKER MSI-X self-test: starting");
    status = __Run(g_test);
    cleanup = DeviceInterruptDestroy(&g_test->Interrupt);
    while (cleanup == OS_EOK && g_test->Acquired != 0) {
        cleanup = ReleaseDeviceIo(&g_test->Device->IoSpaces[g_test->Acquired - 1]);
        if (cleanup == OS_EOK) {
            g_test->Acquired--;
        }
    }
    if (status == OS_EOK && cleanup == OS_EOK) {
        WARNING("ROCKER MSI-X self-test: PASS (306 messages, 3 vectors, 3 reservations, 6 enable/disable cycles)");
    } else {
        ERROR("ROCKER MSI-X self-test: FAILED status=%u cleanup=%u counts=%u/%u/%u",
            status, cleanup, atomic_load(&g_test->Counts[0]),
            atomic_load(&g_test->Counts[1]), atomic_load(&g_test->Counts[2]));
    }
    if (cleanup == OS_EOK) {
        identification = &g_test->Device->Base.Identification;
        free(identification->Description);
        free(identification->Manufacturer);
        free(identification->Product);
        free(identification->Revision);
        free(identification->Serial);
        free(g_test->Device);
        free(g_test);
        g_test = NULL;
    }
}

oserr_t
OnLoad(void)
{
    return gracht_server_register_protocol(__crt_get_module_server(), &ctt_driver_server_protocol)
        ? OS_EUNKNOWN : OS_EOK;
}

void
OnUnload(void)
{
}

void
ctt_driver_get_device_protocols_invocation(
    _In_ struct gracht_message* message,
    _In_ const uuid_t           device)
{
    (void)message;
    (void)device;
}

void
sys_device_event_protocol_device_invocation(void)
{
}

void
sys_device_event_device_update_invocation(void)
{
}

oserr_t
OnUnregister(
    _In_ Device_t* device)
{
    (void)device;
    return OS_ENOTSUPPORTED;
}

oserr_t
OnEvent(
    _In_ struct ioset_event* event)
{
    (void)event;
    return OS_ENOTSUPPORTED;
}

void
ctt_driver_ioctl_invocation(
    _In_ struct gracht_message* message,
    _In_ const uuid_t           device,
    _In_ const unsigned int     request,
    _In_ const uint8_t*         data,
    _In_ const uint32_t         length)
{
    (void)device;
    (void)request;
    (void)data;
    (void)length;
    ctt_driver_ioctl_response(message, NULL, 0, OS_ENOTSUPPORTED);
}
