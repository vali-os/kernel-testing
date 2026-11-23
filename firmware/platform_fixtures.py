"""Small DT fixtures for shared-tree and platform publication checks."""
import struct
import sys
from pathlib import Path


def cells(*values):
    return struct.pack('>' + 'I' * len(values), *values)


def string(value):
    return value.encode() + b'\0'


class Tree:
    def __init__(self):
        self.structure = bytearray()
        self.strings = bytearray()

    def begin(self, name):
        self.structure += cells(1) + string(name)
        self.structure += b'\0' * (-len(self.structure) % 4)

    def prop(self, name, value):
        offset = len(self.strings)
        self.strings += string(name)
        self.structure += cells(3, len(value), offset) + value
        self.structure += b'\0' * (-len(self.structure) % 4)

    def end(self):
        self.structure += cells(2)

    def finish(self):
        structure = self.structure + cells(9)
        strings_offset = 56 + len(structure)
        return (cells(0xd00dfeed, strings_offset + len(self.strings), 56,
                      strings_offset, 40, 17, 16, 0, len(self.strings), len(structure))
                + bytes(16) + structure + self.strings)


def controllers(t):
    """GPIO and clock controller cases for DeviceControllerEnable.

    The client nodes use made-up compatibles, because deviced must find
    controllers through the standard "mboxes", "gpio-controller" and
    "#clock-cells" properties only. mailbox@2000 is named by the client node
    "firmware", which has an enabled GPIO expander and clocks. mailbox@2100 is
    named by a second client node whose expander is disabled and which has no
    clocks. mailbox@2200 has two client nodes, each with an enabled child that
    has #clock-cells, and the first one has two GPIO controllers with
    different compatibles. Both kinds are ambiguous until the manifest's
    controller compatibles pick one child. clock@2300 is its
    own clock controller. gpio@3000 is its own GPIO controller. gpio@3100 has
    a malformed #gpio-cells and gpio@3200 uses three cells.

    The route-* nodes are targets for manifest clock routes: one usable node,
    two nodes with the same compatible, one with two clock cells and one that
    is disabled.
    """
    for name, handle in [('mailbox@2000', 200), ('mailbox@2100', 201), ('mailbox@2200', 211)]:
        t.begin(name)
        t.prop('compatible', string('brcm,bcm2835-mbox'))
        t.prop('reg', cells(int(name[-4:], 16), 0x40))
        t.prop('#mbox-cells', cells(0))
        t.prop('phandle', cells(handle))
        t.end()
    t.begin('firmware')
    t.prop('compatible', b'test,mailbox-client\0simple-mfd\0')
    t.prop('mboxes', cells(200))
    t.prop('phandle', cells(202))
    t.begin('gpio')
    t.prop('compatible', string('test,gpio-expander'))
    t.prop('gpio-controller', b'')
    t.prop('#gpio-cells', cells(2))
    t.prop('phandle', cells(203))
    t.end()
    t.begin('clocks')
    t.prop('compatible', string('test,clocks'))
    t.prop('#clock-cells', cells(1))
    t.prop('phandle', cells(204))
    t.end()
    t.end()
    t.begin('firmware-other')
    t.prop('compatible', string('test,mailbox-client'))
    t.prop('mboxes', cells(201))
    t.begin('gpio')
    t.prop('compatible', string('test,gpio-expander'))
    t.prop('gpio-controller', b'')
    t.prop('#gpio-cells', cells(2))
    t.prop('status', string('disabled'))
    t.end()
    t.end()
    for name, clocks in [('client-a', 'test,clocks-a'), ('client-b', 'test,clocks-b')]:
        t.begin(name)
        t.prop('mboxes', cells(211))
        t.begin('clocks')
        t.prop('compatible', string(clocks))
        t.prop('#clock-cells', cells(1))
        t.end()
        if name == 'client-a':
            for child, compatible in [('gpio-a', 'test,gpio-expander'), ('gpio-b', 'test,other-gpio')]:
                t.begin(child)
                t.prop('compatible', string(compatible))
                t.prop('gpio-controller', b'')
                t.prop('#gpio-cells', cells(2))
                t.end()
        t.end()
    t.begin('clock@2300')
    t.prop('compatible', string('test,own-clock'))
    t.prop('reg', cells(0x2300, 0x40))
    t.prop('#clock-cells', cells(1))
    t.prop('phandle', cells(212))
    t.end()
    for name, compatible, clock_cells, status in [('route-a', 'test,route-target', 1, None),
                                                  ('route-b', 'test,route-double', 1, None),
                                                  ('route-c', 'test,route-double', 1, None),
                                                  ('route-d', 'test,route-wide', 2, None),
                                                  ('route-e', 'test,route-disabled', 1, 'disabled')]:
        t.begin(name)
        t.prop('compatible', string(compatible))
        t.prop('#clock-cells', cells(clock_cells))
        if status is not None:
            t.prop('status', string(status))
        t.end()
    for name, handle, gpio_cells in [('gpio@3000', 206, cells(2)),
                                      ('gpio@3100', 207, cells(2, 2)),
                                      ('gpio@3200', 208, cells(3))]:
        t.begin(name)
        t.prop('compatible', string('test,gpio'))
        t.prop('reg', cells(int(name[-4:], 16), 0x40))
        t.prop('gpio-controller', b'')
        t.prop('#gpio-cells', gpio_cells)
        t.prop('phandle', cells(handle))
        t.end()
    t.begin('orphan-reset')
    t.prop('#reset-cells', cells(1))
    t.prop('phandle', cells(210))
    t.end()
    # Waits for firmware clock 7 until mailbox@2000 enables its clock controller.
    t.begin('held@4000')
    t.prop('compatible', string('test,held'))
    t.prop('reg', cells(0x4000, 0x40))
    t.prop('clocks', cells(204, 7))
    t.end()
    # Waits for a reset controller that never gets an owner.
    t.begin('never@4100')
    t.prop('compatible', string('test,never'))
    t.prop('reg', cells(0x4100, 0x40))
    t.prop('resets', cells(210, 1))
    t.end()
    # GPIO line references. "lines" has no compatible, so it is not published.
    t.begin('lines')
    t.prop('enable-gpios', cells(203, 4, 1))
    t.prop('gpios', cells(206, 1, 0, 203, 6, 0))
    t.prop('bad-gpios', cells(207, 1, 0))
    t.prop('three-gpios', cells(208, 1, 2, 3))
    t.end()


