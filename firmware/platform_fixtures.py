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


def fixture(console, bad_reg=False):
    t = Tree()
    t.begin('')
    t.prop('#address-cells', cells(2))
    t.prop('#size-cells', cells(2))
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
    t.prop('interrupt-parent', cells(1))
    t.begin('device@120')
    t.prop('compatible', b'test,device\0test,generic\0')
    t.prop('reg', cells(0x120, 0x20, 0x200, 0 if bad_reg else 0x40))
    t.prop('interrupts', cells(0, 40, 4, 0, 41, 1))
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
        t.end()
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
