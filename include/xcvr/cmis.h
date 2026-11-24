/*
 * OIF CMIS (Common Management Interface Specification, Rev 5.x) decoder for
 * QSFP-DD, OSFP and other CMIS modules.
 *
 * CMIS exposes a single device at 0x50: a 128-byte lower page that is always
 * visible, and a 128-byte upper window (offsets 128..255) whose contents are
 * selected by the bank (byte 126) and page (byte 127) select registers.
 *
 * Pages this decoder uses:
 *   Lower    module state, module-level flags and monitors, page select
 *   00h      vendor identity (serial ID)
 *   01h      advertising: wavelength, monitor support, Tx bias scaling
 *   02h      module and lane alarm/warning thresholds
 *   10h      lane control (Tx output disable)
 *   11h      lane status, latched lane flags and lane monitors
 */
#ifndef XCVR_CMIS_H
#define XCVR_CMIS_H

#include "xcvr/types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CMIS_ADDR 0x50

/* Lower page offsets */
enum {
	CMIS_ID                 = 0,
	CMIS_REVISION           = 1,
	CMIS_MEM_CHAR           = 2,   /* bit 7: flat memory */
	CMIS_MODULE_STATE       = 3,   /* bits 3..1 */
	CMIS_MODULE_FLAGS_TV    = 9,   /* latched temperature / Vcc flags */
	CMIS_TEMP_MON           = 14,  /* s16, 1/256 C */
	CMIS_VCC_MON            = 16,  /* u16, 100 uV */
	CMIS_MODULE_CONTROL     = 26,  /* bit 4: LowPwrRequestSW */
	CMIS_BANK_SELECT        = 126,
	CMIS_PAGE_SELECT        = 127,
};

#define CMIS_MEM_FLAT           0x80
#define CMIS_CTRL_LOW_PWR_SW    0x10

/* Upper page 00h offsets (absolute, 128..255) */
enum {
	CMIS_P00_ID             = 128,
	CMIS_P00_VENDOR_NAME    = 129, /* 16 bytes */
	CMIS_P00_VENDOR_OUI     = 145, /* 3 bytes */
	CMIS_P00_VENDOR_PN      = 148, /* 16 bytes */
	CMIS_P00_VENDOR_REV     = 164, /* 2 bytes */
	CMIS_P00_VENDOR_SN      = 166, /* 16 bytes */
	CMIS_P00_DATE_CODE      = 182, /* 8 bytes */
	CMIS_P00_CONNECTOR      = 203,
};

/* Upper page 01h */
enum {
	CMIS_P01_WAVELENGTH     = 138, /* u16, 0.05 nm */
	CMIS_P01_MON_SUPPORT    = 159, /* bit 0 temp, bit 1 vcc */
	CMIS_P01_LANE_MON_SUP   = 160, /* bit 0 bias, bit 1 tx pwr, bit 2 rx pwr, bits 4..3 bias scale */
};

/* Upper page 02h: thresholds, each block HA, LA, HW, LW (2 bytes each) */
enum {
	CMIS_P02_TEMP           = 128,
	CMIS_P02_VCC            = 136,
	CMIS_P02_TX_POWER       = 176,
	CMIS_P02_TX_BIAS        = 184,
	CMIS_P02_RX_POWER       = 192,
};

/* Upper page 10h */
enum {
	CMIS_P10_TX_DISABLE     = 130, /* bit n = lane n+1 output disabled */
};

/* Upper page 11h: one bit per lane for flags, 2 bytes per lane for monitors */
enum {
	CMIS_P11_TX_FAULT       = 135,
	CMIS_P11_TX_LOS         = 136,
	CMIS_P11_TX_PWR_HA      = 139, /* 139..142: HA, LA, HW, LW */
	CMIS_P11_TX_BIAS_HA     = 143, /* 143..146 */
	CMIS_P11_RX_LOS         = 147,
	CMIS_P11_RX_PWR_HA      = 149, /* 149..152 */
	CMIS_P11_TX_POWER       = 154, /* 8 x u16, 0.1 uW */
	CMIS_P11_TX_BIAS        = 170, /* 8 x u16, 2 uA x scale */
	CMIS_P11_RX_POWER       = 186, /* 8 x u16, 0.1 uW */
};

enum cmis_module_state {
	CMIS_STATE_RESERVED = 0,
	CMIS_STATE_LOW_PWR  = 1,
	CMIS_STATE_PWR_UP   = 2,
	CMIS_STATE_READY    = 3,
	CMIS_STATE_PWR_DN   = 4,
	CMIS_STATE_FAULT    = 5,
};

/* Snapshot of the pages the decoder needs. Upper pages are indexed by
 * (offset - 128). */
struct cmis_image {
	uint8_t lower[128];
	uint8_t p00[128];
	uint8_t p01[128];
	uint8_t p02[128];
	uint8_t p10[128];
	uint8_t p11[128];
};

/* True if the SFF-8024 identifier is a CMIS module this decoder handles. */
bool cmis_is_cmis_id(uint8_t id);

const char *cmis_module_state_name(int state);
int cmis_module_state(const uint8_t lower[128]);

/* Decode identity and capabilities from lower page, page 00h and 01h.
 * Returns 0, or -EINVAL if the identifier is not CMIS. */
int cmis_parse_info(const struct cmis_image *img, struct xcvr_info *info);

/* Decode thresholds from page 02h. Flat-memory modules have none. */
void cmis_parse_thresholds(const struct cmis_image *img, const struct xcvr_info *info,
			   struct xcvr_threshold_set *out);

/* Decode monitors, lane status and latched flags (lower page, 10h, 11h). */
void cmis_parse_dom(const struct cmis_image *img, const struct xcvr_info *info,
		    struct xcvr_dom *out);

#ifdef __cplusplus
}
#endif

#endif /* XCVR_CMIS_H */