def supplies(t):
    """Regulators and their consumers for the supply rules.

    All regulator lines sit on gpio@3000 (phandle 206), which a test enables
    as a GPIO controller. Until then every regulator with a line waits.
    reg-switched has the active-low flag in its reference, which
    enable-active-high overrides. reg-low has no enable-active-high, so its
    enable line is active low. reg-select has an active-low selector line.
    reg-vin uses vin-supply and gets no owner, so stuck@5200 waits forever.
    """
    t.begin('reg-switched')
    t.prop('compatible', string('regulator-fixed'))
    t.prop('regulator-name', string('switched'))
    t.prop('regulator-min-microvolt', cells(3300000))
    t.prop('regulator-max-microvolt', cells(3300000))
    t.prop('gpios', cells(206, 10, 1))
    t.prop('enable-active-high', b'')
    t.prop('startup-delay-us', cells(1000))
    t.prop('off-on-delay-us', cells(20000))
    t.prop('phandle', cells(220))
    t.end()
    t.begin('reg-low')
    t.prop('compatible', string('regulator-fixed'))
    t.prop('regulator-min-microvolt', cells(1800000))
    t.prop('regulator-max-microvolt', cells(1800000))
    t.prop('gpio', cells(206, 11, 0))
    t.prop('regulator-boot-on', b'')
    t.prop('phandle', cells(221))
    t.end()
    t.begin('reg-always')
    t.prop('compatible', string('regulator-fixed'))
    t.prop('regulator-min-microvolt', cells(5000000))
    t.prop('regulator-max-microvolt', cells(5000000))
    t.prop('regulator-always-on', b'')
    t.prop('phandle', cells(222))
    t.end()
    t.begin('reg-select')
    t.prop('compatible', string('regulator-gpio'))
    t.prop('regulator-min-microvolt', cells(1800000))
    t.prop('regulator-max-microvolt', cells(3300000))
    t.prop('regulator-settling-time-us', cells(5000))
    t.prop('startup-delay-us', cells(2000))
    t.prop('gpios', cells(206, 12, 1))
    t.prop('states', cells(1800000, 1, 3300000, 0))
    t.prop('enable-gpios', cells(206, 13, 0))
    t.prop('enable-active-high', b'')
    t.prop('phandle', cells(223))
    t.end()
    t.begin('reg-vin')
    t.prop('compatible', string('regulator-fixed'))
    t.prop('vin-supply', cells(222))
    t.prop('phandle', cells(224))
    t.end()
    t.begin('powered@5000')
    t.prop('compatible', string('test,powered'))
    t.prop('reg', cells(0x5000, 0x40))
    t.prop('vdd-supply', cells(220))
    t.prop('vio-supply', cells(223))
    t.prop('vaux-supply', cells(222))
    t.prop('vlow-supply', cells(221))
    t.prop('clocks', cells(204, 9, 10))
    t.end()
    t.begin('shared@5100')
    t.prop('compatible', string('test,shared'))
    t.prop('reg', cells(0x5100, 0x40))
    t.prop('vdd-supply', cells(220))
    t.end()
    t.begin('stuck@5200')
    t.prop('compatible', string('test,stuck'))
    t.prop('reg', cells(0x5200, 0x40))
    t.prop('vdd-supply', cells(224))
    t.end()


