#include "test.h"

#include <errno.h>

#include "xcvr/module.h"

static xcvr_bus *sfp_bus(void)
{
	char err[128];
	xcvr_bus *bus = xcvr_bus_open("mem:sfp", err, sizeof(err));
	fx_sfp_a0(xcvr_mem_sfp_page(bus, 0x50), 0);
	fx_sfp_a2(xcvr_mem_sfp_page(bus, 0x51));
	bus->retry_delay_us = 0;
	return bus;
}

static void test_open_errors(void)
{
	char err[128];
	CHECK(xcvr_bus_open("bogus:x", err, sizeof(err)) == NULL);
	CHECK(strstr(err, "unknown bus type") != NULL);
	CHECK(xcvr_bus_open("mem:qsfp", err, sizeof(err)) == NULL);
	CHECK(xcvr_bus_open("sim:/tmp/x.sock", err, sizeof(err)) == NULL);
	CHECK(xcvr_bus_open("sim:localhost:7000", err, sizeof(err)) == NULL);

	xcvr_bus *b = xcvr_bus_open("sim:/tmp/optisim.sock:3", err, sizeof(err));
	CHECK(b != NULL);
	xcvr_bus_close(b);
	b = xcvr_bus_open("sim:127.0.0.1:7000:0", err, sizeof(err));
	CHECK(b != NULL);
	xcvr_bus_close(b);
}

static void test_chunking(void)
{
	xcvr_bus *bus = sfp_bus();
	bus->max_xfer = 16;
	uint8_t buf[256];
	CHECK_EQ_INT(xcvr_bus_read(bus, 0x50, 0, buf, 256), 0);
	CHECK_EQ_INT(bus->stats.reads, 16);
	CHECK_EQ_INT(bus->stats.bytes_read, 256);
	CHECK(memcmp(buf, xcvr_mem_sfp_page(bus, 0x50), 256) == 0);

	/* A read straddling offset 128 is split at the page boundary. */
	bus->stats.reads = 0;
	bus->max_xfer = 64;
	CHECK_EQ_INT(xcvr_bus_read(bus, 0x50, 120, buf, 16), 0);
	CHECK_EQ_INT(bus->stats.reads, 2);

	CHECK_EQ_INT(xcvr_bus_read(bus, 0x50, 250, buf, 10), -EINVAL);
	xcvr_bus_close(bus);
}

static void test_retries(void)
{
	xcvr_bus *bus = sfp_bus();
	uint8_t b;
	bus->retries = 2;

	xcvr_mem_fail_next(bus, 2);
	CHECK_EQ_INT(xcvr_bus_read(bus, 0x50, 0, &b, 1), 0);
	CHECK_EQ_INT(b, 0x03);
	CHECK_EQ_INT(bus->stats.retries, 2);
	CHECK_EQ_INT(bus->stats.errors, 0);

	xcvr_mem_fail_next(bus, 3);
	CHECK_EQ_INT(xcvr_bus_read(bus, 0x50, 0, &b, 1), -EIO);
	CHECK_EQ_INT(bus->stats.errors, 1);
	xcvr_bus_close(bus);
}

static void test_absent_module(void)
{
	xcvr_bus *bus = sfp_bus();
	struct xcvr_module m;
	CHECK_EQ_INT(xcvr_bus_present(bus), 1);
	xcvr_mem_set_present(bus, 0);
	CHECK_EQ_INT(xcvr_bus_present(bus), 0);
	unsigned long retries = bus->stats.retries;
	CHECK_EQ_INT(xcvr_module_load(bus, &m), -ENXIO);
	CHECK_EQ_INT(bus->stats.retries, retries); /* NACK is not retried */
	xcvr_bus_close(bus);
}

static void test_sfp_module(void)
{
	xcvr_bus *bus = sfp_bus();
	struct xcvr_module m;
	struct xcvr_dom d;
	CHECK_EQ_INT(xcvr_module_load(bus, &m), 0);
	CHECK_EQ_INT(m.info.type, XCVR_TYPE_SFP);
	CHECK(m.info.cc_base_ok && m.info.cc_ext_ok && m.info.cc_dmi_ok);
	CHECK(m.thresholds.valid);
	CHECK_EQ_INT(xcvr_module_poll(&m, &d), 0);
	CHECK_NEAR(d.rx_power_mw[0], 0.2, 1e-9);

	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 1, true), 0);
	CHECK(xcvr_mem_sfp_page(bus, 0x51)[SFF8472_STATUS_CONTROL] & SFF8472_SC_SOFT_TX_DISABLE);
	xcvr_module_poll(&m, &d);
	CHECK(d.tx_disabled[0]);
	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 1, false), 0);
	CHECK(!(xcvr_mem_sfp_page(bus, 0x51)[SFF8472_STATUS_CONTROL] & SFF8472_SC_SOFT_TX_DISABLE));
	CHECK_EQ_INT(xcvr_module_set_low_power(&m, true), -EOPNOTSUPP);
	xcvr_bus_close(bus);
}

static void test_unsupported_identifier(void)
{
	xcvr_bus *bus = sfp_bus();
	struct xcvr_module m;
	xcvr_mem_sfp_page(bus, 0x50)[0] = 0x11; /* QSFP28 / SFF-8636 */
	CHECK_EQ_INT(xcvr_module_load(bus, &m), -EPROTONOSUPPORT);
	CHECK_EQ_INT(m.info.identifier, 0x11);
	xcvr_bus_close(bus);
}

const struct test_case bus_tests[] = {
	{ "open_errors", test_open_errors },
	{ "chunking", test_chunking },
	{ "retries", test_retries },
	{ "absent_module", test_absent_module },
	{ "sfp_module", test_sfp_module },
	{ "unsupported_identifier", test_unsupported_identifier },
	{ NULL, NULL },
};
