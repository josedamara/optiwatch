// Control socket: a Unix stream socket accepting one command per line and
// answering with one JSON object per line.
//
//   ping
//   ports                         summary of every port
//   port NAME                     full detail for one port
//   alarms [NAME]                 active alarms
//   events [N]                    last N events (default 20)
//   stats                         daemon counters
//   tx-disable NAME LANES on|off  LANES: all | 3 | 0,2,5 | 0-3
//   show ports | show port NAME | show alarms    same data rendered as text
#pragma once

#include <poll.h>

#include <string>
#include <vector>

#include "daemon.hpp"

namespace optiwatch {

class ControlServer {
public:
	explicit ControlServer(Daemon &d) : d_(d) {}
	~ControlServer();
	ControlServer(const ControlServer &) = delete;
	ControlServer &operator=(const ControlServer &) = delete;

	bool open(const std::string &path, std::string &err);

	// Append fds to wait on, then call service() with the poll results.
	void collect(std::vector<pollfd> &fds) const;
	void service(const std::vector<pollfd> &fds);

	// Execute one command line and return the JSON reply (exposed for tests).
	std::string execute(const std::string &line);

private:
	struct Client {
		int fd;
		std::string in;
	};
	void accept_clients();
	bool read_client(Client &c);

	Daemon &d_;
	std::string path_;
	int listen_fd_ = -1;
	std::vector<Client> clients_;
};

} // namespace optiwatch
