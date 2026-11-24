#include "alarm.hpp"

#include "util.hpp"

namespace optiwatch {

const char *severity_name(Severity s)
{
	switch (s) {
	case Severity::Critical: return "critical";
	case Severity::Major:    return "major";
	case Severity::Minor:    return "minor";
	case Severity::Warning:  return "warning";
	}
	return "?";
}

bool AlarmTable::active(const std::string &port, const std::string &type, int lane) const
{
	return active_.count({ port, type, lane }) != 0;
}

void AlarmTable::to_json(Json &j, const Alarm &a)
{
	j.kv("alarm", a.type);
	if (a.lane >= 0)
		j.kv("lane", a.lane);
	j.kv("severity", severity_name(a.severity));
	j.kv("message", a.message);
	if (!a.unit.empty()) {
		j.kv("value", a.value);
		j.kv("threshold", a.threshold);
		j.kv("unit", a.unit);
	}
}

bool AlarmTable::raise(const Alarm &a)
{
	Key k{ a.port, a.type, a.lane };
	auto it = active_.find(k);
	if (it != active_.end()) {
		it->second.value = a.value;
		return false;
	}
	Alarm stored = a;
	stored.raised_at = iso_now();
	stored.raised_ms = mono_ms();
	active_.emplace(k, stored);
	raised_total_++;

	LOG_WARN("%s: %s alarm raised: %s%s", a.port.c_str(), severity_name(a.severity),
		 a.message.c_str(), a.lane >= 0 ? (" (lane " + std::to_string(a.lane) + ")").c_str() : "");
	log_.emit(a.port, "alarm_raised", [&](Json &j) { to_json(j, stored); });
	return true;
}

bool AlarmTable::clear(const std::string &port, const std::string &type, int lane, const char *reason)
{
	auto it = active_.find({ port, type, lane });
	if (it == active_.end())
		return false;
	Alarm a = it->second;
	active_.erase(it);
	int64_t dur = mono_ms() - a.raised_ms;

	LOG_INFO("%s: alarm cleared: %s (%s)", port.c_str(), a.message.c_str(), reason);
	log_.emit(port, "alarm_cleared", [&](Json &j) {
		j.kv("alarm", a.type);
		if (a.lane >= 0)
			j.kv("lane", a.lane);
		j.kv("severity", severity_name(a.severity));
		j.kv("reason", reason);
		j.kv("duration_ms", dur);
		if (!a.unit.empty()) {
			j.kv("value", a.value);
			j.kv("unit", a.unit);
		}
	});
	return true;
}

void AlarmTable::clear_port(const std::string &port, const char *reason)
{
	std::vector<Key> keys;
	for (const auto &[k, a] : active_)
		if (std::get<0>(k) == port)
			keys.push_back(k);
	for (const auto &k : keys)
		clear(std::get<0>(k), std::get<1>(k), std::get<2>(k), reason);
}

std::vector<Alarm> AlarmTable::list(const std::string &port) const
{
	std::vector<Alarm> out;
	for (const auto &[k, a] : active_)
		if (port.empty() || a.port == port)
			out.push_back(a);
	return out;
}

} // namespace optiwatch
