"""Discovery, hot plug, CMIS bring-up and module-level failures."""

import os
import signal
import subprocess
import unittest

from optisim import CmisModule, SfpModule

from .harness import CLI, DaemonTestCase, wait_until


class LifecycleTest(DaemonTestCase):
    def test_discovery(self):
        ports = {p["name"]: p for p in self.d.ok("ports")["ports"]}
        self.assertEqual(set(ports), {"p0", "p1", "p2", "p3"})
        self.assertEqual(ports["p0"]["state"], "ready")
        self.assertEqual(ports["p0"]["module"]["type"], "SFF-8472")
        self.assertEqual(ports["p0"]["module"]["serial"], "ITSFP0")
        self.assertEqual(ports["p1"]["module"]["type"], "CMIS")
        self.assertEqual(ports["p1"]["module"]["identifier_name"], "QSFP-DD")
        self.assertEqual(ports["p2"]["state"], "empty")
        self.assertNotIn("module", ports["p2"])

        p1 = self.port("p1")
        self.assertEqual(p1["module"]["lanes"], 8)
        self.assertEqual(len(p1["dom"]["lanes"]), 8)
        self.assertAlmostEqual(p1["dom"]["lanes"][0]["tx_power_dbm"], 1.0, places=1)
        self.assertEqual(p1["dom"]["module_state"], "ModuleReady")
        self.assertAlmostEqual(p1["thresholds"]["tx_bias"]["high_alarm"], 75.0, places=1)
        self.assertEqual(self.alarms(), [])

    def test_cmis_power_up_sequence(self):
        self.plug(2, "qsfpdd", serial="NEWQDD", pwrup_delay=0.4)
        e = self.wait_event("module_ready", "p2")
        self.assertGreaterEqual(e["power_up_ms"], 300)
        transitions = [(x["from"], x["to"]) for x in self.events("state_changed", "p2")]
        self.assertEqual(transitions, [("empty", "inserted"), ("inserted", "powering_up"),
                                       ("powering_up", "ready")])
        self.assertFalse(self.module(2).describe()["low_power_request"])

    def test_hot_unplug_clears_alarms(self):
        self.ctl.set(0, "temp_c", 85)
        self.wait_alarm("p0", "temperature_high_alarm")
        self.ctl.unplug(0)
        self.wait_state("p0", "empty")
        self.assertEqual(self.alarms("p0"), [])
        cleared = self.wait_event("alarm_cleared", "p0", alarm="temperature_high_alarm")
        self.assertEqual(cleared["reason"], "module_removed")
        self.wait_event("module_removed", "p0", serial="ITSFP0")

    def test_hot_swap_picks_up_new_module(self):
        self.ctl.unplug(0)
        self.wait_state("p0", "empty")
        self.plug(0, "sfp", serial="SWAPPED1")
        self.wait_state("p0", "ready")
        self.assertEqual(self.port("p0")["module"]["serial"], "SWAPPED1")
        self.wait_event("module_inserted", "p0", serial="SWAPPED1")

    def test_power_up_timeout_and_recovery(self):
        m = CmisModule(serial="STUCK1", pwrup_delay=0.1)
        m.set_fault("stuck_pwrup")
        self.install(3, m)
        a = self.wait_alarm("p3", "power_up_timeout", timeout=6)
        self.assertEqual(a["severity"], "major")
        self.assertIn("ModulePwrUp", a["message"])
        self.assertEqual(self.state("p3"), "fault")
        m.set_fault("stuck_pwrup", on=False)
        self.wait_state("p3", "ready", timeout=6)
        self.wait_alarm_cleared("p3", "power_up_timeout")

    def test_module_fault_state(self):
        self.module(1).set_fault("module_fault")
        a = self.wait_alarm("p1", "module_fault")
        self.assertEqual(a["severity"], "critical")
        self.wait_state("p1", "fault")
        self.module(1).set_fault("module_fault", on=False)
        self.wait_state("p1", "ready", timeout=6)
        self.wait_alarm_cleared("p1", "module_fault")

    def test_module_drops_to_low_power_is_brought_back(self):
        # Something else (or a module reset) puts the module back into LowPwr.
        self.module(1).write(0x50, 26, bytes([0x10]))
        self.wait_event("state_changed", "p1", to="powering_up", timeout=3)
        self.wait_state("p1", "ready")

    def test_unsupported_module(self):
        m = SfpModule(serial="QSFP28")
        m.a0[0] = 0x11            # QSFP28 (SFF-8636) is not managed
        self.install(3, m)
        a = self.wait_alarm("p3", "unsupported_module")
        self.assertIn("0x11", a["message"])
        self.assertEqual(self.state("p3"), "fault")
        self.ctl.unplug(3)
        self.wait_state("p3", "empty")
        self.wait_alarm_cleared("p3", "unsupported_module")

    def test_checksum_alarm(self):
        m = SfpModule(serial="BADCC")
        m.set_fault("corrupt_checksum")
        self.install(3, m)
        self.wait_state("p3", "ready")
        a = self.wait_alarm("p3", "eeprom_checksum")
        self.assertEqual(a["severity"], "minor")
        self.assertIn("base", a["message"])


