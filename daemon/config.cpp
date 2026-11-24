#include "config.hpp"

#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>

namespace optiwatch {

static std::string trim(const std::string &s)
{
	size_t b = s.find_first_not_of(" \t\r");
	if (b == std::string::npos)
		return "";
	size_t e = s.find_last_not_of(" \t\r");
	return s.substr(b, e - b + 1);
}

static bool to_int(const std::string &v, int &out)
{
	char *end = nullptr;
	long x = std::strtol(v.c_str(), &end, 10);
	if (v.empty() || *end || x < 0 || x > 3600000)
		return false;
	out = static_cast<int>(x);
	return true;
}

static bool to_double(const std::string &v, double &out)
{
	char *end = nullptr;
	out = std::strtod(v.c_str(), &end);
	return !v.empty() && !*end;
}

static bool to_bool(const std::string &v, bool &out)
{
	if (v == "true" || v == "yes" || v == "on" || v == "1") {
		out = true;
		return true;
	}
	if (v == "false" || v == "no" || v == "off" || v == "0") {
		out = false;
		return true;
	}
	return false;
}

static bool set_daemon_key(Config &cfg, const std::string &k, const std::string &v)
{
	Tunables &t = cfg.tunables;
	const std::map<std::string, int *> ints = {
		{ "poll_interval_ms", &t.poll_interval_ms },
		{ "fast_poll_ms", &t.fast_poll_ms },
		{ "insert_settle_ms", &t.insert_settle_ms },
		{ "power_up_timeout_ms", &t.power_up_timeout_ms },
		{ "max_poll_errors", &t.max_poll_errors },
		{ "max_load_attempts", &t.max_load_attempts },
		{ "fault_retry_ms", &t.fault_retry_ms },
	};
	const std::map<std::string, double *> doubles = {
		{ "hysteresis_temp_c", &t.hysteresis.temp_c },
		{ "hysteresis_vcc_v", &t.hysteresis.vcc_v },
		{ "hysteresis_bias_ma", &t.hysteresis.bias_ma },
		{ "hysteresis_power_db", &t.hysteresis.power_db },
	};
	if (k == "control_socket") {
		cfg.control_socket = v;
		return true;
	}
	if (k == "event_log") {
		cfg.event_log = v;
		return true;
	}
	if (auto it = ints.find(k); it != ints.end())
		return to_int(v, *it->second);
	if (auto it = doubles.find(k); it != doubles.end())
		return to_double(v, *it->second) && *it->second >= 0;
	return false;
}

static bool set_port_key(PortConfig &p, const std::string &k, const std::string &v)
{
	if (k == "bus") {
		p.bus = v;
		return true;
	}
	if (k == "als")
		return to_bool(v, p.als);
	if (k == "max_xfer") {
		int x;
		if (!to_int(v, x) || x < 1 || x > 128)
			return false;
		p.max_xfer = static_cast<unsigned>(x);
		return true;
	}
	if (k == "retries")
		return to_int(v, p.retries) && p.retries <= 10;
	return false;
}

bool load_config_file(const std::string &path, Config &cfg, std::string &err)
{
	std::ifstream in(path);
	if (!in) {
		err = "cannot open " + path;
		return false;
	}

	enum { NONE, DAEMON, PORT } section = NONE;
	std::string line;
	int lineno = 0;
	while (std::getline(in, line)) {
		lineno++;
		auto where = [&] { return path + ":" + std::to_string(lineno) + ": "; };
		size_t hash = line.find_first_of(";#");
		if (hash != std::string::npos)
			line.erase(hash);
		line = trim(line);
		if (line.empty())
			continue;

		if (line.front() == '[') {
			if (line.back() != ']') {
				err = where() + "unterminated section header";
				return false;
			}
			std::string name = trim(line.substr(1, line.size() - 2));
			if (name == "daemon") {
				section = DAEMON;
			} else if (name.rfind("port ", 0) == 0) {
				PortConfig p;
				p.name = trim(name.substr(5));
				if (p.name.empty()) {
					err = where() + "port section needs a name";
					return false;
				}
				for (const auto &existing : cfg.ports)
					if (existing.name == p.name) {
						err = where() + "duplicate port '" + p.name + "'";
						return false;
					}
				cfg.ports.push_back(p);
				section = PORT;
			} else {
				err = where() + "unknown section [" + name + "]";
				return false;
			}
			continue;
		}

		size_t eq = line.find('=');
		if (eq == std::string::npos) {
			err = where() + "expected key = value";
			return false;
		}
		std::string k = trim(line.substr(0, eq));
		std::string v = trim(line.substr(eq + 1));
		bool ok = section == DAEMON ? set_daemon_key(cfg, k, v)
			: section == PORT ? set_port_key(cfg.ports.back(), k, v)
			: false;
		if (!ok) {
			err = where() + (section == NONE ? "key outside of a section" : "invalid key or value '" + k + "'");
			return false;
		}
	}

	for (const auto &p : cfg.ports)
		if (p.bus.empty()) {
			err = path + ": port '" + p.name + "' has no bus";
			return false;
		}
	return true;
}

bool parse_port_arg(const std::string &arg, PortConfig &out, std::string &err)
{
	size_t eq = arg.find('=');
	if (eq == std::string::npos || eq == 0 || eq + 1 == arg.size()) {
		err = "--port expects NAME=BUS-SPEC, got '" + arg + "'";
		return false;
	}
	out.name = arg.substr(0, eq);
	out.bus = arg.substr(eq + 1);
	return true;
}

} // namespace optiwatch
