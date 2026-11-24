// Active alarm table.
//
// An alarm is identified by (port, type, lane); lane is -1 for module-wide
// conditions. Raising an already-active alarm only refreshes its value, so
// callers can evaluate conditions on every poll without flooding the event
// log. Every raise and clear is written to the event log.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

#include "event_log.hpp"

namespace optiwatch {

// ITU-T X.733 style perceived severity.
enum class Severity { Critical, Major, Minor, Warning };
const char *severity_name(Severity s);

struct Alarm {
	std::string port;
	std::string type;      // e.g. "rx_power_low_alarm", "rx_los", "i2c_comm_failure"
	int lane = -1;         // -1: module-wide
	Severity severity = Severity::Minor;
	double value = 0;      // measured value that triggered it (display units)
	double threshold = 0;  // crossed threshold (display units)
	std::string unit;
	std::string message;
	std::string raised_at; // ISO-8601
	int64_t raised_ms = 0; // monotonic
};

class AlarmTable {
public:
	explicit AlarmTable(EventLog &log) : log_(log) {}

	bool active(const std::string &port, const std::string &type, int lane) const;

	// Returns true if this call raised a new alarm.
	bool raise(const Alarm &a);
	// Returns true if an active alarm was cleared.
	bool clear(const std::string &port, const std::string &type, int lane, const char *reason);
	void clear_port(const std::string &port, const char *reason);

	std::vector<Alarm> list(const std::string &port = "") const;
	size_t size() const { return active_.size(); }
	uint64_t raised_total() const { return raised_total_; }

	static void to_json(Json &j, const Alarm &a);

private:
	using Key = std::tuple<std::string, std::string, int>;
	std::map<Key, Alarm> active_;
	EventLog &log_;
	uint64_t raised_total_ = 0;
};

} // namespace optiwatch
