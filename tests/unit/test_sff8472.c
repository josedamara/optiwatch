#include "test.h"

#include <errno.h>

#include "xcvr/sff8472.h"

static void test_helpers(void)
{
	CHECK_NEAR(xcvr_mw_to_dbm(1.0), 0.0, 1e-9);
	CHECK_NEAR(xcvr_mw_to_dbm(0.5012), -3.0, 0.01);
	CHECK_NEAR(xcvr_mw_to_dbm(0.0), XCVR_DBM_FLOOR, 1e-9);
	CHECK_NEAR(xcvr_dbm_to_mw(-10.0), 0.1, 1e-12);

	uint8_t buf[] = { 1, 2, 250, 10 };
	CHECK_EQ_INT(xcvr_checksum8(buf, 4), 7); /* 263 mod 256 */

	char s[8];
	const uint8_t raw[6] = { 'A', 'B', ' ', 'C', ' ', ' ' };
	xcvr_copy_ascii(s, sizeof(s), raw, 6);
	CHECK_STR(s, "AB C");
	const uint8_t bad[3] = { 'X', 0x01, 0xFF };
	xcvr_copy_ascii(s, sizeof(s), bad, 3);
	CHECK_STR(s, "X??");
}

static void test_parse_a0(void)
{
	uint8_t a0[256];
	struct xcvr_info info;
	fx_sfp_a0(a0, 0);

	CHECK_EQ_INT(sff8472_parse_a0(a0, &info), 0);
	CHECK_EQ_INT(info.type, XCVR_TYPE_SFP);
	CHECK_STR(info.vendor, "OPTIWATCH LABS");
	CHECK_STR(info.part_number, "OW-SFP10G-SR");
	CHECK_STR(info.revision, "A1");
	CHECK_STR(info.serial, "OW24110001");
	CHECK_STR(info.date_code, "241105");
	CHECK_NEAR(info.wavelength_nm, 850, 0);
	CHECK_EQ_INT(info.connector, 0x07);
	CHECK_EQ_INT(info.oui[1], 0x1B);
	CHECK(info.ddm_supported);
	CHECK(!info.ext_calibrated);
	CHECK(info.soft_tx_disable);
	CHECK(info.cc_base_ok);
	CHECK(info.cc_ext_ok);
	CHECK_EQ_INT(info.lanes, 1);
}

static void test_checksum_errors(void)
{
	uint8_t a0[256];
	struct xcvr_info info;
	fx_sfp_a0(a0, 0);
	a0[SFF8472_VENDOR_NAME] ^= 0x20;  /* corrupt base area */
	a0[SFF8472_VENDOR_SN + 3] ^= 0x01; /* corrupt extended area */
	sff8472_parse_a0(a0, &info);
	CHECK(!info.cc_base_ok);
	CHECK(!info.cc_ext_ok);

	uint8_t a2[256];
	fx_sfp_a0(a0, 0);
	sff8472_parse_a0(a0, &info);
	fx_sfp_a2(a2);
	sff8472_check_a2(a2, &info);
	CHECK(info.cc_dmi_ok);
	a2[3] ^= 0x80;
	sff8472_check_a2(a2, &info);
	CHECK(!info.cc_dmi_ok);
}

static void test_not_sfp(void)
{
	uint8_t a0[256];
	struct xcvr_info info;
	fx_sfp_a0(a0, 0);
	a0[0] = 0x11; /* QSFP28 is SFF-8636, not SFF-8472 */
	CHECK_EQ_INT(sff8472_parse_a0(a0, &info), -EINVAL);
}

static void test_internal_cal_dom(void)
{
	uint8_t a0[256], a2[256];
	struct xcvr_info info;
	struct xcvr_dom dom;
	fx_sfp_a0(a0, 0);
	fx_sfp_a2(a2);
	sff8472_parse_a0(a0, &info);

	a2[SFF8472_STATUS_CONTROL] = SFF8472_SC_RX_LOS;
	a2[SFF8472_ALARM_FLAGS] = 0x80;     /* temp high alarm */
	a2[SFF8472_ALARM_FLAGS + 1] = 0x40; /* rx power low alarm */
	a2[SFF8472_WARN_FLAGS] = 0x08;      /* bias high warning */
	sff8472_parse_dom(a2, &info, &dom);

	CHECK_NEAR(dom.temp_c, 35.5, 1e-9);
	CHECK_NEAR(dom.vcc_v, 3.3, 1e-9);
	CHECK_NEAR(dom.tx_bias_ma[0], 6.0, 1e-9);
	CHECK_NEAR(dom.tx_power_mw[0], 0.5012, 1e-9);
	CHECK_NEAR(dom.rx_power_mw[0], 0.2, 1e-9);
	CHECK(dom.rx_los[0]);
	CHECK(!dom.tx_fault[0]);
	CHECK(!dom.tx_disabled[0]);
	CHECK(dom.data_ready);
	CHECK_EQ_INT(dom.flags[XCVR_M_TEMP][0], XCVR_FLAG_HIGH_ALARM);
	CHECK_EQ_INT(dom.flags[XCVR_M_RX_POWER][0], XCVR_FLAG_LOW_ALARM);
	CHECK_EQ_INT(dom.flags[XCVR_M_TX_BIAS][0], XCVR_FLAG_HIGH_WARN);
	CHECK_EQ_INT(dom.flags[XCVR_M_VCC][0], 0);
}

