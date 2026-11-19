"""Check the Raspberry Pi 4 SD chain in the serial log of a QEMU raspi4b boot.

The boot itself is run with build/boot-tests/run_dma_2d_boot.py --arch rpi4
(see BUILDING.md). This script reads the serial.log it leaves behind and
checks, in order, that the firmware driver, deviced and the SDHCI driver
worked together up to the point QEMU allows:

1. The firmware driver bound to the mailbox, read the firmware revision,
   board model, board revision and ARM memory, and started serving.
2. deviced accepted DeviceControllerEnable for the GPIO expander and the
   firmware clocks.
3. deviced handed the waiting SD host (mmc@7e340000) to the SDHCI driver.
4. The SDHCI driver logged the SD configuration of the slot.
5. DevicePrepare worked: the driver found its register window, and read the
   input clock through the firmware driver.
6. The driver logged the host version and capabilities.
7. The driver refused the host because QEMU's EMMC2 model has no ADMA2.
8. The device was released.

It also checks that no GPIO request reached the firmware driver (nothing in
this chain asks for card power on QEMU), that the firmware driver did not
crash, and that no SDHCI warning or error appeared other than the expected
rejection. With --baseline it lists the warning and error lines that are new
compared with an earlier serial log, ignoring timestamps, core numbers,
thread numbers and device numbers.

The result goes to stdout as JSON, and to --output when given. The exit code
is 0 when every check passed.
"""
import argparse
import json
from pathlib import Path
import re
import sys

# Each step is (name, pattern). They must appear in this order in the log.
# Device and node numbers differ between builds, so they are matched loosely.
STEPS = (
    ("firmware driver: firmware revision, board model and revision",
     r"RPI-FW (\d+): firmware revision 0x[0-9a-f]{8} \(\d+\), board model 0x[0-9a-f]{8}, board revision 0x[0-9a-f]{8}"),
    ("firmware driver: ARM memory and message page",
     r"RPI-FW \d+: ARM memory at 0x[0-9a-f]{8}, \d+ MiB, message page at device address 0x[0-9a-f]{8}"),
    ("deviced: DeviceControllerEnable accepted for GPIO and clocks",
     r"Platform: mailbox@7e00b880 works as a controller \(gpio node \d+, [1-9]\d* clock nodes\)"),
    ("deviced: the waiting SD host is handed to its driver",
     r"Platform: mmc@7e340000 has every dependency owner, binding its driver"),
    ("SDHCI driver: registered the platform device",
     r"SDHCI (\d+): registered platform device \"brcm,bcm2711-emmc2\""),
    ("SDHCI driver: SD configuration",
     r"SDHCI \d+: SD config: memory slot, 4 lines, max none, removable, cd polling, wp host, supplies vmmc vqmmc, host reg 0"),
    ("SDHCI driver: DevicePrepare worked and the register window was found",
     r"SDHCI \d+: host registers are device resource 0"),
    ("SDHCI driver: host version and capabilities",
     r"SDHCI \d+: host version 0x[0-9a-f]{4}, capabilities 0x[0-9a-f]{8} 0x[0-9a-f]{8}"),
    ("SDHCI driver: input clock read through the firmware driver",
     r"SDHCI \d+: base clock \d+ Hz in the capabilities, platform input clock [1-9]\d* Hz"),
    ("SDHCI driver: host refused for its missing ADMA2 engine",
     r"SDHCI \d+: host has no ADMA2, and PIO and SDMA are not implemented"),
    ("SDHCI driver: device released",
     r"SDHCI \d+: device released"),
)

# Lines that must appear somewhere, in any order. The firmware driver logs
# that it serves only after DeviceControllerEnable returned, and deviced may
# hand the SD host to its driver before that.
ANYWHERE = (
    ("firmware driver: serving GPIO lines and clocks", r"RPI-FW \d+: serving GPIO lines clocks"),
)

# SDHCI warning and error lines expected on QEMU: the rejection, and the base
# clock warning. QEMU's EMMC2 model reports a 52 MHz base clock, while its
# firmware model answers every clock rate request with a fixed 700 MHz.
EXPECTED_SDHCI_PROBLEMS = (
    r"SDHCI \d+: capability base clock \d+ Hz and platform input clock \d+ Hz differ",
    r"SDHCI \d+: host has no ADMA2",
    r"SDHCI \d+: preparing the controller failed: 8",
)

# Lines that show the chain went wrong, wherever they appear.
FAILURES = (
    ("firmware driver crashed", r"Crashed in module rpi-firmware|Crashed in module sdhci"),
    ("a GPIO request reached the firmware driver", r"RPI-FW.*tag 0x0003[08]04[13]"),
    ("guest panic", r"DebugPanic\(|FATAL EXCEPTION OCCURRED"),
)

LEVEL = re.compile(r"\[(?:warn|error|fatal)[-\]]")
PREFIX = re.compile(r"^\d+ \[([a-z]+)-\d+-\d+\] ")


def normalize(line):
    # Drop the timestamp, core and thread, and every number, so lines from two
    # boots can be compared.
    match = PREFIX.match(line)
    level = match.group(1) if match else ""
    text = line[match.end():] if match else line
    return level + " " + re.sub(r"0x[0-9a-f]+|\d+", "N", text)


def problem_lines(text):
    return [line for line in text.splitlines() if LEVEL.search(line)]


def check(text):
    results = []
    position = 0
    passed = True
    for name, pattern in STEPS:
        match = re.compile(pattern).search(text, position)
        if match is None:
            results.append({"step": name, "found": False})
            passed = False
            continue
        line_start = text.rfind("\n", 0, match.start()) + 1
        line_end = text.find("\n", match.end())
        results.append({"step": name, "found": True,
                        "line": text[line_start:line_end if line_end >= 0 else len(text)]})
        position = match.end()

    for name, pattern in ANYWHERE:
        match = re.search(pattern, text)
        results.append({"step": name, "found": match is not None, "line": match.group(0) if match else None})
        passed &= match is not None

    failures = []
    for name, pattern in FAILURES:
        for line in text.splitlines():
            if re.search(pattern, line):
                failures.append({"failure": name, "line": line})
    unexpected = []
    for line in problem_lines(text):
        if "SDHCI" not in line:
            continue
        if not any(re.search(pattern, line) for pattern in EXPECTED_SDHCI_PROBLEMS):
            unexpected.append(line)
    passed &= not failures and not unexpected
    return passed, results, failures, unexpected


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("serial", type=Path, help="serial.log of the raspi4b boot")
    parser.add_argument("--baseline", type=Path, help="serial.log of an earlier boot to compare with")
    parser.add_argument("--output", type=Path, help="file for the JSON result")
    args = parser.parse_args()

    text = args.serial.read_text(errors="replace")
    passed, steps, failures, unexpected = check(text)
    result = {"pass": passed, "steps": steps, "failures": failures, "unexpected_sdhci_problems": unexpected}
    if args.baseline is not None:
        known = {normalize(line) for line in problem_lines(args.baseline.read_text(errors="replace"))}
        result["new_warnings_and_errors"] = [line for line in problem_lines(text) if normalize(line) not in known]
    output = json.dumps(result, indent=2) + "\n"
    if args.output is not None:
        args.output.write_text(output)
    sys.stdout.write(output)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
