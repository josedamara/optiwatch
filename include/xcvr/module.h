/*
 * Module access layer: ties a bus to the right decoder.
 *
 * xcvr_module_load() reads the identifier byte, picks SFF-8472 or CMIS,
 * reads the static parts of the memory map and decodes identity and
 * thresholds. xcvr_module_poll() then refreshes only the live registers,
 * keeping per-poll bus traffic small.
 */
#ifndef XCVR_MODULE_H
#define XCVR_MODULE_H

#include "xcvr/bus.h"
#include "xcvr/cmis.h"
#include "xcvr/sff8472.h"
#include "xcvr/types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xcvr_module {
	xcvr_bus *bus;
	struct xcvr_info info;
	struct xcvr_threshold_set thresholds;
	struct {
		uint8_t a0[256];
		uint8_t a2[256];
	} sfp;
	struct cmis_image cmis;
};

/* Identify and read the module behind bus.
 * Returns 0, -ENXIO if nothing answers, -EPROTONOSUPPORT for an
 * identifier this library does not manage, or another negative errno for
 * bus errors. Checksum results are reported in m->info.cc_*_ok. */
int xcvr_module_load(xcvr_bus *bus, struct xcvr_module *m);

/* Refresh live diagnostics, status and flags. */
int xcvr_module_poll(struct xcvr_module *m, struct xcvr_dom *dom);

/* Soft TX disable. lane_mask bit n = lane n (SFP only has lane 0). */
int xcvr_module_set_tx_disable(struct xcvr_module *m, unsigned lane_mask, bool disable);

/* CMIS: request low power (true) or high power (false) through LowPwrRequestSW. */
int xcvr_module_set_low_power(struct xcvr_module *m, bool low_power);

/* CMIS: read the current module state (enum cmis_module_state). */
int xcvr_module_state(struct xcvr_module *m, int *state);

/* CMIS: select an upper page (bank 0) and verify the selection took. */
int xcvr_cmis_select_page(xcvr_bus *bus, uint8_t page);

/* CMIS: read 'len' bytes of an upper page starting at absolute offset 'off' (>= 128). */
int xcvr_cmis_read_page(xcvr_bus *bus, uint8_t page, uint8_t off, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* XCVR_MODULE_H */
