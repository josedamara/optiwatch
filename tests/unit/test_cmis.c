#include "test.h"

#include <errno.h>

#include "xcvr/module.h"

static xcvr_bus *cmis_bus(void)
{
	char err[128];
	xcvr_bus *bus = xcvr_bus_open("mem:cmis", err, sizeof(err));
	fx_cmis_fill(bus);
	return bus;
}

static void test_load_info(void)
{
	xcvr_bus *bus = cmis_bus();
	struct xcvr_module m;
	CHECK_EQ_INT(xcvr_module_load(bus, &m), 0);
	CHECK_EQ_INT(m.info.type, XCVR_TYPE_CMIS);
	CHECK_EQ_INT(m.info.identifier, 0x18);
	CHECK_EQ_INT(m.info.lanes, 8);
	CHECK_STR(m.info.vendor, "OPTIWATCH LABS");
	CHECK_STR(m.info.part_number, "OW-QDD-400G-DR4");
	CHECK_STR(m.info.serial, "OWQ2400777");
	CHECK_NEAR(m.info.wavelength_nm, 1311.0, 1e-9);
	CHECK_EQ_INT(m.info.bias_multiplier, 2);
	CHECK(m.info.ddm_supported);
	CHECK(!m.info.flat_mem);
	CHECK_STR(xcvr_connector_name(m.info.connector), "MPO 1x12");
	xcvr_bus_close(bus);
}

static void test_thresholds(void)
{
	xcvr_bus *bus = cmis_bus();
	struct xcvr_module m;
	xcvr_module_load(bus, &m);
	const struct xcvr_threshold_set *t = &m.thresholds;
	CHECK(t->valid);
	CHECK_NEAR(t->m[XCVR_M_TEMP].high_alarm, 80, 1e-9);
	CHECK_NEAR(t->m[XCVR_M_TEMP].low_alarm, -10, 1e-9);
	CHECK_NEAR(t->m[XCVR_M_VCC].high_warn, 3.5, 1e-9);
	CHECK_NEAR(t->m[XCVR_M_TX_POWER].high_alarm, 3.0, 1e-9);
	CHECK_NEAR(t->m[XCVR_M_TX_BIAS].high_alarm, 30.0, 1e-9); /* 7500 * 2 uA * 2 */
	CHECK_NEAR(t->m[XCVR_M_RX_POWER].low_alarm, 0.03, 1e-9);
	xcvr_bus_close(bus);
}

static void test_poll(void)
{
	xcvr_bus *bus = cmis_bus();
	struct xcvr_module m;
	struct xcvr_dom d;
	xcvr_module_load(bus, &m);
	CHECK_EQ_INT(xcvr_module_poll(&m, &d), 0);

	CHECK_EQ_INT(d.module_state, CMIS_STATE_READY);
	CHECK(d.data_ready);
	CHECK_NEAR(d.temp_c, 40.0, 1e-9);
	CHECK_NEAR(d.vcc_v, 3.29, 1e-9);
	CHECK_EQ_INT(d.lanes, 8);
	for (unsigned l = 0; l < 8; l++) {
		CHECK_NEAR(d.tx_power_mw[l], 1.0, 1e-9);
		CHECK_NEAR(d.rx_power_mw[l], 0.5, 1e-9);
		CHECK_NEAR(d.tx_bias_ma[l], 7.0 + l, 0.01);
	}
	CHECK(d.rx_los[2]);
	CHECK(!d.rx_los[1]);
	CHECK(d.tx_disabled[5]);
	CHECK(!d.tx_disabled[4]);
	CHECK_EQ_INT(d.flags[XCVR_M_RX_POWER][2], XCVR_FLAG_LOW_ALARM);
	CHECK_EQ_INT(d.flags[XCVR_M_RX_POWER][3], 0);
	CHECK_EQ_INT(d.flags[XCVR_M_TEMP][0], XCVR_FLAG_HIGH_WARN);
	xcvr_bus_close(bus);
}

static void test_tx_disable(void)
{
	xcvr_bus *bus = cmis_bus();
	struct xcvr_module m;
	struct xcvr_dom d;
	xcvr_module_load(bus, &m);

	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 0x03, true), 0);
	CHECK_EQ_INT(xcvr_mem_cmis_page(bus, 0x10)[CMIS_P10_TX_DISABLE - 128], 0x23);
	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 0x20, false), 0);
	xcvr_module_poll(&m, &d);
	CHECK(d.tx_disabled[0] && d.tx_disabled[1] && !d.tx_disabled[5]);
	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 0x100, true), -EINVAL);
	xcvr_bus_close(bus);
}

static void test_low_power_and_state(void)
{
	xcvr_bus *bus = cmis_bus();
	struct xcvr_module m;
	xcvr_module_load(bus, &m);

	CHECK_EQ_INT(xcvr_module_set_low_power(&m, true), 0);
	CHECK(xcvr_mem_cmis_page(bus, 0xFF)[CMIS_MODULE_CONTROL] & CMIS_CTRL_LOW_PWR_SW);
	CHECK_EQ_INT(xcvr_module_set_low_power(&m, false), 0);
	CHECK(!(xcvr_mem_cmis_page(bus, 0xFF)[CMIS_MODULE_CONTROL] & CMIS_CTRL_LOW_PWR_SW));

	xcvr_mem_cmis_page(bus, 0xFF)[CMIS_MODULE_STATE] = CMIS_STATE_PWR_UP << 1;
	int st = -1;
	CHECK_EQ_INT(xcvr_module_state(&m, &st), 0);
	CHECK_EQ_INT(st, CMIS_STATE_PWR_UP);
	CHECK_STR(cmis_module_state_name(st), "ModulePwrUp");
	xcvr_bus_close(bus);
}

static void test_flat_memory(void)
{
	xcvr_bus *bus = cmis_bus();
	xcvr_mem_cmis_page(bus, 0xFF)[CMIS_MEM_CHAR] = CMIS_MEM_FLAT;
	struct xcvr_module m;
	struct xcvr_dom d;
	CHECK_EQ_INT(xcvr_module_load(bus, &m), 0);
	CHECK(m.info.flat_mem);
	CHECK(!m.info.ddm_supported);
	CHECK(!m.thresholds.valid);
	CHECK_EQ_INT(xcvr_module_poll(&m, &d), 0);
	CHECK_EQ_INT(xcvr_module_set_tx_disable(&m, 1, true), -EOPNOTSUPP);
	xcvr_bus_close(bus);
}

const struct test_case cmis_tests[] = {
	{ "load_info", test_load_info },
	{ "thresholds", test_thresholds },
	{ "poll", test_poll },
	{ "tx_disable", test_tx_disable },
	{ "low_power_and_state", test_low_power_and_state },
	{ "flat_memory", test_flat_memory },
	{ NULL, NULL },
};
