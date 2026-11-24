/*
 * Common transceiver data types shared by the SFF-8472 and CMIS decoders.
 *
 * All decoded values are in engineering units:
 *   temperature  degrees C
 *   supply       volts
 *   bias current milliamps
 *   optical pwr  milliwatts (use xcvr_mw_to_dbm() for dBm)
 */
#ifndef XCVR_TYPES_H
#define XCVR_TYPES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XCVR_MAX_LANES 8

enum xcvr_type {
	XCVR_TYPE_UNKNOWN = 0,
	XCVR_TYPE_SFP,  /* SFF-8472 two-wire map at 0xA0/0xA2 */
	XCVR_TYPE_CMIS, /* OIF CMIS paged map at 0xA0 (QSFP-DD, OSFP, QSFP+CMIS) */
};

/* Monitored quantities. TEMP and VCC are module-wide, the rest per lane. */
enum xcvr_metric {
	XCVR_M_TEMP = 0,
	XCVR_M_VCC,
	XCVR_M_TX_BIAS,
	XCVR_M_TX_POWER,
	XCVR_M_RX_POWER,
	XCVR_M_COUNT
};

/* Bits used in xcvr_dom.flags[][] for module-reported alarm/warning flags. */
enum {
	XCVR_FLAG_HIGH_ALARM = 1u << 0,
	XCVR_FLAG_LOW_ALARM  = 1u << 1,
	XCVR_FLAG_HIGH_WARN  = 1u << 2,
	XCVR_FLAG_LOW_WARN   = 1u << 3,
};

struct xcvr_thresholds {
	double high_alarm;
	double low_alarm;
	double high_warn;
	double low_warn;
};

struct xcvr_threshold_set {
	bool valid;
	struct xcvr_thresholds m[XCVR_M_COUNT];
};

/* Static identity read once at insertion. */
struct xcvr_info {
	enum xcvr_type type;
	uint8_t identifier;     /* SFF-8024 identifier byte */
	uint8_t connector;      /* SFF-8024 connector code */
	char vendor[17];
	char part_number[17];
	char revision[5];
	char serial[17];
	char date_code[9];      /* YYMMDDLL as stored, NUL terminated */
	uint8_t oui[3];
	double wavelength_nm;   /* 0 if not specified */
	unsigned lanes;         /* 1 for SFP, 1..8 for CMIS */

	bool ddm_supported;     /* module implements digital diagnostics */
	bool ext_calibrated;    /* SFF-8472 external calibration in use */
	bool soft_tx_disable;   /* TX can be disabled through the management interface */
	bool flat_mem;          /* CMIS: only the lower page + page 00h exist */
	uint8_t spec_rev;       /* SFF-8472 byte 94 or CMIS revision byte */
	unsigned bias_multiplier; /* CMIS Tx bias scaling (1, 2 or 4) */

	bool cc_base_ok;        /* checksum results; true when not applicable */
	bool cc_ext_ok;
	bool cc_dmi_ok;
};

/* Live readings, refreshed on every poll. */
struct xcvr_dom {
	double temp_c;
	double vcc_v;
	unsigned lanes;
	double tx_bias_ma[XCVR_MAX_LANES];
	double tx_power_mw[XCVR_MAX_LANES];
	double rx_power_mw[XCVR_MAX_LANES];

	bool tx_disabled[XCVR_MAX_LANES];
	bool tx_fault[XCVR_MAX_LANES];
	bool rx_los[XCVR_MAX_LANES];

	/* Module-reported alarm/warning flags: flags[metric][lane]. TEMP and VCC use lane 0. */
	uint8_t flags[XCVR_M_COUNT][XCVR_MAX_LANES];

	int module_state;       /* CMIS module state (enum cmis_module_state); 0 for SFP */
	bool data_ready;        /* SFP: !Data_Ready_Bar; CMIS: state == Ready */
};

/* Helpers -------------------------------------------------------------- */

/* Optical power in dBm. Zero/negative power is clamped to XCVR_DBM_FLOOR. */
#define XCVR_DBM_FLOOR (-40.0)
double xcvr_mw_to_dbm(double mw);
double xcvr_dbm_to_mw(double dbm);

const char *xcvr_type_name(enum xcvr_type t);
const char *xcvr_metric_name(enum xcvr_metric m);
const char *xcvr_metric_unit(enum xcvr_metric m);
const char *xcvr_identifier_name(uint8_t id);
const char *xcvr_connector_name(uint8_t code);

/* Sum of bytes modulo 256 (the SFF-8472 CC_BASE/CC_EXT/CC_DMI algorithm). */
uint8_t xcvr_checksum8(const uint8_t *p, size_t n);

/* Copy a space-padded ASCII field into a NUL-terminated, right-trimmed string. */
void xcvr_copy_ascii(char *dst, size_t dstlen, const uint8_t *src, size_t srclen);

#ifdef __cplusplus
}
#endif

#endif /* XCVR_TYPES_H */
