#include "port.hpp"

#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace optiwatch {

const char *port_state_name(PortState s)
{
	switch (s) {
	case PortState::Empty:      return "empty";
	case PortState::Inserted:   return "inserted";
	case PortState::PoweringUp: return "powering_up";
	case PortState::Ready:      return "ready";
	case PortState::Fault:      return "fault";
	}
	return "?";
}

namespace {

// Alarms that describe the port's ability to talk to / bring up the module.
// They are cleared once the module reaches READY again.
const char *const kInitAlarms[] = { "i2c_comm_failure", "power_up_timeout", "module_fault" };

bool is_power(enum xcvr_metric m)
{
	return m == XCVR_M_TX_POWER || m == XCVR_M_RX_POWER;
}

// Engineering value -> value shown to operators (optical power in dBm).
double display(enum xcvr_metric m, double v)
{
	return is_power(m) ? xcvr_mw_to_dbm(v) : v;
}

const char *display_unit(enum xcvr_metric m)
{
	return is_power(m) ? "dBm" : xcvr_metric_unit(m);
}

const char *pretty_metric(enum xcvr_metric m)
{
	switch (m) {
	case XCVR_M_TEMP:     return "Temperature";
	case XCVR_M_VCC:      return "Supply voltage";
	case XCVR_M_TX_BIAS:  return "Tx bias";
	case XCVR_M_TX_POWER: return "Tx power";
	case XCVR_M_RX_POWER: return "Rx power";
	default:              return "?";
	}
}

} // namespace

Port::Port(PortConfig cfg, const Tunables &t, AlarmTable &alarms, EventLog &events)
	: cfg_(std::move(cfg)), t_(t), alarms_(alarms), events_(events)
{
}

Port::~Port()
{
	if (bus_)
		xcvr_bus_close(bus_);
}

bool Port::open(std::string &err)
{
	char buf[256];
	bus_ = xcvr_bus_open(cfg_.bus.c_str(), buf, sizeof(buf));
	if (!bus_) {
		err = cfg_.name + ": " + buf;
		return false;
	}
	if (cfg_.max_xfer)
		bus_->max_xfer = cfg_.max_xfer;
	if (cfg_.retries >= 0)
		bus_->retries = static_cast<unsigned>(cfg_.retries);
	state_since_ = mono_ms();
	return true;
}

void Port::enter(PortState s, const std::string &why)
{
	if (s == state_)
		return;
	PortState old = state_;
	state_ = s;
	state_since_ = mono_ms();
	LOG_INFO("%s: %s -> %s (%s)", cfg_.name.c_str(), port_state_name(old), port_state_name(s), why.c_str());
	events_.emit(cfg_.name, "state_changed", [&](Json &j) {
		j.kv("from", port_state_name(old));
		j.kv("to", port_state_name(s));
		j.kv("reason", why);
	});
}

void Port::fault(const std::string &alarm_type, Severity sev, const std::string &msg, bool permanent)
{
	Alarm a;
	a.port = cfg_.name;
	a.type = alarm_type;
	a.severity = sev;
	a.message = msg;
	alarms_.raise(a);
	permanent_fault_ = permanent;
	enter(PortState::Fault, msg);
}

int64_t Port::tick(int64_t now)
{
	if (now < next_due_)
		return next_due_;

	if (!xcvr_bus_present(bus_)) {
		if (state_ != PortState::Empty)
			handle_removed();
		next_due_ = now + t_.poll_interval_ms;
		return next_due_;
	}

	switch (state_) {
	case PortState::Empty:
		counters_.insertions++;
		enter(PortState::Inserted, "module present");
		next_due_ = now + t_.insert_settle_ms;
		break;
	case PortState::Inserted:
		do_load(now);
		break;
	case PortState::PoweringUp:
		do_power_up(now);
		break;
	case PortState::Ready:
		do_poll(now);
		break;
	case PortState::Fault:
		if (!permanent_fault_ && now - state_since_ >= t_.fault_retry_ms) {
			load_attempts_ = 0;
			consecutive_errors_ = 0;
			power_up_requested_ = false;
			enter(PortState::Inserted, "retrying initialisation");
			next_due_ = now;
		} else {
			next_due_ = now + t_.poll_interval_ms;
		}
		break;
	}
	return next_due_;
}

