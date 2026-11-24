"""Tests for the simulator's module models (no sockets involved)."""

import struct
import time
import unittest

from optisim import CmisModule, I2CError, SfpModule
from optisim.common import checksum8, dbm_to_mw


def u16(b, off):
    return struct.unpack_from(">H", b, off)[0]


def s16(b, off):
    return struct.unpack_from(">h", b, off)[0]


class SfpModelTest(unittest.TestCase):
    def setUp(self):
        self.m = SfpModule(serial="TESTSN1")

    def test_serial_id_and_checksums(self):
        a0 = self.m.read(0x50, 0, 128)
        self.assertEqual(a0[0], 0x03)
        self.assertEqual(a0[68:75], b"TESTSN1")
        self.assertEqual(a0[63], checksum8(a0[0:63]))
        self.assertEqual(a0[95], checksum8(a0[64:95]))
        a2 = self.m.read(0x51, 0, 96)
        self.assertEqual(a2[95], checksum8(a2[0:95]))

    def test_live_values(self):
        self.m.set_value("temp_c", 41.5)
        self.m.set_value("rx_power_dbm", -10.0)
        a2 = self.m.read(0x51, 96, 10)
        self.assertAlmostEqual(s16(a2, 0) / 256.0, 41.5, places=2)
        self.assertAlmostEqual(u16(a2, 8) * 1e-4, dbm_to_mw(-10.0), places=4)

    def test_alarm_flags_follow_thresholds(self):
        self.m.set_value("temp_c", 80)          # above 75 C high alarm
        a2 = self.m.read(0x51, 112, 6)
        self.assertTrue(a2[0] & 0x80)           # temp high alarm
        self.assertTrue(a2[4] & 0x80)           # temp high warning
        self.m.set_value("temp_c", 30)
        a2 = self.m.read(0x51, 112, 6)
        self.assertEqual(a2[0] & 0x80, 0)

    def test_soft_tx_disable(self):
        self.m.write(0x51, 110, bytes([0x40]))
        sc = self.m.read(0x51, 110, 1)[0]
        self.assertTrue(sc & 0x40)
        self.assertTrue(sc & 0x80)              # TX_DISABLE state reflects it
        self.assertEqual(u16(self.m.read(0x51, 102, 2), 0), 0)
        self.m.write(0x51, 110, bytes([0x00]))
        self.assertGreater(u16(self.m.read(0x51, 102, 2), 0), 0)

    def test_serial_id_is_read_only(self):
        self.m.write(0x50, 20, b"EVIL")
        self.assertEqual(self.m.read(0x50, 20, 4), b"OPTI")

    def test_faults(self):
        self.m.set_fault("rx_los")
        sc = self.m.read(0x51, 110, 1)[0]
        self.assertTrue(sc & 0x02)
        self.m.set_fault("rx_los", on=False)
        self.m.set_fault("tx_fault")
        self.assertTrue(self.m.read(0x51, 110, 1)[0] & 0x04)
        self.m.set_fault("corrupt_checksum")
        a0 = self.m.read(0x50, 0, 64)
        self.assertNotEqual(a0[63], checksum8(a0[0:63]))
        with self.assertRaises(ValueError):
            self.m.set_fault("no_such_fault")

    def test_bus_faults(self):
        self.m.nack = True
        with self.assertRaises(I2CError):
            self.m.read(0x50, 0, 1)
        self.m.nack = False
        self.m.error_rate = 1.0
        with self.assertRaises(I2CError):
            self.m.read(0x50, 0, 1)

    def test_external_calibration_raw_values(self):
        m = SfpModule(ext_cal=True)
        m.set_value("bias_ma", 8.0)
        a2 = m.read(0x51, 0, 128)
        # calibrated = 2.0 * raw  ->  raw = 8 mA / 2 uA / 2
        self.assertEqual(u16(a2, 100), 2000)
        self.assertEqual(struct.unpack_from(">H", a2, 76)[0], 0x0200)


