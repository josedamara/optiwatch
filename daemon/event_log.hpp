// Structured event log: one JSON object per line, plus an in-memory ring
// buffer so recent events can be queried over the control socket.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "json.hpp"

namespace optiwatch {

class EventLog {
public:
	explicit EventLog(size_t capacity = 512) : capacity_(capacity) {}
	~EventLog();
	EventLog(const EventLog &) = delete;
	EventLog &operator=(const EventLog &) = delete;

	// path: file to append to, "-" for stdout, "" to keep events in memory only.
	bool open(const std::string &path, std::string &err);
	void reopen(); // after log rotation (SIGHUP)

	using Fields = std::function<void(Json &)>;
	void emit(const std::string &port, const std::string &event, const Fields &fields = nullptr);

	// Most recent events (oldest first), each a serialised JSON object.
	std::vector<std::string> recent(size_t n) const;
	uint64_t count() const { return seq_; }

private:
	std::string path_;
	FILE *file_ = nullptr;
	bool owns_file_ = false;
	size_t capacity_;
	std::deque<std::string> ring_;
	uint64_t seq_ = 0;
};

} // namespace optiwatch
