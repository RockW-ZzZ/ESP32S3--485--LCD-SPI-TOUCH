"""Compile the actual C codec as a freestanding host DLL and test it via ctypes.

Uses the already installed Visual Studio compiler; no pip packages required.
Firmware itself is built with the installed Xtensa GCC toolchain.
"""
import argparse
import ctypes as c
from pathlib import Path
import random
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
U8 = c.c_uint8
U16 = c.c_uint16
SIZE = c.c_size_t
LIB = None


def reference_crc(data):
    # Independent table-based oracle, plus fixed published/common RTU vectors.
    table = []
    for value in range(256):
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
        table.append(value)
    value = 0xFFFF
    for byte in data:
        value = (value >> 8) ^ table[(value ^ byte) & 255]
    return value


def adu(payload):
    payload = bytes(payload)
    return payload + reference_crc(payload).to_bytes(2, 'little')


def request(slave=1, function=3, start=0, count=2, values=None, capacity=256):
    out = (U8 * 258)(*([0xCD] * 258))
    n = SIZE(999)
    vals = (U16 * len(values))(*values) if values is not None else None
    result = LIB.mb_build_request(slave, function, start, count, vals, out, capacity, c.byref(n))
    assert bytes(out[capacity:]) == bytes([0xCD] * (258 - capacity)), 'buffer overwrite'
    return result, bytes(out[:n.value]), n.value


def check(q, r):
    exception = U8(0xFF)
    qa, ra = (U8 * len(q))(*q), (U8 * len(r))(*r)
    result = LIB.mb_check_response(qa, len(q), ra, len(r), c.byref(exception))
    return result, exception.value


