# OptiWatch — optical transceiver manager

A Linux daemon in C/C++ that discovers, monitors and controls pluggable optical
transceivers (SFP/SFP+ and QSFP-DD/OSFP) over I2C, implemented from the public
**SFF-8472** and **OIF CMIS** management specifications.

> Work in progress — see the commit history for the build-up step by step.

## Planned layout

| Path | What |
|------|------|
| `include/xcvr/`, `lib/` | C11 library: I2C bus abstraction, SFF-8472 and CMIS memory-map decoders |
| `daemon/` | C++17 daemon: per-port state machine, hot-plug, alarm manager, control socket |
| `tools/` | `optiwatch` command-line client (talks to the `optiwatchd` daemon) |
| `sim/` | Python transceiver simulator with fault injection |
| `kernel/` | Linux kernel module that emulates an I2C adapter with modules behind it |
| `tests/` | C unit tests and Python integration tests |

## License

MIT
