"""SFF-8472 SFP/SFP+ module model (A0h serial ID + A2h diagnostics)."""

import struct
import time

from .common import (BusModel, I2CError, ENXIO, checksum8, dbm_to_mw, mw_to_dbm,
                     put_ascii, put_s16, put_u16, temp_raw, vcc_raw, bias_raw, power_raw)

A0, A2 = 0x50, 0x51

# Alarm/warning thresholds in engineering units: (high alarm, low alarm, high warn, low warn)
DEFAULT_THRESHOLDS = {
    "temp_c": (75.0, -5.0, 70.0, 0.0),
    "vcc_v": (3.6, 3.0, 3.5, 3.1),
    "bias_ma": (12.0, 2.0, 10.0, 3.0),
    "tx_power_mw": (dbm_to_mw(1.0), dbm_to_mw(-9.0), dbm_to_mw(0.0), dbm_to_mw(-8.0)),
    "rx_power_mw": (dbm_to_mw(1.0), dbm_to_mw(-14.0), dbm_to_mw(0.0), dbm_to_mw(-12.0)),
}

RX_LOS_ASSERT_DBM = -20.0

# External calibration constants used when ext_cal=True. Chosen so that the
# raw values differ from the calibrated ones and the conversion is exercised.
EXT_BIAS_SLOPE = 2.0     # calibrated = 2.0 * raw
EXT_TEMP_OFFSET = -256   # calibrated = raw - 1.0 C
EXT_RX_PWR1 = 0.5        # calibrated = 0.5 * raw


