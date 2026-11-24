"""Shared encoding helpers and the module base class."""

import errno
import math
import random
import struct
import threading
import time

ENXIO = errno.ENXIO  # address NACK: module absent or not answering
EIO = errno.EIO      # transfer error


class I2CError(Exception):
    """Raised by a model to make the bus transaction fail with an errno."""

    def __init__(self, code):
        super().__init__(errno.errorcode.get(code, str(code)))
        self.code = code


# --- unit encoders ---------------------------------------------------------

def clamp_u16(v):
    return max(0, min(0xFFFF, int(round(v))))


def clamp_s16(v):
    return max(-0x8000, min(0x7FFF, int(round(v))))


def put_u16(buf, off, v):
    struct.pack_into(">H", buf, off, clamp_u16(v))


def put_s16(buf, off, v):
    struct.pack_into(">h", buf, off, clamp_s16(v))


def put_ascii(buf, off, text, width):
    raw = text.encode("ascii")[:width].ljust(width, b" ")
    buf[off:off + width] = raw


def checksum8(data):
    return sum(data) & 0xFF


def dbm_to_mw(dbm):
    return 10.0 ** (dbm / 10.0)


def mw_to_dbm(mw):
    return -40.0 if mw <= 0 else max(-40.0, 10.0 * math.log10(mw))


def temp_raw(c):
    return c * 256.0


def vcc_raw(v):
    return v / 100e-6


def bias_raw(ma, multiplier=1):
    return ma / (0.002 * multiplier)


def power_raw(mw):
    return mw / 1e-4


# --- module base -----------------------------------------------------------

class BusModel:
    """Base class for an emulated module behind one cage.

    Subclasses implement _read(addr, off, n) and _write(addr, off, data) on
    their memory map. This class adds bus-level fault injection that applies
    to every transaction, and transaction counters.
    """

    kind = "base"

    def __init__(self, seed=0):
        self.lock = threading.RLock()
        self.faults = {}          # name -> value (True or lane set)
        self.nack = False         # every transaction NACKs (hung / dead module)
        self.error_rate = 0.0     # probability of EIO per transaction
        self.delay_ms = 0.0       # added latency per transaction
        self.noise = 0.0          # +/- jitter on analog values, fraction
        self.rng = random.Random(seed)
        self.inserted_at = time.monotonic()
        self.stats = {"reads": 0, "writes": 0, "errors": 0}

    # bus entry points ------------------------------------------------------
    def _bus_check(self):
        if self.delay_ms:
            time.sleep(self.delay_ms / 1000.0)
        if self.nack:
            self.stats["errors"] += 1
            raise I2CError(ENXIO)
        if self.error_rate and self.rng.random() < self.error_rate:
            self.stats["errors"] += 1
            raise I2CError(EIO)

    def read(self, addr, off, n):
        with self.lock:
            self._bus_check()
            if off + n > 256:
                raise I2CError(EIO)
            self.stats["reads"] += 1
            return bytes(self._read(addr, off, n))

    def write(self, addr, off, data):
        with self.lock:
            self._bus_check()
            if off + len(data) > 256:
                raise I2CError(EIO)
            self.stats["writes"] += 1
            self._write(addr, off, bytes(data))

    # fault and value control -------------------------------------------
    def set_fault(self, name, on=True, lane=None):
        with self.lock:
            if name not in self.FAULTS:
                raise ValueError("unknown fault %r (known: %s)" % (name, ", ".join(sorted(self.FAULTS))))
            if lane is None:
                if on:
                    self.faults[name] = True
                else:
                    self.faults.pop(name, None)
                return
            lanes = self.faults.get(name)
            if not isinstance(lanes, set):
                lanes = set()
            if on:
                lanes.add(int(lane))
            else:
                lanes.discard(int(lane))
            if lanes:
                self.faults[name] = lanes
            else:
                self.faults.pop(name, None)

    def fault_active(self, name, lane=0):
        v = self.faults.get(name)
        if v is True:
            return True
        return isinstance(v, set) and lane in v

    def jitter(self, value):
        if not self.noise:
            return value
        return value * (1.0 + self.rng.uniform(-self.noise, self.noise))

    def describe(self):
        return {
            "kind": self.kind,
            "faults": {k: (sorted(v) if isinstance(v, set) else v) for k, v in self.faults.items()},
            "nack": self.nack,
            "error_rate": self.error_rate,
            "stats": dict(self.stats),
        }
