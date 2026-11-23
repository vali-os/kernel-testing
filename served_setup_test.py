"""Run served's setup job with /data failing at each step, on a POSIX host
with ASan/UBSan."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SETUP = 'services/served/server/setup.c'

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-served-setup-') as directory:
    output = Path(directory)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-D_POSIX_C_SOURCE=200809L',
             '-DVALI', '-DTESTING', '-DSERVICEAPI=static inline', '-DSERVICEABI=',
             '-fms-extensions', '-Wall', '-Wextra', '-Werror',
             '-Wno-unused-function', '-Wno-unused-parameter', '-Wno-unknown-warning-option',
             '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    flags += ['-I' + str(ROOT / path) for path in
              ('testing/include', 'services/served/include', 'librt/libos/include',
               'librt/libddk/include', 'librt/libds/include', 'librt/libgracht/include')]
    binary = str(output / 'test')

    # setup.c is built with exit renamed to __TestExit, after stdlib.h has
    # declared the real one, so a call to exit is counted by the test instead
    # of ending the test process.
    hook = output / 'exit_hook.h'
    hook.write_text('#include <stdlib.h>\nvoid __TestExit(int);\n#define exit __TestExit\n')

    def build(setup=None):
        setupObject = str(output / 'setup.o')
        subprocess.run(flags + ['-include', str(hook), '-c', setup or str(ROOT / SETUP),
                                '-o', setupObject], check=True)
        subprocess.run(flags + [str(ROOT / 'testing/served_setup_test.c'), setupObject,
                                '-o', binary], check=True)

    build()
    subprocess.run([binary], check=True, timeout=60)
    if args.mutation_check:
        text = (ROOT / SETUP).read_text()
        for before, after, reason in (
            ('              oserr);\n        return;\n    }\n\n    // The first thing',
             '              oserr);\n        exit(-1);\n    }\n\n    // The first thing',
             'served exits when it cannot create its paths'),
            ('    oserr = StateLoad();\n    if (oserr != OS_EOK) {',
             '    oserr = StateLoad();\n    if (false) {',
             'setup goes on with a state that failed to load'),
            ('    oserr_t oserr = ServerEnsurePaths();\n    if (oserr != OS_EOK) {',
             '    oserr_t oserr = ServerEnsurePaths();\n    if (false) {',
             'setup goes on without its paths'),
        ):
            assert text.count(before) == 1, before
            mutant = output / 'setup.c'
            mutant.write_text(text.replace(before, after))
            build(str(mutant))
            if subprocess.run([binary], capture_output=True, timeout=60).returncode == 0:
                raise RuntimeError('Undetected regression: ' + reason)
            print('Rejected mutation: ' + reason, flush=True)