class BusFailureTest(DaemonTestCase):
    def test_nack_raises_comm_failure_and_recovers(self):
        self.ctl.bus(0, nack=True)
        a = self.wait_alarm("p0", "i2c_comm_failure")
        self.assertEqual(a["severity"], "major")
        self.wait_state("p0", "fault")
        self.ctl.bus(0, nack=False)
        self.wait_state("p0", "ready", timeout=6)
        self.wait_alarm_cleared("p0", "i2c_comm_failure")

    def test_flaky_bus_is_absorbed_by_retries(self):
        self.ctl.bus(1, error_rate=0.2)
        self.settle(10)
        p1 = self.port("p1")
        self.assertEqual(p1["state"], "ready")
        self.assertGreater(p1["bus_stats"]["retries"], 0)
        self.assertIsNone(self.find_alarm("p1", "i2c_comm_failure"))

    def test_simulator_restart(self):
        # The sim: backend reconnects on its own after the server goes away.
        old = self.sim
        cages = list(old.chassis.cages)
        old.stop()
        from optisim.server import Simulator
        self.sim = Simulator(self.i2c_path, old.control_endpoint, ncages=4).start()
        self.sim.chassis.cages[:] = cages
        self.ctl.close()
        from optisim.server import SimClient
        self.ctl = SimClient(self.sim.control_endpoint)
        self.settle(5)
        self.wait_state("p0", "ready", timeout=6)
        self.wait_state("p1", "ready", timeout=6)


class ControlInterfaceTest(DaemonTestCase):
    def test_command_errors(self):
        for line in ("bogus", "port nope", "port", "tx-disable p1 9 on", "tx-disable p1 0 maybe",
                     "tx-disable p2 0 on", "events -1", "alarms nope", "show"):
            r = self.d.call(line)
            self.assertFalse(r["ok"], line)
            self.assertIn("error", r)

    def test_stats_and_events(self):
        s = self.d.ok("stats")
        self.assertEqual(s["ports"], 4)
        self.assertGreater(s["uptime_ms"], 0)
        ev = self.d.ok("events 5")["events"]
        self.assertLessEqual(len(ev), 5)
        self.assertEqual(ev[-1]["seq"], max(e["seq"] for e in ev))
        self.assertEqual(self.events("daemon_started")[0]["ports"], 4)

    def test_cli(self):
        env = dict(os.environ, OPTIWATCH_SOCKET=self.sock_path)
        out = subprocess.run([CLI, "show", "ports"], env=env, capture_output=True, text=True, check=True)
        self.assertIn("OW-QDD-400G-FR8", out.stdout)
        self.assertIn("p2      empty", out.stdout)
        out = subprocess.run([CLI, "port", "p0"], env=env, capture_output=True, text=True, check=True)
        self.assertIn('"serial": "ITSFP0"', out.stdout)
        bad = subprocess.run([CLI, "port", "nope"], env=env, capture_output=True, text=True)
        self.assertEqual(bad.returncode, 1)
        down = subprocess.run([CLI, "-s", "/nonexistent.sock", "ping"], capture_output=True, text=True)
        self.assertEqual(down.returncode, 1)
        self.assertIn("is optiwatchd running", down.stderr)

    def test_client_limit(self):
        import json
        import socket
        conns = []
        for _ in range(20):
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            s.connect(self.sock_path)
            s.settimeout(2)
            conns.append(s)
        # The daemon serves 16 clients; the rest get an error line and are closed.
        refused = 0
        for s in conns[16:]:
            data = s.recv(4096)
            if data:
                self.assertEqual(json.loads(data)["error"], "too many clients")
                refused += 1
        self.assertEqual(refused, 4)
        # Connected clients are still served.
        conns[0].sendall(b"ping\n")
        self.assertTrue(json.loads(conns[0].recv(4096))["ok"])
        for s in conns:
            s.close()
        wait_until(lambda: self.d.call("ping")["ok"], what="ping after client storm")

    def test_sigterm_and_sighup(self):
        os.rename(self.events_path, self.events_path + ".1")
        self.proc.send_signal(signal.SIGHUP)                  # log rotation
        wait_until(lambda: os.path.exists(self.events_path), what="event log reopened")
        self.ctl.set(0, "temp_c", 90)
        self.wait_event("alarm_raised", "p0", alarm="temperature_high_alarm")
        self.proc.send_signal(signal.SIGTERM)
        self.assertEqual(self.proc.wait(timeout=5), 0)
        self.assertEqual(self.events()[-1]["event"], "daemon_stopped")
        self.assertFalse(os.path.exists(self.sock_path))


if __name__ == "__main__":
    unittest.main()
