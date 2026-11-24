/*
 * CMIS decoder. Table references are to OIF-CMIS-05.2.
 */
#include "xcvr/cmis.h"

#include <errno.h>
#include <string.h>

#define UP(off) ((off) - 128)

static uint16_t be16(const uint8_t *p)
{
	return (uint16_t)((unsigned)p[0] << 8 | p[1]);
}

static int16_t be16s(const uint8_t *p)
{
	return (int16_t)be16(p);
}

bool cmis_is_cmis_id(uint8_t id)
{
	/* SFF-8024: 0x18 QSFP-DD, 0x19 OSFP, 0x1A SFP-DD, 0x1E QSFP+ with CMIS */
	return id == 0x18 || id == 0x19 || id == 0x1A || id == 0x1E;
}

const char *cmis_module_state_name(int state)
{
	switch (state) {
	case CMIS_STATE_LOW_PWR: return "ModuleLowPwr";
	case CMIS_STATE_PWR_UP:  return "ModulePwrUp";
	case CMIS_STATE_READY:   return "ModuleReady";
	case CMIS_STATE_PWR_DN:  return "ModulePwrDn";
	case CMIS_STATE_FAULT:   return "ModuleFault";
	default:                 return "Reserved";
	}
}

int cmis_module_state(const uint8_t lower[128])
{
	return (lower[CMIS_MODULE_STATE] >> 1) & 0x7;
}

static unsigned lanes_for_id(uint8_t id)
{
	switch (id) {
	case 0x1A: return 2; /* SFP-DD */
	case 0x1E: return 4; /* QSFP+ / QSFP28 form factor */
	default:   return 8; /* QSFP-DD, OSFP */
	}
}

int cmis_parse_info(const struct cmis_image *img, struct xcvr_info *info)
{
	memset(info, 0, sizeof(*info));
	info->type = XCVR_TYPE_CMIS;
	info->identifier = img->lower[CMIS_ID];
	info->spec_rev = img->lower[CMIS_REVISION];
	info->flat_mem = (img->lower[CMIS_MEM_CHAR] & CMIS_MEM_FLAT) != 0;
	info->lanes = lanes_for_id(info->identifier);
	info->bias_multiplier = 1;
	info->cc_base_ok = info->cc_ext_ok = info->cc_dmi_ok = true;

	const uint8_t *p0 = img->p00;
	info->connector = p0[UP(CMIS_P00_CONNECTOR)];
	xcvr_copy_ascii(info->vendor, sizeof(info->vendor), p0 + UP(CMIS_P00_VENDOR_NAME), 16);
	xcvr_copy_ascii(info->part_number, sizeof(info->part_number), p0 + UP(CMIS_P00_VENDOR_PN), 16);
	xcvr_copy_ascii(info->revision, sizeof(info->revision), p0 + UP(CMIS_P00_VENDOR_REV), 2);
	xcvr_copy_ascii(info->serial, sizeof(info->serial), p0 + UP(CMIS_P00_VENDOR_SN), 16);
	xcvr_copy_ascii(info->date_code, sizeof(info->date_code), p0 + UP(CMIS_P00_DATE_CODE), 8);
	memcpy(info->oui, p0 + UP(CMIS_P00_VENDOR_OUI), 3);
	/* Page 00h byte 222: checksum over bytes 128..221. */
	info->cc_base_ok = xcvr_checksum8(p0, UP(CMIS_P00_CHECKSUM)) == p0[UP(CMIS_P00_CHECKSUM)];

	if (info->flat_mem) {
		/* Passive copper: no page 01h/02h, no monitors. */
		info->ddm_supported = false;
		info->soft_tx_disable = false;
	} else {
		const uint8_t *p1 = img->p01;
		info->wavelength_nm = be16(p1 + UP(CMIS_P01_WAVELENGTH)) * 0.05;
		info->ddm_supported = (p1[UP(CMIS_P01_MON_SUPPORT)] & 0x03) != 0;
		unsigned scale = (p1[UP(CMIS_P01_LANE_MON_SUP)] >> 3) & 0x3;
		info->bias_multiplier = scale == 1 ? 2 : scale == 2 ? 4 : 1;
		info->soft_tx_disable = true; /* page 10h output disable is mandatory for paged modules */
	}

	return cmis_is_cmis_id(info->identifier) ? 0 : -EINVAL;
}

static void read_quad(const uint8_t *p, bool is_signed, double scale, struct xcvr_thresholds *t)
{
	double v[4];
	for (int i = 0; i < 4; i++)
		v[i] = (is_signed ? (double)be16s(p + 2 * i) : (double)be16(p + 2 * i)) * scale;
	t->high_alarm = v[0];
	t->low_alarm = v[1];
	t->high_warn = v[2];
	t->low_warn = v[3];
}