class CmisModelTest(unittest.TestCase):
    def setUp(self):
        self.m = CmisModule(pwrup_delay=0.05)

    def state(self):
        return (self.m.read(0x50, 3, 1)[0] >> 1) & 7

    def select(self, page):
        self.m.write(0x50, 127, bytes([page]))

    def power_up(self):
        self.m.write(0x50, 26, bytes([0x00]))
        deadline = time.monotonic() + 2
        while self.state() != 3 and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertEqual(self.state(), 3)

    def test_starts_in_low_power(self):
        self.assertEqual(self.state(), 1)
        self.select(0x11)
        self.assertEqual(u16(self.m.read(0x50, 154, 2), 0), 0)   # lanes dark

    def test_power_up_sequence(self):
        self.m.write(0x50, 26, bytes([0x00]))
        self.assertEqual(self.state(), 2)     # ModulePwrUp
        self.power_up()
        self.select(0x11)
        tx = [u16(self.m.read(0x50, 154 + 2 * l, 2), 0) * 1e-4 for l in range(8)]
        for v in tx:
            self.assertAlmostEqual(v, dbm_to_mw(1.0), places=3)

    def test_stuck_in_pwrup(self):
        self.m.set_fault("stuck_pwrup")
        self.m.write(0x50, 26, bytes([0x00]))
        time.sleep(0.1)
        self.assertEqual(self.state(), 2)

    def test_module_fault_state(self):
        self.power_up()
        self.m.set_fault("module_fault")
        self.assertEqual(self.state(), 5)

    def test_page_select(self):
        self.select(0x02)
        self.assertEqual(self.m.read(0x50, 127, 1)[0], 0x02)
        self.select(0x55)                     # not implemented: ignored
        self.assertEqual(self.m.read(0x50, 127, 1)[0], 0x02)

    def test_latched_flags_clear_on_read(self):
        self.power_up()
        self.m.set_value("rx_power_dbm", -30, lane=4)
        self.select(0x11)
        flags = self.m.read(0x50, 147, 6)
        self.assertTrue(flags[0] & (1 << 4))  # Rx LOS lane 4
        self.assertTrue(flags[3] & (1 << 4))  # Rx power low alarm lane 4 (byte 150)
        # condition persists: flag re-latches on the next update
        self.assertTrue(self.m.read(0x50, 147, 1)[0] & (1 << 4))
        self.m.set_value("rx_power_dbm", -2, lane=4)
        self.m.read(0x50, 147, 1)             # clears the last latched value
        self.assertEqual(self.m.read(0x50, 147, 1)[0], 0)

    def test_module_state_changed_flag(self):
        self.m.read(0x50, 8, 1)               # clear
        self.m.write(0x50, 26, bytes([0x00]))
        self.assertTrue(self.m.read(0x50, 8, 1)[0] & 1)
        self.assertEqual(self.m.read(0x50, 8, 1)[0] & 1, 0)

    def test_lane_tx_disable(self):
        self.power_up()
        self.select(0x10)
        self.m.write(0x50, 130, bytes([0x81]))
        self.select(0x11)
        self.assertEqual(u16(self.m.read(0x50, 154, 2), 0), 0)        # lane 0 dark
        self.assertGreater(u16(self.m.read(0x50, 156, 2), 0), 0)      # lane 1 lit
        self.assertEqual(u16(self.m.read(0x50, 154 + 14, 2), 0), 0)   # lane 7 dark

    def test_thresholds_page(self):
        self.select(0x02)
        p2 = self.m.read(0x50, 128, 72)
        self.assertEqual(s16(p2, 0) / 256.0, 75.0)                   # temp high alarm
        self.assertAlmostEqual(u16(p2, 56) * 2e-3 * 2, 75.0, places=2)  # bias HA, x2 scale


if __name__ == "__main__":
    unittest.main()