void Port::handle_removed()
{
	counters_.removals++;
	alarms_.clear_port(cfg_.name, "module_removed");
	events_.emit(cfg_.name, "module_removed", [&](Json &j) {
		if (mod_.info.type != XCVR_TYPE_UNKNOWN)
			j.kv("serial", mod_.info.serial);
	});
	mod_ = xcvr_module{};
	dom_ = xcvr_dom{};
	have_dom_ = false;
	als_mask_ = 0;
	permanent_fault_ = false;
	power_up_requested_ = false;
	load_attempts_ = 0;
	consecutive_errors_ = 0;
	enter(PortState::Empty, "module removed");
}

void Port::do_load(int64_t now)
{
	int rc = xcvr_module_load(bus_, &mod_);
	if (rc == -EPROTONOSUPPORT) {
		char msg[96];
		std::snprintf(msg, sizeof(msg), "Unsupported module (identifier 0x%02x, %s)",
			      mod_.info.identifier, xcvr_identifier_name(mod_.info.identifier));
		fault("unsupported_module", Severity::Minor, msg, true);
		return;
	}
	if (rc) {
		counters_.load_failures++;
		LOG_WARN("%s: EEPROM read failed: %s", cfg_.name.c_str(), std::strerror(-rc));
		if (++load_attempts_ >= t_.max_load_attempts) {
			fault("i2c_comm_failure", Severity::Major,
			      std::string("Cannot read module EEPROM: ") + std::strerror(-rc));
			next_due_ = now + t_.poll_interval_ms;
		} else {
			next_due_ = now + t_.fast_poll_ms;
		}
		return;
	}

	const xcvr_info &in = mod_.info;
	events_.emit(cfg_.name, "module_inserted", [&](Json &j) {
		j.kv("type", xcvr_type_name(in.type));
		j.kv("identifier", xcvr_identifier_name(in.identifier));
		j.kv("vendor", in.vendor);
		j.kv("part_number", in.part_number);
		j.kv("serial", in.serial);
		j.kv("wavelength_nm", in.wavelength_nm);
		j.kv("lanes", in.lanes);
	});
	LOG_INFO("%s: %s %s %s s/n %s", cfg_.name.c_str(), xcvr_identifier_name(in.identifier),
		 in.vendor, in.part_number, in.serial);

	bool cc_ok = in.cc_base_ok && in.cc_ext_ok && in.cc_dmi_ok;
	set_discrete("eeprom_checksum", -1, !cc_ok, Severity::Minor,
		     std::string("EEPROM checksum mismatch (") + (in.cc_base_ok ? "" : "base ") +
			     (in.cc_ext_ok ? "" : "ext ") + (in.cc_dmi_ok ? "" : "dmi ") + ")");

	if (in.type == XCVR_TYPE_CMIS && !in.flat_mem) {
		enter(PortState::PoweringUp, "CMIS module, requesting high power");
		do_power_up(now);
		return;
	}
	for (const char *a : kInitAlarms)
		alarms_.clear(cfg_.name, a, -1, "recovered");
	enter(PortState::Ready, "module initialised");
	next_due_ = now; // poll right away
}

void Port::do_power_up(int64_t now)
{
	next_due_ = now + t_.fast_poll_ms;
	if (!power_up_requested_) {
		int rc = xcvr_module_set_low_power(&mod_, false);
		if (rc) {
			fault("i2c_comm_failure", Severity::Major,
			      std::string("Cannot write module control: ") + std::strerror(-rc));
			return;
		}
		power_up_requested_ = true;
	}

	int st = 0;
	int rc = xcvr_module_state(&mod_, &st);
	if (rc) {
		if (++consecutive_errors_ >= t_.max_poll_errors)
			fault("i2c_comm_failure", Severity::Major,
			      std::string("Cannot read module state: ") + std::strerror(-rc));
		return;
	}
	consecutive_errors_ = 0;

	if (st == CMIS_STATE_READY) {
		int64_t took = now - state_since_;
		for (const char *a : kInitAlarms)
			alarms_.clear(cfg_.name, a, -1, "recovered");
		events_.emit(cfg_.name, "module_ready", [&](Json &j) { j.kv("power_up_ms", took); });
		enter(PortState::Ready, "ModuleReady");
		next_due_ = now;
	} else if (st == CMIS_STATE_FAULT) {
		fault("module_fault", Severity::Critical, "Module reported ModuleFault");
	} else if (now - state_since_ >= t_.power_up_timeout_ms) {
		fault("power_up_timeout", Severity::Major,
		      std::string("Module stuck in ") + cmis_module_state_name(st) + " after " +
			      std::to_string(t_.power_up_timeout_ms) + " ms");
	}
}

