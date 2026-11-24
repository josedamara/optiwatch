/*
 * xcvrdump: one-shot decoder for a single module, in the spirit of
 * `ethtool -m`. Useful for bring-up and for checking a bus without the daemon.
 *
 *   xcvrdump i2c:/dev/i2c-3
 *   xcvrdump sim:/tmp/optisim.sock:1 --power-up
 *   xcvrdump sim:/tmp/optisim.sock:0 --hex
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xcvr/module.h"

static void hexdump(const char *title, const uint8_t *p, size_t n, unsigned base)
{
	printf("%s\n", title);
	for (size_t i = 0; i < n; i += 16) {
		printf("  %02zx:", base + i);
		for (size_t j = 0; j < 16 && i + j < n; j++)
			printf(" %02x", p[i + j]);
		printf("\n");
	}
}

static const char *flag_str(uint8_t f)
{
	if (f & XCVR_FLAG_HIGH_ALARM) return "HIGH ALARM";
	if (f & XCVR_FLAG_LOW_ALARM)  return "LOW ALARM";
	if (f & XCVR_FLAG_HIGH_WARN)  return "high warn";
	if (f & XCVR_FLAG_LOW_WARN)   return "low warn";
	return "";
}

static void print_thresholds(const struct xcvr_threshold_set *t)
{
	if (!t->valid)
		return;
	printf("\nThresholds            high alarm   high warn    low warn   low alarm\n");
	for (int m = 0; m < XCVR_M_COUNT; m++) {
		const struct xcvr_thresholds *q = &t->m[m];
		printf("  %-12s %-4s %11.4f %11.4f %11.4f %11.4f\n", xcvr_metric_name((enum xcvr_metric)m),
		       xcvr_metric_unit((enum xcvr_metric)m), q->high_alarm, q->high_warn, q->low_warn,
		       q->low_alarm);
	}
}

static int power_up(struct xcvr_module *m)
{
	int rc = xcvr_module_set_low_power(m, false);
	if (rc)
		return rc;
	for (int i = 0; i < 100; i++) {
		int st;
		if ((rc = xcvr_module_state(m, &st)))
			return rc;
		if (st == CMIS_STATE_READY)
			return 0;
		struct timespec ts = { 0, 50 * 1000 * 1000 };
		nanosleep(&ts, NULL);
	}
	return -ETIMEDOUT;
}

int main(int argc, char **argv)
{
	const char *spec = NULL;
	int hex = 0, pwr = 0;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--hex"))
			hex = 1;
		else if (!strcmp(argv[i], "--power-up"))
			pwr = 1;
		else if (argv[i][0] != '-' && !spec)
			spec = argv[i];
		else {
			fprintf(stderr, "usage: %s BUS-SPEC [--hex] [--power-up]\n", argv[0]);
			return 2;
		}
	}
	if (!spec) {
		fprintf(stderr, "usage: %s BUS-SPEC [--hex] [--power-up]\n", argv[0]);
		return 2;
	}

	char err[256];
	xcvr_bus *bus = xcvr_bus_open(spec, err, sizeof(err));
	if (!bus) {
		fprintf(stderr, "xcvrdump: %s\n", err);
		return 1;
	}
	if (!xcvr_bus_present(bus)) {
		printf("%s: no module present\n", spec);
		xcvr_bus_close(bus);
		return 3;
	}

	struct xcvr_module m;
	int rc = xcvr_module_load(bus, &m);
	if (rc) {
		fprintf(stderr, "xcvrdump: load failed: %s (identifier 0x%02x)\n", strerror(-rc),
			m.info.identifier);
		xcvr_bus_close(bus);
		return 1;
	}
	if (pwr && m.info.type == XCVR_TYPE_CMIS && (rc = power_up(&m)))
		fprintf(stderr, "xcvrdump: power-up: %s\n", strerror(-rc));

	const struct xcvr_info *in = &m.info;
	printf("Identifier      : 0x%02x (%s), %s map\n", in->identifier,
	       xcvr_identifier_name(in->identifier), xcvr_type_name(in->type));
	printf("Vendor          : %s (OUI %02x:%02x:%02x)\n", in->vendor, in->oui[0], in->oui[1], in->oui[2]);
	printf("Part number     : %s rev %s\n", in->part_number, in->revision);
	printf("Serial number   : %s\n", in->serial);
	printf("Date code       : %s\n", in->date_code);
	printf("Connector       : 0x%02x (%s)\n", in->connector, xcvr_connector_name(in->connector));
	printf("Wavelength      : %.2f nm\n", in->wavelength_nm);
	printf("Lanes           : %u\n", in->lanes);
	printf("Diagnostics     : %s%s\n", in->ddm_supported ? "yes" : "no",
	       in->ext_calibrated ? " (externally calibrated)" : "");
	printf("Checksums       : base %s, ext %s, dmi %s\n", in->cc_base_ok ? "ok" : "BAD",
	       in->cc_ext_ok ? "ok" : "BAD", in->cc_dmi_ok ? "ok" : "BAD");

	struct xcvr_dom d;
	if ((rc = xcvr_module_poll(&m, &d))) {
		fprintf(stderr, "xcvrdump: poll failed: %s\n", strerror(-rc));
		xcvr_bus_close(bus);
		return 1;
	}
	if (in->type == XCVR_TYPE_CMIS)
		printf("Module state    : %s\n", cmis_module_state_name(d.module_state));
	printf("Temperature     : %.2f C %s\n", d.temp_c, flag_str(d.flags[XCVR_M_TEMP][0]));
	printf("Supply voltage  : %.4f V %s\n", d.vcc_v, flag_str(d.flags[XCVR_M_VCC][0]));
	printf("\nLane  Bias(mA)  Tx(mW)   Tx(dBm)  Rx(mW)   Rx(dBm)  Status\n");
	for (unsigned l = 0; l < d.lanes; l++) {
		printf("%4u  %8.3f  %7.4f  %7.2f  %7.4f  %7.2f ", l, d.tx_bias_ma[l], d.tx_power_mw[l],
		       xcvr_mw_to_dbm(d.tx_power_mw[l]), d.rx_power_mw[l], xcvr_mw_to_dbm(d.rx_power_mw[l]));
		if (d.tx_disabled[l]) printf(" TX-DISABLED");
		if (d.tx_fault[l])    printf(" TX-FAULT");
		if (d.rx_los[l])      printf(" RX-LOS");
		for (int mm = XCVR_M_TX_BIAS; mm < XCVR_M_COUNT; mm++)
			if (d.flags[mm][l])
				printf(" %s:%s", xcvr_metric_name((enum xcvr_metric)mm), flag_str(d.flags[mm][l]));
		printf("\n");
	}
	print_thresholds(&m.thresholds);

	if (hex) {
		printf("\n");
		if (in->type == XCVR_TYPE_SFP) {
			hexdump("A0h:", m.sfp.a0, 256, 0);
			hexdump("A2h (0..127):", m.sfp.a2, 128, 0);
		} else {
			hexdump("Lower page:", m.cmis.lower, 128, 0);
			hexdump("Page 00h:", m.cmis.p00, 128, 128);
		}
	}

	printf("\nBus: %lu reads, %lu writes, %lu retries, %lu errors\n", bus->stats.reads,
	       bus->stats.writes, bus->stats.retries, bus->stats.errors);
	xcvr_bus_close(bus);
	return 0;
}
