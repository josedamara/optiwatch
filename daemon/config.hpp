// Daemon configuration: INI file plus command-line overrides.
//
//   [daemon]
//   control_socket   = /run/optiwatchd.sock
//   event_log        = /var/log/optiwatchd/events.jsonl   ; "-" = stdout
//   poll_interval_ms = 1000
//
//   [port eth0]
//   bus = i2c:/dev/i2c-3
//   als = true          ; automatic laser shutdown on Rx LOS (ITU-T G.664)
#pragma once

#include <string>
#include <vector>

namespace optiwatch {

struct Hysteresis {
	double temp_c = 1.0;
	double vcc_v = 0.05;
	double bias_ma = 0.5;
	double power_db = 0.5;
};

struct Tunables {
	int poll_interval_ms = 1000;   // READY ports
	int fast_poll_ms = 100;        // ports that are coming up
	int insert_settle_ms = 200;    // wait after MOD_ABS deasserts before touching the EEPROM
	int power_up_timeout_ms = 5000;
	int max_poll_errors = 3;       // consecutive I2C failures before the port is faulted
	int max_load_attempts = 3;
	int fault_retry_ms = 5000;     // how often a faulted port retries initialisation
	Hysteresis hysteresis;
};

struct PortConfig {
	std::string name;
	std::string bus;
	bool als = false;
	unsigned max_xfer = 0;         // 0: backend default
	int retries = -1;              // -1: library default
};

struct Config {
	std::string control_socket = "/tmp/optiwatchd.sock";
	std::string event_log = "-";
	Tunables tunables;
	std::vector<PortConfig> ports;
};

// Parse an INI file into cfg. Returns false and sets err on failure.
bool load_config_file(const std::string &path, Config &cfg, std::string &err);

// Parse "NAME=SPEC" as given to --port.
bool parse_port_arg(const std::string &arg, PortConfig &out, std::string &err);

} // namespace optiwatch
