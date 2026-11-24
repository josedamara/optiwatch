"""Simulator server: a chassis of cages exposed over two sockets.

I2C socket (binary, used by optiwatchd's sim: bus backend)
    request   op port addr offset len [data]
    response  status [data]
    op is b'R' (read), b'W' (write) or b'P' (presence / MOD_ABS).

Control socket (JSON lines, used by tests and optisim-inject)
    {"cmd": "status"}
    {"cmd": "plug", "cage": 0, "kind": "sfp" | "qsfpdd", ...model options}
    {"cmd": "unplug", "cage": 0}
    {"cmd": "set", "cage": 0, "field": "rx_power_dbm", "value": -25, "lane": 3}
    {"cmd": "fault", "cage": 0, "name": "tx_fault", "on": true, "lane": 1}
    {"cmd": "bus", "cage": 0, "nack": true, "error_rate": 0.2, "delay_ms": 5}
    {"cmd": "peek", "cage": 0, "addr": 81, "offset": 110, "len": 1, "page": 16}
    Every reply is {"ok": true, ...} or {"ok": false, "error": "..."}.
"""

import json
import os
import socket
import socketserver
import threading

from . import make_module
from .common import ENXIO, EIO, I2CError


class Chassis:
    """A set of cages; each holds a module model or nothing."""

    def __init__(self, ncages):
        self.lock = threading.RLock()
        self.cages = [None] * ncages
        self.plug_events = 0

    def module(self, cage):
        with self.lock:
            if not 0 <= cage < len(self.cages):
                raise ValueError("cage %d out of range (0..%d)" % (cage, len(self.cages) - 1))
            return self.cages[cage]

    def plug(self, cage, kind, **opts):
        with self.lock:
            self.module(cage)
            self.cages[cage] = make_module(kind, **opts)
            self.plug_events += 1
            return self.cages[cage]

    def unplug(self, cage):
        with self.lock:
            self.module(cage)
            self.cages[cage] = None
            self.plug_events += 1

    # I2C side --------------------------------------------------------------
    def i2c(self, op, cage, addr, off, data_or_len):
        try:
            mod = self.module(cage)
        except ValueError:
            raise I2CError(ENXIO)
        if op == "P":
            return bytes([1 if mod is not None else 0])
        if mod is None:
            raise I2CError(ENXIO)
        if op == "R":
            return mod.read(addr, off, data_or_len)
        mod.write(addr, off, data_or_len)
        return b""