class CodecTests(unittest.TestCase):
    def test_crc_and_request_vectors(self):
        s = b'123456789'
        self.assertEqual(LIB.mb_crc16((U8 * len(s))(*s), len(s)), 0x4B37)
        self.assertEqual(request()[1], bytes.fromhex('01 03 00 00 00 02 C4 0B'))
        self.assertEqual(request(function=6, start=1, count=1, values=[3])[1],
                         bytes.fromhex('01 06 00 01 00 03 98 0B'))
        self.assertEqual(LIB.mb_crc16(None, 0), 0xFFFF)

    def test_valid_read_and_write(self):
        for fn in (3, 4):
            q = request(function=fn)[1]
            self.assertEqual(check(q, adu([1, fn, 4, 0x12, 0x34, 0xAB, 0xCD])), (0, 0))
        for fn, count, values in ((6, 1, [0x1234]), (16, 3, [1, 0x1234, 0xFFFF])):
            q = request(function=fn, start=10, count=count, values=values)[1]
            self.assertEqual(check(q, adu(q[:6])), (0, 0))
        q = request(function=16, start=10, count=3, values=[1, 0x1234, 0xFFFF])[1]
        self.assertEqual(q[6:-2], bytes.fromhex('06 00 01 12 34 FF FF'))

    def test_exception(self):
        for fn in (3, 4, 6, 16):
            q = request(function=fn, count=1, values=[123])[1]
            self.assertEqual(check(q, adu([1, fn | 0x80, 2])), (4, 2))
            self.assertEqual(check(q, adu([1, fn | 0x80, 0])), (3, 0))
            self.assertEqual(check(q, adu([1, fn | 0x80, 2, 0])), (3, 0))

    def test_corruption_and_mismatch(self):
        q = request()[1]
        valid = adu([1, 3, 4, 0, 1, 0, 2])
        for index in range(len(valid)):
            bad = bytearray(valid); bad[index] ^= 1
            self.assertEqual(check(q, bad), (2, 0))
        for payload in ([2, 3, 4, 0, 1, 0, 2], [1, 4, 4, 0, 1, 0, 2],
                        [1, 3, 2, 0, 1], [1, 3, 3, 0, 1, 0, 2]):
            self.assertEqual(check(q, adu(payload)), (3, 0))
        for fn in (6, 16):
            q = request(function=fn, count=1, values=[5])[1]
            echo = bytearray(q[:6]); echo[5] ^= 1
            self.assertEqual(check(q, adu(echo)), (3, 0))

    def test_limits_and_overflow(self):
        for fn in (3, 4):
            result, q, _ = request(slave=247, function=fn, start=65411, count=125)
            self.assertEqual(result, 0)
            r = adu([247, fn, 250] + [0xAB, 0xCD] * 125)
            self.assertEqual(len(r), 255)
            self.assertEqual(check(q, r), (0, 0))
            self.assertEqual(request(function=fn, count=126)[0], 1)
        result, q, n = request(function=16, start=65413, count=123, values=list(range(123)))
        self.assertEqual((result, n), (0, 255))
        self.assertEqual(check(q, adu(q[:6])), (0, 0))
        self.assertEqual(request(function=16, count=124, values=[0] * 124)[0], 1)
        for kwargs in ({'slave': 0}, {'slave': 248}, {'count': 0}, {'start': 65535},
                       {'function': 5}, {'capacity': 7}, {'function': 6, 'count': 1},
                       {'function': 16}, {'function': 6, 'values': [1, 2]}):
            self.assertEqual(request(**kwargs)[::2], (1, 0))
        self.assertEqual(request(start=65535, count=1)[0], 0)

    def test_short_and_invalid_buffers(self):
        q = request()[1]
        for n in range(5):
            self.assertEqual(check(q, bytes(n)), (3, 0))
        self.assertEqual(check(q, bytes(257)), (3, 0))
        for n in range(8):
            self.assertEqual(check(q[:n], adu([1, 3, 0])), (1, 0))
        bad = bytearray(q); bad[-1] ^= 1
        self.assertEqual(check(bad, adu([1, 3, 0])), (1, 0))
        n = SIZE(999)
        self.assertEqual(LIB.mb_build_request(1, 3, 0, 1, None, None, 256, c.byref(n)), 1)
        self.assertEqual(n.value, 0)

    def test_random_roundtrips(self):
        rng = random.Random(20261004)
        for _ in range(500):
            fn = rng.choice([3, 4, 6, 16])
            count = 1 if fn == 6 else rng.randint(1, 123 if fn == 16 else 125)
            slave = rng.randint(1, 247)
            start = rng.randint(0, 65536 - count)
            values = [rng.randrange(65536) for _ in range(count)]
            status, q, _ = request(slave, fn, start, count, values)
            self.assertEqual(status, 0)
            self.assertEqual(q[-2:], reference_crc(q[:-2]).to_bytes(2, 'little'))
            payload = q[:6] if fn in (6, 16) else bytes([slave, fn, count * 2]) + b''.join(
                value.to_bytes(2, 'big') for value in values)
            self.assertEqual(check(q, adu(payload)), (0, 0))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--msvc-bin', type=Path, default=Path(
        'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'))
    parser.add_argument('--sdk-include', type=Path, default=Path(
        'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0'))
    args = parser.parse_args()
    build = ROOT / 'tests/build'
    build.mkdir(parents=True, exist_ok=True)
    obj, dll = build / 'modbus_codec.obj', build / 'modbus_codec.dll'
    subprocess.run([str(args.msvc_bin / 'cl.exe'), '/nologo', '/TC', '/std:c11',
        '/GS-', '/Zl', '/W4', '/WX', '/wd4244', '/wd4267', '/O2',
        '/I' + str(args.msvc_bin.parents[2] / 'include'),
        '/I' + str(args.sdk_include / 'ucrt'), '/I' + str(args.sdk_include / 'shared'),
        '/I' + str(ROOT / 'components/board_drivers/include'), '/c',
        str(ROOT / 'components/board_drivers/modbus_codec.c'), '/Fo' + str(obj)], check=True)
    subprocess.run([str(args.msvc_bin / 'link.exe'), '/nologo', '/dll', '/noentry',
        '/nodefaultlib', '/machine:x64', '/export:mb_crc16', '/export:mb_build_request',
        '/export:mb_check_response', f'/out:{dll}', str(obj)], check=True)
    LIB = c.CDLL(str(dll))
    LIB.mb_crc16.argtypes = [c.POINTER(U8), SIZE]; LIB.mb_crc16.restype = U16
    LIB.mb_build_request.argtypes = [U8, U8, U16, U16, c.POINTER(U16), c.POINTER(U8), SIZE, c.POINTER(SIZE)]
    LIB.mb_check_response.argtypes = [c.POINTER(U8), SIZE, c.POINTER(U8), SIZE, c.POINTER(U8)]
    unittest.main(argv=['test_modbus_codec'], verbosity=2)
