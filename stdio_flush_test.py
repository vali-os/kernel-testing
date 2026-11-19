"""Test io_buffer_flush_all with the target headers on a POSIX host."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-stdio-flush-') as directory:
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-g']
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include')]
    binary = str(Path(directory) / 'test')
    command = flags + [str(ROOT / 'testing/stdio_flush_test.c'), '-o', binary]
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, timeout=30)
    if args.mutation_check:
        # The old check read the flags of every descriptor's stream, also
        # when the descriptor had none.
        source = (ROOT / 'librt/libc/stdio/core/file_stream.c').read_text()
        anchor = 'if (object->Stream != NULL && (object->Stream->Flags & flags)) {'
        assert source.count(anchor) == 1
        mutant = Path(directory) / 'file-stream-mutant.c'
        mutant.write_text(source.replace(anchor, 'if (object->Stream->Flags & flags) {'))
        subprocess.run(command + ['-I' + str(ROOT / 'librt/libc/stdio/core'),
                       f'-DFILE_STREAM_SOURCE="{mutant}"'], check=True)
        if subprocess.run([binary], capture_output=True, timeout=30).returncode == 0:
            raise RuntimeError('Undetected mutation: descriptor without a stream flushed')
        print('Rejected mutation: descriptor without a stream flushed')
