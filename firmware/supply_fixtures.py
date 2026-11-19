"""DT fixture for the regulator and SD host decoders.

Each node name says what the decoder must do with it. supply_decode_test.c
checks every node by name. Nodes sit directly below the root, which uses one
address cell and one size cell.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from platform_fixtures import Tree, cells, string  # noqa: E402

GPIO = 1        # two-cell GPIO controller
GPIO3 = 2       # three-cell GPIO controller
SUPPLY = 3      # regulator that SD hosts name as a supply


def regulator(t, name, compatible, props):
    t.begin(name)
    t.prop('compatible', string(compatible))
    for key, value in props:
        t.prop(key, value)
    t.end()


def fixed(t, name, *props):
    regulator(t, name, 'regulator-fixed', props)


def gpio(t, name, *props):
    regulator(t, name, 'regulator-gpio', props)


def host(t, name, *props, child=None, ranges=1):
    address = int(name.split('@')[1], 16) * 0x1000
    t.begin(name)
    t.prop('compatible', string('test,sdhci'))
    t.prop('reg', cells(*[v for i in range(ranges) for v in (address + i * 0x400, 0x100)]))
    for key, value in props:
        t.prop(key, value)
    if child is not None:
        t.prop('#address-cells', cells(1))
        t.prop('#size-cells', cells(0))
        t.begin(child)
        t.prop('reg', cells(1))
        t.prop('compatible', string('test,wifi'))
        t.end()
    t.end()


def build():
    t = Tree()
    t.begin('')
    t.prop('#address-cells', cells(1))
    t.prop('#size-cells', cells(1))
    t.begin('gpio')
    t.prop('gpio-controller', b'')
    t.prop('#gpio-cells', cells(2))
    t.prop('phandle', cells(GPIO))
    t.end()
    t.begin('gpio3')
    t.prop('gpio-controller', b'')
    t.prop('#gpio-cells', cells(3))
    t.prop('phandle', cells(GPIO3))
    t.end()

    volts = [('regulator-min-microvolt', cells(3300000)), ('regulator-max-microvolt', cells(3300000))]
    fixed(t, 'r-ok-fixed', ('regulator-name', string('ok')), *volts, ('gpio', cells(GPIO, 3, 0)),
          ('enable-active-high', b''), ('startup-delay-us', cells(100)), ('off-on-delay-us', cells(200)),
          ('regulator-boot-on', b''), ('regulator-always-on', b''), ('status', string('okay')),
          ('phandle', cells(SUPPLY)))
    fixed(t, 'r-fixed-novolt')
    fixed(t, 'r-fixed-onevolt', ('regulator-min-microvolt', cells(3300000)))
    fixed(t, 'r-fixed-diff', ('regulator-min-microvolt', cells(1800000)),
          ('regulator-max-microvolt', cells(3300000)))
    fixed(t, 'r-fixed-both-gpio', ('gpio', cells(GPIO, 1, 0)), ('gpios', cells(GPIO, 2, 0)))
    fixed(t, 'r-fixed-two-lines', ('gpios', cells(GPIO, 1, 0, GPIO, 2, 0)))
    fixed(t, 'r-fixed-vin', ('vin-supply', cells(SUPPLY)))
    fixed(t, 'r-fixed-pinctrl', ('pinctrl-0', cells(GPIO)))
    fixed(t, 'r-fixed-badbool', ('regulator-always-on', cells(1)))
    fixed(t, 'r-fixed-baddelay', ('startup-delay-us', cells(1, 2)))
    fixed(t, 'r-fixed-3cells', ('gpios', cells(GPIO3, 1, 0, 0)))
    fixed(t, 'r-fixed-flagged', ('gpios', cells(GPIO, 4, 1)), ('enable-active-high', b''))
    fixed(t, 'r-disabled', ('status', string('disabled')))
    regulator(t, 'r-other', 'regulator-pwm', [])

    states = ('states', cells(1800000, 1, 3300000, 0))
    limits = [('regulator-min-microvolt', cells(1800000)), ('regulator-max-microvolt', cells(3300000))]
    gpio(t, 'r-ok-gpio', ('gpios', cells(GPIO, 5, 1)), states, *limits,
         ('regulator-settling-time-us', cells(5000)), ('startup-delay-us', cells(10)),
         ('enable-gpios', cells(GPIO, 6, 1)), ('gpios-states', cells(1)), ('regulator-name', string('io')))
    gpio(t, 'r-gpio-noline', states)
    gpio(t, 'r-gpio-twolines', ('gpios', cells(GPIO, 5, 0, GPIO, 6, 0)), states)
    gpio(t, 'r-gpio-nostates', ('gpios', cells(GPIO, 5, 0)))
    gpio(t, 'r-gpio-badstates', ('gpios', cells(GPIO, 5, 0)), ('states', cells(1800000, 1, 3300000)))
    gpio(t, 'r-gpio-value2', ('gpios', cells(GPIO, 5, 0)), ('states', cells(1800000, 2)))
    gpio(t, 'r-gpio-dupvolt', ('gpios', cells(GPIO, 5, 0)), ('states', cells(1800000, 1, 1800000, 0)))
    gpio(t, 'r-gpio-dupvalue', ('gpios', cells(GPIO, 5, 0)), ('states', cells(1800000, 1, 3300000, 1)))
    gpio(t, 'r-gpio-outside', ('gpios', cells(GPIO, 5, 0)), ('states', cells(5000000, 1)), *limits)
    gpio(t, 'r-gpio-offon', ('gpios', cells(GPIO, 5, 0)), states, ('off-on-delay-us', cells(1)))
    gpio(t, 'r-gpio-badhint', ('gpios', cells(GPIO, 5, 0)), states, ('gpios-states', cells(2)))
    gpio(t, 'r-gpio-toomany', ('gpios', cells(GPIO, 5, 0)),
         ('states', cells(*[v for i in range(9) for v in (1000000 + i, i % 2)])))
    gpio(t, 'r-gpio-enable-low', ('gpios', cells(GPIO, 5, 0)), states, ('enable-gpios', cells(GPIO, 6, 0)))

    timings = ['cap-sd-highspeed', 'cap-mmc-highspeed', 'sd-uhs-sdr12', 'sd-uhs-sdr25', 'sd-uhs-sdr50',
               'sd-uhs-sdr104', 'sd-uhs-ddr50', 'mmc-ddr-3_3v', 'mmc-ddr-1_8v', 'mmc-ddr-1_2v',
               'mmc-hs200-1_8v', 'mmc-hs200-1_2v', 'mmc-hs400-1_8v', 'mmc-hs400-1_2v',
               'mmc-hs400-enhanced-strobe']
    host(t, 'mmc@100')
    host(t, 'mmc@200', ('bus-width', cells(8)), ('max-frequency', cells(50000000)), ('non-removable', b''),
         ('disable-wp', b''), ('no-1-8-v', b''), ('vmmc-supply', cells(SUPPLY)), ('vqmmc-supply', cells(SUPPLY)),
         ('reg-names', b'host\0cfg\0'), *[(name, b'') for name in timings], ranges=2)
    host(t, 'mmc@300', ('cd-gpios', cells(GPIO, 7, 1)), ('cd-inverted', b''),
         ('wp-gpios', cells(GPIO, 8, 0)), ('wp-inverted', b''), ('reg-names', b'cfg\0host\0'), ranges=2)
    host(t, 'mmc@400', ('non-removable', b''), child='wifi@1')
    host(t, 'mmc@500', ('non-removable', b''), ('broken-cd', b''))
    host(t, 'mmc@600', ('non-removable', b''), ('cd-gpios', cells(GPIO, 7, 0)))
    host(t, 'mmc@700', ('broken-cd', b''), ('cd-gpios', cells(GPIO, 7, 0)))
    host(t, 'mmc@800', ('disable-wp', b''), ('wp-gpios', cells(GPIO, 8, 0)))
    host(t, 'mmc@900', ('bus-width', cells(2)))
    host(t, 'mmc@a00', ('reg-names', b'cfg\0'))
    host(t, 'mmc@b00', ('reg-names', b'host\0'), ranges=2)
    host(t, 'mmc@c00', ('reg-names', b'host'))
    host(t, 'mmc@d00', ('cd-gpios', cells(GPIO, 7, 0, GPIO, 8, 0)))
    host(t, 'mmc@e00', ('cd-gpios', cells(GPIO, 7, 0x10)))
    host(t, 'mmc@f00', ('max-frequency', cells(0)))
    host(t, 'mmc@1000', ('broken-cd', cells(1)))
    host(t, 'mmc@1200', ('vmmc-supply', cells(SUPPLY, SUPPLY)))
    host(t, 'mmc@1300', ('cd-gpios', cells(GPIO3, 7, 0, 0)))
    host(t, 'mmc@1400', ('broken-cd', b''), ('mmc-ddr-3_3v', b''), ('vmmc-supply', cells(SUPPLY)))
    host(t, 'serial@1100', ('cd-gpios', cells(GPIO, 7, 0)))
    t.end()
    return t.finish()


if __name__ == '__main__':
    Path(sys.argv[1]).write_bytes(build())
