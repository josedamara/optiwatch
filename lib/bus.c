/* Generic bus layer: spec parsing, chunking, retries, statistics. */
#include "xcvr/bus.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_MAX_XFER 32u
#define DEFAULT_RETRIES 2u
#define DEFAULT_RETRY_DELAY_US 2000u

static void sleep_us(unsigned us)
{
	struct timespec ts = { .tv_sec = us / 1000000u, .tv_nsec = (long)(us % 1000000u) * 1000L };
	while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
		;
}

xcvr_bus *xcvr_bus_open(const char *spec, char *err, size_t errlen)
{
	xcvr_bus *bus = NULL;

	if (!spec) {
		snprintf(err, errlen, "no bus spec");
		return NULL;
	}
	if (strncmp(spec, "i2c:", 4) == 0)
		bus = xcvr_bus_open_i2cdev(spec + 4, err, errlen);
	else if (strncmp(spec, "sim:", 4) == 0)
		bus = xcvr_bus_open_sim(spec + 4, err, errlen);
	else if (strncmp(spec, "mem:", 4) == 0)
		bus = xcvr_bus_open_mem(spec + 4, err, errlen);
	else
		snprintf(err, errlen, "unknown bus type in '%s' (expected i2c:, sim: or mem:)", spec);

	if (!bus)
		return NULL;

	snprintf(bus->spec, sizeof(bus->spec), "%s", spec);
	if (bus->max_xfer == 0)
		bus->max_xfer = DEFAULT_MAX_XFER;
	bus->retries = DEFAULT_RETRIES;
	bus->retry_delay_us = DEFAULT_RETRY_DELAY_US;
	return bus;
}

void xcvr_bus_close(xcvr_bus *bus)
{
	if (bus && bus->ops->close)
		bus->ops->close(bus);
}

/* Largest chunk starting at off that fits max_xfer and stays inside one
 * 128-byte half of the map (CMIS and SFF-8472 both page at 128). */
static size_t chunk_len(const xcvr_bus *bus, unsigned off, size_t remaining)
{
	size_t n = remaining;
	size_t to_boundary = 128u - (off % 128u);
	if (n > bus->max_xfer)
		n = bus->max_xfer;
	if (n > to_boundary)
		n = to_boundary;
	return n;
}

int xcvr_bus_read(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len)
{
	if ((size_t)off + len > 256)
		return -EINVAL;

	unsigned pos = off;
	size_t done = 0;
	while (done < len) {
		size_t n = chunk_len(bus, pos, len - done);
		int rc = -EIO;
		for (unsigned attempt = 0; attempt <= bus->retries; attempt++) {
			if (attempt) {
				bus->stats.retries++;
				sleep_us(bus->retry_delay_us * attempt);
			}
			rc = bus->ops->read(bus, addr, (uint8_t)pos, buf + done, n);
			if (rc == 0 || rc == -ENXIO) /* NACK on address: module absent, don't retry */
				break;
		}
		if (rc) {
			bus->stats.errors++;
			return rc;
		}
		bus->stats.reads++;
		bus->stats.bytes_read += n;
		done += n;
		pos += (unsigned)n;
	}
	return 0;
}

int xcvr_bus_write(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len)
{
	if ((size_t)off + len > 256)
		return -EINVAL;

	unsigned pos = off;
	size_t done = 0;
	while (done < len) {
		/* Module EEPROM write pages are 8 bytes in SFF-8472; stay conservative. */
		size_t n = chunk_len(bus, pos, len - done);
		if (n > 8)
			n = 8;
		int rc = -EIO;
		for (unsigned attempt = 0; attempt <= bus->retries; attempt++) {
			if (attempt) {
				bus->stats.retries++;
				sleep_us(bus->retry_delay_us * attempt);
			}
			rc = bus->ops->write(bus, addr, (uint8_t)pos, buf + done, n);
			if (rc == 0 || rc == -ENXIO)
				break;
		}
		if (rc) {
			bus->stats.errors++;
			return rc;
		}
		bus->stats.writes++;
		bus->stats.bytes_written += n;
		done += n;
		pos += (unsigned)n;
	}
	return 0;
}

int xcvr_bus_write_byte(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t val)
{
	return xcvr_bus_write(bus, addr, off, &val, 1);
}

int xcvr_bus_present(xcvr_bus *bus)
{
	if (bus->ops->present) {
		int p = bus->ops->present(bus);
		if (p != -ENOTSUP)
			return p < 0 ? 0 : p;
	}
	/* No MOD_ABS signal: a module is present if its EEPROM ACKs. Use the raw
	 * op so a missing module doesn't count as a bus error. Only a NACK means
	 * "absent"; a transient bus error must not look like a removal. */
	uint8_t id;
	return bus->ops->read(bus, 0x50, 0, &id, 1) != -ENXIO;
}
