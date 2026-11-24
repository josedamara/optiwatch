/* Hand-built EEPROM images with known contents. */
#include "test.h"

#include "xcvr/bus.h"
#include "xcvr/cmis.h"
#include "xcvr/sff8472.h"

void fx_put16(uint8_t *p, unsigned v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)v;
}

void fx_put_ascii(uint8_t *p, const char *s, size_t width)
{
	size_t n = strlen(s);
	for (size_t i = 0; i < width; i++)
		p[i] = i < n ? (uint8_t)s[i] : ' ';
}

/* 10G-SR style SFP+, 850 nm, LC, internally or externally calibrated. */
void fx_sfp_a0(uint8_t a0[256], int ext_cal)
{
	memset(a0, 0, 256);
	a0[SFF8472_ID] = 0x03;
	a0[SFF8472_EXT_ID] = 0x04;
	a0[SFF8472_CONNECTOR] = 0x07;
	a0[3] = 0x10; /* 10GBASE-SR */
	fx_put_ascii(a0 + SFF8472_VENDOR_NAME, "OPTIWATCH LABS", 16);
	a0[SFF8472_VENDOR_OUI] = 0x00;
	a0[SFF8472_VENDOR_OUI + 1] = 0x1B;
	a0[SFF8472_VENDOR_OUI + 2] = 0x21;
	fx_put_ascii(a0 + SFF8472_VENDOR_PN, "OW-SFP10G-SR", 16);
	fx_put_ascii(a0 + SFF8472_VENDOR_REV, "A1", 4);
	fx_put16(a0 + SFF8472_WAVELENGTH, 850);
	a0[SFF8472_CC_BASE] = xcvr_checksum8(a0, 63);
	fx_put_ascii(a0 + SFF8472_VENDOR_SN, "OW24110001", 16);
	fx_put_ascii(a0 + SFF8472_DATE_CODE, "241105  ", 8);
	a0[SFF8472_DIAG_TYPE] = (uint8_t)(SFF8472_DIAG_DDM |
		(ext_cal ? SFF8472_DIAG_EXT_CAL : SFF8472_DIAG_INT_CAL));
	a0[SFF8472_ENHANCED_OPTS] = SFF8472_EOPT_ALARM_FLAGS | SFF8472_EOPT_SOFT_TXDIS;
	a0[SFF8472_COMPLIANCE] = 0x08; /* Rev 12.3+ */
	a0[SFF8472_CC_EXT] = xcvr_checksum8(a0 + 64, 31);
}

/* Internally calibrated A2h:
 *   temp 35.5 C, Vcc 3.30 V, bias 6.0 mA, Tx 0.5012 mW (-3 dBm), Rx 0.2 mW (-7 dBm)
 * Thresholds: temp 75/-5/70/0, Vcc 3.6/3.0/3.5/3.1, bias 12/2/10/3 mA,
 *             Tx 1.0/0.1/0.8/0.125 mW, Rx 1.0/0.02/0.8/0.025 mW */
void fx_sfp_a2(uint8_t a2[256])
{
	memset(a2, 0, 256);
	static const unsigned th[5][4] = {
		{ 75 * 256, (unsigned)(-5 * 256) & 0xFFFF, 70 * 256, 0 },
		{ 36000, 30000, 35000, 31000 },
		{ 6000, 1000, 5000, 1500 },
		{ 10000, 1000, 8000, 1250 },
		{ 10000, 200, 8000, 250 },
	};
	for (int m = 0; m < 5; m++)
		for (int i = 0; i < 4; i++)
			fx_put16(a2 + 8 * m + 2 * i, th[m][i]);

	a2[SFF8472_CC_DMI] = xcvr_checksum8(a2, 95);

	fx_put16(a2 + 96, (unsigned)(35.5 * 256));
	fx_put16(a2 + 98, 33000);
	fx_put16(a2 + 100, 3000);
	fx_put16(a2 + 102, 5012);
	fx_put16(a2 + 104, 2000);
}

static void put_quad(uint8_t *p, const unsigned v[4])
{
	for (int i = 0; i < 4; i++)
		fx_put16(p + 2 * i, v[i]);
}

