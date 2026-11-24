/*
 * In-process memory backend used by the unit tests.
 *
 *   mem:sfp   two flat 256-byte devices at 0x50 (A0h) and 0x51 (A2h)
 *   mem:cmis  one paged device at 0x50: lower page 0..127, upper page 128..255
 *             selected by the page-select byte at offset 127
 */
#include "xcvr/bus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CMIS_PAGE_SELECT 127

struct mem_priv {
	int cmis;
	int present;
	unsigned fail_next;
	uint8_t sfp[2][256];
	uint8_t lower[128];
	uint8_t pages[256][128];
};

static int mem_check(struct mem_priv *p, uint8_t addr)
{
	if (!p->present)
		return -ENXIO;
	if (p->fail_next) {
		p->fail_next--;
		return -EIO;
	}
	if (p->cmis ? addr != 0x50 : (addr != 0x50 && addr != 0x51))
		return -ENXIO;
	return 0;
}

static uint8_t *cmis_byte(struct mem_priv *p, unsigned off)
{
	if (off < 128)
		return &p->lower[off];
	return &p->pages[p->lower[CMIS_PAGE_SELECT]][off - 128];
}

static int mem_read(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len)
{
	struct mem_priv *p = bus->priv;
	int rc = mem_check(p, addr);
	if (rc)
		return rc;
	for (size_t i = 0; i < len; i++) {
		unsigned o = off + (unsigned)i;
		buf[i] = p->cmis ? *cmis_byte(p, o) : p->sfp[addr - 0x50][o];
	}
	return 0;
}

static int mem_write(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len)
{
	struct mem_priv *p = bus->priv;
	int rc = mem_check(p, addr);
	if (rc)
		return rc;
	for (size_t i = 0; i < len; i++) {
		unsigned o = off + (unsigned)i;
		if (p->cmis)
			*cmis_byte(p, o) = buf[i];
		else
			p->sfp[addr - 0x50][o] = buf[i];
	}
	return 0;
}

static int mem_present(xcvr_bus *bus)
{
	return ((struct mem_priv *)bus->priv)->present;
}

static void mem_close(xcvr_bus *bus)
{
	free(bus->priv);
	free(bus);
}

static const struct xcvr_bus_ops mem_ops = {
	.read = mem_read,
	.write = mem_write,
	.present = mem_present,
	.close = mem_close,
};

xcvr_bus *xcvr_bus_open_mem(const char *kind, char *err, size_t errlen)
{
	int cmis;
	if (strcmp(kind, "sfp") == 0)
		cmis = 0;
	else if (strcmp(kind, "cmis") == 0)
		cmis = 1;
	else {
		snprintf(err, errlen, "mem bus kind must be 'sfp' or 'cmis', got '%s'", kind);
		return NULL;
	}

	xcvr_bus *bus = calloc(1, sizeof(*bus));
	struct mem_priv *p = calloc(1, sizeof(*p));
	if (!bus || !p) {
		free(bus);
		free(p);
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	p->cmis = cmis;
	p->present = 1;
	bus->ops = &mem_ops;
	bus->priv = p;
	return bus;
}

uint8_t *xcvr_mem_sfp_page(xcvr_bus *bus, uint8_t addr)
{
	struct mem_priv *p = bus->priv;
	return (addr == 0x50 || addr == 0x51) ? p->sfp[addr - 0x50] : NULL;
}

uint8_t *xcvr_mem_cmis_page(xcvr_bus *bus, uint8_t page)
{
	struct mem_priv *p = bus->priv;
	return page == 0xFF ? p->lower : p->pages[page];
}

void xcvr_mem_set_present(xcvr_bus *bus, int present)
{
	((struct mem_priv *)bus->priv)->present = present;
}

void xcvr_mem_fail_next(xcvr_bus *bus, unsigned n)
{
	((struct mem_priv *)bus->priv)->fail_next = n;
}
