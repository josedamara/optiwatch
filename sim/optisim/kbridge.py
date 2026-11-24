"""Bridge simulator models into the xcvr_i2c_emu kernel module.

The kernel module provides real I2C adapters (/dev/i2c-N through i2c-dev)
but only stores bytes. This bridge gives those bytes behaviour: every
interval it

  1. pulls the registers the host may have written (soft TX disable,
     CMIS LowPwr request, lane TX disable) out of the kernel into the model,
  2. lets the model advance (power-up state machine, alarm flags, live
     analog values), and
  3. pushes the rendered memory images back through debugfs.

The kernel keeps host-owned bytes when an image is reloaded, so the host's
writes are never lost to a refresh.

Usage (root, module loaded):
    python3 -m optisim.kbridge --cages sfp,qsfpdd
    optisim-inject set 0 temp_c 80          # same control socket as optisim
"""

import argparse
import os
import signal
import sys
import threading

from .cmis import CmisModule
from .server import Simulator

DEBUGFS = "/sys/kernel/debug/xcvr_i2c_emu"


# debugfs attribute files (bool/u32) are not seekable, so use plain
# read()/write() on a fresh descriptor, which starts at offset 0.
def _write(path, data):
    fd = os.open(path, os.O_WRONLY)
    try:
        view = memoryview(data)
        while view:
            view = view[os.write(fd, view):]
    finally:
        os.close(fd)


def _read(path, n):
    fd = os.open(path, os.O_RDONLY)
    try:
        out = bytearray()
        while len(out) < n:
            chunk = os.read(fd, n - len(out))
            if not chunk:
                break
            out += chunk
        return bytes(out)
    finally:
        os.close(fd)


def find_adapters(nr):
    """Map cage index -> /dev/i2c-N by adapter name."""
    out = {}
    base = "/sys/bus/i2c/devices"   # adapters appear here as i2c-N
    for entry in os.listdir(base):
        if not entry.startswith("i2c-"):
            continue
        try:
            with open(os.path.join(base, entry, "name")) as f:
                name = f.read().strip()
        except OSError:
            continue
        if name.startswith("xcvr-emu cage"):
            idx = int(name.rsplit("cage", 1)[1])
            if idx < nr:
                out[idx] = "/dev/" + entry
    return out


class KernelBridge:
    def __init__(self, chassis, root=DEBUGFS, interval=0.1):
        self.chassis = chassis
        self.root = root
        self.interval = interval
        self.loaded = [None] * len(chassis.cages)
        self._stop = threading.Event()
        self._thread = None
        self.lock = threading.Lock()

    def path(self, cage, name):
        return os.path.join(self.root, "cage%d" % cage, name)

    # --- image rendering --------------------------------------------------
    @staticmethod
    def render(mod):
        """Return (a0, a2, pages) byte images for a module model."""
        with mod.lock:
            if isinstance(mod, CmisModule):
                mod._render()
                lower = bytearray(mod.lower)
                pages = bytearray(256 * 128)
                for p, data in mod.pages.items():
                    pages[p * 128:(p + 1) * 128] = data
                # Latched flags: hand the kernel what latched since the last
                # refresh; the kernel clears them when the host reads.
                for (page, off), bits in list(mod.latched.items()):
                    if page == "L":
                        lower[off] = bits
                    else:
                        pages[page * 128 + off - 128] = bits
                mod.latched.clear()
                upper0 = bytes(mod.pages[0x00])
                a0 = bytes(lower) + upper0
                if mod.fault_active("corrupt_checksum"):
                    pages[222 - 128] ^= 0xFF
                return a0, bytes(256), bytes(pages)
            a0 = bytes(mod._read(0x50, 0, 256))
            mod._render_a2()
            return a0, bytes(mod.a2), bytes(256 * 128)

    def pull_host_writes(self, cage, mod):
        with mod.lock:
            if isinstance(mod, CmisModule):
                lower = _read(self.path(cage, "a0"), 128)
                p10 = _read(self.path(cage, "pages"), 0x11 * 128)[0x10 * 128:0x11 * 128]
                mod.lower[26] = lower[26] & 0x50
                mod.pages[0x10][130 - 128] = p10[130 - 128] & ((1 << mod.lanes) - 1)
            else:
                a2 = _read(self.path(cage, "a2"), 256)
                mod.a2[110] = (mod.a2[110] & ~0x40) | (a2[110] & 0x40)

    def push_bus_faults(self, cage, mod):
        _write(self.path(cage, "nack"), b"1" if mod.nack else b"0")
        every = int(round(1.0 / mod.error_rate)) if mod.error_rate > 0 else 0
        _write(self.path(cage, "error_every"), str(every).encode())

    # --- sync loop --------------------------------------------------------
    def sync(self):
        with self.lock:
            for cage, mod in enumerate(list(self.chassis.cages)):
                if mod is None:
                    if self.loaded[cage] is not None:
                        _write(self.path(cage, "present"), b"0")
                        self.loaded[cage] = None
                    continue
                if self.loaded[cage] is not mod:
                    # New insertion: load a complete image while the cage is empty.
                    _write(self.path(cage, "present"), b"0")
                    a0, a2, pages = self.render(mod)
                    _write(self.path(cage, "a0"), a0)
                    _write(self.path(cage, "a2"), a2)
                    _write(self.path(cage, "pages"), pages)
                    self.push_bus_faults(cage, mod)
                    _write(self.path(cage, "present"), b"1")
                    self.loaded[cage] = mod
                    continue
                self.pull_host_writes(cage, mod)
                a0, a2, pages = self.render(mod)
                _write(self.path(cage, "a0"), a0)
                _write(self.path(cage, "a2"), a2)
                _write(self.path(cage, "pages"), pages)
                self.push_bus_faults(cage, mod)

    def _run(self):
        while not self._stop.wait(self.interval):
            try:
                self.sync()
            except OSError as e:
                print("kbridge: %s" % e, file=sys.stderr)

    def start(self):
        self.sync()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        return self

    def stop(self):
        self._stop.set()
        if self._thread:
            self._thread.join()
        for cage in range(len(self.loaded)):
            try:
                _write(self.path(cage, "present"), b"0")
            except OSError:
                pass


def main(argv=None):
    ap = argparse.ArgumentParser(prog="optisim-kbridge", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--debugfs", default=DEBUGFS)
    ap.add_argument("--control", default="/tmp/optisim-ctl.sock")
    ap.add_argument("--cages", default="sfp,qsfpdd")
    ap.add_argument("--interval", type=float, default=0.1)
    args = ap.parse_args(argv)

    kinds = [k.strip() for k in args.cages.split(",")]
    sim = Simulator(None, args.control, ncages=len(kinds))
    for i, kind in enumerate(kinds):
        if kind and kind != "empty":
            sim.chassis.plug(i, kind, serial="OWKRN%05d" % i)
    sim.start()
    bridge = KernelBridge(sim.chassis, args.debugfs, args.interval).start()
    for cage, dev in sorted(find_adapters(len(kinds)).items()):
        print("cage %d -> %s" % (cage, dev))
    print("kbridge running, control=%s" % args.control, flush=True)

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    stop.wait()
    bridge.stop()
    sim.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
