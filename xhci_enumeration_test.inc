// Uses the production port scanner and context builder from xhci_dma_test.c.
oserr_t
UsbEventPort(
    _In_ uuid_t  device,
    _In_ uint8_t port)
{
    CHECK(device == 123 && port < 4);
    g_portEvents[port]++;
    return OS_EOK;
}

static void
__TestStartupPorts(void)
{
    Device_t            device = { .Id = 123 };
    XhciController_t    controller = { .Base = { .Device = &device, .PortCount = 3 } };
    XhciPortRegisters_t ports[3] = { 0 };

    controller.Ports = ports;
    g_ports = ports;
    g_portCount = 3;
    ports[0].StatusControl = XHCI_PORT_STATUS_POWER | XHCI_PORT_STATUS_CONNECTED;
    ports[1].StatusControl = ports[0].StatusControl | XHCI_PORT_STATUS_CONNECT_CHANGE;
    ports[2].StatusControl = XHCI_PORT_STATUS_POWER;
    XhciPortScan(&controller, true);
    CHECK(g_portEvents[0] == 1 && g_portEvents[1] == 1 && g_portEvents[2] == 0);
    CHECK(ports[1].StatusControl == ports[0].StatusControl);
    XhciPortScan(&controller, false);
    CHECK(g_portEvents[0] == 1 && g_portEvents[1] == 1);

    ports[0].StatusControl |= XHCI_PORT_STATUS_RESET_CHANGE;
    ports[1].StatusControl = XHCI_PORT_STATUS_POWER | XHCI_PORT_STATUS_CONNECT_CHANGE;
    ports[2].StatusControl |= XHCI_PORT_STATUS_CONNECTED | XHCI_PORT_STATUS_CONNECT_CHANGE;
    XhciPortScan(&controller, false);
    CHECK(g_portEvents[0] == 1 && g_portEvents[1] == 2 && g_portEvents[2] == 1);
    XhciPortScan(&controller, false);
    CHECK(g_portEvents[1] == 2 && g_portEvents[2] == 1);
    g_ports = NULL;
    g_portCount = 0;
}

static void
__CheckHubRoute(
    _InOut_ XhciController_t* controller,
    _In_    uint8_t           hub,
    _In_    uint8_t           port,
    _In_    oserr_t           expectedStatus,
    _In_    uint32_t          expectedRoute,
    _In_    uint8_t           expectedRoot)
{
    UsbManagerTransfer_t transfer = { .Address = { .HubAddress = hub, .PortAddress = port } };
    XhciEndpoint_t       endpoint = { 0 };
    XhciDevice_t*        device = NULL;

    CHECK(XhciDeviceEnsure(controller, &transfer, &endpoint, &device) == expectedStatus);
    if (expectedStatus == OS_EINCOMPLETE) {
        CHECK(device->RouteString == expectedRoute && device->RootPort == expectedRoot);
        CHECK(__DestroyDevice(controller, device) == OS_EOK);
        XhciRingRelease(&controller->CommandRing, 1);
    } else {
        CHECK(device == NULL && endpoint.Device == NULL);
    }
}

static void
__TestHubRoutes(void)
{
    XhciController_t     controller = { .Base = { .DmaContext = (struct DMAContext*)42, .PortCount = 32 } };
    XhciDevice_t         hub = { .UsbAddress = 6, .RootPort = 3 };
    XhciDevice_t         branch = { .Parent = &hub, .PortAddress = 4, .Speed = USBSPEED_FULL };
    XhciEndpoint_t       endpoint = { 0 };
    XhciDevice_t         leaf = { .Parent = &branch, .PortAddress = 2, .DefaultEndpoint = &endpoint };
    UsbManagerTransfer_t transfer = { .Speed = USBSPEED_FULL, .MaxPacketSize = 8 };
    uint32_t            context[33 * 8] = { 0 };
    reg32_t             doorbell = 0;
    reg32_t*            slot;

    ELEMENT_INIT(&hub.Header, 0, &hub);
    list_append(&controller.Devices, &hub.Header);
    controller.Doorbells = &doorbell;
    controller.ContextSize = 32;
    CHECK(XhciRingInitialize((struct DMAContext*)42, &controller.CommandRing, 8) == OS_EOK);
    __CheckHubRoute(&controller, 6, 3, OS_EINCOMPLETE, 0x3, 3);
    hub.Depth = 1;
    hub.RouteString = 3;
    __CheckHubRoute(&controller, 6, 5, OS_EINCOMPLETE, 0x53, 3);
    hub.Depth = 4;
    hub.RouteString = 0x4321;
    __CheckHubRoute(&controller, 6, 5, OS_EINCOMPLETE, 0x54321, 3);
    hub.Depth = 5;
    __CheckHubRoute(&controller, 6, 5, OS_EINVALPARAMS, 0, 0);
    hub.Depth = 0;
    hub.RouteString = 0;
    __CheckHubRoute(&controller, 6, 32, OS_EINCOMPLETE, 0xf, 3);
    __CheckHubRoute(&controller, 6, 0, OS_EINVALPARAMS, 0, 0);
    __CheckHubRoute(&controller, 9, 1, OS_EINVALPARAMS, 0, 0);

    // An unaddressed peer must not become the parent of another root device.
    hub.UsbAddress = 0;
    __CheckHubRoute(&controller, 0, 20, OS_EINCOMPLETE, 0, 21);
    __CheckHubRoute(&controller, 0, 32, OS_EINVALPARAMS, 0, 0);

    // Full-speed descendants use the port on their high-speed TT ancestor.
    hub.Speed = USBSPEED_HIGH;
    hub.SlotId = 7;
    leaf.InputContext = (uint8_t*)context;
    __BuildAddressContext(&controller, &leaf, &transfer);
    slot = context + 8;
    CHECK(slot[2] == (XHCI_SLOT_CONTEXT_TT_HUB(7) | XHCI_SLOT_CONTEXT_TT_PORT(4)));
    leaf.Parent = &hub;
    __BuildAddressContext(&controller, &leaf, &transfer);
    CHECK(slot[2] == (XHCI_SLOT_CONTEXT_TT_HUB(7) | XHCI_SLOT_CONTEXT_TT_PORT(2)));
    CHECK(XhciRingDestroy(&controller.CommandRing) == OS_EOK && g_maps == 0);
}