void cmis_parse_thresholds(const struct cmis_image *img, const struct xcvr_info *info,
			   struct xcvr_threshold_set *out)
{
	memset(out, 0, sizeof(*out));
	if (info->flat_mem || !info->ddm_supported)
		return;

	const uint8_t *p2 = img->p02;
	read_quad(p2 + UP(CMIS_P02_TEMP), true, 1.0 / 256.0, &out->m[XCVR_M_TEMP]);
	read_quad(p2 + UP(CMIS_P02_VCC), false, 100e-6, &out->m[XCVR_M_VCC]);
	read_quad(p2 + UP(CMIS_P02_TX_POWER), false, 1e-4, &out->m[XCVR_M_TX_POWER]);
	read_quad(p2 + UP(CMIS_P02_TX_BIAS), false, 2e-3 * info->bias_multiplier,
		  &out->m[XCVR_M_TX_BIAS]);
	read_quad(p2 + UP(CMIS_P02_RX_POWER), false, 1e-4, &out->m[XCVR_M_RX_POWER]);
	out->valid = true;
}

/* Collect one bit per lane from four consecutive flag bytes (HA, LA, HW, LW). */
static void lane_flags(const uint8_t *p, unsigned lanes, uint8_t out[XCVR_MAX_LANES])
{
	static const uint8_t bits[4] = {
		XCVR_FLAG_HIGH_ALARM, XCVR_FLAG_LOW_ALARM, XCVR_FLAG_HIGH_WARN, XCVR_FLAG_LOW_WARN,
	};
	for (unsigned l = 0; l < lanes; l++) {
		uint8_t f = 0;
		for (int i = 0; i < 4; i++)
			if (p[i] & (1u << l))
				f |= bits[i];
		out[l] = f;
	}
}

void cmis_parse_dom(const struct cmis_image *img, const struct xcvr_info *info,
		    struct xcvr_dom *out)
{
	memset(out, 0, sizeof(*out));
	out->lanes = info->lanes;
	out->module_state = cmis_module_state(img->lower);
	out->data_ready = out->module_state == CMIS_STATE_READY;

	out->temp_c = be16s(img->lower + CMIS_TEMP_MON) / 256.0;
	out->vcc_v = be16(img->lower + CMIS_VCC_MON) * 100e-6;

	/* Lower page byte 9 (Table 8-10): bits 0..3 temp HA/LA/HW/LW, bits 4..7 Vcc. */
	uint8_t tv = img->lower[CMIS_MODULE_FLAGS_TV];
	static const uint8_t order[4] = {
		XCVR_FLAG_HIGH_ALARM, XCVR_FLAG_LOW_ALARM, XCVR_FLAG_HIGH_WARN, XCVR_FLAG_LOW_WARN,
	};
	for (int i = 0; i < 4; i++) {
		if (tv & (1u << i))
			out->flags[XCVR_M_TEMP][0] |= order[i];
		if (tv & (1u << (i + 4)))
			out->flags[XCVR_M_VCC][0] |= order[i];
	}

	if (info->flat_mem)
		return;

	const uint8_t *p10 = img->p10;
	const uint8_t *p11 = img->p11;
	double bias_lsb = 2e-3 * info->bias_multiplier;
	for (unsigned l = 0; l < out->lanes; l++) {
		out->tx_power_mw[l] = be16(p11 + UP(CMIS_P11_TX_POWER) + 2 * l) * 1e-4;
		out->tx_bias_ma[l] = be16(p11 + UP(CMIS_P11_TX_BIAS) + 2 * l) * bias_lsb;
		out->rx_power_mw[l] = be16(p11 + UP(CMIS_P11_RX_POWER) + 2 * l) * 1e-4;
		out->tx_disabled[l] = (p10[UP(CMIS_P10_TX_DISABLE)] >> l) & 1u;
		out->tx_fault[l] = (p11[UP(CMIS_P11_TX_FAULT)] >> l) & 1u;
		out->rx_los[l] = (p11[UP(CMIS_P11_RX_LOS)] >> l) & 1u;
	}
	lane_flags(p11 + UP(CMIS_P11_TX_PWR_HA), out->lanes, out->flags[XCVR_M_TX_POWER]);
	lane_flags(p11 + UP(CMIS_P11_TX_BIAS_HA), out->lanes, out->flags[XCVR_M_TX_BIAS]);
	lane_flags(p11 + UP(CMIS_P11_RX_PWR_HA), out->lanes, out->flags[XCVR_M_RX_POWER]);
}
