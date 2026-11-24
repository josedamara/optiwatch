#include "control.hpp"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>

#include "util.hpp"

namespace optiwatch {

namespace {

constexpr size_t kMaxClients = 16;
constexpr size_t kMaxLine = 1024;

std::vector<std::string> split(const std::string &s)
{
	std::istringstream in(s);
	std::vector<std::string> out;
	std::string w;
	while (in >> w)
		out.push_back(w);
	return out;
}

std::string error_reply(const std::string &msg)
{
	Json j;
	j.begin_object().kv("ok", false).kv("error", msg).end_object();
	return j.str();
}

std::string text_reply(const std::string &text)
{
	Json j;
	j.begin_object().kv("ok", true).kv("text", text).end_object();
	return j.str();
}

// "all", "3", "0,2,5", "0-3" -> bit mask. Returns false on syntax error.
bool parse_lanes(const std::string &s, unsigned &mask)
{
	if (s == "all") {
		mask = 0xFFu;
		return true;
	}
	mask = 0;
	std::istringstream in(s);
	std::string part;
	while (std::getline(in, part, ',')) {
		int a = -1, b = -1;
		char dash = 0;
		std::istringstream p(part);
		if (!(p >> a) || a < 0 || a > 7)
			return false;
		b = a;
		if (p >> dash) {
			if (dash != '-' || !(p >> b) || b < a || b > 7)
				return false;
		}
		for (int l = a; l <= b; l++)
			mask |= 1u << l;
	}
	return mask != 0;
}

void send_all(int fd, const std::string &s)
{
	size_t off = 0;
	while (off < s.size()) {
		ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				pollfd p{ fd, POLLOUT, 0 };
				if (::poll(&p, 1, 1000) <= 0)
					return; // client is not reading; drop the rest
				continue;
			}
			return;
		}
		off += static_cast<size_t>(n);
	}
}

std::string pad(std::string s, size_t w)
{
	if (s.size() < w)
		s.append(w - s.size(), ' ');
	return s;
}

std::string num(double v, int prec)
{
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.*f", prec, v);
	return buf;
}

std::string render_ports(Daemon &d)
{
	std::ostringstream o;
	o << pad("PORT", 8) << pad("STATE", 13) << pad("TYPE", 10) << pad("PART", 18) << pad("SERIAL", 13)
	  << pad("TEMP(C)", 9) << pad("MIN RX(dBm)", 13) << "ALARMS\n";
	for (auto &p : d.ports) {
		const xcvr_info &in = p->module().info;
		bool known = in.type != XCVR_TYPE_UNKNOWN;
		std::string temp = "-", rx = "-";
		if (p->has_dom()) {
			const xcvr_dom &dom = p->dom();
			temp = num(dom.temp_c, 1);
			double min_rx = 1e9;
			for (unsigned l = 0; l < dom.lanes; l++)
				min_rx = std::min(min_rx, dom.rx_power_mw[l]);
			rx = num(xcvr_mw_to_dbm(min_rx), 2);
		}
		o << pad(p->name(), 8) << pad(port_state_name(p->state()), 13)
		  << pad(known ? xcvr_type_name(in.type) : "-", 10) << pad(known ? in.part_number : "-", 18)
		  << pad(known ? in.serial : "-", 13) << pad(temp, 9) << pad(rx, 13)
		  << d.alarms.list(p->name()).size() << "\n";
	}
	return o.str();
}

std::string render_alarms(Daemon &d, const std::string &port)
{
	auto list = d.alarms.list(port);
	std::sort(list.begin(), list.end(), [](const Alarm &a, const Alarm &b) {
		return a.severity != b.severity ? a.severity < b.severity : a.raised_ms < b.raised_ms;
	});
	std::ostringstream o;
	if (list.empty())
		return "No active alarms\n";
	o << pad("SEVERITY", 10) << pad("PORT", 8) << pad("LANE", 6) << pad("ALARM", 24) << pad("RAISED", 26)
	  << "MESSAGE\n";
	for (const Alarm &a : list)
		o << pad(severity_name(a.severity), 10) << pad(a.port, 8)
		  << pad(a.lane >= 0 ? std::to_string(a.lane) : "-", 6) << pad(a.type, 24) << pad(a.raised_at, 26)
		  << a.message << "\n";
	return o.str();
}