def _recv_exact(sock, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None
        buf += chunk
    return bytes(buf)


class I2CHandler(socketserver.BaseRequestHandler):
    def handle(self):
        chassis = self.server.chassis
        sock = self.request
        while True:
            hdr = _recv_exact(sock, 5)
            if hdr is None:
                return
            op, cage, addr, off, n = chr(hdr[0]), hdr[1], hdr[2], hdr[3], hdr[4]
            payload = b""
            if op == "W":
                payload = _recv_exact(sock, n)
                if payload is None:
                    return
            try:
                if op not in "RWP":
                    raise I2CError(EIO)
                data = chassis.i2c(op, cage, addr, off, payload if op == "W" else n)
                sock.sendall(b"\x00" + data)
            except I2CError as e:
                sock.sendall(bytes([e.code & 0xFF]))


class ControlHandler(socketserver.StreamRequestHandler):
    def handle(self):
        for line in self.rfile:
            line = line.strip()
            if not line:
                continue
            try:
                req = json.loads(line)
                reply = dispatch(self.server.chassis, req)
                reply.setdefault("ok", True)
            except Exception as e:  # report any error back to the client
                reply = {"ok": False, "error": "%s: %s" % (type(e).__name__, e)}
            self.wfile.write((json.dumps(reply) + "\n").encode())
            self.wfile.flush()


def dispatch(chassis, req):
    cmd = req.get("cmd")
    cage = int(req.get("cage", 0))
    if cmd == "status":
        with chassis.lock:
            return {"cages": [None if m is None else m.describe() for m in chassis.cages]}
    if cmd == "plug":
        opts = {k: v for k, v in req.items() if k not in ("cmd", "cage", "kind")}
        mod = chassis.plug(cage, req.get("kind", "sfp"), **opts)
        return {"module": mod.describe()}
    if cmd == "unplug":
        chassis.unplug(cage)
        return {}

    mod = chassis.module(cage)
    if mod is None:
        raise ValueError("cage %d is empty" % cage)
    if cmd == "set":
        mod.set_value(req["field"], req["value"], req.get("lane"))
    elif cmd == "fault":
        mod.set_fault(req["name"], bool(req.get("on", True)), req.get("lane"))
    elif cmd == "bus":
        with mod.lock:
            if "nack" in req:
                mod.nack = bool(req["nack"])
            if "error_rate" in req:
                mod.error_rate = float(req["error_rate"])
            if "delay_ms" in req:
                mod.delay_ms = float(req["delay_ms"])
            if "noise" in req:
                mod.noise = float(req["noise"])
    elif cmd == "peek":
        # Read the map without disturbing it the way a host read would (no
        # clear-on-read): used by tests to check what the daemon wrote.
        with mod.lock:
            addr, off, n = int(req.get("addr", 0x50)), int(req["offset"]), int(req.get("len", 1))
            if mod.kind == "sfp":
                mem = mod.a0 if addr == 0x50 else mod.a2
                data = bytes(mem[off:off + n])
            else:
                page = int(req.get("page", 0))
                data = bytes(mod.lower[o] if o < 128 else mod.pages[page][o - 128]
                             for o in range(off, off + n))
        return {"data": list(data)}
    elif cmd == "describe":
        return {"module": mod.describe()}
    else:
        raise ValueError("unknown command %r" % cmd)
    return {"module": mod.describe()}


class _ThreadingUnixServer(socketserver.ThreadingMixIn, socketserver.UnixStreamServer):
    daemon_threads = True


class _ThreadingTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def _make_server(endpoint, handler, chassis):
    """endpoint is a filesystem path (unix socket) or 'host:port' (TCP)."""
    if "/" in endpoint or endpoint.endswith(".sock"):
        if os.path.exists(endpoint):
            os.unlink(endpoint)
        srv = _ThreadingUnixServer(endpoint, handler)
    else:
        host, port = endpoint.rsplit(":", 1)
        srv = _ThreadingTCPServer((host, int(port)), handler)
    srv.chassis = chassis
    return srv


class Simulator:
    """Runs the I2C and control servers on background threads."""

    def __init__(self, i2c_endpoint, control_endpoint, ncages=4):
        """i2c_endpoint may be None when something else (the kernel
        bridge) exposes the modules to the host."""
        self.chassis = Chassis(ncages)
        self.i2c_endpoint = i2c_endpoint
        self.control_endpoint = control_endpoint
        self._servers = [_make_server(control_endpoint, ControlHandler, self.chassis)]
        if i2c_endpoint:
            self._servers.append(_make_server(i2c_endpoint, I2CHandler, self.chassis))
        self._threads = []

    def start(self):
        for srv in self._servers:
            t = threading.Thread(target=srv.serve_forever, daemon=True)
            t.start()
            self._threads.append(t)
        return self

    def stop(self):
        for srv in self._servers:
            srv.shutdown()
            srv.server_close()
        for ep in (self.i2c_endpoint, self.control_endpoint):
            if ep and "/" in ep and os.path.exists(ep):
                os.unlink(ep)


class SimClient:
    """Client for the control socket."""

    def __init__(self, endpoint, timeout=5.0):
        if "/" in endpoint or endpoint.endswith(".sock"):
            self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self.sock.settimeout(timeout)
            self.sock.connect(endpoint)
        else:
            host, port = endpoint.rsplit(":", 1)
            self.sock = socket.create_connection((host, int(port)), timeout=timeout)
        self.rfile = self.sock.makefile("rb")

    def call(self, cmd, **kw):
        kw["cmd"] = cmd
        self.sock.sendall((json.dumps(kw) + "\n").encode())
        reply = json.loads(self.rfile.readline())
        if not reply.get("ok"):
            raise RuntimeError(reply.get("error", "simulator error"))
        return reply

    # convenience wrappers
    def plug(self, cage, kind="sfp", **opts):
        return self.call("plug", cage=cage, kind=kind, **opts)

    def unplug(self, cage):
        return self.call("unplug", cage=cage)

    def set(self, cage, field, value, lane=None):
        return self.call("set", cage=cage, field=field, value=value, lane=lane)

    def fault(self, cage, name, on=True, lane=None):
        return self.call("fault", cage=cage, name=name, on=on, lane=lane)

    def bus(self, cage, **kw):
        return self.call("bus", cage=cage, **kw)

    def peek(self, cage, offset, length=1, addr=0x50, page=0):
        return self.call("peek", cage=cage, offset=offset, len=length, addr=addr, page=page)["data"]

    def status(self):
        return self.call("status")["cages"]

    def close(self):
        self.rfile.close()
        self.sock.close()
