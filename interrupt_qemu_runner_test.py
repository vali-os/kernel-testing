"""Exercise the actual boot runner with a deterministic clock and QEMU process."""
import contextlib
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import interrupt_qemu_test as runner


class BootRunnerTest(unittest.TestCase):
    def run_boot(self, milestones=None, failure_at=None, exit_at=None,
                 timeout=100, edu=True, rocker=False, failure_marker='MSSync timeout',
                 kernel_selftest=True, intx=False, disk=False):
        milestones = milestones if milestones is not None else {
            'Interrupt allocation self-test: PASS': 0,
            'Interrupt allocation self-test: CPUs=4 ': 0,
            'CpuCoreStart 1 is online': 0,
            'CpuCoreStart 2 is online': 0,
            'CpuCoreStart 3 is online': 0,
            'EDU MSI self-test: PASS': 50,
            'storage ready': 50,
        }
        self.now = 0

        class Process:
            returncode = 1
            stopped = False

            def poll(process):
                return 1 if process.stopped or (
                    exit_at is not None and self.now >= exit_at) else None

            def terminate(process):
                process.stopped = True

            def wait(process, timeout):
                pass

        process = Process()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            variables = root / 'original-vars.fd'
            variables.write_bytes(b'firmware')
            output = root / 'boot'

            def serial_text():
                text = '\n'.join(marker for marker, at in milestones.items()
                                 if self.now >= at)
                if failure_at is not None and self.now >= failure_at:
                    text += '\n' + failure_marker
                return text

            def sleep(seconds):
                self.now += seconds
                (output / 'serial.log').write_text(serial_text())

            def launch(*args, **kwargs):
                self.command = args[0]
                (output / 'serial.log').write_text(serial_text())
                return process

            arguments = [
                'interrupt_qemu_test.py', '--qemu', str(root / 'qemu'),
                '--code', str(root / 'code.fd'), '--vars', str(variables),
                '--esp', str(root / 'esp'), '--output', str(output),
                '--smp', '4', '--require', 'storage ready',
                '--timeout', str(timeout),
            ]
            if edu:
                arguments.append('--edu')
            if rocker:
                arguments.append('--rocker')
            if kernel_selftest:
                arguments.append('--kernel-selftest')
            if intx:
                arguments.append('--intx')
            if intx or disk:
                arguments += ['--disk', str(root / 'disk.img')]
            try:
                with patch('sys.argv', arguments), \
                        patch.object(runner.subprocess, 'Popen', side_effect=launch), \
                        patch.object(runner.time, 'monotonic', side_effect=lambda: self.now), \
                        patch.object(runner.time, 'sleep', side_effect=sleep), \
                        contextlib.redirect_stdout(io.StringIO()):
                    runner.main()
            finally:
                self.assertIsNotNone(process.poll(), 'QEMU must be cleaned up')
                self.assertEqual(variables.read_bytes(), b'firmware')

    def test_late_userspace_gets_full_observation(self):
        self.run_boot()
        self.assertEqual(self.now, 90)

    def test_delayed_failure_after_readiness_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Kernel boot failed'):
            self.run_boot(failure_at=60)
        self.assertEqual(self.now, 60)

    def test_each_required_milestone_starts_observation(self):
        markers = [
            'Interrupt allocation self-test: PASS',
            'Interrupt allocation self-test: CPUs=4 ',
            'CpuCoreStart 1 is online', 'CpuCoreStart 2 is online',
            'CpuCoreStart 3 is online', 'EDU MSI self-test: PASS',
            'storage ready',
        ]
        for late in markers:
            with self.subTest(late=late):
                self.run_boot(milestones={marker: 50 if marker == late else 0
                                          for marker in markers})
                self.assertEqual(self.now, 90)

    def test_startup_cannot_consume_overall_timeout(self):
        with self.assertRaisesRegex(RuntimeError, 'before timeout'):
            self.run_boot(timeout=70)
        self.assertEqual(self.now, 70)

    def test_failure_at_acceptance_boundary_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'Kernel boot failed'):
            self.run_boot(failure_at=90)

    def test_exit_at_acceptance_boundary_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'QEMU exited'):
            self.run_boot(exit_at=90)

    def test_edu_is_optional_without_flag(self):
        markers = {
            'Interrupt allocation self-test: PASS': 0,
            'Interrupt allocation self-test: CPUs=4 ': 0,
            'CpuCoreStart 1 is online': 0,
            'CpuCoreStart 2 is online': 0,
            'CpuCoreStart 3 is online': 0,
            'storage ready': 0,
        }
        self.run_boot(milestones=markers, edu=False)
        self.assertEqual(self.now, 40)
        with self.assertRaisesRegex(RuntimeError, 'before timeout'):
            self.run_boot(milestones=markers)

    def test_rocker_requires_its_own_delivery_marker(self):
        markers = {
            'Interrupt allocation self-test: PASS': 0,
            'Interrupt allocation self-test: CPUs=4 ': 0,
            'CpuCoreStart 1 is online': 0,
            'CpuCoreStart 2 is online': 0,
            'CpuCoreStart 3 is online': 0,
            'EDU MSI self-test: PASS': 0,
            'storage ready': 0,
        }
        with self.assertRaisesRegex(RuntimeError, 'before timeout'):
            self.run_boot(milestones=markers, rocker=True)
        markers['ROCKER MSI-X self-test: PASS'] = 50
        self.run_boot(milestones=markers, rocker=True)
        self.assertEqual(self.now, 90)
        with self.assertRaisesRegex(RuntimeError, 'Kernel boot failed'):
            self.run_boot(milestones=markers, rocker=True, failure_at=90,
                          failure_marker='ROCKER MSI-X self-test: FAILED')

    def test_current_kernel_requires_device_delivery_without_apic_test(self):
        markers = {
            'CpuCoreStart 1 is online': 0,
            'CpuCoreStart 2 is online': 0,
            'CpuCoreStart 3 is online': 0,
            'EDU MSI self-test: PASS': 0,
            'ROCKER MSI-X self-test: PASS': 50,
            'storage ready': 0,
        }
        self.run_boot(milestones=markers, kernel_selftest=False, rocker=True)
        self.assertEqual(self.now, 90)
        del markers['ROCKER MSI-X self-test: PASS']
        with self.assertRaisesRegex(RuntimeError, 'before timeout'):
            self.run_boot(milestones=markers, kernel_selftest=False, rocker=True)

    def test_intx_requires_callback_and_consumed_completion(self):
        markers = {
            'CpuCoreStart 1 is online': 0,
            'CpuCoreStart 2 is online': 0,
            'CpuCoreStart 3 is online': 0,
            'storage ready': 0,
            'OnEvent interrupt status=0x1': 0,
            'VirtioSplitQueuePoll queue=0': 50,
        }
        self.run_boot(milestones=markers, kernel_selftest=False, edu=False, intx=True)
        self.assertEqual(self.now, 90)
        self.assertIn('virtio-blk-pci,drive=osdisk,vectors=0', self.command)
        self.run_boot(milestones=markers, kernel_selftest=False, edu=False, disk=True)
        self.assertIn('virtio-blk-pci,drive=osdisk', self.command)
        self.assertFalse(any('vectors=0' in argument for argument in self.command))
        for missing in ('OnEvent interrupt status=0x1', 'VirtioSplitQueuePoll queue=0'):
            with self.subTest(missing=missing), self.assertRaisesRegex(RuntimeError, 'before timeout'):
                self.run_boot(milestones={key: at for key, at in markers.items() if key != missing},
                              kernel_selftest=False, edu=False, intx=True)


if __name__ == '__main__':
    unittest.main()
