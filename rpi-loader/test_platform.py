#!/usr/bin/env python3
"""Build actual early parser/platform sources and test physical ownership policy."""
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
LOADER = ROOT / 'boot/rpi/loader'


def fixture(board=4, reservation='static', initrd=True, malformed=False, memory=True,
            compatible=True, reverse_initrd=False, duplicate_chosen=False,
            initrd_start=0x4000000, initrd_end=0x4012345, stray=False,
            dynamic_size=0x100, alignment=None, allocation=None, no_map=False, ram_first=False,
            size_cells=2, size_value=None, alignment_value=None, reusable=False,
            memory_size=0x40000000, placeholder=None):
    """Build a small DTB. size_cells sets #size-cells of the root and of
    reserved-memory, and the width of every address and size pair. size_value
    and alignment_value replace the raw bytes of a dynamic pool's size and
    alignment properties, so tests can pick a width other than the parent's.
    reservation='mixed' gives the static pool a zero-length tuple before its
    real range. placeholder adds an enabled nvram@3000000 reservation, like
    the firmware's nvram nodes: 'zero' gives it reg = <0x3000000 0>, 'empty'
    gives it neither reg nor size, 'empty-reg' gives it reg = <> and a 4 KiB
    size, and 'zero-size' gives it only size = <0 0>. memory_size sets the RAM
    bank's length, so tests can describe an empty bank."""
    strings = bytearray()
    structure = bytearray()

    def word(value):
        structure.extend(struct.pack('>I', value))

    def data(value):
        structure.extend(value)
        structure.extend(bytes(-len(structure) % 4))

    def begin(name):
        word(1)
        data(name.encode() + b'\0')

    def prop(name, value):
        offset = len(strings)
        strings.extend(name.encode() + b'\0')
        word(3)
        word(len(value))
        word(offset)
        data(value)

    def cells(*values):
        return struct.pack('>' + 'I' * len(values), *values)

    def pair(address, size):
        # Two address cells, then one or two size cells.
        if size_cells == 1:
            return cells(address >> 32, address & 0xffffffff, size)
        return cells(address >> 32, address & 0xffffffff, size >> 32, size & 0xffffffff)

    begin('')
    if compatible:
        prop('compatible', f'raspberrypi,{board}-model-b\0brcm,bcm{2711 if board == 4 else 2712}\0'.encode())
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(size_cells))
    def memory_node():
        begin('memory@0')
        prop('reg', pair(0, memory_size))
        prop('device_type', b'memory\0')
        if malformed:
            prop('status', b'okay')
        word(2)

    if memory and ram_first:
        memory_node()
    # Put reservations before RAM to exercise order-independent exclusions.
    begin('reserved-memory')
    prop('#address-cells', cells(2))
    prop('#size-cells', cells(size_cells))
    prop('ranges', b'')
    begin('pool@2000000')
    if reservation == 'dynamic':
        prop('size', size_value if size_value is not None else cells(0, dynamic_size))
        if alignment_value is not None:
            prop('alignment', alignment_value)
        elif alignment is not None:
            prop('alignment', cells(0, alignment))
        if allocation is not None:
            prop('alloc-ranges', pair(*allocation))
    elif reservation == 'mixed':
        prop('reg', pair(0, 0) + pair(0x2000000, 0x100))
    else:
        prop('reg', pair(0x2000000, 0x100))
    if reservation == 'no-map' or no_map:
        prop('no-map', b'')
    if reusable:
        prop('reusable', b'')
    word(2)
    if placeholder is not None:
        begin('nvram@3000000')
        if placeholder == 'zero':
            prop('reg', pair(0x3000000, 0))
        elif placeholder == 'empty-reg':
            prop('reg', b'')
            prop('size', cells(0, 0x1000))
        elif placeholder == 'zero-size':
            prop('size', cells(0, 0))
        prop('status', b'okay\0')
        word(2)
    word(2)
    if memory and not ram_first:
        memory_node()
    begin('chosen')
    if initrd is not None:
        endpoints = [('linux,initrd-start', initrd_start)]
        if initrd:
            endpoints.append(('linux,initrd-end', initrd_end))
        if reverse_initrd:
            endpoints.reverse()
        for name, address in endpoints:
            prop(name, cells(0, address))
    word(2)
    if duplicate_chosen:
        begin('chosen')
        word(2)
    if stray:
        begin('unrelated-device')
        # Same property names under another binding must not affect boot state.
        prop('compatible', b'other,board\0')
        prop('linux,initrd-start', cells(0, 7))
        prop('cpu-release-addr', cells(0, 3))
        word(2)
    begin('cpus')
    begin('cpu@1')
    prop('cpu-release-addr', cells(0, 0x1100088))
    word(2)
    word(2)
    word(2)
    word(9)
    reservations = struct.pack('>QQQQ', 0x300123, 4, 0, 0)
    offset = 40 + len(reservations)
    string_offset = offset + len(structure)
    header = struct.pack('>10I', 0xd00dfeed, string_offset + len(strings), offset,
                         string_offset, 40, 17, 16, 0, len(strings), len(structure))
    return header + reservations + structure + strings


