/*
 * SFF-8472 decoder. Section/table numbers refer to SFF-8472 Rev 12.4.
 *
 * Raw diagnostic units (Section 9.2):
 *   temperature  signed 16-bit, 1/256 C per LSB
 *   Vcc          unsigned 16-bit, 100 uV per LSB
 *   Tx bias      unsigned 16-bit, 2 uA per LSB
 *   Tx/Rx power  unsigned 16-bit, 0.1 uW per LSB
 *
 * Externally calibrated modules (A0h byte 92 bit 4) report raw A/D counts
 * that must be converted with the constants at A2h 56..91 (Section 9.3).
 * Thresholds are stored in the same format as the live values, so the same
 * conversion is applied to both.
 */
#include "xcvr/sff8472.h"

#include <errno.h>
#include <string.h>

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static int16_t be16s(const uint8_t *p)
{
	return (int16_t)be16(p);
}

/* IEEE-754 single precision, big-endian (used by Rx_PWR(0..4)). */
static float be_float(const uint8_t *p)
{
	uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
	float f;
	memcpy(&f, &u, sizeof(f));
	return f;
}

int sff8472_parse_a0(const uint8_t a0[256], struct xcvr_info *info)
{
	memset(info, 0, sizeof(*info));
	info->type = XCVR_TYPE_SFP;
	info->identifier = a0[SFF8472_ID];
	info->connector = a0[SFF8472_CONNECTOR];
	info->lanes = 1;
	info->bias_multiplier = 1;

	xcvr_copy_ascii(info->vendor, sizeof(info->vendor), a0 + SFF8472_VENDOR_NAME, 16);
	xcvr_copy_ascii(info->part_number, sizeof(info->part_number), a0 + SFF8472_VENDOR_PN, 16);
	xcvr_copy_ascii(info->revision, sizeof(info->revision), a0 + SFF8472_VENDOR_REV, 4);
	xcvr_copy_ascii(info->serial, sizeof(info->serial), a0 + SFF8472_VENDOR_SN, 16);
	xcvr_copy_ascii(info->date_code, sizeof(info->date_code), a0 + SFF8472_DATE_CODE, 8);
	memcpy(info->oui, a0 + SFF8472_VENDOR_OUI, 3);
	info->wavelength_nm = be16(a0 + SFF8472_WAVELENGTH);

	uint8_t diag = a0[SFF8472_DIAG_TYPE];
	info->ddm_supported = (diag & SFF8472_DIAG_DDM) != 0;
	info->ext_calibrated = (diag & SFF8472_DIAG_EXT_CAL) != 0;
	info->soft_tx_disable = (a0[SFF8472_ENHANCED_OPTS] & SFF8472_EOPT_SOFT_TXDIS) != 0;
	info->spec_rev = a0[SFF8472_COMPLIANCE];

	info->cc_base_ok = xcvr_checksum8(a0, SFF8472_CC_BASE) == a0[SFF8472_CC_BASE];
	info->cc_ext_ok = xcvr_checksum8(a0 + 64, SFF8472_CC_EXT - 64) == a0[SFF8472_CC_EXT];
	info->cc_dmi_ok = true; /* until A2h has been checked */

	/* SFF-8472 covers SFP (0x03) and modules soldered to the board (0x02). */
	if (info->identifier != 0x03 && info->identifier != 0x02)
		return -EINVAL;
	return 0;
}

void sff8472_check_a2(const uint8_t a2[256], struct xcvr_info *info)
{
	info->cc_dmi_ok = !info->ddm_supported ||
			  xcvr_checksum8(a2, SFF8472_CC_DMI) == a2[SFF8472_CC_DMI];
}

/* Linear external calibration: value = slope * raw + offset, where slope is
 * unsigned 8.8 fixed point and offset a signed 16-bit count (Table 9-6). */
static double ext_linear(const uint8_t *a2, unsigned slope_off, double raw)
{
	double slope = be16(a2 + slope_off) / 256.0;
	double offset = be16s(a2 + slope_off + 2);
	return slope * raw + offset;
}

/* Rx power external calibration: 4th order polynomial in raw counts. */
static double ext_rx_power(const uint8_t *a2, double raw)
{
	double sum = 0.0;
	double x = 1.0;
	for (int i = 0; i <= 4; i++) {
		/* Rx_PWR(4) is stored first, Rx_PWR(0) last. */
		sum += (double)be_float(a2 + SFF8472_RX_PWR4 + 4 * (4 - i)) * x;
		x *= raw;
	}
	return sum < 0.0 ? 0.0 : sum;
}

enum { CAL_TXI = 76, CAL_TXP = 80, CAL_T = 84, CAL_V = 88 };

