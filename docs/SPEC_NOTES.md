# Spec notes: the registers OptiWatch uses

These notes map each feature to the register it relies on. They were written
while implementing the decoders and are the quickest way to check the code
against the specifications.

References:

* **SFF-8472** Rev 12.4, *Management Interface for SFP+* (SNIA SFF TWG)
* **OIF-CMIS-05.2**, *Common Management Interface Specification* (OIF)
* **SFF-8024** Rev 4.x, *SFF Module Management Reference Code Tables* (identifier and connector codes)
* **ITU-T G.664**, *Optical safety procedures and requirements for optical transport systems* (automatic laser shutdown)
* **ITU-T X.733**, *Alarm reporting function* (perceived severity levels)

## SFF-8472 (SFP / SFP+)

Two 256-byte maps on the two-wire bus.

### A0h (I2C 0x50): serial ID

| Bytes | Field | Used for |
|------:|-------|----------|
| 0 | Identifier (SFF-8024) | `0x03` SFP, `0x02` soldered. Anything else is rejected |
| 2 | Connector (SFF-8024 Table 4-3) | display |
| 20-35 | Vendor name, ASCII, space padded | identity |
| 37-39 | Vendor OUI | identity |
| 40-55 | Vendor part number | identity |
| 56-59 | Vendor revision | identity |
| 60-61 | Wavelength, nm | identity |
| 63 | CC_BASE = sum(0..62) mod 256 | `eeprom_checksum` alarm |
| 68-83 | Vendor serial number | identity, swap detection in events |
| 84-91 | Date code YYMMDDLL | identity |
| 92 | Diagnostic monitoring type: bit 6 DDM implemented, bit 5 internally calibrated, bit 4 externally calibrated | decoder path |
| 93 | Enhanced options: bit 7 alarm/warning flags, bit 6 soft TX_DISABLE | whether `tx-disable` is allowed |
| 94 | SFF-8472 compliance | display |
| 95 | CC_EXT = sum(64..94) mod 256 | `eeprom_checksum` alarm |

### A2h (I2C 0x51): diagnostics

| Bytes | Field | Used for |
|------:|-------|----------|
| 0-39 | Thresholds: temp, Vcc, bias, Tx power, Rx power; each high alarm, low alarm, high warning, low warning (2 bytes) | alarm evaluation |
| 56-75 | Rx_PWR(4)..Rx_PWR(0), IEEE-754 floats | external calibration of Rx power |
| 76-91 | Tx_I, Tx_PWR, T, V slope (unsigned 8.8) and offset (signed 16) | external calibration |
| 95 | CC_DMI = sum(0..94) mod 256 | `eeprom_checksum` alarm |
| 96-105 | Temperature, Vcc, Tx bias, Tx power, Rx power | live readings |
| 110 | Status/control: bit 7 TX_DISABLE state, bit 6 soft TX disable (host writes), bit 2 TX_FAULT, bit 1 RX_LOS, bit 0 Data_Ready_Bar | `tx_fault`, `rx_los`, `tx-disable` |
| 112-113 | Alarm flags | exposed in `xcvr_dom.flags` |
| 116-117 | Warning flags | exposed in `xcvr_dom.flags` |

Units (Section 9.2): temperature is signed 1/256 C, Vcc 100 uV, bias 2 uA,
optical power 0.1 uW. For externally calibrated modules the raw counts go
through `slope * raw + offset` (Rx power through the 4th order polynomial).
Thresholds are stored in the same format as the live values, so the same
conversion applies to them.

## CMIS (QSFP-DD, OSFP, QSFP+ with CMIS)

One device at 0x50. Bytes 0-127 (lower page) are always visible; bytes
128-255 show the upper page selected by byte 127 (bank in byte 126, OptiWatch
uses bank 0). The library writes the page select, reads it back and fails
the transfer if the module did not accept it. Reads and writes are split so
they never cross offset 128.

### Lower page

| Byte | Field | Used for |
|-----:|-------|----------|
| 0 | Identifier (SFF-8024): `0x18` QSFP-DD, `0x19` OSFP, `0x1A` SFP-DD, `0x1E` QSFP+ CMIS | type detection |
| 1 | CMIS revision | display |
| 2 | bit 7 flat memory (no paging, no monitors) | decoder path |
| 3 | Module state, bits 3-1: 1 LowPwr, 2 PwrUp, 3 Ready, 4 PwrDn, 5 Fault | port state machine |
| 8 | bit 0 latched ModuleStateChanged | simulator |
| 9 | Latched temp (bits 0-3) and Vcc (bits 4-7) flags: HA, LA, HW, LW | `xcvr_dom.flags` |
| 14-15 | Temperature, signed 1/256 C | live reading |
| 16-17 | Vcc, 100 uV | live reading |
| 26 | Module control: bit 4 LowPwrRequestSW | power-up request |
| 127 | Page select | paging |

Latched flags clear when the host reads them; the condition sets them again
while it persists.

### Page 00h

Vendor name 129-144, OUI 145-147, part number 148-163, revision 164-165,
serial 166-181, date code 182-189, connector 203, checksum 222 over 128-221.

### Page 01h (advertising)

| Byte | Field |
|-----:|-------|
| 138-139 | Nominal wavelength, units of 0.05 nm |
| 159 | Module monitor support: bit 0 temperature, bit 1 Vcc |
| 160 | Lane monitor support: bit 0 bias, bit 1 Tx power, bit 2 Rx power; bits 4-3 Tx bias scaling (x1, x2, x4) |

### Page 02h (thresholds)

Each block is high alarm, low alarm, high warning, low warning, 2 bytes each:
temperature 128, Vcc 136, Tx power 176, Tx bias 184 (scaled like the
monitor), Rx power 192.

### Page 10h (lane control)

Byte 130: Tx output disable, one bit per lane.

### Page 11h (lane status and monitors)

| Byte | Field |
|-----:|-------|
| 128-131 | Data path state, 4 bits per lane |
| 135 | Latched Tx fault, bit per lane |
| 136 | Latched Tx LOS |
| 139-142 | Latched Tx power HA, LA, HW, LW |
| 143-146 | Latched Tx bias HA, LA, HW, LW |
| 147 | Latched Rx LOS |
| 149-152 | Latched Rx power HA, LA, HW, LW |
| 154-169 | Tx power lanes 1-8, 0.1 uW |
| 170-185 | Tx bias lanes 1-8, 2 uA times the page 01h scaling |
| 186-201 | Rx power lanes 1-8, 0.1 uW |

### Module power-up

After insertion a paged CMIS module sits in ModuleLowPwr while the host
holds it there (LPMode pin or LowPwrRequestSW). OptiWatch clears
LowPwrRequestSW, polls byte 3 every `fast_poll_ms` until ModuleReady and
raises `power_up_timeout` if that takes longer than `power_up_timeout_ms`.
ModuleFault raises `module_fault`. If a ready module drops back to a lower
state the daemon requests high power again.

## Automatic laser shutdown

G.664 defines ALS for line systems: when the receiver loses signal, the
transmitter on the same span is shut down (the fibre is probably cut and
emitting light would be an eye hazard), and restarted once light returns.
OptiWatch implements the per-lane decision with soft TX disable; it does not
implement the timed restart pulses of the full procedure.
