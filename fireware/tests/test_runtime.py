"""Real C RS485/Modbus/serial drivers with mocked IDF I/O, plus input policies.

Uses installed MSVC for host DLLs. Exercises concurrent Python/native callers;
does not simulate UART electrical timing or replace an on-board dual-bus test.
"""
import argparse
import concurrent.futures as futures
import ctypes as c
from pathlib import Path
import queue
import subprocess
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
LIB = MOCK = None
U8, U16, SIZE = c.c_uint8, c.c_uint16, c.c_size_t


class UartEvent(c.Structure):
    _fields_ = [('type', c.c_int), ('size', SIZE), ('timeout', c.c_bool)]


class Button(c.Structure):
    _fields_ = [('raw', c.c_bool), ('stable', c.c_bool), ('long_sent', c.c_bool),
                ('changed', c.c_int64), ('down', c.c_int64)]


class Gate(c.Structure):
    _fields_ = [('enabled', c.c_bool), ('physical_down', c.c_bool),
               ('wait_release', c.c_bool), ('reset_pending', c.c_bool)]


class MockIDF:
    CALLBACK = c.CFUNCTYPE(c.c_int64, c.c_int, *([c.c_ssize_t] * 4))

    def __init__(self):
        self.locks, self.queues, self.configs, self.pins = {}, {}, {}, {}
        self.rx, self.responses, self.writes, self.delays, self.errors = {}, {}, {}, {}, []
        self.started = {p: threading.Event() for p in range(3)}
        self.active = {p: 0 for p in range(3)}
        self.peak = {p: 0 for p in range(3)}
        self.next_lock = 1000
        self.fail_install = None
        self.callback = self.CALLBACK(self.dispatch)

    def configure(self, p, response=b'', delay=0):
        self.responses[p] = response
        self.delays[p] = delay
        self.started[p].clear()
        self.peak[p] = 0

    def dispatch(self, op, a, b, d, e):
        try:
            return self.call(op, a, b, d, e)
        except BaseException as exc:
            self.errors.append((op, repr(exc)))
            return -1

    def call(self, op, a, b, d, e):
        if op == 1:
            self.next_lock += 1
            self.locks[self.next_lock] = threading.Lock()
            return self.next_lock
        if op == 2:
            return int(self.locks[a].acquire(timeout=b / 1000))
        if op == 3:
            self.locks[a].release()
            return 1
        if op == 4:
            del self.locks[a]
        elif op == 5:
            try:
                kind, length, idle = self.queues[a].get(timeout=d / 1000)
            except queue.Empty:
                return 0
            event = c.cast(b, c.POINTER(UartEvent)).contents
            event.type, event.size, event.timeout = kind, length, idle
            return 1
        elif op == 6:
            while True:
                try:
                    self.queues[a].get_nowait()
                except queue.Empty:
                    return 1
        elif op == 7:
            return time.monotonic_ns() // 1000
        elif op == 8:
            self.configs[a] = tuple((c.c_int * 6).from_address(b))
        elif op == 9:
            if self.fail_install == a:
                self.fail_install = None
                return -1
            if d:
                self.queues[a + 100] = queue.Queue()
                c.cast(d, c.POINTER(c.c_void_p))[0] = a + 100
        elif op == 11:
            self.pins[a] = (b, d, e)
        elif op == 17:
            self.rx[a] = b''
        elif op == 18:
            c.cast(b, c.POINTER(SIZE))[0] = len(self.rx.get(a, b''))
        elif op == 19:
            self.writes[a] = c.string_at(b, d)
            self.active[a] += 1
            self.peak[a] = max(self.peak[a], self.active[a])
            self.started[a].set()
            time.sleep(self.delays.get(a, 0))
            response = self.responses.get(a, b'')
            self.rx[a] = response
            if response:
                # A FIFO fragment followed by an idle event tests ADU assembly.
                self.queues[a + 100].put((0, 3, False))
                self.queues[a + 100].put((0, len(response) - 3, True))
            self.active[a] -= 1
            return d
        elif op == 21:
            data = self.rx.get(a, b'')[:d]
            c.memmove(b, data, len(data))
            self.rx[a] = self.rx.get(a, b'')[len(data):]
            return len(data)
        return 0


def adu(data):
    crc = 0xffff
    for value in data:
        crc ^= value
        for _ in range(8):
            crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return bytes(data) + crc.to_bytes(2, 'little')


