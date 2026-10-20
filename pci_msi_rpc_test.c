/** Check how the ioctl wrapper handles missing or invalid RPC results. */
#include "../librt/libos/device.c"
#include <ddk/types/pci-interrupt.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { printf("line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int sendStatus, waitStatus, decodeStatus;
static uint32_t responseLength;
static oserr_t serviceStatus;

gracht_client_t* GetGrachtClient(void) { return NULL; }
uuid_t GetDeviceService(void) { return 1; }
int OsErrToErrNo(oserr_t status) { (void)status; abort(); }

int sys_device_ioctl(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t device, const unsigned int request, const uint8_t* data, const uint32_t count)
{
    (void)client; (void)context; (void)device; (void)request; (void)data; (void)count;
    return sendStatus;
}
int gracht_client_await(gracht_client_t* client, struct gracht_message_context* context, unsigned int flags)
{
    (void)client; (void)context; (void)flags;
    return waitStatus;
}
int sys_device_ioctl_result(gracht_client_t* client, struct gracht_message_context* context,
    uint8_t* data, uint32_t* length, oserr_t* status)
{
    (void)client; (void)context; (void)data;
    *length = responseLength;
    *status = serviceStatus;
    return decodeStatus;
}
int ctt_driver_ioctl(gracht_client_t* client, struct gracht_message_context* context,
    const uuid_t device, const unsigned int request, const uint8_t* data, const uint32_t count)
{
    (void)client; (void)context; (void)device; (void)request; (void)data; (void)count;
    abort();
}
int ctt_driver_ioctl_result(gracht_client_t* client, struct gracht_message_context* context,
    uint8_t* data, uint32_t* length, oserr_t* status)
{
    (void)client; (void)context; (void)data; (void)length; (void)status;
    abort();
}
int main(void)
{
    uint32_t value = 0;
    struct PciInterruptSupport support;
    serviceStatus = OS_EOK;
    responseLength = sizeof(value);
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY, &value, sizeof(value)) == OS_EOK);
    // The old one-word reply cannot stand in for the expanded capacity record.
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY, &support, sizeof(support)) == OS_EPROTOCOL);
    responseLength = sizeof(support);
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY, &support, sizeof(support)) == OS_EOK);
    sendStatus = OS_EBUSY;
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT, &value, sizeof(value)) == OS_EPROTOCOL);
    sendStatus = 0; waitStatus = -1;
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT, &value, sizeof(value)) == OS_EPROTOCOL);
    waitStatus = 0; decodeStatus = -1;
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT, &value, sizeof(value)) == OS_EPROTOCOL);
    decodeStatus = 0; responseLength = 0;
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT_QUERY, &value, sizeof(value)) == OS_EPROTOCOL);
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT, &value, sizeof(value)) == OS_EOK);
    serviceStatus = OS_EBUSY;
    CHECK(OSDeviceIOCtl(42, OSIOCTLREQUEST_PCI_INTERRUPT, &value, sizeof(value)) == OS_EBUSY);
    puts("PCI MSI RPC: send/wait/decode failures remain protocol errors; service status preserved PASS");
    return 0;
}
