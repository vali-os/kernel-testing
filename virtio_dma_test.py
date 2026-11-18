"""Run split-queue and block DMA failure/teardown regressions on a POSIX host."""
import os
import argparse
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='virtio-dma-') as directory:
    includes = ['testing/netadapter/include', 'testing/include', 'testing', 'modules/virtio/common/include',
                'librt/libos/include', 'librt/libgracht/include', 'librt/libddk/include', 'librt/libds/include', 'librt/libusb/include',
                ]
    for service in ("device", "storage", "process", "file", "contracts/filesystem", "contracts/usbhost", "contracts/driver", "contracts/storage"):
        subprocess.run(["python3", str(ROOT / "librt/libgracht/generator/parser.py"),
                        "--service", str(ROOT / "protocols" / (service + ".gr")),
                        "--out", directory, "--lang-c", "--client", "--server"], check=True)
    command = [os.environ.get('CC', 'clang'), '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DVALI', '-DTESTING',
               '-DSERVICEAPI=static inline', '-DSERVICEABI=', '-fms-extensions', '-Wall', '-Wextra', '-Werror',
               '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-sign-compare',
               '-ffunction-sections', '-fdata-sections']
    command += ['-I'+directory] + ['-I'+str(ROOT/path) for path in includes]
    command += [str(ROOT/'testing/virtio_dma_test.c'), str(ROOT/'testing/net_shm_mock.c'),
                '-Wl,--gc-sections', '-o', directory+'/test']
    subprocess.run(command, check=True)
    subprocess.run([directory+'/test'], check=True)
    if args.mutation_check:
        source = (ROOT / 'modules/virtio/blk/main.c').read_text()
        anchor = 'if (device->CleanupRequests != NULL) {'
        assert anchor in source
        event_guard = ('    if (event == NULL || !(event->events & IOSETSYN)) {\n'
                       '        return OS_ENOENT;\n    }\n\n')
        assert source.count(event_guard) == 1
        event_body = '    // The device may be removed while this event waits for the module lock.'
        assert source.count(event_body) == 1
        mutations = {
            'cleanup monitor skipped pending response': source.replace(
                anchor, 'if (device->CleanupRequests == NULL) {'),
            'IPC event waited for device lock': source.replace(event_guard, '').replace(
                event_body, event_guard + event_body),
        }
        mutant = Path(directory) / 'main-mutant.c'
        for name, body in mutations.items():
            mutant.write_text(body)
            subprocess.run(command + ['-I' + str(ROOT / 'modules/virtio/blk'),
                           f'-DVIRTIO_BLK_MAIN_SOURCE="{mutant}"'], check=True)
            if subprocess.run([directory+'/test']).returncode == 0:
                raise RuntimeError('Undetected regression: ' + name)
            print('Rejected mutation: ' + name, flush=True)
