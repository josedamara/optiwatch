"""Run the simulator: python3 -m optisim --cages sfp,qsfpdd,empty,empty"""

import argparse
import signal
import sys
import threading

from .server import Simulator


def main(argv=None):
    ap = argparse.ArgumentParser(prog="optisim", description="Pluggable optics simulator")
    ap.add_argument("--i2c", default="/tmp/optisim.sock",
                    help="I2C endpoint: unix socket path or host:port (default %(default)s)")
    ap.add_argument("--control", default="/tmp/optisim-ctl.sock",
                    help="control endpoint: unix socket path or host:port (default %(default)s)")
    ap.add_argument("--cages", default="sfp,qsfpdd,empty,empty",
                    help="comma separated initial contents: sfp, qsfpdd or empty")
    ap.add_argument("--pwrup-delay", type=float, default=0.5,
                    help="CMIS ModulePwrUp duration in seconds")
    ap.add_argument("--noise", type=float, default=0.0,
                    help="relative jitter applied to analog readings, e.g. 0.01")
    args = ap.parse_args(argv)

    kinds = [k.strip() for k in args.cages.split(",")]
    sim = Simulator(args.i2c, args.control, ncages=len(kinds))
    for i, kind in enumerate(kinds):
        if kind and kind != "empty":
            opts = {"pwrup_delay": args.pwrup_delay} if kind != "sfp" else {}
            sim.chassis.plug(i, kind, serial="OWSIM%05d" % i, **opts)
            sim.chassis.cages[i].noise = args.noise
    sim.start()
    print("optisim: %d cages, i2c=%s control=%s" % (len(kinds), args.i2c, args.control), flush=True)

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    stop.wait()
    sim.stop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
