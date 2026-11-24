#include "xcvr/types.h"

#include <math.h>
#include <string.h>

double xcvr_mw_to_dbm(double mw)
{
	if (!(mw > 0.0))
		return XCVR_DBM_FLOOR;
	double dbm = 10.0 * log10(mw);
	return dbm < XCVR_DBM_FLOOR ? XCVR_DBM_FLOOR : dbm;
}

double xcvr_dbm_to_mw(double dbm)
{
	return pow(10.0, dbm / 10.0);
}

const char *xcvr_type_name(enum xcvr_type t)
{
	switch (t) {
	case XCVR_TYPE_SFP:  return "SFF-8472";
	case XCVR_TYPE_CMIS: return "CMIS";
	default:             return "unknown";
	}
}

const char *xcvr_metric_name(enum xcvr_metric m)
{
	static const char *const names[XCVR_M_COUNT] = {
		"temperature", "vcc", "tx_bias", "tx_power", "rx_power",
	};
	return (unsigned)m < XCVR_M_COUNT ? names[m] : "?";
}

const char *xcvr_metric_unit(enum xcvr_metric m)
{
	static const char *const units[XCVR_M_COUNT] = { "C", "V", "mA", "mW", "mW" };
	return (unsigned)m < XCVR_M_COUNT ? units[m] : "";
}

/* SFF-8024 Table 4-1 (subset) */
const char *xcvr_identifier_name(uint8_t id)
{
	switch (id) {
	case 0x00: return "Unknown";
	case 0x02: return "Module soldered to motherboard";
	case 0x03: return "SFP/SFP+/SFP28";
	case 0x0C: return "QSFP";
	case 0x0D: return "QSFP+";
	case 0x11: return "QSFP28";
	case 0x18: return "QSFP-DD";
	case 0x19: return "OSFP";
	case 0x1A: return "SFP-DD";
	case 0x1E: return "QSFP+ (CMIS)";
	default:   return "Reserved/vendor specific";
	}
}

/* SFF-8024 Table 4-3 (subset) */
const char *xcvr_connector_name(uint8_t code)
{
	switch (code) {
	case 0x00: return "Unknown";
	case 0x01: return "SC";
	case 0x07: return "LC";
	case 0x0B: return "Optical pigtail";
	case 0x0C: return "MPO 1x12";
	case 0x0D: return "MPO 2x16";
	case 0x21: return "Copper pigtail";
	case 0x22: return "RJ45";
	case 0x23: return "No separable connector";
	case 0x24: return "MXC 2x16";
	case 0x25: return "CS optical";
	case 0x26: return "SN optical";
	case 0x27: return "MPO 2x12";
	case 0x28: return "MPO 1x16";
	default:   return "Reserved/vendor specific";
	}
}

uint8_t xcvr_checksum8(const uint8_t *p, size_t n)
{
	unsigned sum = 0;
	for (size_t i = 0; i < n; i++)
		sum += p[i];
	return (uint8_t)(sum & 0xFFu);
}

void xcvr_copy_ascii(char *dst, size_t dstlen, const uint8_t *src, size_t srclen)
{
	if (dstlen == 0)
		return;
	size_t n = srclen < dstlen - 1 ? srclen : dstlen - 1;
	for (size_t i = 0; i < n; i++) {
		uint8_t c = src[i];
		dst[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
	}
	dst[n] = '\0';
	while (n > 0 && dst[n - 1] == ' ')
		dst[--n] = '\0';
}
