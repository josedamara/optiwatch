/*
 * SFF-8472 (Rev 12.4) "Management Interface for SFP+" decoder.
 *
 * The module exposes two 256-byte maps on the two-wire bus:
 *   A0h (I2C 0x50): serial ID, compliance codes, options     (Table 4-1)
 *   A2h (I2C 0x51): thresholds, calibration, live diagnostics (Table 4-3 / 9-5)
 *
 * The parse functions are pure: they take raw byte images and fill decoded
 * structures, so they can be tested without hardware.
 */
#ifndef XCVR_SFF8472_H
#define XCVR_SFF8472_H

#include "xcvr/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SFF8472_ADDR_A0 0x50
#define SFF8472_ADDR_A2 0x51

/* A0h offsets */
enum {
	SFF8472_ID              = 0,
	SFF8472_EXT_ID          = 1,
	SFF8472_CONNECTOR       = 2,
	SFF8472_VENDOR_NAME     = 20,  /* 16 bytes */
	SFF8472_VENDOR_OUI      = 37,  /* 3 bytes */
	SFF8472_VENDOR_PN       = 40,  /* 16 bytes */
	SFF8472_VENDOR_REV      = 56,  /* 4 bytes */
	SFF8472_WAVELENGTH      = 60,  /* 2 bytes, nm */
	SFF8472_CC_BASE         = 63,  /* sum of 0..62 */
	SFF8472_VENDOR_SN       = 68,  /* 16 bytes */
	SFF8472_DATE_CODE       = 84,  /* 8 bytes YYMMDDLL */
	SFF8472_DIAG_TYPE       = 92,
	SFF8472_ENHANCED_OPTS   = 93,
	SFF8472_COMPLIANCE      = 94,
	SFF8472_CC_EXT          = 95,  /* sum of 64..94 */
};

/* A0h byte 92: diagnostic monitoring type */
#define SFF8472_DIAG_DDM        0x40
#define SFF8472_DIAG_INT_CAL    0x20
#define SFF8472_DIAG_EXT_CAL    0x10
#define SFF8472_DIAG_RX_AVG     0x08

/* A0h byte 93: enhanced options */
#define SFF8472_EOPT_ALARM_FLAGS 0x80
#define SFF8472_EOPT_SOFT_TXDIS  0x40

/* A2h offsets */
enum {
	SFF8472_THRESHOLDS      = 0,   /* 40 bytes: temp, vcc, bias, txpwr, rxpwr x (HA, LA, HW, LW) */
	SFF8472_EXT_CAL_CONST   = 56,  /* 36 bytes */
	SFF8472_RX_PWR4         = 56,
	SFF8472_TX_I_SLOPE      = 76,
	SFF8472_CC_DMI          = 95,  /* sum of 0..94 */
	SFF8472_DIAG            = 96,  /* 10 bytes: temp, vcc, bias, txpwr, rxpwr */
	SFF8472_STATUS_CONTROL  = 110,
	SFF8472_ALARM_FLAGS     = 112, /* 2 bytes */
	SFF8472_WARN_FLAGS      = 116, /* 2 bytes */
	SFF8472_PAGE_SELECT     = 127,
};

/* A2h byte 110: status / control */
#define SFF8472_SC_TX_DISABLE_STATE 0x80
#define SFF8472_SC_SOFT_TX_DISABLE  0x40
#define SFF8472_SC_TX_FAULT         0x04
#define SFF8472_SC_RX_LOS           0x02
#define SFF8472_SC_DATA_NOT_READY   0x01

/* Decode A0h. Fills identity, capability bits and CC_BASE/CC_EXT results.
 * Returns 0, or -EINVAL if the identifier is not an SFP-family module. */
int sff8472_parse_a0(const uint8_t a0[256], struct xcvr_info *info);

/* Verify CC_DMI over A2h 0..94 and record it in info->cc_dmi_ok. */
void sff8472_check_a2(const uint8_t a2[256], struct xcvr_info *info);

/* Decode alarm/warning thresholds (A2h 0..39), applying external
 * calibration when the module requires it. */
void sff8472_parse_thresholds(const uint8_t a2[256], const struct xcvr_info *info,
			      struct xcvr_threshold_set *out);

/* Decode live diagnostics, status and flags (A2h 96..117). Constants for
 * external calibration are taken from A2h 56..91 of the same image. */
void sff8472_parse_dom(const uint8_t a2[256], const struct xcvr_info *info,
		       struct xcvr_dom *out);

#ifdef __cplusplus
}
#endif

#endif /* XCVR_SFF8472_H */