void Port::do_poll(int64_t now)
{
	next_due_ = now + t_.poll_interval_ms;
	xcvr_dom d{};
	int rc = xcvr_module_poll(&mod_, &d);
	counters_.polls++;
	if (rc) {
		counters_.poll_errors++;
		LOG_WARN("%s: poll failed: %s", cfg_.name.c_str(), std::strerror(-rc));
		if (++consecutive_errors_ >= t_.max_poll_errors) {
			have_dom_ = false;
			fault("i2c_comm_failure", Severity::Major,
			      std::to_string(consecutive_errors_) + " consecutive I2C failures: " + std::strerror(-rc));
		} else {
			next_due_ = now + t_.fast_poll_ms;
		}
		return;
	}
	consecutive_errors_ = 0;
	dom_ = d;
	have_dom_ = true;

	if (mod_.info.type == XCVR_TYPE_CMIS && !mod_.info.flat_mem && d.module_state != CMIS_STATE_READY) {
		if (d.module_state == CMIS_STATE_FAULT) {
			fault("module_fault", Severity::Critical, "Module reported ModuleFault");
		} else {
			power_up_requested_ = false;
			enter(PortState::PoweringUp, std::string("module left ModuleReady (") +
							     cmis_module_state_name(d.module_state) + ")");
			next_due_ = now;
		}
		return;
	}

	evaluate_alarms();
	if (cfg_.als && run_als())
		next_due_ = now + t_.fast_poll_ms; // show the new Tx state promptly
}

void Port::set_discrete(const std::string &type, int lane, bool on, Severity sev, const std::string &msg)
{
	if (on) {
		Alarm a;
		a.port = cfg_.name;
		a.type = type;
		a.lane = lane;
		a.severity = sev;
		a.message = msg;
		alarms_.raise(a);
	} else {
		alarms_.clear(cfg_.name, type, lane, "recovered");
	}
}

void Port::eval_threshold(enum xcvr_metric m, int lane, double value, bool enabled, bool suppress_low)
{
	const xcvr_thresholds &th = mod_.thresholds.m[m];
	double v = display(m, value);
	double h = m == XCVR_M_TEMP ? t_.hysteresis.temp_c
		 : m == XCVR_M_VCC ? t_.hysteresis.vcc_v
		 : m == XCVR_M_TX_BIAS ? t_.hysteresis.bias_ma
		 : t_.hysteresis.power_db;

	struct Kind {
		const char *suffix;
		double limit;
		bool high;
		Severity sev;
		bool cond;
	} kinds[4] = {
		{ "high_alarm", display(m, th.high_alarm), true, Severity::Major, false },
		{ "high_warning", display(m, th.high_warn), true, Severity::Minor, false },
		{ "low_alarm", display(m, th.low_alarm), false, Severity::Major, false },
		{ "low_warning", display(m, th.low_warn), false, Severity::Minor, false },
	};

	const std::string base = xcvr_metric_name(m);
	for (Kind &k : kinds) {
		bool was = alarms_.active(cfg_.name, base + "_" + k.suffix, lane);
		// Raise on crossing the threshold, clear only once the value is back
		// inside it by the hysteresis margin, so a noisy reading near the
		// limit does not flap.
		if (k.high)
			k.cond = was ? v > k.limit - h : v > k.limit;
		else
			k.cond = was ? v < k.limit + h : v < k.limit;
		if (!enabled || (suppress_low && !k.high))
			k.cond = false;
	}
	// An alarm supersedes the warning on the same side.
	if (kinds[0].cond)
		kinds[1].cond = false;
	if (kinds[2].cond)
		kinds[3].cond = false;

	for (const Kind &k : kinds) {
		std::string type = base + "_" + k.suffix;
		if (k.cond) {
			Alarm a;
			a.port = cfg_.name;
			a.type = type;
			a.lane = lane;
			a.severity = k.sev;
			a.value = v;
			a.threshold = k.limit;
			a.unit = display_unit(m);
			char msg[160];
			std::snprintf(msg, sizeof(msg), "%s %s %s: %.2f %s %s %.2f %s", pretty_metric(m),
				      k.high ? "high" : "low", k.sev == Severity::Major ? "alarm" : "warning", v,
				      a.unit.c_str(), k.high ? ">" : "<", k.limit, a.unit.c_str());
			a.message = msg;
			alarms_.raise(a);
		} else {
			alarms_.clear(cfg_.name, type, lane, enabled ? "recovered" : "suppressed");
		}
	}
}

