"""Fault injection CLI for a running simulator.

Examples
    optisim-inject status
    optisim-inject plug 2 qsfpdd
    optisim-inject unplug 0
    optisim-inject set 0 temp_c 82
    optisim-inject set 1 rx_power_dbm -25 --lane 3
    optisim-inject fault 1 tx_fault --lane 0
    optisim-inject fault 1 tx_fault --lane 0 --off
    optisim-inject bus 0 --nack
    optisim-inject bus 0 --error-rate 0.3
    optisim-inject scenario overheat 0
"""

import argparse
import json
import sys
import time

from .server import SimClient

SCENARIOS = {
    # name: (description, list of (delay_s, method, args, kwargs))
    "overheat": ("ramp temperature through warning and alarm, then recover",
                 [(0, "set", ("temp_c", 72), {}), (2, "set", ("temp_c", 80), {}),
                  (2, "set", ("temp_c", 40), {})]),
    "fiber-cut": ("drop receive power on every lane to LOS, then restore",
                  [(0, "set", ("rx_power_dbm", -40), {}), (3, "set", ("rx_power_dbm", -2), {})]),
    "flaky-bus": ("30% I2C error rate for 5 seconds",
                  [(0, "bus", (), {"error_rate": 0.3}), (5, "bus", (), {"error_rate": 0.0})]),
    "hot-swap": ("unplug the module and plug a fresh one back in",
                 [(0, "unplug", (), {}), (2, "plug", (), {"kind": "sfp"})]),
}


def main(argv=None):
    ap = argparse.ArgumentParser(prog="optisim-inject", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--control", default="/tmp/optisim-ctl.sock", help="simulator control endpoint")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("status")
    p = sub.add_parser("plug")
    p.add_argument("cage", type=int)
    p.add_argument("kind", choices=["sfp", "qsfpdd"])
    p.add_argument("--serial")
    p = sub.add_parser("unplug")
    p.add_argument("cage", type=int)
    p = sub.add_parser("set")
    p.add_argument("cage", type=int)
    p.add_argument("field")
    p.add_argument("value", type=float)
    p.add_argument("--lane", type=int)
    p = sub.add_parser("fault")
    p.add_argument("cage", type=int)
    p.add_argument("name")
    p.add_argument("--lane", type=int)
    p.add_argument("--off", action="store_true")
    p = sub.add_parser("bus")
    p.add_argument("cage", type=int)
    p.add_argument("--nack", action="store_true")
    p.add_argument("--no-nack", action="store_true")
    p.add_argument("--error-rate", type=float)
    p.add_argument("--delay-ms", type=float)
    p = sub.add_parser("scenario")
    p.add_argument("name", choices=sorted(SCENARIOS))
    p.add_argument("cage", type=int)

    args = ap.parse_args(argv)
    c = SimClient(args.control)
    try:
        if args.cmd == "status":
            out = c.status()
        elif args.cmd == "plug":
            opts = {"serial": args.serial} if args.serial else {}
            out = c.plug(args.cage, args.kind, **opts)
        elif args.cmd == "unplug":
            out = c.unplug(args.cage)
        elif args.cmd == "set":
            out = c.set(args.cage, args.field, args.value, args.lane)
        elif args.cmd == "fault":
            out = c.fault(args.cage, args.name, not args.off, args.lane)
        elif args.cmd == "bus":
            kw = {}
            if args.nack or args.no_nack:
                kw["nack"] = bool(args.nack)
            if args.error_rate is not None:
                kw["error_rate"] = args.error_rate
            if args.delay_ms is not None:
                kw["delay_ms"] = args.delay_ms
            out = c.bus(args.cage, **kw)
        else:
            desc, steps = SCENARIOS[args.name]
            print("scenario %s: %s" % (args.name, desc))
            for delay, method, a, kw in steps:
                time.sleep(delay)
                print("  %s %s %s" % (method, a, kw or ""))
                getattr(c, method)(args.cage, *a, **kw)
            out = {"ok": True}
        print(json.dumps(out, indent=2))
    finally:
        c.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
