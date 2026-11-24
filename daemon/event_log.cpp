#include "event_log.hpp"

#include <cerrno>
#include <cstring>

#include "util.hpp"

namespace optiwatch {

EventLog::~EventLog()
{
	if (owns_file_ && file_)
		std::fclose(file_);
}

bool EventLog::open(const std::string &path, std::string &err)
{
	path_ = path;
	if (path.empty())
		return true;
	if (path == "-") {
		file_ = stdout;
		owns_file_ = false;
		return true;
	}
	file_ = std::fopen(path.c_str(), "ae");
	if (!file_) {
		err = "open " + path + ": " + std::strerror(errno);
		return false;
	}
	owns_file_ = true;
	return true;
}

void EventLog::reopen()
{
	if (!owns_file_)
		return;
	FILE *f = std::fopen(path_.c_str(), "ae");
	if (!f) {
		LOG_ERR("event log: reopen %s: %s", path_.c_str(), std::strerror(errno));
		return;
	}
	std::fclose(file_);
	file_ = f;
	LOG_INFO("event log reopened");
}

void EventLog::emit(const std::string &port, const std::string &event, const Fields &fields)
{
	Json j;
	j.begin_object();
	j.kv("seq", ++seq_);
	j.kv("ts", iso_now());
	j.kv("port", port);
	j.kv("event", event);
	if (fields)
		fields(j);
	j.end_object();

	const std::string &line = j.str();
	if (file_) {
		std::fputs(line.c_str(), file_);
		std::fputc('\n', file_);
		std::fflush(file_);
	}
	ring_.push_back(line);
	while (ring_.size() > capacity_)
		ring_.pop_front();
	LOG_DEBUG("event %s", line.c_str());
}

std::vector<std::string> EventLog::recent(size_t n) const
{
	size_t start = ring_.size() > n ? ring_.size() - n : 0;
	return { ring_.begin() + static_cast<std::ptrdiff_t>(start), ring_.end() };
}

} // namespace optiwatch