def firmware_nodes(t):
    """Nodes for the firmware node request (OSIOCTLREQUEST_FIRMWARE_NODE).

    described@6000 has a property larger than one request chunk, a flag, a
    number, a string list and three children: one with "reg", one disabled
    with "reg", and one without "reg". huge@6100 has a description larger
    than DEVICE_FIRMWARE_NODE_MAX_LENGTH.
    """
    t.begin('described@6000')
    t.prop('compatible', string('test,described'))
    t.prop('reg', cells(0x6000, 0x40))
    t.prop('big', bytes((i * 7) & 0xff for i in range(3000)))
    t.prop('flag', b'')
    t.prop('number', cells(7))
    t.prop('names', b'one\0two\0')
    for name, reg, status in [('fn@1', 1, None), ('fn@2', 2, 'disabled'), ('plain', None, None)]:
        t.begin(name)
        if reg is not None:
            t.prop('reg', cells(reg, 4))
        if status is not None:
            t.prop('status', string(status))
        t.end()
    t.end()
    t.begin('huge@6100')
    t.prop('compatible', string('test,huge'))
    t.prop('reg', cells(0x6100, 0x40))
    t.prop('blob', bytes(33000))
    t.end()


def fixture(console, bad_reg=False):
    t = Tree()
    t.begin('')
    t.prop('#address-cells', cells(2))
    t.prop('#size-cells', cells(2))
    t.begin('memory@0')
    t.prop('device_type', string('memory'))
    t.prop('reg', cells(0, 0, 0, 0x10000000))
    t.end()
    t.begin('chosen')
    t.prop('stdout-path', string(console))
    t.end()
    t.begin('aliases')
    t.prop('serial0', string('/soc/serial@1000'))
    t.end()
    t.begin('gic')
    t.prop('compatible', string('arm,gic-400'))
    t.prop('phandle', cells(1))
    t.prop('#interrupt-cells', cells(3))
    t.prop('interrupt-controller', b'')
    t.end()
    t.begin('soc')
    t.prop('compatible', string('simple-bus'))
    t.prop('#address-cells', cells(1))
    t.prop('#size-cells', cells(1))
    t.prop('ranges', cells(0, 1, 0, 0x10000))
    t.prop('dma-ranges', cells(0x40000000, 0, 0, 0x10000000))
    t.prop('interrupt-parent', cells(1))
    t.begin('device@120')
    t.prop('compatible', b'test,device\0test,generic\0')
    t.prop('reg', cells(0x120, 0x20, 0x200, 0 if bad_reg else 0x40))
    t.prop('interrupts', cells(0, 40, 4, 0, 41, 1))
    t.prop('clocks', cells(10))
    t.end()
    t.begin('serial@1000')
    t.prop('compatible', string('arm,pl011'))
    t.prop('reg', cells(0x1000, 0x100))
    t.end()
    # Exercise entry-array growth, provider indexing and sibling ancestry.
    for i in range(70):
        t.begin(f'provider{i}')
        t.prop('phandle', cells(i + 10))
        t.prop('#clock-cells', cells(0))
        if i == 0:
            t.prop('compatible', string('fixed-clock'))
            t.prop('clock-frequency', cells(24000000))
        t.end()
    t.begin('dependency-owner')
    t.prop('compatible', string('test,dependency-owner'))
    t.prop('phandle', cells(100))
    for prop in ['#clock-cells', '#reset-cells', '#power-domain-cells', '#phy-cells']:
        t.prop(prop, cells(1))
    t.begin('default-pins')
    t.prop('phandle', cells(101))
    t.end()
    t.end()
    t.begin('dependent')
    t.prop('compatible', string('test,dependent'))
    t.prop('power-domains', cells(100, 11))
    t.prop('vdd-supply', cells(100))
    t.prop('clocks', cells(100, 22))
    t.prop('resets', cells(100, 33))
    t.prop('pinctrl-names', string('default'))
    t.prop('pinctrl-0', cells(101))
    t.prop('phys', cells(100, 44))
    t.end()
    controllers(t)
    supplies(t)
    firmware_nodes(t)
    for name, compatible, status in [('disabled', 'simple-bus', 'disabled'),
                                      ('pcie', 'brcm,bcm2712-pcie', 'okay'),
                                      ('i2c', 'test,i2c', 'okay')]:
        t.begin(name)
        t.prop('compatible', string(compatible))
        t.prop('status', string(status))
        t.begin('excluded')
        t.prop('compatible', string('test,excluded'))
        t.end()
        t.end()
    # An unrepresentable bus must also exclude its descendants.
    t.begin('oversized')
    t.prop('compatible', b'simple-bus\0' + b'x' * 520 + b'\0')
    t.begin('excluded')
    t.prop('compatible', string('test,excluded'))
    t.end()
    t.end()
    t.end()
    t.end()
    return t.finish()


if __name__ == '__main__':
    output = Path(sys.argv[1])
    output.mkdir(parents=True, exist_ok=True)
    for name, console in [('alias', 'serial0:115200n8'),
                          ('absolute', '/soc/serial@1000:115200'),
                          ('missing-alias', 'missing:115200')]:
        (output / f'{name}.dtb').write_bytes(fixture(console))
    (output / 'bad-reg.dtb').write_bytes(fixture('serial0', bad_reg=True))
    (output / 'bad-tail.dtb').write_bytes(fixture('serial0')[:-1])