std::string render_port(Daemon &d, Port &p)
{
	std::ostringstream o;
	const xcvr_info &in = p.module().info;
	o << "Port " << p.name() << "  state " << port_state_name(p.state()) << "\n";
	if (in.type != XCVR_TYPE_UNKNOWN) {
		o << "  Module     " << xcvr_identifier_name(in.identifier) << " (" << xcvr_type_name(in.type) << ")\n"
		  << "  Vendor     " << in.vendor << "  " << in.part_number << " rev " << in.revision << "\n"
		  << "  Serial     " << in.serial << "  date " << in.date_code << "\n"
		  << "  Optics     " << num(in.wavelength_nm, 1) << " nm, " << xcvr_connector_name(in.connector)
		  << ", " << in.lanes << " lane(s)\n";
	}
	if (p.has_dom()) {
		const xcvr_dom &dom = p.dom();
		o << "  Temp       " << num(dom.temp_c, 2) << " C   Vcc " << num(dom.vcc_v, 3) << " V";
		if (in.type == XCVR_TYPE_CMIS)
			o << "   " << cmis_module_state_name(dom.module_state);
		o << "\n\n  " << pad("LANE", 6) << pad("BIAS(mA)", 10) << pad("TX(dBm)", 9) << pad("RX(dBm)", 9)
		  << "STATUS\n";
		for (unsigned l = 0; l < dom.lanes; l++) {
			std::string st;
			if (dom.tx_disabled[l])
				st += (p.als_mask() >> l & 1u) ? "ALS-SHUTDOWN " : "TX-DISABLED ";
			if (dom.tx_fault[l])
				st += "TX-FAULT ";
			if (dom.rx_los[l])
				st += "RX-LOS ";
			if (!st.empty())
				st.pop_back();
			o << "  " << pad(std::to_string(l), 6) << pad(num(dom.tx_bias_ma[l], 2), 10)
			  << pad(num(xcvr_mw_to_dbm(dom.tx_power_mw[l]), 2), 9)
			  << pad(num(xcvr_mw_to_dbm(dom.rx_power_mw[l]), 2), 9) << (st.empty() ? "ok" : st) << "\n";
		}
	}
	o << "\n" << render_alarms(d, p.name());
	return o.str();
}

} // namespace

ControlServer::~ControlServer()
{
	for (auto &c : clients_)
		::close(c.fd);
	if (listen_fd_ >= 0) {
		::close(listen_fd_);
		::unlink(path_.c_str());
	}
}

bool ControlServer::open(const std::string &path, std::string &err)
{
	path_ = path;
	sockaddr_un sa{};
	sa.sun_family = AF_UNIX;
	if (path.size() >= sizeof(sa.sun_path)) {
		err = "control socket path too long";
		return false;
	}
	std::memcpy(sa.sun_path, path.c_str(), path.size() + 1);

	listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (listen_fd_ < 0) {
		err = std::string("socket: ") + std::strerror(errno);
		return false;
	}
	::unlink(path.c_str()); // stale socket from a previous run
	if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&sa), sizeof(sa)) < 0 ||
	    ::listen(listen_fd_, 64) < 0) {
		err = "bind " + path + ": " + std::strerror(errno);
		::close(listen_fd_);
		listen_fd_ = -1;
		return false;
	}
	::chmod(path.c_str(), 0660);
	return true;
}

void ControlServer::collect(std::vector<pollfd> &fds) const
{
	fds.push_back({ listen_fd_, POLLIN, 0 });
	for (const auto &c : clients_)
		fds.push_back({ c.fd, POLLIN, 0 });
}

void ControlServer::accept_clients()
{
	for (;;) {
		int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0)
			return;
		if (clients_.size() >= kMaxClients) {
			send_all(fd, error_reply("too many clients") + "\n");
			::close(fd);
			continue;
		}
		clients_.push_back({ fd, {} });
	}
}

// Returns false when the client should be dropped.
bool ControlServer::read_client(Client &c)
{
	char buf[512];
	for (;;) {
		ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
		if (n == 0)
			return false;
		if (n < 0)
			return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
		c.in.append(buf, static_cast<size_t>(n));
		size_t nl;
		while ((nl = c.in.find('\n')) != std::string::npos) {
			std::string line = c.in.substr(0, nl);
			c.in.erase(0, nl + 1);
			send_all(c.fd, execute(line) + "\n");
		}
		if (c.in.size() > kMaxLine) {
			send_all(c.fd, error_reply("line too long") + "\n");
			return false;
		}
	}
}

