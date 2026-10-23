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
 * QEMU EDU MSI test driver, built only with VALI_EDU_SELFTEST.
 * Binds QEMU's 1234:11e8 device and sends PCI messages through the normal
 * deviced/libddk path.
 * Register definitions: https://www.qemu.org/docs/master/specs/edu.html
 */
#include <ddk/busdevice.h>
#include <ddk/convert.h>
#include <ddk/interrupt.h>
#include <ddk/utils.h>
#include <os/device.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <threads.h>
#include <time.h>
#include <ctt_driver_service_server.h>

extern gracht_server_t *__crt_get_module_server(void);
struct ioset_event;

struct EduTest {
    BusDevice_t *Device;
    DeviceInterrupt_t Interrupt;
    _Atomic(unsigned int) Count;
    int Acquired;
};

// If cleanup fails, keep resources alive for any fast handler still mapped.
// The self-test reports the failure.
static struct EduTest *g_test;

static irqstatus_t
__Interrupt(InterruptFunctionTable_t *functions, void *context)
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
__Raise(struct EduTest *test)
{
    unsigned before = atomic_load(&test->Count);
    WriteDeviceIo(&test->Device->IoSpaces[0], 0x60, 1, 4);
    for (unsigned i = 0; i < 2000; i++) {
        if (atomic_load(&test->Count) == before + 1) {
            return OS_EOK;
        }
        __Sleep();
    }
    ERROR("EDU MSI timeout: pending=%x identification=%x",
          (unsigned)ReadDeviceIo(&test->Device->IoSpaces[0], 0x24, 4),
          (unsigned)ReadDeviceIo(&test->Device->IoSpaces[0], 0, 4));
    return OS_ETIMEOUT;
}

