#!/usr/bin/env python3
"""Reply to Vali's opt-in Ethernet hardware test on a dedicated Linux interface."""
import argparse
from pathlib import Path
import socket
import time

ETHERTYPE = 0x88B5


def reply(frame, mac):
    if len(frame) not in (60, 1514):
        return None
    if frame[:6] != b'\xff' * 6 or frame[6] & 1 or frame[6:12] == bytes(6):
        return None
    if frame[12:23] != b'\x88\xb5VALIGEN1\x01':
        return None
    sequence = int.from_bytes(frame[23:27], 'big')
    if not sequence or len(frame) != (60 if sequence & 1 else 1514):
        return None
    if any(value != ((offset ^ sequence) & 255)
           for offset, value in enumerate(frame[27:], 27)):
        return None
    return frame[6:12] + mac + frame[12:22] + b'\x02' + frame[23:]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('interface', help='Dedicated Ethernet interface connected to the Pi')
    parser.add_argument('--seconds', type=int, default=120)
    args = parser.parse_args()
    if args.seconds <= 0 or '/' in args.interface:
        parser.error('use an interface name and a positive duration')
    mac = bytes.fromhex((Path('/sys/class/net') / args.interface / 'address').read_text().strip().replace(':', ''))
    count = 0
    sizes = set()
    with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(ETHERTYPE)) as sock:
        sock.bind((args.interface, 0))
        sock.settimeout(1)
        deadline = time.monotonic() + args.seconds
        print(f'Listening on {args.interface}; no IP address required', flush=True)
        while time.monotonic() < deadline:
            try:
                frame = sock.recv(2048)
            except socket.timeout:
                continue
            response = reply(frame, mac)
            if response is None:
                continue
            sock.send(response)
            count += 1
            sizes.add(len(frame))
            print(f'Pi TX verified, RX reply sent: seq={int.from_bytes(frame[23:27], "big")} bytes={len(frame)} mac={frame[6:12].hex(":")}', flush=True)
    print(f'{count} replies sent; frame sizes={sorted(sizes)}. Check Pi serial TX/RX PASS for end-to-end confirmation.')
    return 0 if sizes == {60, 1514} else 1


if __name__ == '__main__':
    raise SystemExit(main())