static void test_negative_temperature(void)
{
	uint8_t a0[256], a2[256];
	struct xcvr_info info;
	struct xcvr_dom dom;
	fx_sfp_a0(a0, 0);
	fx_sfp_a2(a2);
	sff8472_parse_a0(a0, &info);
	fx_put16(a2 + 96, 0xF600); /* -10.0 C */
	sff8472_parse_dom(a2, &info, &dom);
	CHECK_NEAR(dom.temp_c, -10.0, 1e-9);
}

static void test_thresholds(void)
{
	uint8_t a0[256], a2[256];
	struct xcvr_info info;
	struct xcvr_threshold_set th;
	fx_sfp_a0(a0, 0);
	fx_sfp_a2(a2);
	sff8472_parse_a0(a0, &info);
	sff8472_parse_thresholds(a2, &info, &th);

	CHECK(th.valid);
	CHECK_NEAR(th.m[XCVR_M_TEMP].high_alarm, 75, 1e-9);
	CHECK_NEAR(th.m[XCVR_M_TEMP].low_alarm, -5, 1e-9);
	CHECK_NEAR(th.m[XCVR_M_VCC].low_warn, 3.1, 1e-9);
	CHECK_NEAR(th.m[XCVR_M_TX_BIAS].high_warn, 10.0, 1e-9);
	CHECK_NEAR(th.m[XCVR_M_TX_POWER].low_warn, 0.125, 1e-9);
	CHECK_NEAR(th.m[XCVR_M_RX_POWER].low_alarm, 0.02, 1e-9);
}

static void put_float(uint8_t *p, float f)
{
	uint32_t u;
	memcpy(&u, &f, 4);
	p[0] = (uint8_t)(u >> 24);
	p[1] = (uint8_t)(u >> 16);
	p[2] = (uint8_t)(u >> 8);
	p[3] = (uint8_t)u;
}

static void test_external_calibration(void)
{
	uint8_t a0[256], a2[256];
	struct xcvr_info info;
	struct xcvr_dom dom;
	fx_sfp_a0(a0, 1);
	memset(a2, 0, sizeof(a2));
	sff8472_parse_a0(a0, &info);
	CHECK(info.ext_calibrated);

	/* Rx_PWR = 0 + 2*raw (only Rx_PWR(1) set) */
	put_float(a2 + 56 + 12, 2.0f);
	/* Tx_I: slope 1.5 (0x0180), offset +100 */
	fx_put16(a2 + 76, 0x0180);
	fx_put16(a2 + 78, 100);
	/* Tx_PWR: slope 1.0, offset 0 */
	fx_put16(a2 + 80, 0x0100);
	/* T: slope 1.0, offset -512 counts (-2 C) */
	fx_put16(a2 + 84, 0x0100);
	fx_put16(a2 + 86, (unsigned)(-512) & 0xFFFF);
	/* V: slope 0.5, offset 0 */
	fx_put16(a2 + 88, 0x0080);

	fx_put16(a2 + 96, 30 * 256); /* raw 30 C -> 28 C */
	fx_put16(a2 + 98, 60000);    /* raw -> 30000 -> 3.0 V */
	fx_put16(a2 + 100, 2000);    /* 1.5*2000+100 = 3100 -> 6.2 mA */
	fx_put16(a2 + 102, 4000);    /* 0.4 mW */
	fx_put16(a2 + 104, 1500);    /* 2*1500 = 3000 -> 0.3 mW */
	sff8472_parse_dom(a2, &info, &dom);

	CHECK_NEAR(dom.temp_c, 28.0, 1e-9);
	CHECK_NEAR(dom.vcc_v, 3.0, 1e-9);
	CHECK_NEAR(dom.tx_bias_ma[0], 6.2, 1e-9);
	CHECK_NEAR(dom.tx_power_mw[0], 0.4, 1e-9);
	CHECK_NEAR(dom.rx_power_mw[0], 0.3, 1e-6);

	/* Thresholds go through the same conversion. */
	fx_put16(a2 + 0, 80 * 256); /* temp HA raw 80 -> 78 C */
	struct xcvr_threshold_set th;
	sff8472_parse_thresholds(a2, &info, &th);
	CHECK_NEAR(th.m[XCVR_M_TEMP].high_alarm, 78.0, 1e-9);
}

static void test_no_ddm(void)
{
	uint8_t a0[256], a2[256];
	struct xcvr_info info;
	struct xcvr_dom dom;
	struct xcvr_threshold_set th;
	fx_sfp_a0(a0, 0);
	a0[SFF8472_DIAG_TYPE] = 0;
	fx_sfp_a2(a2);
	sff8472_parse_a0(a0, &info);
	CHECK(!info.ddm_supported);
	sff8472_parse_thresholds(a2, &info, &th);
	CHECK(!th.valid);
	sff8472_parse_dom(a2, &info, &dom);
	CHECK_NEAR(dom.temp_c, 0.0, 0);
}

const struct test_case sff8472_tests[] = {
	{ "helpers", test_helpers },
	{ "parse_a0", test_parse_a0 },
	{ "checksum_errors", test_checksum_errors },
	{ "not_sfp", test_not_sfp },
	{ "internal_cal_dom", test_internal_cal_dom },
	{ "negative_temperature", test_negative_temperature },
	{ "thresholds", test_thresholds },
	{ "external_calibration", test_external_calibration },
	{ "no_ddm", test_no_ddm },
	{ NULL, NULL },
};