class SfpModule(BusModel):
    """An SFP+ module. Analog values are set in engineering units and are
    rendered into the A2h map on every read, so changes show up live."""

    kind = "sfp"
    FAULTS = {"tx_fault", "rx_los", "data_not_ready", "corrupt_checksum"}

    def __init__(self, serial="OW24110001", vendor="OPTIWATCH LABS", part="OW-SFP10G-SR",
                 wavelength_nm=850, ext_cal=False, seed=0, **_):
        super().__init__(seed=seed)
        self.serial = serial
        self.vendor = vendor
        self.part = part
        self.wavelength_nm = wavelength_nm
        self.ext_cal = ext_cal
        self.thresholds = dict(DEFAULT_THRESHOLDS)
        self.values = {
            "temp_c": 35.0,
            "vcc_v": 3.30,
            "bias_ma": 6.5,
            "tx_power_dbm": -2.5,
            "rx_power_dbm": -5.0,
        }
        self.a0 = bytearray(256)
        self.a2 = bytearray(256)
        self._build_a0()
        self._build_a2_static()

    # --- static content ---------------------------------------------------
    def _build_a0(self):
        a0 = self.a0
        a0[0] = 0x03                      # SFP/SFP+
        a0[1] = 0x04                      # function defined by two-wire ID
        a0[2] = 0x07                      # LC
        a0[3] = 0x10                      # 10GBASE-SR
        a0[11] = 0x06                     # 64B/66B
        a0[12] = 103                      # 10.3 GBd
        put_ascii(a0, 20, self.vendor, 16)
        a0[37:40] = bytes((0x00, 0x1B, 0x21))
        put_ascii(a0, 40, self.part, 16)
        put_ascii(a0, 56, "A1", 4)
        put_u16(a0, 60, self.wavelength_nm)
        a0[63] = checksum8(a0[0:63])
        a0[64:66] = bytes((0x00, 0x1A))   # options: TX_DISABLE, TX_FAULT, RX_LOS
        put_ascii(a0, 68, self.serial, 16)
        put_ascii(a0, 84, time.strftime("%y%m%d", time.gmtime()) + "  ", 8)
        a0[92] = 0x40 | (0x10 if self.ext_cal else 0x20)   # DDM, int/ext calibrated
        a0[93] = 0x80 | 0x40              # alarm flags + soft TX_DISABLE implemented
        a0[94] = 0x08                     # SFF-8472 Rev 12.3
        a0[95] = checksum8(a0[64:95])

    def _cal_raw(self, field, value):
        """Engineering value -> raw register value, applying (inverse)
        external calibration when the module is externally calibrated."""
        if field == "temp_c":
            raw = temp_raw(value)
            return raw - EXT_TEMP_OFFSET if self.ext_cal else raw
        if field == "vcc_v":
            return vcc_raw(value)
        if field == "bias_ma":
            raw = bias_raw(value)
            return raw / EXT_BIAS_SLOPE if self.ext_cal else raw
        if field == "tx_power_mw":
            return power_raw(value)
        if field == "rx_power_mw":
            raw = power_raw(value)
            return raw / EXT_RX_PWR1 if self.ext_cal else raw
        raise KeyError(field)

    def _put_field(self, off, field, value):
        raw = self._cal_raw(field, value)
        if field == "temp_c":
            put_s16(self.a2, off, raw)
        else:
            put_u16(self.a2, off, raw)

    def _build_a2_static(self):
        a2 = self.a2
        order = ["temp_c", "vcc_v", "bias_ma", "tx_power_mw", "rx_power_mw"]
        for m, field in enumerate(order):
            for i, v in enumerate(self.thresholds[field]):
                self._put_field(8 * m + 2 * i, field, v)
        # External calibration constants (A2h 56..91)
        a2[56:92] = bytes(36)
        if self.ext_cal:
            struct.pack_into(">f", a2, 68, EXT_RX_PWR1)        # Rx_PWR(1)
            struct.pack_into(">H", a2, 76, int(EXT_BIAS_SLOPE * 256))
            struct.pack_into(">H", a2, 80, 0x0100)             # Tx power slope 1.0
            struct.pack_into(">H", a2, 84, 0x0100)             # temperature slope 1.0
            struct.pack_into(">h", a2, 86, EXT_TEMP_OFFSET)
            struct.pack_into(">H", a2, 88, 0x0100)             # Vcc slope 1.0
        a2[95] = checksum8(a2[0:95])

    # --- live content -----------------------------------------------------
    def effective(self):
        """Current values as the module would measure them."""
        v = self.values
        tx_off = bool(self.a2[110] & 0x40)
        tx_mw = 0.0 if (tx_off or self.fault_active("tx_fault")) else dbm_to_mw(v["tx_power_dbm"])
        rx_mw = 0.0 if self.fault_active("rx_los") else dbm_to_mw(v["rx_power_dbm"])
        return {
            "temp_c": self.jitter(v["temp_c"]),
            "vcc_v": self.jitter(v["vcc_v"]),
            "bias_ma": 0.0 if tx_off else self.jitter(v["bias_ma"]),
            "tx_power_mw": self.jitter(tx_mw),
            "rx_power_mw": self.jitter(rx_mw),
            "tx_disabled": tx_off,
        }

    def _render_a2(self):
        a2 = self.a2
        e = self.effective()
        for off, field in ((96, "temp_c"), (98, "vcc_v"), (100, "bias_ma"),
                           (102, "tx_power_mw"), (104, "rx_power_mw")):
            self._put_field(off, field, e[field])

        sc = a2[110] & 0x40   # keep host-written soft TX disable
        if e["tx_disabled"]:
            sc |= 0x80
        if self.fault_active("tx_fault"):
            sc |= 0x04
        if self.fault_active("rx_los") or mw_to_dbm(e["rx_power_mw"]) < RX_LOS_ASSERT_DBM:
            sc |= 0x02
        if self.fault_active("data_not_ready"):
            sc |= 0x01
        a2[110] = sc

        # Real-time alarm (112/113) and warning (116/117) flags, Table 9-12.
        alarm = [0, 0]
        warn = [0, 0]
        layout = (("temp_c", 0, 7), ("vcc_v", 0, 5), ("bias_ma", 0, 3),
                  ("tx_power_mw", 0, 1), ("rx_power_mw", 1, 7))
        for field, byte, hi_bit in layout:
            if field == "bias_ma" and e["tx_disabled"]:
                continue
            ha, la, hw, lw = self.thresholds[field]
            val = e[field]
            if val > ha:
                alarm[byte] |= 1 << hi_bit
            if val < la:
                alarm[byte] |= 1 << (hi_bit - 1)
            if val > hw:
                warn[byte] |= 1 << hi_bit
            if val < lw:
                warn[byte] |= 1 << (hi_bit - 1)
        a2[112], a2[113] = alarm
        a2[116], a2[117] = warn

    # --- bus --------------------------------------------------------------
    def _read(self, addr, off, n):
        if addr == A0:
            data = bytearray(self.a0)
            if self.fault_active("corrupt_checksum"):
                data[20] ^= 0x01   # flips a vendor-name bit, CC_BASE no longer matches
            return data[off:off + n]
        if addr == A2:
            self._render_a2()
            return self.a2[off:off + n]
        raise I2CError(ENXIO)

    def _write(self, addr, off, data):
        if addr == A0:
            return                 # serial ID is read-only; writes are ignored
        if addr != A2:
            raise I2CError(ENXIO)
        for i, b in enumerate(data):
            o = off + i
            if o == 110:
                self.a2[110] = (self.a2[110] & ~0x40) | (b & 0x40)
            elif o == 127 or 128 <= o <= 247:
                self.a2[o] = b     # page select, user EEPROM

    # --- control ----------------------------------------------------------
    def set_value(self, field, value, lane=None):
        with self.lock:
            if field not in self.values:
                raise ValueError("unknown field %r (known: %s)" % (field, ", ".join(sorted(self.values))))
            self.values[field] = float(value)

    def describe(self):
        d = super().describe()
        e = self.effective()
        d.update({
            "serial": self.serial,
            "values": dict(self.values),
            "tx_disabled": e["tx_disabled"],
        })
        return d
