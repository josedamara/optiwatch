// Minimal streaming JSON writer (no dependencies).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace optiwatch {

class Json {
public:
	Json &begin_object() { open('{'); return *this; }
	Json &end_object() { close('}'); return *this; }
	Json &begin_array() { open('['); return *this; }
	Json &end_array() { close(']'); return *this; }

	Json &key(std::string_view k)
	{
		sep();
		quote(k);
		out_ += ':';
		after_key_ = true;
		return *this;
	}

	Json &value(std::string_view s) { sep(); quote(s); return *this; }
	Json &value(const char *s) { return s ? value(std::string_view(s)) : null(); }
	Json &value(const std::string &s) { return value(std::string_view(s)); }
	Json &value(bool b) { sep(); out_ += b ? "true" : "false"; return *this; }
	Json &null() { sep(); out_ += "null"; return *this; }

	Json &value(double d)
	{
		sep();
		if (!std::isfinite(d)) {
			out_ += "null";
			return *this;
		}
		char buf[32];
		std::snprintf(buf, sizeof(buf), "%.6g", d);
		out_ += buf;
		return *this;
	}

	template <typename T>
	std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, Json &> value(T v)
	{
		sep();
		out_ += std::to_string(v);
		return *this;
	}

	// key + value shorthand
	template <typename T>
	Json &kv(std::string_view k, T &&v) { key(k); return value(std::forward<T>(v)); }

	// Splice an already serialised JSON value.
	Json &raw(std::string_view j) { sep(); out_ += j; return *this; }

	const std::string &str() const { return out_; }

	static std::string escape(std::string_view s)
	{
		Json j;
		j.quote(s);
		return j.out_;
	}

private:
	void open(char c)
	{
		sep();
		out_ += c;
		first_.push_back(true);
	}

	void close(char c)
	{
		out_ += c;
		if (!first_.empty())
			first_.pop_back();
	}

	void sep()
	{
		if (after_key_) {
			after_key_ = false;
			return;
		}
		if (!first_.empty()) {
			if (!first_.back())
				out_ += ',';
			first_.back() = false;
		}
	}

	void quote(std::string_view s)
	{
		out_ += '"';
		for (char ch : s) {
			auto c = static_cast<unsigned char>(ch);
			switch (c) {
			case '"': out_ += "\\\""; break;
			case '\\': out_ += "\\\\"; break;
			case '\n': out_ += "\\n"; break;
			case '\r': out_ += "\\r"; break;
			case '\t': out_ += "\\t"; break;
			default:
				if (c < 0x20) {
					char buf[8];
					std::snprintf(buf, sizeof(buf), "\\u%04x", c);
					out_ += buf;
				} else {
					out_ += static_cast<char>(c);
				}
			}
		}
		out_ += '"';
	}

	std::string out_;
	std::vector<bool> first_;
	bool after_key_ = false;
};

} // namespace optiwatch
