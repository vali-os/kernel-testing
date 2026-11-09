/**
 * Copyright 2026, Philip Meulengracht
 *
 * This program is free software : you can redistribute it and / or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation ? , either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * QEMU EDU INTx test driver, built only with VALI_PCI_INTX_SELFTEST.
 * Binds QEMU's 1234:11e8 device and raises its PCI INTx pin through the normal
 * deviced/libddk path.
 * Register definitions: https://www.qemu.org/docs/master/specs/edu.html
 */
#include <ddk/interrupts/pci.h>
#include <ddk/devices/bus.h>
#include <ddk/convert.h>
#include <ddk/interrupts/interrupt.h>
#include <ddk/utils.h>
#include <os/device.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>
#include <ctt_driver_service_server.h>

extern gracht_server_t *__crt_get_module_server(void);
struct ioset_event;

/** @brief Mapped EDU registers and counter shared with its fast interrupt handler. */
struct __EduTest {
    BusDevice_t *Device;
    DeviceInterrupt_t Interrupt;
    _Atomic(unsigned int) Count;
    int Acquired;
};

// If cleanup fails, keep resources alive for any fast handler still mapped.
// The self-test reports the failure.
static struct __EduTest *g_test;

static irqstatus_t
__Interrupt(
    _In_ InterruptFunctionTable_t* functions,
    _In_ void*                     context)
{
    InterruptResourceTable_t *resources = context;
    _Atomic(unsigned int) *count = (_Atomic(unsigned int) *)INTERRUPT_RESOURCE(resources, 0);
    DeviceIo_t *io = INTERRUPT_IOSPACE(resources, 0);
    uint32_t pending = (uint32_t)functions->ReadIoSpace(io, 0x24, 4);
    if (!pending) {
        return IRQSTATUS_NOT_HANDLED;
    }
    functions->WriteIoSpace(io, 0x64, pending, 4);
    atomic_fetch_add(count, 1);
    return IRQSTATUS_HANDLED;
}

static void
__Sleep(void)
{
    struct timespec duration = {.tv_sec = 0, .tv_nsec = 1000000};
    thrd_sleep(&duration, NULL);
}

static oserr_t
__Raise(
    _In_ struct __EduTest* test)
{
    unsigned int before = atomic_load(&test->Count);
    unsigned int i;

    WriteDeviceIo(&test->Device->IoSpaces[0], 0x60, 1, 4);
    for (i = 0; i < 2000; i++) {
        if (atomic_load(&test->Count) == before + 1) {
            return OS_EOK;
        }
        __Sleep();
    }
    ERROR("EDU INTx timeout: pending=%x identification=%x",
          (unsigned)ReadDeviceIo(&test->Device->IoSpaces[0], 0x24, 4),
          (unsigned)ReadDeviceIo(&test->Device->IoSpaces[0], 0, 4));
    return OS_ETIMEOUT;
}

