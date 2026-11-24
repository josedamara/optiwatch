#include "xcvr/module.h"

#include <errno.h>
#include <string.h>

int xcvr_cmis_select_page(xcvr_bus *bus, uint8_t page)
{
	int rc = xcvr_bus_write_byte(bus, CMIS_ADDR, CMIS_PAGE_SELECT, page);
	if (rc)
		return rc;
	uint8_t readback;
	rc = xcvr_bus_read(bus, CMIS_ADDR, CMIS_PAGE_SELECT, &readback, 1);
	if (rc)
		return rc;
	/* A module that does not implement the page keeps the old value. */
	return readback == page ? 0 : -EIO;
}

int xcvr_cmis_read_page(xcvr_bus *bus, uint8_t page, uint8_t off, uint8_t *buf, size_t len)
{
	if (off < 128)
		return -EINVAL;
	int rc = xcvr_cmis_select_page(bus, page);
	return rc ? rc : xcvr_bus_read(bus, CMIS_ADDR, off, buf, len);
}

static int load_sfp(struct xcvr_module *m)
{
	int rc = xcvr_bus_read(m->bus, SFF8472_ADDR_A0, 0, m->sfp.a0, 256);
	if (rc)
		return rc;
	if (sff8472_parse_a0(m->sfp.a0, &m->info))
		return -EPROTONOSUPPORT;

	if (m->info.ddm_supported) {
		/* Thresholds, calibration constants and CC_DMI live in A2h 0..95. */
		rc = xcvr_bus_read(m->bus, SFF8472_ADDR_A2, 0, m->sfp.a2, 128);
		if (rc)
			return rc;
		sff8472_check_a2(m->sfp.a2, &m->info);
		sff8472_parse_thresholds(m->sfp.a2, &m->info, &m->thresholds);
	}
	return 0;
}

static int load_cmis(struct xcvr_module *m)
{
	struct cmis_image *img = &m->cmis;
	int rc = xcvr_bus_read(m->bus, CMIS_ADDR, 0, img->lower, 128);
	if (rc)
		return rc;

	bool flat = (img->lower[CMIS_MEM_CHAR] & CMIS_MEM_FLAT) != 0;
	if (flat) {
		/* Page 00h is the only upper page and needs no page select. */
		rc = xcvr_bus_read(m->bus, CMIS_ADDR, 128, img->p00, 128);
	} else {
		rc = xcvr_cmis_read_page(m->bus, 0x00, 128, img->p00, 128);
		if (!rc)
			rc = xcvr_cmis_read_page(m->bus, 0x01, 128, img->p01, 128);
		if (!rc)
			rc = xcvr_cmis_read_page(m->bus, 0x02, 128, img->p02, 128);
	}
	if (rc)
		return rc;

	if (cmis_parse_info(img, &m->info))
		return -EPROTONOSUPPORT;
	cmis_parse_thresholds(img, &m->info, &m->thresholds);
	return 0;
}

int xcvr_module_load(xcvr_bus *bus, struct xcvr_module *m)
{
	memset(m, 0, sizeof(*m));
	m->bus = bus;

	uint8_t id;
	int rc = xcvr_bus_read(bus, 0x50, 0, &id, 1);
	if (rc)
		return rc;

	if (id == 0x03 || id == 0x02)
		return load_sfp(m);
	if (cmis_is_cmis_id(id))
		return load_cmis(m);

	m->info.identifier = id;
	return -EPROTONOSUPPORT;
}

static int poll_sfp(struct xcvr_module *m, struct xcvr_dom *dom)
{
	/* Diagnostics, status/control and flags: A2h 96..119. */
	int rc = xcvr_bus_read(m->bus, SFF8472_ADDR_A2, SFF8472_DIAG, m->sfp.a2 + SFF8472_DIAG, 24);
	if (rc)
		return rc;
	sff8472_parse_dom(m->sfp.a2, &m->info, dom);
	return 0;
}

static int poll_cmis(struct xcvr_module *m, struct xcvr_dom *dom)
{
	struct cmis_image *img = &m->cmis;
	/* Lower page 0..31: state, latched module flags, monitors, control. */
	int rc = xcvr_bus_read(m->bus, CMIS_ADDR, 0, img->lower, 32);
	if (rc)
		return rc;

	if (!m->info.flat_mem) {
		rc = xcvr_cmis_read_page(m->bus, 0x10, 128, img->p10, 8);
		if (!rc)
			rc = xcvr_cmis_read_page(m->bus, 0x11, 128, img->p11, 74); /* 128..201 */
		if (rc)
			return rc;
	}
	cmis_parse_dom(img, &m->info, dom);
	return 0;
}

int xcvr_module_poll(struct xcvr_module *m, struct xcvr_dom *dom)
{
	switch (m->info.type) {
	case XCVR_TYPE_SFP:  return poll_sfp(m, dom);
	case XCVR_TYPE_CMIS: return poll_cmis(m, dom);
	default:             return -ENODEV;
	}
}

int xcvr_module_set_tx_disable(struct xcvr_module *m, unsigned lane_mask, bool disable)
{
	if (!m->info.soft_tx_disable)
		return -EOPNOTSUPP;

	if (m->info.type == XCVR_TYPE_SFP) {
		if (!(lane_mask & 1u))
			return -EINVAL;
		uint8_t sc;
		int rc = xcvr_bus_read(m->bus, SFF8472_ADDR_A2, SFF8472_STATUS_CONTROL, &sc, 1);
		if (rc)
			return rc;
		sc = disable ? (uint8_t)(sc | SFF8472_SC_SOFT_TX_DISABLE)
			     : (uint8_t)(sc & ~SFF8472_SC_SOFT_TX_DISABLE);
		return xcvr_bus_write_byte(m->bus, SFF8472_ADDR_A2, SFF8472_STATUS_CONTROL, sc);
	}

	if (m->info.type == XCVR_TYPE_CMIS) {
		unsigned valid = (1u << m->info.lanes) - 1u;
		if (!(lane_mask & valid))
			return -EINVAL;
		uint8_t v;
		int rc = xcvr_cmis_read_page(m->bus, 0x10, CMIS_P10_TX_DISABLE, &v, 1);
		if (rc)
			return rc;
		v = disable ? (uint8_t)(v | (lane_mask & valid)) : (uint8_t)(v & ~(lane_mask & valid));
		return xcvr_bus_write_byte(m->bus, CMIS_ADDR, CMIS_P10_TX_DISABLE, v);
	}
	return -ENODEV;
}

int xcvr_module_set_low_power(struct xcvr_module *m, bool low_power)
{
	if (m->info.type != XCVR_TYPE_CMIS)
		return -EOPNOTSUPP;
	uint8_t v;
	int rc = xcvr_bus_read(m->bus, CMIS_ADDR, CMIS_MODULE_CONTROL, &v, 1);
	if (rc)
		return rc;
	v = low_power ? (uint8_t)(v | CMIS_CTRL_LOW_PWR_SW) : (uint8_t)(v & ~CMIS_CTRL_LOW_PWR_SW);
	return xcvr_bus_write_byte(m->bus, CMIS_ADDR, CMIS_MODULE_CONTROL, v);
}

int xcvr_module_state(struct xcvr_module *m, int *state)
{
	if (m->info.type != XCVR_TYPE_CMIS)
		return -EOPNOTSUPP;
	uint8_t v;
	int rc = xcvr_bus_read(m->bus, CMIS_ADDR, CMIS_MODULE_STATE, &v, 1);
	if (rc)
		return rc;
	m->cmis.lower[CMIS_MODULE_STATE] = v;
	*state = (v >> 1) & 0x7;
	return 0;
}
