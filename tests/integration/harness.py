"""Test harness: runs optisim in-process and optiwatchd as a subprocess."""

import json
import os
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

from optisim.server import SimClient, Simulator

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BUILD = os.environ.get("OPTIWATCH_BUILD", os.path.join(ROOT, "build"))
DAEMON = os.path.join(BUILD, "optiwatchd")
CLI = os.path.join(BUILD, "optiwatch")

CONFIG = """
[daemon]
poll_interval_ms    = 100
fast_poll_ms        = 50
insert_settle_ms    = 50
power_up_timeout_ms = 1500
fault_retry_ms      = 500
max_poll_errors     = 3
hysteresis_temp_c   = 1.0
hysteresis_power_db = 0.5

[port p0]
bus = sim:{i2c}:0

[port p1]
bus = sim:{i2c}:1
als = {als}

[port p2]
bus = sim:{i2c}:2

[port p3]
bus = sim:{i2c}:3
"""


def wait_until(predicate, timeout=5.0, interval=0.05, what="condition"):
    deadline = time.monotonic() + timeout
    last = None
    while time.monotonic() < deadline:
        last = predicate()
        if last:
            return last
        time.sleep(interval)
    raise AssertionError("timed out after %.1fs waiting for %s (last: %r)" % (timeout, what, last))


class DaemonClient:
    def __init__(self, path):
        self.path = path

    def call(self, line):
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect(self.path)
        try:
            s.sendall((line + "\n").encode())
            buf = b""
            while not buf.endswith(b"\n"):
                chunk = s.recv(65536)
                if not chunk:
                    break
                buf += chunk
        finally:
            s.close()
        return json.loads(buf)

    def ok(self, line):
        r = self.call(line)
        if not r.get("ok"):
            raise AssertionError("%s -> %s" % (line, r))
        return r


class DaemonTestCase(unittest.TestCase):
    """Fresh simulator + daemon per test.

    Cages: 0 = SFP+, 1 = QSFP-DD (ALS enabled when ALS = True), 2 and 3 empty.
    """

    ALS = False
    CAGES = ("sfp", "qsfpdd", None, None)

    def setUp(self):
        if not os.path.exists(DAEMON):
            self.skipTest("optiwatchd not built (run make)")
        self.dir = tempfile.mkdtemp(prefix="optiwatch-it-")
        self.i2c_path = os.path.join(self.dir, "i2c.sock")
        self.sock_path = os.path.join(self.dir, "optiwatchd.sock")
        self.events_path = os.path.join(self.dir, "events.jsonl")
        self.sim = Simulator(self.i2c_path, os.path.join(self.dir, "ctl.sock"), ncages=4).start()
        self.ctl = SimClient(self.sim.control_endpoint)
        for i, kind in enumerate(self.CAGES):
            if kind:
                self.plug(i, kind, serial="IT%s%d" % (kind.upper()[:3], i))

        cfg = os.path.join(self.dir, "optiwatchd.conf")
        with open(cfg, "w") as f:
            f.write(CONFIG.format(i2c=self.i2c_path, als="true" if self.ALS else "false"))
        self.log = open(os.path.join(self.dir, "daemon.log"), "w")
        self.proc = subprocess.Popen(
            [DAEMON, "-c", cfg, "-s", self.sock_path, "-e", self.events_path, "-v"],
            stdout=self.log, stderr=subprocess.STDOUT)
        wait_until(lambda: os.path.exists(self.sock_path), what="control socket")
        self.d = DaemonClient(self.sock_path)
        for i, kind in enumerate(self.CAGES):
            if kind:
                self.wait_state("p%d" % i, "ready")

    def tearDown(self):
        if getattr(self, "proc", None) and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if getattr(self, "log", None):
            self.log.close()
            if self._outcome_failed():
                with open(self.log.name) as f:
                    print("\n--- optiwatchd log ---\n" + f.read()[-4000:])
        if getattr(self, "ctl", None):
            self.ctl.close()
        if getattr(self, "sim", None):
            self.sim.stop()
        shutil.rmtree(getattr(self, "dir", ""), ignore_errors=True)

    def _outcome_failed(self):
        outcome = getattr(self, "_outcome", None)
        if outcome is None:
            return False
        errors = getattr(outcome, "errors", None)  # Python < 3.11
        if errors is not None:
            return any(exc for _, exc in errors)
        result = getattr(outcome, "result", None)
        return bool(result and (result.errors or result.failures))

    # --- simulator helpers ------------------------------------------------
    def plug(self, cage, kind, **opts):
        if kind == "qsfpdd":
            opts.setdefault("pwrup_delay", 0.2)
        return self.ctl.plug(cage, kind, **opts)

    def module(self, cage):
        return self.sim.chassis.cages[cage]

    def install(self, cage, mod):
        """Seat a pre-built (possibly pre-faulted) module object."""
        with self.sim.chassis.lock:
            self.sim.chassis.cages[cage] = mod

    # --- daemon helpers ---------------------------------------------------
    def port(self, name):
        return self.d.ok("port " + name)["port"]

    def state(self, name):
        return self.port(name)["state"]

    def wait_state(self, name, state, timeout=5.0):
        return wait_until(lambda: self.state(name) == state, timeout, what="%s %s" % (name, state))

    def alarms(self, port=None):
        return self.d.ok("alarms" + (" " + port if port else ""))["alarms"]

    def find_alarm(self, port, alarm, lane=None):
        for a in self.alarms(port):
            if a["alarm"] == alarm and (lane is None or a.get("lane") == lane):
                return a
        return None

    def wait_alarm(self, port, alarm, lane=None, timeout=5.0):
        return wait_until(lambda: self.find_alarm(port, alarm, lane), timeout,
                          what="alarm %s/%s lane %s" % (port, alarm, lane))

    def wait_alarm_cleared(self, port, alarm, lane=None, timeout=5.0):
        wait_until(lambda: self.find_alarm(port, alarm, lane) is None, timeout,
                   what="alarm %s/%s cleared" % (port, alarm))

    def events(self, event=None, port=None):
        out = []
        with open(self.events_path) as f:
            for line in f:
                e = json.loads(line)
                if (event is None or e["event"] == event) and (port is None or e["port"] == port):
                    out.append(e)
        return out

    def wait_event(self, event, port=None, timeout=5.0, **match):
        def find():
            for e in self.events(event, port):
                if all(e.get(k) == v for k, v in match.items()):
                    return e
            return None
        return wait_until(find, timeout, what="event %s %s" % (event, match))

    def settle(self, polls=3):
        time.sleep(0.1 * polls + 0.05)