static oserr_t
__Run(
    _In_ struct __EduTest* test)
{
    struct OSIOCtlBusControl control = {
        .Flags = __DEVICEMANAGER_IOCTL_MMIO_ENABLE
    };
    uint32_t count;
    unsigned int iteration;
    oserr_t status;

    // No DMA or MSI is used. The firmware BAR and ACPI GSI must reach this
    // driver through ordinary deviced enumeration and the public DDK API.
    if (test->Device->IoSpaces[0].Type != DeviceIoMemoryBased ||
        test->Device->InterruptLine == INTERRUPT_NONE) {
        return OS_ENOTSUPPORTED;
    }
    status = OSDeviceIOCtl(test->Device->Base.Id, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control));
    if (status != OS_EOK) {
        return status;
    }
    status = AcquireDeviceIo(&test->Device->IoSpaces[0]);
    if (status != OS_EOK) {
        return status;
    }
    test->Acquired = 1;
    DeviceInterruptInitialize(&test->Interrupt, &test->Device->Base);
    status = DeviceInterruptAllocatePci(&test->Interrupt, 1, 1,
        INTERRUPT_STRATEGY_INTx, &count);
    if (status != OS_EOK) {
        return status;
    }
    if (count != 1 || DeviceInterruptGetPciMode(&test->Interrupt) != INTERRUPT_STRATEGY_INTx) {
        return OS_EUNKNOWN;
    }
    status = DeviceInterruptSetHandler(&test->Interrupt, 0, __Interrupt);
    if (status != OS_EOK) {
        return status;
    }
    status = DeviceInterruptAddIoResource(&test->Interrupt, 0, &test->Device->IoSpaces[0]);
    if (status != OS_EOK) {
        return status;
    }
    status = DeviceInterruptAddMemoryResource(&test->Interrupt, 0,
        (uintptr_t)&test->Count, sizeof(test->Count), 0);
    if (status != OS_EOK) {
        return status;
    }
    status = DeviceInterruptEnable(&test->Interrupt);
    if (status != OS_EOK) {
        return status;
    }
    control.Flags |= __DEVICEMANAGER_IOCTL_ENABLE;
    status = OSDeviceIOCtl(test->Device->Base.Id, OSIOCTLREQUEST_BUS_CONTROL,
        &control, sizeof(control));
    if (status != OS_EOK) {
        return status;
    }
    // EDU asserts its real PCI pin after a register write. Each assertion must
    // enter the registered fast handler, be acknowledged there, and complete
    // before the next one. A software-pended GIC interrupt cannot pass this.
    for (iteration = 0; iteration < 32; iteration++) {
        status = __Raise(test);
        if (status != OS_EOK) {
            return status;
        }
    }
    return atomic_load(&test->Count) == 32 ? OS_EOK : OS_EUNKNOWN;
}

void
ctt_driver_register_device_invocation(
    _In_ struct gracht_message*   message,
    _In_ const struct sys_device* device)
{
    DeviceIdentification_t* id;
    oserr_t                  status;
    oserr_t                  cleanup;

    (void)message;
    if (g_test != NULL) {
        return;
    }
    g_test = calloc(1, sizeof(*g_test));
    if (g_test == NULL) {
        ERROR("EDU INTx self-test: FAILED allocating test state");
        return;
    }
    g_test->Device = (BusDevice_t *)from_sys_device(device);
    if (g_test->Device == NULL) {
        ERROR("EDU INTx self-test: FAILED decoding device");
        free(g_test);
        g_test = NULL;
        return;
    }
    atomic_init(&g_test->Count, 0);
    WARNING("EDU INTx self-test: starting");
    status = __Run(g_test);
    // Only this test raises EDU interrupts. Clear any pending cause before teardown.
    if (g_test->Acquired) {
        WriteDeviceIo(&g_test->Device->IoSpaces[0], 0x64, UINT32_MAX, 4);
    }
    cleanup = DeviceInterruptDestroy(&g_test->Interrupt);
    if (cleanup == OS_EOK && g_test->Acquired) {
        cleanup = ReleaseDeviceIo(&g_test->Device->IoSpaces[0]);
        if (cleanup == OS_EOK) {
            g_test->Acquired = 0;
        }
    }
    if (status == OS_EOK && cleanup == OS_EOK) {
        WARNING("EDU INTx self-test: PASS (32 device-generated interrupts)");
    } else {
        ERROR("EDU INTx self-test: FAILED status=%u cleanup=%u count=%u", status, cleanup,
              atomic_load(&g_test->Count));
    }
    if (cleanup == OS_EOK) {
        id = &g_test->Device->Base.Identification;
        free(id->Description);
        free(id->Manufacturer);
        free(id->Product);
        free(id->Revision);
        free(id->Serial);
        free(g_test->Device);
        free(g_test);
        g_test = NULL;
    }
}

oserr_t
OnLoad(void)
{
    return gracht_server_register_protocol(__crt_get_module_server(), &ctt_driver_server_protocol)
               ? OS_EUNKNOWN
               : OS_EOK;
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