void Port::evaluate_alarms()
{
	const bool th = mod_.thresholds.valid;
	if (th) {
		eval_threshold(XCVR_M_TEMP, -1, dom_.temp_c, true, false);
		eval_threshold(XCVR_M_VCC, -1, dom_.vcc_v, true, false);
	}
	for (unsigned l = 0; l < dom_.lanes; l++) {
		int lane = static_cast<int>(l);
		bool tx_off = dom_.tx_disabled[l];
		if (th) {
			// Tx monitors are meaningless while the laser is off on purpose.
			eval_threshold(XCVR_M_TX_BIAS, lane, dom_.tx_bias_ma[l], !tx_off, false);
			eval_threshold(XCVR_M_TX_POWER, lane, dom_.tx_power_mw[l], !tx_off, false);
			// LOS is the root cause; don't also report low Rx power.
			eval_threshold(XCVR_M_RX_POWER, lane, dom_.rx_power_mw[l], true, dom_.rx_los[l]);
		}
		set_discrete("rx_los", lane, dom_.rx_los[l], Severity::Critical, "Loss of signal");
		set_discrete("tx_fault", lane, dom_.tx_fault[l] && !tx_off, Severity::Critical, "Transmitter fault");
	}
}

// Automatic laser shutdown (ITU-T G.664, simplified): when a lane loses its
// receive signal the far end is assumed unreachable (fibre cut), so the
// local transmitter on that lane is shut off for eye safety and restored
// once light returns.
bool Port::run_als()
{
	if (!mod_.info.soft_tx_disable)
		return false;
	unsigned before = als_mask_;
	for (unsigned l = 0; l < dom_.lanes; l++) {
		unsigned bit = 1u << l;
		int rc = 0;
		if (dom_.rx_los[l] && !(als_mask_ & bit) && !dom_.tx_disabled[l]) {
			rc = xcvr_module_set_tx_disable(&mod_, bit, true);
			if (!rc) {
				als_mask_ |= bit;
				events_.emit(cfg_.name, "als_shutdown", [&](Json &j) { j.kv("lane", l); });
			}
		} else if (!dom_.rx_los[l] && (als_mask_ & bit)) {
			rc = xcvr_module_set_tx_disable(&mod_, bit, false);
			if (!rc) {
				als_mask_ &= ~bit;
				events_.emit(cfg_.name, "als_restore", [&](Json &j) { j.kv("lane", l); });
			}
		}
		if (rc)
			LOG_WARN("%s: ALS lane %u: %s", cfg_.name.c_str(), l, std::strerror(-rc));
	}
	return als_mask_ != before;
}

int Port::set_tx_disable(unsigned lane_mask, bool disable, std::string &err)
{
	if (state_ != PortState::Ready) {
		err = "port " + cfg_.name + " is " + port_state_name(state_) + ", not ready";
		return -EAGAIN;
	}
	unsigned valid = (1u << mod_.info.lanes) - 1u;
	lane_mask &= valid;
	int rc = xcvr_module_set_tx_disable(&mod_, lane_mask, disable);
	if (rc) {
		err = std::string("tx-disable failed: ") + std::strerror(-rc);
		return rc;
	}
	if (!disable)
		als_mask_ &= ~lane_mask; // operator override
	events_.emit(cfg_.name, "tx_disable", [&](Json &j) {
		j.kv("lane_mask", lane_mask);
		j.kv("disabled", disable);
		j.kv("source", "operator");
	});
	next_due_ = 0; // refresh readings on the next loop iteration
	return 0;
}