def read(port, timeout=500):
    regs, exc = (U16 * 1)(0xffff), U8(0xff)
    result = LIB.modbus_read_registers(port, 1, 3, 0, 1, regs, timeout, c.byref(exc))
    return result, regs[0], exc.value


class RuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert LIB.rs485_init(0, 9600, 0, 1) == 0
        MOCK.fail_install = 2
        assert LIB.rs485_init(1, 19200, 2, 3) == -1
        assert LIB.rs485_init(1, 19200, 2, 3) == 0  # Failure only affects its own port.
        assert LIB.serial_port_init(115200, 0, 1) == 0

    def tearDown(self):
        self.assertEqual(MOCK.errors, [])

    def test_uart_allocation_and_independent_configuration(self):
        self.assertEqual(MOCK.pins, {0: (43, 44, -1), 1: (17, 18, 21), 2: (11, 12, 14)})
        self.assertEqual([MOCK.configs[p][0] for p in range(3)], [115200, 9600, 19200])
        self.assertEqual(MOCK.configs[2][2:4], (2, 3))
        self.assertEqual(len(MOCK.locks), 3)
        self.assertEqual(len(MOCK.queues), 2)
        self.assertEqual(LIB.rs485_init(0, 9600, 0, 1), 0x103)
        self.assertEqual(LIB.rs485_init(2, 9600, 0, 1), 0x102)
        self.assertEqual(read(-1), (0x102, 0xffff, 0))

    def test_same_slave_address_on_two_concurrent_buses(self):
        MOCK.configure(1, adu([1, 3, 2, 0x12, 0x34]), 0.04)
        MOCK.configure(2, adu([1, 3, 2, 0xab, 0xcd]), 0.04)
        with futures.ThreadPoolExecutor(2) as pool:
            a, b = pool.submit(read, 0), pool.submit(read, 1)
            self.assertEqual(a.result(), (0, 0x1234, 0))
            self.assertEqual(b.result(), (0, 0xabcd, 0))
        self.assertEqual(MOCK.writes[1], MOCK.writes[2])

    def test_timeout_on_first_bus_does_not_block_second(self):
        MOCK.configure(1)
        MOCK.configure(2, adu([1, 3, 2, 0, 99]))
        with futures.ThreadPoolExecutor(2) as pool:
            first = pool.submit(read, 0, 1000)
            self.assertTrue(MOCK.started[1].wait(1))
            self.assertEqual(pool.submit(read, 1).result(timeout=0.5), (0, 99, 0))
            self.assertFalse(first.done())
            self.assertEqual(first.result(), (0x107, 0xffff, 0))

    def test_same_bus_is_serialized(self):
        MOCK.configure(1, adu([1, 3, 2, 0, 7]), 0.05)
        with futures.ThreadPoolExecutor(2) as pool:
            results = list(pool.map(read, [0, 0]))
        self.assertEqual(results, [(0, 7, 0), (0, 7, 0)])
        self.assertEqual(MOCK.peak[1], 1)

    def test_both_buses_write_and_exception_paths(self):
        for port in (0, 1):
            for function in (6, 16):
                MOCK.configure(port + 1, adu([1, function, 0, 10, 0, 1]))
                exc, value = U8(), U16(1)
                if function == 6:
                    result = LIB.modbus_write_single(port, 1, 10, 1, 500, c.byref(exc))
                else:
                    result = LIB.modbus_write_multiple(port, 1, 10, 1, c.byref(value), 500, c.byref(exc))
                self.assertEqual(result, 0)
            MOCK.configure(port + 1, adu([1, 0x83, 2]))
            self.assertEqual(read(port), (0x7201, 0xffff, 2))

    def test_gpio41_stable_200ms_on_both_edges_no_long_event(self):
        state = Button()
        step = lambda pressed, ms: LIB.test_button_step(c.byref(state), pressed, ms, 0)
        self.assertEqual(step(True, 0), 0)
        self.assertEqual(step(True, 199), 0)
        self.assertEqual(step(True, 200), 1)
        self.assertTrue(state.stable)
        self.assertEqual(step(True, 5000), 0)
        step(False, 5010)
        step(True, 5100)  # Short release bounce must not change the state.
        self.assertTrue(state.stable)
        step(False, 5200)
        self.assertEqual(step(False, 5399), 0)
        self.assertEqual(step(False, 5400), 2)
        self.assertFalse(state.stable)

    def test_gpio40_hold_2s_once_and_rearm_after_release(self):
        state = Button()
        step = lambda pressed, ms: LIB.test_button_step(c.byref(state), pressed, ms, 1)
        step(True, 0)
        self.assertEqual(step(True, 30), 1)
        self.assertEqual(step(True, 2029), 0)
        self.assertEqual(step(True, 2030), 4)
        self.assertEqual(step(True, 10000), 0)
        step(False, 10010)
        self.assertEqual(step(False, 10040), 2)
        step(True, 10100)
        self.assertEqual(step(True, 10130), 1)
        self.assertEqual(step(True, 12130), 4)

    def test_touch_default_lock_and_held_finger_reenable(self):
        state, reset = Gate(True, False, False, False), c.c_bool()
        def sample(updated, count):
            return bool(LIB.test_gate_filter(c.byref(state), updated, count, c.byref(reset)))
        self.assertFalse(sample(True, 1))  # Enabled on boot.
        LIB.test_gate_set(c.byref(state), False)
        self.assertTrue(sample(False, 0))  # No new frame still cancels previous gesture.
        self.assertTrue(reset.value)
        self.assertTrue(sample(True, 1))
        LIB.test_gate_set(c.byref(state), True)
        self.assertTrue(sample(True, 1))  # Old held contact is not a new press.
        self.assertTrue(reset.value)
        self.assertFalse(sample(True, 0))
        self.assertFalse(sample(True, 1))
        LIB.test_gate_set(c.byref(state), False)
        self.assertTrue(sample(True, 0))
        LIB.test_gate_set(c.byref(state), True)
        self.assertFalse(sample(False, 0))  # Released while disabled: immediately ready.


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--msvc-bin', type=Path, default=Path(
        'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231/bin/Hostx64/x64'))
    parser.add_argument('--sdk-include', type=Path, default=Path(
        'C:/Program Files (x86)/Windows Kits/10/Include/10.0.26100.0'))
    args = parser.parse_args()
    build = ROOT / 'tests/build/runtime'
    build.mkdir(parents=True, exist_ok=True)
    stubs = ROOT / 'tests/host_stubs'
    sources = [ROOT / 'components/board_drivers' / (name + '.c')
               for name in ('rs485', 'modbus_rtu', 'modbus_codec', 'serial_port')]
    sources.append(stubs / 'mock_idf.c')
    subprocess.run([str(args.msvc_bin / 'cl.exe'), '/nologo', '/TC', '/std:c11',
        '/GS-', '/Zl', '/W4', '/WX', '/wd4244', '/wd4267', '/O2',
        '/I' + str(args.msvc_bin.parents[2] / 'include'),
        '/I' + str(args.sdk_include / 'ucrt'), '/I' + str(args.sdk_include / 'shared'),
        '/I' + str(stubs), '/I' + str(ROOT / 'components/board_drivers/include'),
        '/c', *map(str, sources)], cwd=build, check=True)
    exports = ['rs485_init', 'serial_port_init', 'modbus_read_registers',
               'modbus_write_single', 'modbus_write_multiple']
    dll = build / 'runtime.dll'
    subprocess.run([str(args.msvc_bin / 'link.exe'), '/nologo', '/dll', '/noentry',
        '/nodefaultlib', '/machine:x64', f'/out:{dll}', *['/export:' + s for s in exports],
        *[str(build / (p.stem + '.obj')) for p in sources]], check=True)
    LIB = c.CDLL(str(dll))
    MOCK = MockIDF()
    LIB.mock_set_hook.argtypes = [MOCK.CALLBACK]
    LIB.mock_set_hook(MOCK.callback)
    LIB.modbus_read_registers.argtypes = [c.c_int, U8, U8, U16, U16, c.POINTER(U16), c.c_uint32, c.POINTER(U8)]
    LIB.modbus_write_single.argtypes = [c.c_int, U8, U16, U16, c.c_uint32, c.POINTER(U8)]
    LIB.modbus_write_multiple.argtypes = [c.c_int, U8, U16, U16, c.POINTER(U16), c.c_uint32, c.POINTER(U8)]
    LIB.test_button_step.argtypes = [c.POINTER(Button), c.c_bool, c.c_int64, c.c_int]
    LIB.test_gate_set.argtypes = [c.POINTER(Gate), c.c_bool]
    LIB.test_gate_filter.argtypes = [c.POINTER(Gate), c.c_bool, U8, c.POINTER(c.c_bool)]
    LIB.test_gate_filter.restype = c.c_bool
    unittest.main(argv=['test_runtime'], verbosity=2)
