# OptiWatch: optical transceiver manager

[![CI](https://github.com/josedamara/optiwatch/actions/workflows/ci.yml/badge.svg)](https://github.com/josedamara/optiwatch/actions/workflows/ci.yml)

OptiWatch is a Linux daemon written in C and C++ that finds, monitors and
controls pluggable optical transceivers (SFP+ and QSFP-DD) over I2C. It is
built from the public management specs: **SFF-8472** for SFP and **OIF CMIS**
for QSFP-DD/OSFP.

It does what the transceiver layer of a line card does:

* **Discovery and hot plug.** It detects insertion and removal, reads and
  checksums the module EEPROM, and walks CMIS modules through
  LowPwr → PwrUp → Ready.
* **Digital diagnostics.** It reads temperature, supply voltage, and per-lane
  Tx bias, Tx power and Rx power, including SFF-8472 external calibration.
* **Alarms.** Alarms raise and clear against the module's own thresholds,
  with hysteresis, X.733 severities and correlation rules (LOS suppresses
  low-Rx-power alarms, disabled lanes get no Tx alarms).
* **Control.** Supports per-lane Tx disable and automatic laser shutdown on
  loss of signal, in the style of ITU-T G.664.
* **Robust I/O.** Transfers are split at page boundaries, CMIS page selects
  are verified, failed transactions are retried with backoff, and a NACK is
  never mistaken for a bus error.

None of this needs hardware. The daemon runs against three targets:

1. **optisim**, a Python simulator of SFP and QSFP-DD modules, with fault
   injection.
2. **xcvr_i2c_emu.ko**, a Linux kernel module that registers real I2C
   adapters with emulated modules behind them, so the daemon talks through
   `/dev/i2c-N` exactly as it would on a board.
3. Real hardware, through the same `i2c:` backend.

```
$ optiwatch show ports
PORT    STATE        TYPE      PART              SERIAL       TEMP(C)  MIN RX(dBm)  ALARMS
p0      ready        SFF-8472  OW-SFP10G-SR      OWSIM00000   80.0     -5.00        1
p1      ready        CMIS      OW-QDD-400G-FR8   OWSIM00001   45.0     -30.00       2
p2      empty        -         -                 -            -        -            0

$ optiwatch show alarms
SEVERITY  PORT    LANE  ALARM                   RAISED                    MESSAGE
critical  p1      3     rx_los                  2026-11-24T21:19:34.623Z  Loss of signal
major     p0      -     temperature_high_alarm  2026-11-24T21:19:34.223Z  Temperature high alarm: 80.00 C > 75.00 C
minor     p1      5     rx_power_low_warning    2026-11-24T21:19:34.824Z  Rx power low warning: -9.00 dBm < -8.00 dBm

$ optiwatch show port p1
Port p1  state ready
  Module     QSFP-DD (CMIS)
  Vendor     OPTIWATCH LABS  OW-QDD-400G-FR8 rev B0
  Serial     OWSIM00001  date 261124
  Optics     1310.0 nm, LC, 8 lane(s)
  Temp       45.00 C   Vcc 3.300 V   ModuleReady

  LANE  BIAS(mA)  TX(dBm)  RX(dBm)  STATUS
  0     40.00     1.00     -2.00    ok
  ...
  3     40.00     1.00     -30.00   RX-LOS
  ...
  6     0.00      -40.00   -2.00    TX-DISABLED
```

## Quick start

You need Linux (or WSL) with gcc or clang, make and Python 3.8 or newer. No
other dependencies.

```sh
make                 # libxcvr, optiwatchd, optiwatch, xcvrdump, unit tests
make test            # C unit + simulator + integration tests
```

Then run the simulator, the daemon and the CLI in three terminals:

```sh
# 1. Simulated chassis: an SFP+ in cage 0, a QSFP-DD in cage 1, cage 2 empty
tools/optisim --cages sfp,qsfpdd,empty

# 2. The daemon, with the example config (ports p0..p2 on the simulator)
build/optiwatchd -c etc/optiwatchd.conf -e -

# 3. Look around and break things
build/optiwatch show ports
tools/optisim-inject set 0 temp_c 80                 # overheat the SFP
tools/optisim-inject set 1 rx_power_dbm -30 --lane 3 # fibre cut on one lane
tools/optisim-inject fault 1 tx_fault --lane 0
tools/optisim-inject bus 0 --error-rate 0.3          # flaky I2C
tools/optisim-inject unplug 0                        # hot unplug
tools/optisim-inject plug 2 qsfpdd                   # hot plug
tools/optisim-inject scenario fiber-cut 1
build/optiwatch show alarms
build/optiwatch tx-disable p1 0-3 on
build/optiwatch events 10
```

`xcvrdump` decodes one module directly, without the daemon, much like
`ethtool -m`:

```sh
build/xcvrdump sim:/tmp/optisim.sock:1 --power-up
build/xcvrdump i2c:/dev/i2c-3 --hex
```

### Through the Linux I2C stack

```sh
make kmod                                       # needs kernel headers
sudo modprobe i2c-dev
sudo insmod kernel/xcvr_i2c_emu.ko nr_cages=2   # registers "xcvr-emu cage0/1"
sudo PYTHONPATH=sim python3 -m optisim.kbridge --cages sfp,qsfpdd
#   cage 0 -> /dev/i2c-7
#   cage 1 -> /dev/i2c-8
sudo build/optiwatchd -p eth0=i2c:/dev/i2c-7 -p eth1=i2c:/dev/i2c-8 --als
```

The kernel module stores the EEPROM bytes and implements the bus side: the
register pointer, CMIS paging, write protection and clear-on-read flags. It
also injects faults (`nack`, `error_every` in debugfs). The bridge feeds it
live values from the simulator models, so `optisim-inject` works on this
path too.

## How it is built

| Path | Language | What |
|------|----------|------|
| `include/xcvr/`, `lib/` | C11 | **libxcvr**: bus layer (i2c-dev, simulator, in-memory backends), SFF-8472 and CMIS decoders, module access |
| `daemon/` | C++17 | **optiwatchd**: per-port state machine, alarm table, ALS, event log, control socket |
| `tools/` | C, Python | `optiwatch` CLI, `xcvrdump`, `optisim`, `optisim-inject` launchers |
| `sim/optisim/` | Python | module models, I2C and control socket servers, fault injection CLI, kernel bridge |
| `kernel/` | C (kernel) | `xcvr_i2c_emu.ko`, emulated I2C adapters with debugfs control |
| `tests/` | C, Python | unit, simulator, integration and kernel-path tests |
| `docs/` | | [architecture](docs/ARCHITECTURE.md), [register notes](docs/SPEC_NOTES.md) |
| `etc/` | | example config and a hardened systemd unit |

Design points worth reading in the code:

* **The decoders are pure functions** from byte images to engineering units
  ([`lib/sff8472.c`](lib/sff8472.c), [`lib/cmis.c`](lib/cmis.c)), so unit
  tests cover external calibration, signed temperatures, checksums, flat
  memory and Tx bias scaling without any I/O.
* **The bus layer** ([`lib/bus.c`](lib/bus.c)) handles chunking at
  128-byte boundaries, retries with backoff, and NACK versus I/O error
  semantics. [`lib/bus_i2cdev.c`](lib/bus_i2cdev.c) issues combined
  write-offset plus repeated-start-read transactions with `I2C_RDWR`.
* **The port state machine** ([`daemon/port.cpp`](daemon/port.cpp)) covers
  insertion settle time, CMIS power-up with a timeout, recovery from faults,
  and permanent faults that wait for removal.
* **The alarm table** ([`daemon/alarm.cpp`](daemon/alarm.cpp)) evaluates
  thresholds in display units (dBm for optical power), so hysteresis is in
  dB. Alarms supersede warnings, and suppression rules prevent duplicate
  root-cause alarms.
* **The event loop** ([`daemon/main.cpp`](daemon/main.cpp)) is a single
  `poll()` with `signalfd`: no work happens in signal handlers, `SIGHUP`
  reopens the event log, and `SIGTERM` exits cleanly.

## Testing

| Suite | Count | Runs |
|-------|------:|------|
| C unit tests (`tests/unit`) | 23 tests / 179 checks | `make unit` |
| Simulator tests (`tests/sim`) | 22 | `make simtest` |
| Integration tests (`tests/integration`) | 29 | `make itest` |
| Kernel-path tests (`tests/kernel`) | 6 | CI, or as root with the module loaded |

Each integration test starts a fresh simulator and the real `optiwatchd`
binary, injects one fault and checks the daemon's reaction through the
control socket, the event log and the simulator's register contents.
Covered cases include over-temperature with hysteresis and no flapping,
warning-to-alarm escalation, LOS suppression, Tx fault, low Vcc, high bias,
lane Tx disable (by checking the CMIS page 10h register itself), automatic
laser shutdown and restore, hot unplug and swap, CMIS power-up timeout,
ModuleFault, a module dropping back to low power, unsupported module,
EEPROM corruption, I2C NACK and recovery, retries absorbing a 20% error
rate, a simulator restart, client limits, and SIGHUP/SIGTERM handling.

`make asan` runs the whole suite again with AddressSanitizer and
UndefinedBehaviorSanitizer. CI runs gcc and clang builds, the sanitizer
build and cppcheck. It also builds the kernel module, loads it on the
runner and runs the kernel-path tests through `/dev/i2c-N`.

## Limitations

These are deliberate scope cuts, listed in
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#limitations-and-next-steps):

* SFF-8636 QSFP28 modules are detected but not managed.
* The CMIS data path state machine is not driven.
* Monitoring is polling-based rather than driven by the `IntL` interrupt line.
* A single thread services every bus.

## License

MIT
