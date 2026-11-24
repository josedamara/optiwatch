"""CMIS QSFP-DD module model: paged memory, module state machine, latched flags."""

import time

from .common import (BusModel, I2CError, ENXIO, dbm_to_mw, mw_to_dbm, put_ascii,
                     put_s16, put_u16, temp_raw, vcc_raw, bias_raw, power_raw)

ADDR = 0x50

LOW_PWR, PWR_UP, READY, PWR_DN, FAULT = 1, 2, 3, 4, 5
STATE_NAMES = {LOW_PWR: "ModuleLowPwr", PWR_UP: "ModulePwrUp", READY: "ModuleReady",
               PWR_DN: "ModulePwrDn", FAULT: "ModuleFault"}

SUPPORTED_PAGES = (0x00, 0x01, 0x02, 0x10, 0x11)
BIAS_SCALE_CODE = 1          # page 01h byte 160 bits 4..3: 1 => multiplier 2
BIAS_MULTIPLIER = 2

THRESHOLDS = {
    "temp_c": (75.0, -5.0, 70.0, 0.0),
    "vcc_v": (3.6, 3.0, 3.5, 3.1),
    "tx_power_mw": (dbm_to_mw(4.0), dbm_to_mw(-6.0), dbm_to_mw(3.0), dbm_to_mw(-5.0)),
    "bias_ma": (75.0, 10.0, 70.0, 15.0),
    "rx_power_mw": (dbm_to_mw(4.0), dbm_to_mw(-10.0), dbm_to_mw(3.0), dbm_to_mw(-8.0)),
}
RX_LOS_ASSERT_DBM = -20.0

# Page 11h flag byte offsets
P11_TX_FAULT, P11_TX_LOS = 135, 136
P11_TX_PWR, P11_TX_BIAS, P11_RX_LOS, P11_RX_PWR = 139, 143, 147, 149