static oserr_t
__Run(struct EduTest *test)
{
    struct OSIOCtlBusControl control = {
        // MSI writes need bus mastering. Keep INTx disabled during setup.
        .Flags = __DEVICEMANAGER_IOCTL_MMIO_ENABLE | __DEVICEMANAGER_IOCTL_BUSMASTER_ENABLE};
    uint32_t count;
    size_t command;
    oserr_t status;
    status =
        OSDeviceIOCtl(test->Device->Base.Id, OSIOCTLREQUEST_BUS_CONTROL, &control, sizeof(control));
    if (status != OS_EOK) {
        return status;
    }
    status = AcquireDeviceIo(&test->Device->IoSpaces[0]);
    if (status != OS_EOK) {
        return status;
    }
    test->Acquired = 1;
    for (unsigned cycle = 0; cycle < 3; cycle++) {
        // Start with INTx enabled so the saved state differs from the later
        // bus-disable request. No interrupt cause is pending at this point.
        control.Flags |= __DEVICEMANAGER_IOCTL_ENABLE;
        status = OSDeviceIOCtl(
            test->Device->Base.Id,
            OSIOCTLREQUEST_BUS_CONTROL,
            &control,
            sizeof(control)
        );
        if (status != OS_EOK) {
            return status;
        }
        DeviceInterruptInitialize(&test->Interrupt, &test->Device->Base);
        status = DeviceInterruptAllocate(&test->Interrupt, 1, 1, INTERRUPT_STRATEGY_MSI, &count);
        if (status != OS_EOK) {
            return status;
        }
        if (count != 1 || test->Interrupt.Strategy != INTERRUPT_STRATEGY_MSI) {
            return OS_EUNKNOWN;
        }
        // Ordinary bus activation must not undo the reservation's INTx mask.
        control.Flags |= __DEVICEMANAGER_IOCTL_ENABLE;
        status = OSDeviceIOCtl(test->Device->Base.Id, OSIOCTLREQUEST_BUS_CONTROL, &control,
                               sizeof(control));
        if (status != OS_EOK) {
            return status;
        }
        command = 0;
        status =
            IoctlDeviceEx(test->Device->Base.Id, __DEVICEMANAGER_IOCTL_EXT_READ, 4, &command, 2);
        if (status != OS_EOK || !(command & 0x400)) {
            return OS_EUNKNOWN;
        }
        // A raw write during reservation must be rejected without touching MSI.
        if (IoctlDeviceEx(test->Device->Base.Id, __DEVICEMANAGER_IOCTL_EXT_WRITE, 4, &command, 2) !=
            OS_EBUSY) {
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
        status = DeviceInterruptAddMemoryResource(&test->Interrupt, 0, (uintptr_t)&test->Count,
                                                  sizeof(test->Count), 0);
        if (status != OS_EOK) {
            return status;
        }
        // Raise a cause with MSI disabled and check that the handler stays idle.
        unsigned before = atomic_load(&test->Count);
        WriteDeviceIo(&test->Device->IoSpaces[0], 0x60, 1, 4);
        for (unsigned wait = 0; wait < 10; wait++) {
            __Sleep();
        }
        if (atomic_load(&test->Count) != before) {
            return OS_EUNKNOWN;
        }
        WriteDeviceIo(&test->Device->IoSpaces[0], 0x64, 1, 4);
        for (unsigned phase = 0; phase < 2; phase++) {
            status = DeviceInterruptEnable(&test->Interrupt);
            if (status != OS_EOK) {
                return status;
            }
            for (unsigned irq = 0; irq < 32; irq++) {
                status = __Raise(test);
                if (status != OS_EOK) {
                    return status;
                }
            }
            status = DeviceInterruptDisable(&test->Interrupt);
            if (status != OS_EOK) {
                return status;
            }
            before = atomic_load(&test->Count);
            WriteDeviceIo(&test->Device->IoSpaces[0], 0x60, 1, 4);
            for (unsigned wait = 0; wait < 10; wait++) {
                __Sleep();
            }
            if (atomic_load(&test->Count) != before) {
                return OS_EUNKNOWN;
            }
            WriteDeviceIo(&test->Device->IoSpaces[0], 0x64, 1, 4);
        }
        // A newer bus-disable request must survive Release, even though INTx
        // was enabled when this reservation captured the command register.
        control.Flags &= ~__DEVICEMANAGER_IOCTL_ENABLE;
        status = OSDeviceIOCtl(
            test->Device->Base.Id,
            OSIOCTLREQUEST_BUS_CONTROL,
            &control,
            sizeof(control)
        );
        if (status != OS_EOK) {
            return status;
        }
        status = DeviceInterruptDestroy(&test->Interrupt);
        if (status != OS_EOK) {
            return status;
        }
        status = IoctlDeviceEx(
            test->Device->Base.Id,
            __DEVICEMANAGER_IOCTL_EXT_READ,
            4,
            &command,
            2
        );
        if (status != OS_EOK || !(command & 0x400)) {
            ERROR("EDU MSI bus-disable state lost during Release");
            return OS_EUNKNOWN;
        }
        status = DeviceInterruptDestroy(&test->Interrupt);
        if (status != OS_EOK) {
            return status;
        }
    }
    return atomic_load(&test->Count) == 192 ? OS_EOK : OS_EUNKNOWN;
}

void
ctt_driver_register_device_invocation(struct gracht_message *message,
                                      const struct sys_device *device)
{
    oserr_t status;
    (void)message;
    if (g_test != NULL) {
        return;
    }
    g_test = calloc(1, sizeof(*g_test));
    if (g_test == NULL) {
        ERROR("EDU MSI self-test: FAILED allocating test state");
        return;
    }
    g_test->Device = (BusDevice_t *)from_sys_device(device);
    if (g_test->Device == NULL) {
        ERROR("EDU MSI self-test: FAILED decoding device");
        free(g_test);
        g_test = NULL;
        return;
    }
    atomic_init(&g_test->Count, 0);
    WARNING("EDU MSI self-test: starting");
    status = __Run(g_test);
    // Only this test raises EDU interrupts. Clear any pending cause before teardown.
    if (g_test->Acquired) {
        WriteDeviceIo(&g_test->Device->IoSpaces[0], 0x64, UINT32_MAX, 4);
    }
    oserr_t cleanup = DeviceInterruptDestroy(&g_test->Interrupt);
    if (cleanup == OS_EOK && g_test->Acquired) {
        cleanup = ReleaseDeviceIo(&g_test->Device->IoSpaces[0]);
        if (cleanup == OS_EOK) {
            g_test->Acquired = 0;
        }
    }
    if (status == OS_EOK && cleanup == OS_EOK) {
        WARNING("EDU MSI self-test: PASS (192 messages, 3 reservations, 6 enable/disable cycles)");
    } else {
        ERROR("EDU MSI self-test: FAILED status=%u cleanup=%u count=%u", status, cleanup,
              atomic_load(&g_test->Count));
    }
    if (cleanup == OS_EOK) {
        DeviceIdentification_t *id = &g_test->Device->Base.Identification;
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
ctt_driver_get_device_protocols_invocation(struct gracht_message *message, const uuid_t device)
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
OnUnregister(Device_t *device)
{
    (void)device;
    return OS_ENOTSUPPORTED;
}
oserr_t
OnEvent(struct ioset_event *event)
{
    (void)event;
    return OS_ENOTSUPPORTED;
}
void
ctt_driver_ioctl_invocation(struct gracht_message *message, const uuid_t device,
                            const unsigned int request, const uint8_t *data, const uint32_t length)
{
    (void)device;
    (void)request;
    (void)data;
    (void)length;
    ctt_driver_ioctl_response(message, NULL, 0, OS_ENOTSUPPORTED);
}
