"""Threshold alarms, discrete alarms, TX control and automatic laser shutdown."""

import unittest

from .harness import DaemonTestCase


class ThresholdAlarmTest(DaemonTestCase):
    def test_temperature_alarm_with_hysteresis(self):
        # thresholds: high alarm 75 C, high warning 70 C, hysteresis 1 C
        self.ctl.set(0, "temp_c", 80)
        a = self.wait_alarm("p0", "temperature_high_alarm")
        self.assertEqual(a["severity"], "major")
        self.assertEqual(a["unit"], "C")
        self.assertAlmostEqual(a["threshold"], 75.0)
        # the alarm supersedes the warning on the same side
        self.assertIsNone(self.find_alarm("p0", "temperature_high_warning"))

        self.ctl.set(0, "temp_c", 74.5)          # inside the hysteresis band
        self.settle()
        self.assertIsNotNone(self.find_alarm("p0", "temperature_high_alarm"))

        self.ctl.set(0, "temp_c", 73.0)          # clears, warning takes over
        self.wait_alarm_cleared("p0", "temperature_high_alarm")
        self.wait_alarm("p0", "temperature_high_warning")

        self.ctl.set(0, "temp_c", 40.0)
        self.wait_alarm_cleared("p0", "temperature_high_warning")
        raised = [e for e in self.events("alarm_raised", "p0")]
        self.assertEqual([e["alarm"] for e in raised],
                         ["temperature_high_alarm", "temperature_high_warning"])

    def test_no_flapping_near_threshold(self):
        for t in (75.5, 74.6, 75.4, 74.2, 75.8, 74.4):
            self.ctl.set(0, "temp_c", t)
            self.settle(2)
        raised = self.events("alarm_raised", "p0")
        self.assertEqual(len([e for e in raised if e["alarm"] == "temperature_high_alarm"]), 1)
        self.assertEqual(self.events("alarm_cleared", "p0"), [])

    def test_low_vcc(self):
        self.ctl.set(0, "vcc_v", 2.9)
        a = self.wait_alarm("p0", "vcc_low_alarm")
        self.assertAlmostEqual(a["value"], 2.9, places=2)

    def test_rx_power_warning_then_alarm_per_lane(self):
        # Rx thresholds: low warning -8 dBm, low alarm -10 dBm
        self.ctl.set(1, "rx_power_dbm", -9, lane=5)
        w = self.wait_alarm("p1", "rx_power_low_warning", lane=5)
        self.assertEqual(w["severity"], "minor")
        self.assertEqual(w["unit"], "dBm")
        self.ctl.set(1, "rx_power_dbm", -12, lane=5)
        self.wait_alarm("p1", "rx_power_low_alarm", lane=5)
        self.wait_alarm_cleared("p1", "rx_power_low_warning", lane=5)
        others = [a for a in self.alarms("p1") if a.get("lane") != 5]
        self.assertEqual(others, [])

    def test_los_suppresses_low_rx_power(self):
        self.ctl.set(1, "rx_power_dbm", -30, lane=2)
        los = self.wait_alarm("p1", "rx_los", lane=2)
        self.assertEqual(los["severity"], "critical")
        self.settle()
        self.assertIsNone(self.find_alarm("p1", "rx_power_low_alarm", lane=2))
        self.ctl.set(1, "rx_power_dbm", -2, lane=2)
        self.wait_alarm_cleared("p1", "rx_los", lane=2)

    def test_sfp_los_from_status_bits(self):
        self.module(0).set_fault("rx_los")
        self.wait_alarm("p0", "rx_los", lane=0)
        self.module(0).set_fault("rx_los", on=False)
        self.wait_alarm_cleared("p0", "rx_los", lane=0)

    def test_tx_fault(self):
        self.module(1).set_fault("tx_fault", lane=1)
        a = self.wait_alarm("p1", "tx_fault", lane=1)
        self.assertEqual(a["severity"], "critical")
        self.module(1).set_fault("tx_fault", on=False, lane=1)
        self.wait_alarm_cleared("p1", "tx_fault", lane=1)

    def test_high_bias(self):
        self.ctl.set(1, "bias_ma", 72, lane=0)    # warning at 70, alarm at 75
        self.wait_alarm("p1", "tx_bias_high_warning", lane=0)


class TxControlTest(DaemonTestCase):
    def test_cmis_lane_tx_disable(self):
        self.d.ok("tx-disable p1 0-3 on")
        self.assertEqual(self.ctl.peek(1, 130, page=0x10)[0], 0x0F)
        self.settle()
        lanes = self.port("p1")["dom"]["lanes"]
        self.assertEqual([l["tx_disabled"] for l in lanes], [True] * 4 + [False] * 4)
        # dark lanes on purpose: no Tx power / bias alarms
        self.assertEqual([a for a in self.alarms("p1") if a["alarm"].startswith("tx_")], [])
        self.d.ok("tx-disable p1 all off")
        self.assertEqual(self.ctl.peek(1, 130, page=0x10)[0], 0x00)
        self.wait_event("tx_disable", "p1", disabled=False)

    def test_sfp_soft_tx_disable(self):
        self.d.ok("tx-disable p0 0 on")
        self.assertTrue(self.ctl.peek(0, 110, addr=0x51)[0] & 0x40)
        self.settle()
        self.assertTrue(self.port("p0")["dom"]["lanes"][0]["tx_disabled"])
        self.d.ok("tx-disable p0 0 off")
        self.assertFalse(self.ctl.peek(0, 110, addr=0x51)[0] & 0x40)


class AlsTest(DaemonTestCase):
    ALS = True

    def test_automatic_laser_shutdown_and_restore(self):
        self.ctl.set(1, "rx_power_dbm", -35, lane=4)
        self.wait_event("als_shutdown", "p1", lane=4)
        self.assertEqual(self.ctl.peek(1, 130, page=0x10)[0], 1 << 4)
        self.settle()
        lane = self.port("p1")["dom"]["lanes"][4]
        self.assertTrue(lane["als_shutdown"])
        self.assertTrue(lane["tx_disabled"])
        self.assertIsNone(self.find_alarm("p1", "tx_power_low_alarm", lane=4))

        self.ctl.set(1, "rx_power_dbm", -2, lane=4)
        self.wait_event("als_restore", "p1", lane=4)
        self.assertEqual(self.ctl.peek(1, 130, page=0x10)[0], 0)
        self.wait_alarm_cleared("p1", "rx_los", lane=4)

    def test_als_only_on_configured_port(self):
        self.module(0).set_fault("rx_los")
        self.wait_alarm("p0", "rx_los", lane=0)
        self.settle()
        self.assertEqual(self.ctl.peek(0, 110, addr=0x51)[0] & 0x40, 0)
        self.assertEqual(self.events("als_shutdown", "p0"), [])


if __name__ == "__main__":
    unittest.main()
