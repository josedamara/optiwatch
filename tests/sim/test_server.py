"""Tests for the simulator's socket protocols."""

import os
import socket
import tempfile
import unittest

from optisim.server import SimClient, Simulator


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="optisim-")
        self.i2c = os.path.join(self.dir, "i2c.sock")
        self.ctl = os.path.join(self.dir, "ctl.sock")
        self.sim = Simulator(self.i2c, self.ctl, ncages=2).start()
        self.client = SimClient(self.ctl)
        self.raw = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.raw.settimeout(2)
        self.raw.connect(self.i2c)

    def tearDown(self):
        self.raw.close()
        self.client.close()
        self.sim.stop()
        os.rmdir(self.dir)

    def xfer(self, op, cage, addr, off, n, data=b""):
        self.raw.sendall(bytes([ord(op), cage, addr, off, n]) + data)
        status = self.raw.recv(1)[0]
        if status or op == "W":
            return status, b""
        want = 1 if op == "P" else n
        buf = b""
        while len(buf) < want:
            buf += self.raw.recv(want - len(buf))
        return status, buf

    def test_presence_and_hotplug(self):
        self.assertEqual(self.xfer("P", 0, 0, 0, 0), (0, b"\x00"))
        self.client.plug(0, "sfp")
        self.assertEqual(self.xfer("P", 0, 0, 0, 0), (0, b"\x01"))
        self.client.unplug(0)
        self.assertEqual(self.xfer("P", 0, 0, 0, 0), (0, b"\x00"))

    def test_read_write(self):
        self.client.plug(1, "sfp", serial="SRV1")
        status, data = self.xfer("R", 1, 0x50, 68, 4)
        self.assertEqual((status, data), (0, b"SRV1"))
        status, _ = self.xfer("W", 1, 0x51, 110, 1, b"\x40")
        self.assertEqual(status, 0)
        self.assertEqual(self.client.peek(1, 110, addr=0x51)[0] & 0x40, 0x40)

    def test_empty_cage_nacks(self):
        status, _ = self.xfer("R", 0, 0x50, 0, 1)
        self.assertEqual(status, 6)   # ENXIO

    def test_bus_fault_injection(self):
        self.client.plug(0, "sfp")
        self.client.bus(0, nack=True)
        self.assertEqual(self.xfer("R", 0, 0x50, 0, 1)[0], 6)
        self.client.bus(0, nack=False, error_rate=1.0)
        self.assertEqual(self.xfer("R", 0, 0x50, 0, 1)[0], 5)   # EIO

    def test_control_errors(self):
        with self.assertRaises(RuntimeError):
            self.client.set(0, "temp_c", 1)       # empty cage
        self.client.plug(0, "qsfpdd")
        with self.assertRaises(RuntimeError):
            self.client.fault(0, "bogus")
        with self.assertRaises(RuntimeError):
            self.client.plug(9, "sfp")            # no such cage
        status = self.client.status()
        self.assertEqual(status[0]["kind"], "qsfpdd")
        self.assertIsNone(status[1])


if __name__ == "__main__":
    unittest.main()