void ControlServer::service(const std::vector<pollfd> &fds)
{
	std::vector<int> drop;
	for (const pollfd &p : fds) {
		if (!p.revents)
			continue;
		if (p.fd == listen_fd_) {
			accept_clients();
			continue;
		}
		for (auto &c : clients_) {
			if (c.fd != p.fd)
				continue;
			if ((p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & POLLIN))
				drop.push_back(c.fd);
			else if (!read_client(c))
				drop.push_back(c.fd);
		}
	}
	for (int fd : drop) {
		::close(fd);
		clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
					      [fd](const Client &c) { return c.fd == fd; }),
			       clients_.end());
	}
}

std::string ControlServer::execute(const std::string &line)
{
	auto args = split(line);
	if (args.empty())
		return error_reply("empty command");
	const std::string &cmd = args[0];

	if (cmd == "ping") {
		Json j;
		j.begin_object().kv("ok", true).kv("version", OPTIWATCH_VERSION).end_object();
		return j.str();
	}

	if (cmd == "ports") {
		Json j;
		j.begin_object().kv("ok", true).key("ports").begin_array();
		for (auto &p : d_.ports)
			p->to_json(j, false);
		j.end_array().end_object();
		return j.str();
	}

	if (cmd == "port") {
		if (args.size() != 2)
			return error_reply("usage: port NAME");
		Port *p = d_.find_port(args[1]);
		if (!p)
			return error_reply("no such port: " + args[1]);
		Json j;
		j.begin_object().kv("ok", true).key("port");
		p->to_json(j, true);
		j.end_object();
		return j.str();
	}

	if (cmd == "alarms") {
		std::string port = args.size() > 1 ? args[1] : "";
		if (!port.empty() && !d_.find_port(port))
			return error_reply("no such port: " + port);
		Json j;
		j.begin_object().kv("ok", true).key("alarms").begin_array();
		for (const Alarm &a : d_.alarms.list(port)) {
			j.begin_object();
			j.kv("port", a.port);
			AlarmTable::to_json(j, a);
			j.kv("raised_at", a.raised_at);
			j.end_object();
		}
		j.end_array().end_object();
		return j.str();
	}

	if (cmd == "events") {
		size_t n = 20;
		if (args.size() > 1) {
			char *end = nullptr;
			long v = std::strtol(args[1].c_str(), &end, 10);
			if (*end || v <= 0)
				return error_reply("usage: events [N]");
			n = static_cast<size_t>(v);
		}
		Json j;
		j.begin_object().kv("ok", true).key("events").begin_array();
		for (const auto &e : d_.events.recent(n))
			j.raw(e);
		j.end_array().end_object();
		return j.str();
	}

	if (cmd == "stats") {
		Json j;
		j.begin_object().kv("ok", true);
		j.kv("version", OPTIWATCH_VERSION);
		j.kv("uptime_ms", mono_ms() - d_.started_ms);
		j.kv("ports", d_.ports.size());
		j.kv("alarms_active", d_.alarms.size());
		j.kv("alarms_raised_total", d_.alarms.raised_total());
		j.kv("events_total", d_.events.count());
		j.end_object();
		return j.str();
	}

	if (cmd == "tx-disable") {
		if (args.size() != 4 || (args[3] != "on" && args[3] != "off"))
			return error_reply("usage: tx-disable NAME LANES on|off");
		Port *p = d_.find_port(args[1]);
		if (!p)
			return error_reply("no such port: " + args[1]);
		unsigned mask;
		if (!parse_lanes(args[2], mask))
			return error_reply("bad lane list '" + args[2] + "' (all, 3, 0,2,5 or 0-3)");
		std::string err;
		if (p->set_tx_disable(mask, args[3] == "on", err))
			return error_reply(err);
		Json j;
		j.begin_object().kv("ok", true).end_object();
		return j.str();
	}

	if (cmd == "show") {
		if (args.size() >= 2 && args[1] == "ports")
			return text_reply(render_ports(d_));
		if (args.size() >= 2 && args[1] == "alarms")
			return text_reply(render_alarms(d_, args.size() > 2 ? args[2] : ""));
		if (args.size() == 3 && args[1] == "port") {
			Port *p = d_.find_port(args[2]);
			return p ? text_reply(render_port(d_, *p)) : error_reply("no such port: " + args[2]);
		}
		return error_reply("usage: show ports | show port NAME | show alarms [NAME]");
	}

	return error_reply("unknown command '" + cmd + "'");
}

} // namespace optiwatch