class CmisModule(BusModel):
    """An 8-lane QSFP-DD module (400G FR8 style).

    Power-up follows the CMIS module state machine: the module starts in
    ModuleLowPwr with LowPwrRequestSW set (as if the host held LPMode). When
    the host clears it the module moves to ModulePwrUp and, after
    pwrup_delay seconds, to ModuleReady. Lane monitors read zero outside
    ModuleReady, as on real hardware.
    """

    kind = "qsfpdd"
    FAULTS = {"tx_fault", "tx_los", "rx_los", "stuck_pwrup", "module_fault", "corrupt_checksum"}

    def __init__(self, serial="OWQ2400777", vendor="OPTIWATCH LABS", part="OW-QDD-400G-FR8",
                 lanes=8, pwrup_delay=0.5, start_low_power=True, seed=0, **_):
        super().__init__(seed=seed)
        self.serial = serial
        self.vendor = vendor
        self.part = part
        self.lanes = lanes
        self.pwrup_delay = float(pwrup_delay)
        self.thresholds = dict(THRESHOLDS)
        self.values = {
            "temp_c": 45.0,
            "vcc_v": 3.30,
            "bias_ma": [40.0] * lanes,
            "tx_power_dbm": [1.0] * lanes,
            "rx_power_dbm": [-2.0] * lanes,
        }
        self.lower = bytearray(128)
        self.pages = {p: bytearray(128) for p in SUPPORTED_PAGES}
        self.state = LOW_PWR
        self.pwrup_started = None
        self.latched = {}             # (page or 'L', offset) -> latched bits
        self._build_static()
        if start_low_power:
            self.lower[26] |= 0x10

    # --- static content ---------------------------------------------------
    def _build_static(self):
        lo = self.lower
        lo[0] = 0x18                  # QSFP-DD
        lo[1] = 0x50                  # CMIS 5.0
        lo[2] = 0x00                  # paged memory
        lo[85] = 0x02                 # media type: SMF
        p0 = self.pages[0x00]
        p0[0] = 0x18
        put_ascii(p0, 129 - 128, self.vendor, 16)
        p0[145 - 128:148 - 128] = bytes((0x00, 0x1B, 0x21))
        put_ascii(p0, 148 - 128, self.part, 16)
        put_ascii(p0, 164 - 128, "B0", 2)
        put_ascii(p0, 166 - 128, self.serial, 16)
        put_ascii(p0, 182 - 128, time.strftime("%y%m%d", time.gmtime()) + "  ", 8)
        p0[203 - 128] = 0x07          # LC (duplex FR8)
        p0[222 - 128] = sum(p0[0:222 - 128]) & 0xFF   # page 00h checksum (128..221)

        p1 = self.pages[0x01]
        put_u16(p1, 138 - 128, 1310.0 / 0.05)
        p1[159 - 128] = 0x03          # temperature + Vcc monitors
        p1[160 - 128] = 0x07 | (BIAS_SCALE_CODE << 3)

        p2 = self.pages[0x02]
        def quad(off, field, enc, signed=False):
            for i, v in enumerate(self.thresholds[field]):
                (put_s16 if signed else put_u16)(p2, off - 128 + 2 * i, enc(v))
        quad(128, "temp_c", temp_raw, signed=True)
        quad(136, "vcc_v", vcc_raw)
        quad(176, "tx_power_mw", power_raw)
        quad(184, "bias_ma", lambda v: bias_raw(v, BIAS_MULTIPLIER))
        quad(192, "rx_power_mw", power_raw)

    # --- state machine ----------------------------------------------------
    def _set_state(self, new):
        if new != self.state:
            self.state = new
            self._latch(("L", 8), 0x01)     # L-ModuleStateChanged

    def _step_state(self):
        low_pwr_req = bool(self.lower[26] & 0x10)
        if self.fault_active("module_fault"):
            self._set_state(FAULT)
        elif low_pwr_req:
            self._set_state(LOW_PWR)
            self.pwrup_started = None
        elif self.state in (LOW_PWR, PWR_DN):
            self._set_state(PWR_UP)
            self.pwrup_started = time.monotonic()
        elif self.state == PWR_UP:
            if self.fault_active("stuck_pwrup"):
                return
            if self.pwrup_started is None:
                self.pwrup_started = time.monotonic()
            if time.monotonic() - self.pwrup_started >= self.pwrup_delay:
                self._set_state(READY)
        elif self.state == FAULT:
            self._set_state(LOW_PWR if low_pwr_req else PWR_UP)
            self.pwrup_started = time.monotonic()

    def _latch(self, key, bits):
        self.latched[key] = self.latched.get(key, 0) | bits

    # --- live content -----------------------------------------------------
    def tx_disable_mask(self):
        return self.pages[0x10][130 - 128]

    def effective(self):
        ready = self.state == READY
        v = self.values
        lanes = []
        for l in range(self.lanes):
            disabled = bool(self.tx_disable_mask() & (1 << l))
            tx_on = ready and not disabled and not self.fault_active("tx_fault", l)
            rx_on = ready and not self.fault_active("rx_los", l)
            lanes.append({
                "tx_power_mw": self.jitter(dbm_to_mw(v["tx_power_dbm"][l])) if tx_on else 0.0,
                "bias_ma": self.jitter(v["bias_ma"][l]) if (ready and not disabled) else 0.0,
                "rx_power_mw": self.jitter(dbm_to_mw(v["rx_power_dbm"][l])) if rx_on else 0.0,
                "tx_disabled": disabled,
            })
        return {"temp_c": self.jitter(v["temp_c"]), "vcc_v": self.jitter(v["vcc_v"]),
                "ready": ready, "lanes": lanes}

    @staticmethod
    def _compare(val, th):
        ha, la, hw, lw = th
        return ((val > ha) << 0) | ((val < la) << 1) | ((val > hw) << 2) | ((val < lw) << 3)

    def _render(self):
        self._step_state()
        e = self.effective()
        lo = self.lower
        lo[3] = self.state << 1
        put_s16(lo, 14, temp_raw(e["temp_c"]))
        put_u16(lo, 16, vcc_raw(e["vcc_v"]))
        t = self._compare(e["temp_c"], self.thresholds["temp_c"])
        vcc = self._compare(e["vcc_v"], self.thresholds["vcc_v"])
        self._latch(("L", 9), t | (vcc << 4))

        p11 = self.pages[0x11]
        dp = [0, 0, 0, 0]
        for l, lane in enumerate(e["lanes"]):
            put_u16(p11, 154 - 128 + 2 * l, power_raw(lane["tx_power_mw"]))
            put_u16(p11, 170 - 128 + 2 * l, bias_raw(lane["bias_ma"], BIAS_MULTIPLIER))
            put_u16(p11, 186 - 128 + 2 * l, power_raw(lane["rx_power_mw"]))
            dp[l // 2] |= (4 if e["ready"] else 1) << (4 * (l % 2))  # DPActivated / DPDeactivated
            if not e["ready"]:
                continue
            bit = 1 << l
            if self.fault_active("tx_fault", l):
                self._latch((0x11, P11_TX_FAULT), bit)
            if self.fault_active("tx_los", l):
                self._latch((0x11, P11_TX_LOS), bit)
            if self.fault_active("rx_los", l) or mw_to_dbm(lane["rx_power_mw"]) < RX_LOS_ASSERT_DBM:
                self._latch((0x11, P11_RX_LOS), bit)
            checks = [("rx_power_mw", P11_RX_PWR)]
            if not lane["tx_disabled"]:
                checks += [("tx_power_mw", P11_TX_PWR), ("bias_ma", P11_TX_BIAS)]
            for field, base in checks:
                f = self._compare(lane[field], self.thresholds[field])
                for i in range(4):
                    if f & (1 << i):
                        self._latch((0x11, base + i), bit)
        p11[0:4] = bytes(dp)

    def _flag_key(self, page, off):
        if off < 128:
            return ("L", off) if off in (8, 9) else None
        if page == 0x11 and (135 <= off <= 152):
            return (0x11, off)
        return None

    # --- bus --------------------------------------------------------------
    def _read(self, addr, off, n):
        if addr != ADDR:
            raise I2CError(ENXIO)
        self._render()
        page = self.lower[127]
        out = bytearray()
        for o in range(off, off + n):
            key = self._flag_key(page, o)
            if key is not None:
                out.append(self.latched.pop(key, 0))   # latched flags clear on read
            elif o < 128:
                out.append(self.lower[o])
            else:
                data = self.pages.get(page)
                b = data[o - 128] if data is not None else 0
                if page == 0x00 and o == 222 and self.fault_active("corrupt_checksum"):
                    b ^= 0xFF
                out.append(b)
        return out

    def _write(self, addr, off, data):
        if addr != ADDR:
            raise I2CError(ENXIO)
        for i, b in enumerate(data):
            o = off + i
            page = self.lower[127]
            if o == 127:
                if b in SUPPORTED_PAGES:
                    self.lower[127] = b       # unsupported pages are not selectable
            elif o == 126:
                self.lower[126] = 0           # single-bank module
            elif o == 26:
                self.lower[26] = b & 0x50     # LowPwrRequestSW, ForceLowPwr
            elif page == 0x10 and o == 130:
                self.pages[0x10][2] = b & ((1 << self.lanes) - 1)
        self._step_state()

    # --- control ----------------------------------------------------------
    def set_value(self, field, value, lane=None):
        with self.lock:
            if field not in self.values:
                raise ValueError("unknown field %r (known: %s)" % (field, ", ".join(sorted(self.values))))
            if isinstance(self.values[field], list):
                lanes = range(self.lanes) if lane is None else [int(lane)]
                for l in lanes:
                    self.values[field][l] = float(value)
            else:
                self.values[field] = float(value)

    def describe(self):
        d = super().describe()
        d.update({
            "serial": self.serial,
            "state": STATE_NAMES.get(self.state, str(self.state)),
            "low_power_request": bool(self.lower[26] & 0x10),
            "tx_disable_mask": self.tx_disable_mask(),
            "values": {k: (list(v) if isinstance(v, list) else v) for k, v in self.values.items()},
        })
        return d
