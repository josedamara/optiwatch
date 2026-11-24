// Time and logging helpers.
#pragma once

#include <cstdint>
#include <string>

namespace optiwatch {

// Monotonic milliseconds, for scheduling.
int64_t mono_ms();

// Wall clock as ISO-8601 UTC with milliseconds, for events.
std::string iso_now();

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };
void set_log_level(LogLevel level);
void log_msg(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_ERR(...) ::optiwatch::log_msg(::optiwatch::LogLevel::Error, __VA_ARGS__)
#define LOG_WARN(...) ::optiwatch::log_msg(::optiwatch::LogLevel::Warn, __VA_ARGS__)
#define LOG_INFO(...) ::optiwatch::log_msg(::optiwatch::LogLevel::Info, __VA_ARGS__)
#define LOG_DEBUG(...) ::optiwatch::log_msg(::optiwatch::LogLevel::Debug, __VA_ARGS__)

} // namespace optiwatch
