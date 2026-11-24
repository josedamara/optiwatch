// One transceiver cage and the state machine that manages it.
//
//            present                 settle time
//   EMPTY ------------> INSERTED ------------------> load EEPROM
//     ^                    ^                          |
//     | removed            | retry                    | SFF-8472, or CMIS already Ready
//     |                    |                          v
//     +---- (any) ------ FAULT <---- timeout ---- POWERING_UP (CMIS: clear LowPwr,
//                          ^                          |         wait for ModuleReady)
//                          |  I2C failures,           v
//                          +-- ModuleFault ------- READY  (poll diagnostics, alarms, ALS)
//
// A removal in any state clears the port's alarms and returns it to EMPTY.
#pragma once

#include <cstdint>
#include <string>

#include "alarm.hpp"
#include "config.hpp"
#include "event_log.hpp"
#include "json.hpp"
#include "xcvr/module.h"

namespace optiwatch {

enum class PortState { Empty, Inserted, PoweringUp, Ready, Fault };
const char *port_state_name(PortState s);

class Port {
public:
	Port(PortConfig cfg, const Tunables &t, AlarmTable &alarms, EventLog &events);
	~Port();
	Port(const Port &) = delete;
	Port &operator=(const Port &) = delete;

	bool open(std::string &err);

	// Do whatever work is due at 'now'. Returns the next time work is due.
	int64_t tick(int64_t now);

	// Operator command: soft TX disable on the lanes in mask.
	int set_tx_disable(unsigned lane_mask, bool disable, std::string &err);

	const std::string &name() const { return cfg_.name; }
	PortState state() const { return state_; }
	bool has_module() const { return state_ != PortState::Empty; }
	const xcvr_module &module() const { return mod_; }
	const xcvr_dom &dom() const { return dom_; }
	bool has_dom() const { return have_dom_; }
	unsigned als_mask() const { return als_mask_; }

	void to_json(Json &j, bool detail) const;

private:
	void enter(PortState s, const std::string &why);
	void handle_removed();
	void do_load(int64_t now);
	void do_power_up(int64_t now);
	void do_poll(int64_t now);
	void fault(const std::string &alarm_type, Severity sev, const std::string &msg, bool permanent = false);

	void evaluate_alarms();
	void eval_threshold(enum xcvr_metric m, int lane, double value, bool enabled, bool suppress_low);
	void set_discrete(const std::string &type, int lane, bool on, Severity sev, const std::string &msg);
	bool run_als(); // true if any lane changed

	PortConfig cfg_;
	const Tunables &t_;
	AlarmTable &alarms_;
	EventLog &events_;

	xcvr_bus *bus_ = nullptr;
	xcvr_module mod_{};
	xcvr_dom dom_{};
	bool have_dom_ = false;

	PortState state_ = PortState::Empty;
	int64_t state_since_ = 0;
	int64_t next_due_ = 0;
	int load_attempts_ = 0;
	int consecutive_errors_ = 0;
	bool permanent_fault_ = false;  // e.g. unsupported module: wait for removal
	bool power_up_requested_ = false;
	unsigned als_mask_ = 0;         // lanes we shut down for ALS

	struct {
		uint64_t insertions = 0;
		uint64_t removals = 0;
		uint64_t polls = 0;
		uint64_t poll_errors = 0;
		uint64_t load_failures = 0;
	} counters_;
};

} // namespace optiwatch
