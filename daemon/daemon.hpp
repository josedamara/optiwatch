// Shared daemon context.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "alarm.hpp"
#include "config.hpp"
#include "event_log.hpp"
#include "port.hpp"

namespace optiwatch {

#define OPTIWATCH_VERSION "0.1.0"

struct Daemon {
	Config cfg;
	EventLog events;
	AlarmTable alarms{ events };
	std::vector<std::unique_ptr<Port>> ports;
	int64_t started_ms = 0;

	Port *find_port(const std::string &name)
	{
		for (auto &p : ports)
			if (p->name() == name)
				return p.get();
		return nullptr;
	}
};

} // namespace optiwatch
