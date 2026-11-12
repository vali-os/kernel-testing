"""Exercise socket inheritance and packet addressing with real client/netd code."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutation-check', action='store_true')
args = parser.parse_args()
with tempfile.TemporaryDirectory(prefix='vali-socket-io-') as directory:
    output = Path(directory)
    for service in ('net', 'process', 'file', 'device'):
        subprocess.run(['python3', str(ROOT / 'librt/libgracht/generator/parser.py'),
                        '--service', str(ROOT / 'protocols' / (service + '.gr')),
                        '--out', directory, '--lang-c', '--client', '--server'], check=True)
    flags = [os.environ.get('CC', 'clang'), '-std=gnu11', '-DVALI', '-DTESTING',
             '-DMOLLENOS', '-D__time64_t=int64_t', '-DNDEBUG', '-fms-extensions',
             '-Wno-macro-redefined', '-ffunction-sections', '-fdata-sections',
             '-Wl,--gc-sections', '-fsanitize=address,undefined',
             '-fno-sanitize-recover=all', '-mllvm', '-asan-globals=0', '-g', '-I' + directory]
    flags += ['-I' + str(ROOT / path) for path in
              ('librt/libc/include', 'librt/libos/include', 'librt/libds/include',
               'librt/libddk/include', 'librt/libgracht/include', 'services/netd')]
    binary = str(output / 'socket-io')
    command = flags + [str(ROOT / 'testing/socket_io_test.c'),
                       str(ROOT / 'librt/libds/streambuffer.c'),
                       str(ROOT / 'librt/libds/list.c'),
                       str(ROOT / 'librt/libds/queue.c'),
                       str(ROOT / 'librt/libddk/io.c'), '-o', binary]
    environment = dict(os.environ)
    environment['ASAN_OPTIONS'] = environment.get('ASAN_OPTIONS', '') + ':detect_leaks=0'
    subprocess.run(command, check=True)
    subprocess.run([binary], check=True, env=environment)
    if args.mutation_check:
        mutations = {
            'invert unconnected send guard': (
                'librt/libos/services/net.c', 'SOCKET_CLIENT_SOURCE',
                'packet.destination == UUID_INVALID', 'packet.destination != UUID_INVALID'),
            'ignore inherited peer': (
                'librt/libos/services/net.c', 'SOCKET_CLIENT_SOURCE',
                'packet.destination = atomic_load_explicit(&state->Peer, memory_order_acquire);',
                'packet.destination = UUID_INVALID;'),
            'redirect queued packet after reconnect': (
                'services/netd/domains/local.c', 'SOCKET_LOCAL_SOURCE',
                'NetworkManagerSocketGetUnsafe(Packet->destination)',
                'NetworkManagerSocketGetUnsafe(NetworkManagerSocketGetUnsafe(1)->Domain->ConnectedSocket)'),
            'leave shared peer unchanged': (
                'services/netd/domains/local.c', 'SOCKET_LOCAL_SOURCE',
                'atomic_store_explicit(&state->Peer, peer, memory_order_release);',
                '(void)state;'),
            'overflow short receive address': (
                'librt/libos/services/net.c', 'SOCKET_CLIENT_SOURCE',
                'MIN(message->msg_namelen, packet.addresslen)', 'packet.addresslen'),
            'forget truncated address length': (
                'librt/libos/services/net.c', 'SOCKET_CLIENT_SOURCE',
                'message->msg_name != NULL ? packet.addresslen : 0', 'addressBytes'),
        }
        for name, (filename, define, before, after) in mutations.items():
            source = (ROOT / filename).read_text()
            assert source.count(before) == 1, name
            mutant = output / Path(filename).name
            mutant.write_text(source.replace(before, after))
            subprocess.run(command + ['-I' + str((ROOT / filename).parent),
                                      f'-D{define}="{mutant}"'], check=True)
            result = subprocess.run([binary], env=environment, capture_output=True)
            if result.returncode == 0:
                raise RuntimeError('Undetected socket mutation: ' + name)
            print('Rejected socket mutation: ' + name)