void Port::to_json(Json &j, bool detail) const
{
	j.begin_object();
	j.kv("name", cfg_.name);
	j.kv("bus", cfg_.bus);
	j.kv("state", port_state_name(state_));
	j.kv("state_age_ms", mono_ms() - state_since_);
	j.kv("als", cfg_.als);
	j.kv("alarms", alarms_.list(cfg_.name).size());

	const xcvr_info &in = mod_.info;
	if (in.type != XCVR_TYPE_UNKNOWN) {
		j.key("module").begin_object();
		j.kv("type", xcvr_type_name(in.type));
		j.kv("identifier", in.identifier);
		j.kv("identifier_name", xcvr_identifier_name(in.identifier));
		j.kv("vendor", in.vendor);
		j.kv("part_number", in.part_number);
		j.kv("serial", in.serial);
		if (detail) {
			j.kv("revision", in.revision);
			j.kv("date_code", in.date_code);
			j.kv("connector", xcvr_connector_name(in.connector));
			j.kv("wavelength_nm", in.wavelength_nm);
			j.kv("lanes", in.lanes);
			j.kv("ddm", in.ddm_supported);
			j.kv("ext_calibrated", in.ext_calibrated);
			j.kv("checksums_ok", in.cc_base_ok && in.cc_ext_ok && in.cc_dmi_ok);
		}
		j.end_object();
	}

	if (have_dom_) {
		j.key("dom").begin_object();
		j.kv("temp_c", dom_.temp_c);
		j.kv("vcc_v", dom_.vcc_v);
		if (in.type == XCVR_TYPE_CMIS)
			j.kv("module_state", cmis_module_state_name(dom_.module_state));
		if (detail) {
			j.key("lanes").begin_array();
			for (unsigned l = 0; l < dom_.lanes; l++) {
				j.begin_object();
				j.kv("lane", l);
				j.kv("tx_bias_ma", dom_.tx_bias_ma[l]);
				j.kv("tx_power_mw", dom_.tx_power_mw[l]);
				j.kv("tx_power_dbm", xcvr_mw_to_dbm(dom_.tx_power_mw[l]));
				j.kv("rx_power_mw", dom_.rx_power_mw[l]);
				j.kv("rx_power_dbm", xcvr_mw_to_dbm(dom_.rx_power_mw[l]));
				j.kv("tx_disabled", dom_.tx_disabled[l]);
				j.kv("tx_fault", dom_.tx_fault[l]);
				j.kv("rx_los", dom_.rx_los[l]);
				j.kv("als_shutdown", (als_mask_ >> l & 1u) != 0);
				j.end_object();
			}
			j.end_array();
		} else {
			// Summary: worst-case receive power across lanes.
			double min_rx = 1e9;
			for (unsigned l = 0; l < dom_.lanes; l++)
				min_rx = dom_.rx_power_mw[l] < min_rx ? dom_.rx_power_mw[l] : min_rx;
			j.kv("min_rx_power_dbm", xcvr_mw_to_dbm(min_rx));
		}
		j.end_object();
	}

	if (detail && mod_.thresholds.valid) {
		j.key("thresholds").begin_object();
		for (int m = 0; m < XCVR_M_COUNT; m++) {
			auto em = static_cast<enum xcvr_metric>(m);
			const xcvr_thresholds &t = mod_.thresholds.m[m];
			j.key(xcvr_metric_name(em)).begin_object();
			j.kv("unit", display_unit(em));
			j.kv("high_alarm", display(em, t.high_alarm));
			j.kv("high_warning", display(em, t.high_warn));
			j.kv("low_warning", display(em, t.low_warn));
			j.kv("low_alarm", display(em, t.low_alarm));
			j.end_object();
		}
		j.end_object();
	}

	if (detail) {
		j.key("counters").begin_object();
		j.kv("insertions", counters_.insertions);
		j.kv("removals", counters_.removals);
		j.kv("polls", counters_.polls);
		j.kv("poll_errors", counters_.poll_errors);
		j.kv("load_failures", counters_.load_failures);
		j.end_object();
		if (bus_) {
			const xcvr_bus_stats &s = bus_->stats;
			j.key("bus_stats").begin_object();
			j.kv("reads", s.reads);
			j.kv("writes", s.writes);
			j.kv("bytes_read", s.bytes_read);
			j.kv("retries", s.retries);
			j.kv("errors", s.errors);
			j.end_object();
		}
	}
	j.end_object();
}

} // namespace optiwatch