/* 400G QSFP-DD, 8 lanes, paged memory, ModuleReady.
 * Module: 40 C, 3.29 V. Lane n: Tx 1.0 mW, bias (7 + n) mA with 2x scale, Rx 0.5 mW.
 * Lane 2 has Rx LOS and a latched Rx power low alarm. Lane 5 is Tx disabled. */
void fx_cmis_fill(xcvr_bus *bus)
{
	uint8_t *lo = xcvr_mem_cmis_page(bus, 0xFF);
	lo[CMIS_ID] = 0x18;
	lo[CMIS_REVISION] = 0x50;
	lo[CMIS_MEM_CHAR] = 0x00;
	lo[CMIS_MODULE_STATE] = CMIS_STATE_READY << 1;
	lo[CMIS_MODULE_FLAGS_TV] = 0x04; /* temp high warning */
	fx_put16(lo + CMIS_TEMP_MON, 40 * 256);
	fx_put16(lo + CMIS_VCC_MON, 32900);

	uint8_t *p0 = xcvr_mem_cmis_page(bus, 0x00);
	p0[CMIS_P00_ID - 128] = 0x18;
	fx_put_ascii(p0 + CMIS_P00_VENDOR_NAME - 128, "OPTIWATCH LABS", 16);
	fx_put_ascii(p0 + CMIS_P00_VENDOR_PN - 128, "OW-QDD-400G-DR4", 16);
	fx_put_ascii(p0 + CMIS_P00_VENDOR_REV - 128, "B0", 2);
	fx_put_ascii(p0 + CMIS_P00_VENDOR_SN - 128, "OWQ2400777", 16);
	fx_put_ascii(p0 + CMIS_P00_DATE_CODE - 128, "240901  ", 8);
	p0[CMIS_P00_CONNECTOR - 128] = 0x0C;
	p0[CMIS_P00_CHECKSUM - 128] = xcvr_checksum8(p0, CMIS_P00_CHECKSUM - 128);

	uint8_t *p1 = xcvr_mem_cmis_page(bus, 0x01);
	fx_put16(p1 + CMIS_P01_WAVELENGTH - 128, 1311 * 20); /* 1311 nm in 0.05 nm */
	p1[CMIS_P01_MON_SUPPORT - 128] = 0x03;
	p1[CMIS_P01_LANE_MON_SUP - 128] = 0x07 | (1u << 3); /* bias x2 */

	uint8_t *p2 = xcvr_mem_cmis_page(bus, 0x02);
	put_quad(p2 + CMIS_P02_TEMP - 128, (const unsigned[]){ 80 * 256, (unsigned)(-10 * 256) & 0xFFFF, 75 * 256, 0 });
	put_quad(p2 + CMIS_P02_VCC - 128, (const unsigned[]){ 36000, 30000, 35000, 31000 });
	put_quad(p2 + CMIS_P02_TX_POWER - 128, (const unsigned[]){ 30000, 1000, 25000, 1500 });
	put_quad(p2 + CMIS_P02_TX_BIAS - 128, (const unsigned[]){ 7500, 1000, 7000, 1500 }); /* x2 scale */
	put_quad(p2 + CMIS_P02_RX_POWER - 128, (const unsigned[]){ 30000, 300, 25000, 500 });

	uint8_t *p10 = xcvr_mem_cmis_page(bus, 0x10);
	p10[CMIS_P10_TX_DISABLE - 128] = 1u << 5;

	uint8_t *p11 = xcvr_mem_cmis_page(bus, 0x11);
	for (unsigned l = 0; l < 8; l++) {
		fx_put16(p11 + CMIS_P11_TX_POWER - 128 + 2 * l, 10000);
		fx_put16(p11 + CMIS_P11_TX_BIAS - 128 + 2 * l, (7 + l) * 1000 / 4); /* 2 uA x 2 */
		fx_put16(p11 + CMIS_P11_RX_POWER - 128 + 2 * l, 5000);
	}
	p11[CMIS_P11_RX_LOS - 128] = 1u << 2;
	p11[CMIS_P11_RX_PWR_HA - 128 + 1] = 1u << 2; /* low alarm, lane 2 */
}
