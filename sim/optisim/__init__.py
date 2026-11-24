"""optisim: a pluggable optical transceiver simulator.

Emulates the two-wire management interface of SFP (SFF-8472) and QSFP-DD
(CMIS) modules seated in a set of cages, and lets tests inject faults:
alarms, loss of signal, Tx faults, bus errors, checksum corruption,
modules that never finish powering up, hot plug and unplug.

The I2C side speaks the small binary protocol documented in lib/bus_sim.c,
the control side speaks JSON lines (see optisim.server).
"""

from .cmis import CmisModule
from .sff8472 import SfpModule
from .common import I2CError, BusModel

__all__ = ["CmisModule", "SfpModule", "I2CError", "BusModel", "make_module"]


def make_module(kind, **kwargs):
    """Create a module model by kind name: 'sfp' or 'qsfpdd'."""
    kind = kind.lower()
    if kind == "sfp":
        return SfpModule(**kwargs)
    if kind in ("qsfpdd", "qsfp-dd", "cmis", "osfp"):
        return CmisModule(**kwargs)
    raise ValueError("unknown module kind %r (expected sfp or qsfpdd)" % kind)
