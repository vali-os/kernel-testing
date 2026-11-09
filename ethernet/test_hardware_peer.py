import unittest
from hardware_peer import reply


class PeerTests(unittest.TestCase):
    def test_reply_and_corruption(self):
        source = bytes.fromhex('020000000001')
        peer = bytes.fromhex('020000000002')
        for sequence, length in ((1, 60), (2, 1514)):
            frame = (b'\xff' * 6 + source + b'\x88\xb5VALIGEN1\x01' +
                     sequence.to_bytes(4, 'big') +
                     bytes((i ^ sequence) & 255 for i in range(27, length)))
            response = reply(frame, peer)
            self.assertEqual(response, source + peer + frame[12:22] + b'\x02' + frame[23:])
            self.assertIsNone(reply(response, peer))
            self.assertIsNone(reply(frame[:-1], peer))
            damaged = bytearray(frame)
            damaged[-1] ^= 1
            self.assertIsNone(reply(damaged, peer))
            damaged = bytearray(frame)
            damaged[26] ^= 2
            self.assertIsNone(reply(damaged, peer))