# VBootMemoryType values printed by the harness.
MEMORY_FIRMWARE = 1
MEMORY_AVAILABLE = 4

FDT_BEGIN_NODE = 1
FDT_END_NODE = 2
FDT_PROP = 3
FDT_NOP = 4
FDT_END = 9

FIRMWARE = ROOT / 'boot/rpi/firmware'
BCM2712_TREES = ['bcm2712-rpi-cm5-cm5io.dtb', 'bcm2712-rpi-5-b.dtb']

# RAM banks the firmware writes into /memory@0 on a 4 GiB board.
FIRMWARE_BANKS_4G = [(0, 0x3fc00000), (0x40000000, 0xc0000000)]
# The same first bank, with a second bank that ends below 2 GiB. ASan keeps
# its shadow memory from 0x7fff8000 upwards, so the copied tree must be lower.
SANITIZER_BANKS = [(0, 0x3fc00000), (0x40000000, 0x3ff00000)]


def read_tree(data):
    """Return a list of (node path, property name, value) for a DTB."""
    _, _, off_struct, off_strings, _, _, _, _, _, size_struct = struct.unpack_from('>10I', data)
    path = []
    result = []
    offset = off_struct
    while offset < off_struct + size_struct:
        token, = struct.unpack_from('>I', data, offset)
        offset += 4
        if token == FDT_BEGIN_NODE:
            end = data.index(b'\0', offset)
            path.append(data[offset:end].decode())
            offset = (end + 4) & ~3
        elif token == FDT_END_NODE:
            path.pop()
        elif token == FDT_PROP:
            length, name_offset = struct.unpack_from('>II', data, offset)
            name_end = data.index(b'\0', off_strings + name_offset)
            name = data[off_strings + name_offset:name_end].decode()
            result.append(('/' + '/'.join(path[1:]), name, bytes(data[offset + 8:offset + 8 + length])))
            offset = (offset + 8 + length + 3) & ~3
        elif token == FDT_END:
            break
        elif token != FDT_NOP:
            raise ValueError(f'bad token {token}')
    return result


def tree_property(data, node, name):
    for path, property_name, value in read_tree(data):
        if path == node and property_name == name:
            return value
    return None


def replace_property(data, node, name, value):
    """Rebuild a DTB with one existing property value replaced."""
    _, _, off_struct, off_strings, _, _, _, _, size_strings, size_struct = struct.unpack_from('>10I', data)
    output = bytearray()
    path = []
    offset = off_struct
    replaced = False
    while offset < off_struct + size_struct:
        start = offset
        token, = struct.unpack_from('>I', data, offset)
        offset += 4
        if token == FDT_BEGIN_NODE:
            end = data.index(b'\0', offset)
            path.append(data[offset:end].decode())
            offset = (end + 4) & ~3
        elif token == FDT_END_NODE:
            path.pop()
        elif token == FDT_PROP:
            length, name_offset = struct.unpack_from('>II', data, offset)
            name_end = data.index(b'\0', off_strings + name_offset)
            property_name = data[off_strings + name_offset:name_end].decode()
            offset = (offset + 8 + length + 3) & ~3
            if '/' + '/'.join(path[1:]) == node and property_name == name:
                output.extend(struct.pack('>III', FDT_PROP, len(value), name_offset))
                output.extend(value + bytes(-len(value) % 4))
                replaced = True
                continue
        output.extend(data[start:offset])
        if token == FDT_END:
            break
    if not replaced:
        raise KeyError(f'{node}:{name}')
    strings = data[off_strings:off_strings + size_strings]
    result = bytearray(data[:off_struct]) + output + strings
    struct.pack_into('>I', result, 4, len(result))
    struct.pack_into('>I', result, 12, off_struct + len(output))
    struct.pack_into('>I', result, 36, len(output))
    return bytes(result)


def memory_reg(banks):
    return b''.join(struct.pack('>QQ', base, length) for base, length in banks)


class PlatformTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='vali-platform-')
        cls.directory = pathlib.Path(cls.temp.name)
        cls.binary = cls.directory / 'test'
        command = ['clang', '-std=c11', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
                   '-D_GNU_SOURCE', '-g', '-fsanitize=address,undefined', '-no-pie',
                   '-Wl,--wrap=FdtParseStructure',
                   '-Wl,--defsym,__rpi_loader_start=0x80000',
                   '-Wl,--defsym,__rpi_loader_end=0xa0000']
        for include in ['boot/include', 'kernel/include', 'librt/libfdt/include', 
                        'librt/libos/include', 'librt/libc/include',
                        'boot/rpi/loader']:
            command.extend(['-I', str(ROOT / include)])
        command.extend(str(ROOT / source) for source in [
            'testing/rpi-loader/platform_test.c', 'librt/libfdt/parser.c',
            'boot/rpi/loader/devicetree.c', 'boot/rpi/loader/reservations.c',
            'boot/rpi/loader/platform.c'])
        subprocess.run(command + ['-o', str(cls.binary)], check=True)
        # ASan reserves host memory above 0x7fff8000. Trees with RAM above
        # that address use a build without sanitizers, so the copied tree can
        # be placed where the firmware's RAM banks put it.
        cls.plain_binary = cls.directory / 'test-plain'
        plain = [option for option in command if not option.startswith('-fsanitize')]
        subprocess.run(plain + ['-o', str(cls.plain_binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def run_blob(self, data, board=4, expected=0, arena=0x3ff00000, shape='fixture',
                 sanitized=True):
        """Run the platform stage and return the reservations, the memory map
        and the copied tree (None when the stage kept the original tree)."""
        path = self.directory / 'input.dtb'
        copy = self.directory / 'input.dtb.copy'
        path.write_bytes(data)
        copy.unlink(missing_ok=True)
        binary = self.binary if sanitized else self.plain_binary
        output = subprocess.run([str(binary), str(path), str(board), str(expected), hex(arena), shape],
                                env={**os.environ, 'ASAN_OPTIONS': 'detect_leaks=0'},
                                stdout=subprocess.PIPE, text=True)
        self.assertEqual(output.returncode, 0, output.stdout)
        reservations = []
        memory = []
        for line in output.stdout.splitlines():
            fields = line.split()
            if fields[0] == 'reservation':
                reservations.append(tuple(int(field, 0) for field in fields[1:]))
            elif fields[0] == 'memory':
                memory.append(tuple(int(field, 0) for field in fields[1:]))
        return reservations, memory, copy.read_bytes() if copy.exists() else None

    def check_pool(self, result, node, length, alignment, alloc_range, size_cells):
        """Check that one pool lies page aligned inside its alloc-ranges, and
        that the copied tree carries its reg in the parent's cell format."""
        reservations, _, copy = result
        self.assertEqual(len(reservations), 1)
        base, found_length, found_alignment = reservations[0]
        pages = (length + 0xfff) & ~0xfff
        self.assertEqual(found_length, length)
        self.assertEqual(found_alignment, alignment)
        self.assertEqual(base % 0x1000, 0)
        self.assertEqual(base % alignment, 0)
        self.assertGreaterEqual(base, alloc_range[0])
        self.assertLessEqual(base + pages, alloc_range[0] + alloc_range[1])
        size = struct.pack('>I', length) if size_cells == 1 else struct.pack('>Q', length)
        self.assertEqual(tree_property(copy, node, 'reg'), struct.pack('>Q', base) + size)

    def test_pi4_ownership(self):
        self.run_blob(fixture())

    def test_ram_before_reservations(self):
        self.run_blob(fixture(ram_first=True))

    def test_pi5_ownership(self):
        self.run_blob(fixture(board=5), board=5)

    def test_board_mismatch(self):
        self.run_blob(fixture(), board=5, expected=3)

    def test_dynamic_reservation(self):
        self.run_blob(fixture(reservation='dynamic'))

    def test_dynamic_alignment_and_ranges(self):
        self.run_blob(fixture(reservation='dynamic', dynamic_size=0x18000,
                              alignment=0x20000, allocation=(0x20000000, 0x10000000)))

    def test_dynamic_no_map(self):
        self.run_blob(fixture(reservation='dynamic', no_map=True))

    def test_dynamic_bad_alignment(self):
        self.run_blob(fixture(reservation='dynamic', alignment=3), expected=3)

    def test_dynamic_empty_size(self):
        # A size-only request for zero bytes is skipped. No pool is allocated,
        # so the loader keeps the firmware tree instead of writing a copy.
        reservations, memory, copy = self.run_blob(fixture(placeholder='zero-size'))
        self.assertEqual(reservations, [])
        self.assertIsNone(copy)
        self.assertEqual(self.memory_type_at(memory, 0x3000000), MEMORY_AVAILABLE)

    def test_dynamic_one_cell_size_under_two_cell_parent(self):
        result = self.run_blob(fixture(reservation='dynamic', size_value=struct.pack('>I', 0x18000),
                                       allocation=(0x20000000, 0x10000000)))
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x18000, 0x1000,
                        (0x20000000, 0x10000000), 2)

    def test_dynamic_two_cell_size_under_one_cell_parent(self):
        result = self.run_blob(fixture(reservation='dynamic', size_cells=1,
                                       size_value=struct.pack('>II', 0, 0x18000),
                                       allocation=(0x20000000, 0x10000000)))
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x18000, 0x1000,
                        (0x20000000, 0x10000000), 1)

    def test_dynamic_one_cell_size_under_one_cell_parent(self):
        result = self.run_blob(fixture(reservation='dynamic', size_cells=1,
                                       size_value=struct.pack('>I', 0x18000)))
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x18000, 0x1000,
                        (0, 0x40000000), 1)

    def test_dynamic_large_size_under_one_cell_parent(self):
        # The inserted reg has one size cell and cannot hold a length above
        # 32 bits, so a nonzero high cell is rejected.
        self.run_blob(fixture(reservation='dynamic', size_cells=1,
                              size_value=struct.pack('>II', 1, 0)), expected=3)

    def test_dynamic_one_cell_alignment_under_two_cell_parent(self):
        result = self.run_blob(fixture(reservation='dynamic', dynamic_size=0x18000,
                                       alignment_value=struct.pack('>I', 0x200000),
                                       allocation=(0x20000000, 0x10000000)))
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x18000, 0x200000,
                        (0x20000000, 0x10000000), 2)

    def test_dynamic_two_cell_alignment_under_one_cell_parent(self):
        result = self.run_blob(fixture(reservation='dynamic', size_cells=1,
                                       size_value=struct.pack('>I', 0x18000),
                                       alignment_value=struct.pack('>II', 0, 0x200000)))
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x18000, 0x200000,
                        (0, 0x40000000), 1)

    def test_dynamic_bad_size_lengths(self):
        for length in [0, 3, 5, 12]:
            with self.subTest(length=length):
                value = bytes(length - 1) + b'\x10' if length else b''
                self.run_blob(fixture(reservation='dynamic', size_value=value), expected=3)

    def test_dynamic_bad_alignment_lengths(self):
        for length in [0, 3, 5, 12]:
            with self.subTest(length=length):
                value = bytes(length - 1) + b'\x10' if length else b''
                self.run_blob(fixture(reservation='dynamic', alignment_value=value), expected=3)

    def test_dynamic_bcm2712_cma_shape(self):
        # Same shape as /reserved-memory/linux,cma in the BCM2712 trees: a one
        # cell 64 MiB size under a two cell parent, limited to the first GiB.
        # The pool takes the top 64 MiB of RAM, so the tree copy lands below it.
        result = self.run_blob(fixture(board=5, reservation='dynamic', reusable=True,
                                       size_value=struct.pack('>I', 0x4000000),
                                       allocation=(0, 0x40000000)),
                               board=5, arena=0x3bf00000)
        self.check_pool(result, '/reserved-memory/pool@2000000', 0x4000000, 0x1000,
                        (0, 0x40000000), 2)

    def test_bcm2712_firmware_trees(self):
        # Fill /memory@0 the way the firmware does, then run the stage on the
        # bundled trees. The 4 GiB layout puts the tree copy just below 4 GiB.
        runs = [(FIRMWARE_BANKS_4G, 0xfff00000, False), (SANITIZER_BANKS, 0x7fe00000, True)]
        for name in BCM2712_TREES:
            path = FIRMWARE / name
            if not path.exists():
                self.skipTest(f'{path} is missing')
            for banks, arena, sanitized in runs:
                with self.subTest(tree=name, sanitized=sanitized):
                    data = replace_property(path.read_bytes(), '/memory@0', 'reg', memory_reg(banks))
                    result = self.run_blob(data, board=5, arena=arena, shape='firmware',
                                           sanitized=sanitized)
                    self.check_pool(result, '/reserved-memory/linux,cma', 0x4000000, 0x1000,
                                    (0, 0x40000000), 2)

    def test_bcm2712_firmware_trees_unpatched(self):
        # The bundled trees carry a 640 MiB placeholder bank, which is valid.
        # The pool takes its top 64 MiB, so the tree copy lands below 0x24000000.
        for name in BCM2712_TREES:
            path = FIRMWARE / name
            if not path.exists():
                self.skipTest(f'{path} is missing')
            with self.subTest(tree=name):
                result = self.run_blob(path.read_bytes(), board=5, arena=0x23f00000,
                                       shape='firmware')
                self.check_pool(result, '/reserved-memory/linux,cma', 0x4000000, 0x1000,
                                (0, 0x40000000), 2)

    def test_dynamic_unsatisfiable_range(self):
        self.run_blob(fixture(reservation='dynamic', allocation=(0x70000000, 0x1000)), expected=4)

    def memory_type_at(self, memory, address):
        """Return the map type of the entry that holds address, or None."""
        for entry_type, _, base, length in memory:
            if base <= address < base + length:
                return entry_type
        return None

    def test_zero_length_reservation_skipped(self):
        # An enabled reservation with an empty range adds nothing to the map,
        # so the RAM at its address stays available.
        _, memory, _ = self.run_blob(fixture(placeholder='zero'))
        self.assertEqual(self.memory_type_at(memory, 0x3000000), MEMORY_AVAILABLE)

    def test_reservation_without_reg_or_size_skipped(self):
        reservations, memory, _ = self.run_blob(fixture(placeholder='empty'))
        self.assertEqual(reservations, [])
        self.assertEqual(self.memory_type_at(memory, 0x3000000), MEMORY_AVAILABLE)

    def test_empty_reg_wins_over_size(self):
        # reg = <> lists no ranges. It still wins over size, so the node
        # neither reserves a range nor requests a dynamic pool.
        reservations, memory, copy = self.run_blob(fixture(placeholder='empty-reg'))
        self.assertEqual(reservations, [])
        self.assertIsNone(copy)
        self.assertEqual(self.memory_type_at(memory, 0x3000000), MEMORY_AVAILABLE)

    def test_zero_length_tuple_beside_real_reservation(self):
        # The zero-length tuple is skipped, but the pool's real range after it
        # is still reserved.
        _, memory, _ = self.run_blob(fixture(reservation='mixed'))
        self.assertEqual(self.memory_type_at(memory, 0x2000000), MEMORY_FIRMWARE)

    def test_zero_length_memory_bank_rejected(self):
        self.run_blob(fixture(memory_size=0), expected=3)

    def test_no_map_reservation(self):
        self.run_blob(fixture(reservation='no-map'))

    def test_incomplete_initrd(self):
        self.run_blob(fixture(initrd=False), expected=3)

    def test_unterminated_status(self):
        self.run_blob(fixture(malformed=True), expected=3)

    def test_missing_ram(self):
        self.run_blob(fixture(memory=False), expected=4)

    def test_missing_compatible(self):
        self.run_blob(fixture(compatible=False), expected=3)

    def test_initrd_property_order(self):
        self.run_blob(fixture(reverse_initrd=True))

    def test_duplicate_chosen(self):
        self.run_blob(fixture(duplicate_chosen=True), expected=3)

    def test_initrd_overlaps_wrapper(self):
        self.run_blob(fixture(initrd_start=0x80000), expected=3)

    def test_reversed_initrd(self):
        self.run_blob(fixture(initrd_end=0x3000000), expected=3)

    def test_no_initrd(self):
        self.run_blob(fixture(initrd=None))

    def test_property_scope(self):
        self.run_blob(fixture(stray=True))

    def test_nonzero_property_padding(self):
        data = bytearray(fixture())
        # libfdt can leave previous contents after the seven-byte string value.
        offset = data.index(b'memory\0', data.index(b'memory@0\0')) + 7
        data[offset] = 0x6d
        self.run_blob(data)

    def test_unterminated_property_name(self):
        data = bytearray(fixture())
        data[-1] = 0x61
        self.run_blob(data, expected=3)

    def test_bad_block_bounds(self):
        data = bytearray(fixture())
        struct.pack_into('>I', data, 36, 0xffffffff)
        self.run_blob(data, expected=3)

    def test_reservation_terminator_missing(self):
        data = bytearray(fixture())
        data[56:72] = struct.pack('>QQ', 0x4000, 0x100)
        self.run_blob(data, expected=3)

    def test_pi4_firmware_tree_requires_memory_fixups(self):
        # The bundled Pi 4 tree has an empty /memory@0 bank until the firmware fills it.
        path = FIRMWARE / 'bcm2711-rpi-4-b.dtb'
        if not path.exists():
            self.skipTest(f'{path} is missing')
        self.run_blob(path.read_bytes(), 4, 3)


if __name__ == '__main__':
    unittest.main()