/* Convert one raw 16-bit field of the given metric to engineering units. */
static double convert(const uint8_t *a2, const struct xcvr_info *info,
		      enum xcvr_metric m, const uint8_t *raw)
{
	bool ext = info->ext_calibrated;
	switch (m) {
	case XCVR_M_TEMP: {
		double v = be16s(raw);
		if (ext)
			v = ext_linear(a2, CAL_T, v);
		return v / 256.0;
	}
	case XCVR_M_VCC: {
		double v = be16(raw);
		if (ext)
			v = ext_linear(a2, CAL_V, v);
		return v * 100e-6;
	}
	case XCVR_M_TX_BIAS: {
		double v = be16(raw);
		if (ext)
			v = ext_linear(a2, CAL_TXI, v);
		return v * 2e-3;           /* 2 uA -> mA */
	}
	case XCVR_M_TX_POWER: {
		double v = be16(raw);
		if (ext)
			v = ext_linear(a2, CAL_TXP, v);
		return v * 1e-4;           /* 0.1 uW -> mW */
	}
	case XCVR_M_RX_POWER: {
		double v = be16(raw);
		if (ext)
			v = ext_rx_power(a2, v);
		return v * 1e-4;
	}
	default:
		return 0.0;
	}
}

void sff8472_parse_thresholds(const uint8_t a2[256], const struct xcvr_info *info,
			      struct xcvr_threshold_set *out)
{
	memset(out, 0, sizeof(*out));
	if (!info->ddm_supported)
		return;

	/* Table 9-5: five metrics x {high alarm, low alarm, high warn, low warn}, 2 bytes each. */
	for (int m = 0; m < XCVR_M_COUNT; m++) {
		const uint8_t *base = a2 + SFF8472_THRESHOLDS + 8 * m;
		struct xcvr_thresholds *t = &out->m[m];
		t->high_alarm = convert(a2, info, (enum xcvr_metric)m, base + 0);
		t->low_alarm  = convert(a2, info, (enum xcvr_metric)m, base + 2);
		t->high_warn  = convert(a2, info, (enum xcvr_metric)m, base + 4);
		t->low_warn   = convert(a2, info, (enum xcvr_metric)m, base + 6);
	}
	out->valid = true;
}

/* Flag bit positions in A2h bytes 112/113 (alarms) and 116/117 (warnings),
 * Table 9-12: {byte index, high bit, low bit} for each metric. */
static const struct { uint8_t byte, hi, lo; } flag_map[XCVR_M_COUNT] = {
	[XCVR_M_TEMP]     = { 0, 7, 6 },
	[XCVR_M_VCC]      = { 0, 5, 4 },
	[XCVR_M_TX_BIAS]  = { 0, 3, 2 },
	[XCVR_M_TX_POWER] = { 0, 1, 0 },
	[XCVR_M_RX_POWER] = { 1, 7, 6 },
};

void sff8472_parse_dom(const uint8_t a2[256], const struct xcvr_info *info,
		       struct xcvr_dom *out)
{
	memset(out, 0, sizeof(*out));
	out->lanes = 1;

	uint8_t sc = a2[SFF8472_STATUS_CONTROL];
	out->tx_disabled[0] = (sc & (SFF8472_SC_TX_DISABLE_STATE | SFF8472_SC_SOFT_TX_DISABLE)) != 0;
	out->tx_fault[0] = (sc & SFF8472_SC_TX_FAULT) != 0;
	out->rx_los[0] = (sc & SFF8472_SC_RX_LOS) != 0;
	out->data_ready = (sc & SFF8472_SC_DATA_NOT_READY) == 0;

	if (!info->ddm_supported)
		return;

	const uint8_t *d = a2 + SFF8472_DIAG;
	out->temp_c = convert(a2, info, XCVR_M_TEMP, d + 0);
	out->vcc_v = convert(a2, info, XCVR_M_VCC, d + 2);
	out->tx_bias_ma[0] = convert(a2, info, XCVR_M_TX_BIAS, d + 4);
	out->tx_power_mw[0] = convert(a2, info, XCVR_M_TX_POWER, d + 6);
	out->rx_power_mw[0] = convert(a2, info, XCVR_M_RX_POWER, d + 8);

	for (int m = 0; m < XCVR_M_COUNT; m++) {
		uint8_t alarm = a2[SFF8472_ALARM_FLAGS + flag_map[m].byte];
		uint8_t warn = a2[SFF8472_WARN_FLAGS + flag_map[m].byte];
		uint8_t f = 0;
		if (alarm & (1u << flag_map[m].hi)) f |= XCVR_FLAG_HIGH_ALARM;
		if (alarm & (1u << flag_map[m].lo)) f |= XCVR_FLAG_LOW_ALARM;
		if (warn & (1u << flag_map[m].hi))  f |= XCVR_FLAG_HIGH_WARN;
		if (warn & (1u << flag_map[m].lo))  f |= XCVR_FLAG_LOW_WARN;
		out->flags[m][0] = f;
	}
}
