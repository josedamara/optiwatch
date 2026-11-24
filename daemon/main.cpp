// optiwatchd: optical transceiver manager daemon.
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "control.hpp"
#include "daemon.hpp"
#include "util.hpp"

using namespace optiwatch;

static void usage(const char *argv0)
{
	std::fprintf(stderr,
		     "usage: %s [options]\n"
		     "  -c, --config FILE        INI configuration file\n"
		     "  -p, --port NAME=SPEC     add a port (repeatable), e.g. p0=i2c:/dev/i2c-3\n"
		     "                           or p1=sim:/tmp/optisim.sock:1\n"
		     "  -s, --socket PATH        control socket (default /tmp/optiwatchd.sock)\n"
		     "  -e, --events PATH        event log, '-' for stdout, '' to disable\n"
		     "  -i, --poll-ms N          poll interval for ready ports (default 1000)\n"
		     "      --als                enable automatic laser shutdown on all ports\n"
		     "  -v, --verbose            debug logging (repeat for more)\n"
		     "  -q, --quiet              errors only\n"
		     "  -V, --version\n",
		     argv0);
}

int main(int argc, char **argv)
{
	Daemon d;
	std::string err;
	std::vector<PortConfig> cli_ports;
	std::string socket_override, events_override;
	bool have_events_override = false;
	int poll_override = -1;
	bool als_all = false;
	LogLevel level = LogLevel::Info;

	enum { OPT_ALS = 1000 };
	static const option longopts[] = {
		{ "config", required_argument, nullptr, 'c' },
		{ "port", required_argument, nullptr, 'p' },
		{ "socket", required_argument, nullptr, 's' },
		{ "events", required_argument, nullptr, 'e' },
		{ "poll-ms", required_argument, nullptr, 'i' },
		{ "als", no_argument, nullptr, OPT_ALS },
		{ "verbose", no_argument, nullptr, 'v' },
		{ "quiet", no_argument, nullptr, 'q' },
		{ "version", no_argument, nullptr, 'V' },
		{ "help", no_argument, nullptr, 'h' },
		{ nullptr, 0, nullptr, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:p:s:e:i:vqVh", longopts, nullptr)) != -1) {
		switch (opt) {
		case 'c':
			if (!load_config_file(optarg, d.cfg, err)) {
				std::fprintf(stderr, "optiwatchd: %s\n", err.c_str());
				return 2;
			}
			break;
		case 'p': {
			PortConfig p;
			if (!parse_port_arg(optarg, p, err)) {
				std::fprintf(stderr, "optiwatchd: %s\n", err.c_str());
				return 2;
			}
			cli_ports.push_back(p);
			break;
		}
		case 's': socket_override = optarg; break;
		case 'e': events_override = optarg; have_events_override = true; break;
		case 'i': poll_override = std::atoi(optarg); break;
		case OPT_ALS: als_all = true; break;
		case 'v': level = level == LogLevel::Info ? LogLevel::Debug : LogLevel::Debug; break;
		case 'q': level = LogLevel::Error; break;
		case 'V': std::printf("optiwatchd %s\n", OPTIWATCH_VERSION); return 0;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	set_log_level(level);

	Config &cfg = d.cfg;
	for (auto &p : cli_ports) {
		if (std::any_of(cfg.ports.begin(), cfg.ports.end(), [&](const PortConfig &q) { return q.name == p.name; })) {
			std::fprintf(stderr, "optiwatchd: duplicate port '%s'\n", p.name.c_str());
			return 2;
		}
		cfg.ports.push_back(p);
	}
	if (!socket_override.empty())
		cfg.control_socket = socket_override;
	if (have_events_override)
		cfg.event_log = events_override;
	if (poll_override > 0)
		cfg.tunables.poll_interval_ms = poll_override;
	if (als_all)
		for (auto &p : cfg.ports)
			p.als = true;
	if (cfg.ports.empty()) {
		std::fprintf(stderr, "optiwatchd: no ports configured (use --port or a config file)\n");
		usage(argv[0]);
		return 2;
	}

	if (!d.events.open(cfg.event_log, err)) {
		std::fprintf(stderr, "optiwatchd: %s\n", err.c_str());
		return 1;
	}
	for (const auto &pc : cfg.ports) {
		auto port = std::make_unique<Port>(pc, cfg.tunables, d.alarms, d.events);
		if (!port->open(err)) {
			std::fprintf(stderr, "optiwatchd: %s\n", err.c_str());
			return 1;
		}
		d.ports.push_back(std::move(port));
	}

	ControlServer control(d);
	if (!control.open(cfg.control_socket, err)) {
		std::fprintf(stderr, "optiwatchd: %s\n", err.c_str());
		return 1;
	}

	// Signals are delivered through a signalfd so they are handled in the
	// main loop, never inside an I2C transaction.
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGHUP);
	sigprocmask(SIG_BLOCK, &mask, nullptr);
	signal(SIGPIPE, SIG_IGN);
	int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sfd < 0) {
		std::fprintf(stderr, "optiwatchd: signalfd: %s\n", std::strerror(errno));
		return 1;
	}

	d.started_ms = mono_ms();
	LOG_INFO("optiwatchd %s started: %zu port(s), control socket %s", OPTIWATCH_VERSION, d.ports.size(),
		 cfg.control_socket.c_str());
	d.events.emit("-", "daemon_started", [&](Json &j) {
		j.kv("version", OPTIWATCH_VERSION);
		j.kv("ports", d.ports.size());
	});

	bool running = true;
	std::vector<pollfd> fds;
	while (running) {
		int64_t now = mono_ms();
		int64_t next = now + 1000;
		for (auto &p : d.ports)
			next = std::min(next, p->tick(now));

		fds.clear();
		fds.push_back({ sfd, POLLIN, 0 });
		control.collect(fds);
		int timeout = static_cast<int>(std::max<int64_t>(0, next - mono_ms()));
		int n = ::poll(fds.data(), fds.size(), timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			LOG_ERR("poll: %s", std::strerror(errno));
			break;
		}
		if (n == 0)
			continue;

		if (fds[0].revents & POLLIN) {
			signalfd_siginfo si{};
			while (::read(sfd, &si, sizeof(si)) == static_cast<ssize_t>(sizeof(si))) {
				if (si.ssi_signo == SIGHUP) {
					d.events.reopen();
				} else {
					LOG_INFO("signal %u, shutting down", si.ssi_signo);
					running = false;
				}
			}
		}
		control.service(fds);
	}

	d.events.emit("-", "daemon_stopped");
	::close(sfd);
	return 0;
}
