"""End-to-end tests through the real Linux I2C stack.

    optiwatchd --i2c-dev--> /dev/i2c-N --> xcvr_i2c_emu.ko <--debugfs-- kbridge <-- optisim models

Needs root, the xcvr_i2c_emu module loaded with nr_cages >= 3 and i2c-dev.
Skipped unless OPTIWATCH_KERNEL=1 (CI sets it in the kernel job).
"""

import os
import signal
import subprocess
import tempfile
import shutil
import time
import unittest

from optisim.kbridge import DEBUGFS, KernelBridge, find_adapters
from optisim.server import SimClient, Simulator

from tests.integration.harness import BUILD, DAEMON, DaemonClient, wait_until

ENABLED = os.environ.get("OPTIWATCH_KERNEL") == "1" and os.path.isdir(DEBUGFS)

CONFIG = """
[daemon]
poll_interval_ms    = 200
fast_poll_ms        = 50
insert_settle_ms    = 100
power_up_timeout_ms = 3000
fault_retry_ms      = 500
"""


@unittest.skipUnless(ENABLED, "kernel emulator not loaded (set OPTIWATCH_KERNEL=1 as root)")
class KernelPathTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="optiwatch-k-")
        self.sim = Simulator(None, os.path.join(self.dir, "ctl.sock"), ncages=3).start()
        self.ctl = SimClient(self.sim.control_endpoint)
        self.ctl.plug(0, "sfp", serial="KSFP0")
        self.ctl.plug(1, "qsfpdd", serial="KQDD1", pwrup_delay=0.3)
        self.bridge = KernelBridge(self.sim.chassis, interval=0.05).start()
        self.devs = find_adapters(3)
        self.assertEqual(sorted(self.devs), [0, 1, 2], "expected 3 xcvr-emu adapters")

        cfg = os.path.join(self.dir, "k.conf")
        with open(cfg, "w") as f:
            f.write(CONFIG)
            for i in range(3):
                f.write("\n[port k%d]\nbus = i2c:%s\n" % (i, self.devs[i]))
        self.sock = os.path.join(self.dir, "d.sock")
        self.events = os.path.join(self.dir, "events.jsonl")
        self.log = open(os.path.join(self.dir, "daemon.log"), "w")
        self.proc = subprocess.Popen([DAEMON, "-c", cfg, "-s", self.sock, "-e", self.events, "-v"],
                                     stdout=self.log, stderr=subprocess.STDOUT)
        wait_until(lambda: os.path.exists(self.sock), what="control socket")
        self.d = DaemonClient(self.sock)

    def tearDown(self):
        self.proc.send_signal(signal.SIGTERM)
        self.proc.wait(timeout=5)
        self.log.close()
        self.bridge.stop()
        self.ctl.close()
        self.sim.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def port(self, name):
        return self.d.ok("port " + name)["port"]

    def wait_state(self, name, state, timeout=8):
        wait_until(lambda: self.port(name)["state"] == state, timeout, what="%s %s" % (name, state))

    def alarm(self, port, name, lane=None):
        for a in self.d.ok("alarms " + port)["alarms"]:
            if a["alarm"] == name and (lane is None or a.get("lane") == lane):
                return a
        return None

    def test_discovery_over_i2c_dev(self):
        self.wait_state("k0", "ready")
        self.wait_state("k1", "ready")
        self.assertEqual(self.port("k0")["module"]["serial"], "KSFP0")
        self.assertEqual(self.port("k1")["module"]["serial"], "KQDD1")
        self.assertEqual(self.port("k1")["dom"]["module_state"], "ModuleReady")
        self.assertEqual(self.port("k2")["state"], "empty")

    def test_xcvrdump_on_kernel_adapter(self):
        out = subprocess.run([os.path.join(BUILD, "xcvrdump"), "i2c:" + self.devs[0]],
                             capture_output=True, text=True, check=True).stdout
        self.assertIn("OPTIWATCH LABS", out)
        self.assertIn("Checksums       : base ok, ext ok, dmi ok", out)

    def test_alarm_through_kernel(self):
        self.wait_state("k0", "ready")
        self.ctl.set(0, "temp_c", 82)
        wait_until(lambda: self.alarm("k0", "temperature_high_alarm"), 5, what="temp alarm")
        self.ctl.set(1, "rx_power_dbm", -30, lane=6)
        self.wait_state("k1", "ready")
        wait_until(lambda: self.alarm("k1", "rx_los", lane=6), 5, what="LOS lane 6")

    def test_host_writes_reach_the_model(self):
        self.wait_state("k1", "ready")
        self.d.ok("tx-disable k1 2 on")
        wait_until(lambda: self.sim.chassis.cages[1].tx_disable_mask() == 0x04, 3,
                   what="model sees lane 2 disabled")
        self.d.ok("tx-disable k0 0 on")
        wait_until(lambda: self.sim.chassis.cages[0].effective()["tx_disabled"], 3,
                   what="SFP soft TX disable in model")

    def test_unplug_and_replug(self):
        self.wait_state("k0", "ready")
        self.ctl.unplug(0)
        self.wait_state("k0", "empty")
        self.ctl.plug(2, "sfp", serial="KSFP2")
        self.wait_state("k2", "ready")
        self.assertEqual(self.port("k2")["module"]["serial"], "KSFP2")

    def test_kernel_bus_errors_are_retried(self):
        self.wait_state("k0", "ready")
        self.ctl.bus(0, error_rate=0.25)       # kernel fails every 4th transfer
        time.sleep(2)
        p = self.port("k0")
        self.assertEqual(p["state"], "ready")
        self.assertGreater(p["bus_stats"]["retries"], 0)


if __name__ == "__main__":
    unittest.main()
