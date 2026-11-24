#include "util.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>

namespace optiwatch {

static LogLevel g_level = LogLevel::Info;

int64_t mono_ms()
{
	timespec ts{};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

std::string iso_now()
{
	timespec ts{};
	clock_gettime(CLOCK_REALTIME, &ts);
	tm t{};
	gmtime_r(&ts.tv_sec, &t);
	char buf[96];
	std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.tm_year + 1900,
		      t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec,
		      static_cast<int>(ts.tv_nsec / 1000000) % 1000);
	return buf;
}

void set_log_level(LogLevel level)
{
	g_level = level;
}

void log_msg(LogLevel level, const char *fmt, ...)
{
	if (level > g_level)
		return;
	static const char *const names[] = { "ERROR", "WARN", "INFO", "DEBUG" };
	std::fprintf(stderr, "%s %-5s ", iso_now().c_str(), names[static_cast<int>(level)]);
	va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fputc('\n', stderr);
}

} // namespace optiwatch
