/*
 * Simulator backend: talks to optisim (sim/optisim) over a stream socket.
 *
 * Wire protocol (all fields single bytes unless noted):
 *
 *   request   op  port  addr  offset  len  [data * len   when op == 'W']
 *   response  status                       [data * len   when op == 'R' and status == 0]
 *                                          [present      when op == 'P' and status == 0]
 *
 *   op      'R' read, 'W' write, 'P' query MOD_ABS (presence)
 *   status  0 on success, otherwise an errno value (ENXIO = NACK, EIO = bus error)
 *
 * Targets:  /path/to/socket:PORT   or   HOST:TCPPORT:PORT
 * The connection is opened lazily and re-opened after any socket error,
 * so the daemon survives a simulator restart.
 */
#include "xcvr/bus.h"

#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

struct sim_priv {
	int fd;
	uint8_t port;
	char path[108];     /* unix socket path, or empty */
	char host[64];
	char service[16];
};

static void sim_disconnect(struct sim_priv *p)
{
	if (p->fd >= 0)
		close(p->fd);
	p->fd = -1;
}

static int set_timeouts(int fd)
{
	struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
	if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0 ||
	    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0)
		return -errno;
	return 0;
}

static int sim_connect(struct sim_priv *p)
{
	if (p->fd >= 0)
		return 0;

	if (p->path[0]) {
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return -errno;
		struct sockaddr_un sa = { .sun_family = AF_UNIX };
		snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", p->path);
		if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || set_timeouts(fd) < 0) {
			int e = errno;
			close(fd);
			return -e;
		}
		p->fd = fd;
		return 0;
	}

	struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM };
	struct addrinfo *res = NULL;
	if (getaddrinfo(p->host, p->service, &hints, &res) != 0)
		return -EHOSTUNREACH;
	int rc = -ECONNREFUSED;
	for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
		int fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0 && set_timeouts(fd) == 0) {
			p->fd = fd;
			rc = 0;
			break;
		}
		rc = -errno;
		close(fd);
	}
	freeaddrinfo(res);
	return rc;
}

static int send_all(int fd, const uint8_t *buf, size_t len)
{
	while (len) {
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static int recv_all(int fd, uint8_t *buf, size_t len)
{
	while (len) {
		ssize_t n = recv(fd, buf, len, 0);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -errno;
		}
		if (n == 0)
			return -ECONNRESET;
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

/* One request/response exchange. 'in' receives len bytes for reads. */
static int sim_xfer(xcvr_bus *bus, uint8_t op, uint8_t addr, uint8_t off,
		    const uint8_t *out, uint8_t *in, size_t len)
{
	struct sim_priv *p = bus->priv;
	int rc = sim_connect(p);
	if (rc)
		return -EIO;

	uint8_t req[5 + 256];
	req[0] = op;
	req[1] = p->port;
	req[2] = addr;
	req[3] = off;
	req[4] = (uint8_t)len;
	size_t reqlen = 5;
	if (op == 'W') {
		memcpy(req + 5, out, len);
		reqlen += len;
	}

	uint8_t status;
	if ((rc = send_all(p->fd, req, reqlen)) || (rc = recv_all(p->fd, &status, 1))) {
		sim_disconnect(p);
		return -EIO;
	}
	if (status)
		return -(int)status;
	if (in && len && (rc = recv_all(p->fd, in, len))) {
		sim_disconnect(p);
		return -EIO;
	}
	return 0;
}

static int sim_read(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len)
{
	return sim_xfer(bus, 'R', addr, off, NULL, buf, len);
}

static int sim_write(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len)
{
	return sim_xfer(bus, 'W', addr, off, buf, NULL, len);
}

static int sim_present(xcvr_bus *bus)
{
	uint8_t present = 0;
	int rc = sim_xfer(bus, 'P', 0, 0, NULL, &present, 1);
	return rc ? rc : present != 0;
}

static void sim_close(xcvr_bus *bus)
{
	struct sim_priv *p = bus->priv;
	sim_disconnect(p);
	free(p);
	free(bus);
}

static const struct xcvr_bus_ops sim_ops = {
	.read = sim_read,
	.write = sim_write,
	.present = sim_present,
	.close = sim_close,
};

xcvr_bus *xcvr_bus_open_sim(const char *target, char *err, size_t errlen)
{
	struct sim_priv *p = calloc(1, sizeof(*p));
	xcvr_bus *bus = calloc(1, sizeof(*bus));
	if (!p || !bus) {
		free(p);
		free(bus);
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	p->fd = -1;

	const char *last = strrchr(target, ':');
	char *end = NULL;
	long port = last ? strtol(last + 1, &end, 10) : -1;
	if (!last || !end || *end || port < 0 || port > 255) {
		snprintf(err, errlen, "sim target '%s' must end in :PORT (0-255)", target);
		goto fail;
	}
	p->port = (uint8_t)port;

	size_t head = (size_t)(last - target);
	if (target[0] == '/' || target[0] == '.') {
		if (head >= sizeof(p->path)) {
			snprintf(err, errlen, "socket path too long");
			goto fail;
		}
		memcpy(p->path, target, head);
		p->path[head] = '\0';
	} else {
		const char *colon = memchr(target, ':', head);
		if (!colon) {
			snprintf(err, errlen, "sim target '%s' must be /path:PORT or HOST:TCPPORT:PORT", target);
			goto fail;
		}
		size_t hl = (size_t)(colon - target);
		size_t sl = head - hl - 1;
		if (hl == 0 || hl >= sizeof(p->host) || sl == 0 || sl >= sizeof(p->service)) {
			snprintf(err, errlen, "bad host/port in sim target '%s'", target);
			goto fail;
		}
		memcpy(p->host, target, hl);
		memcpy(p->service, colon + 1, sl);
	}

	bus->ops = &sim_ops;
	bus->priv = p;
	bus->max_xfer = 128;
	return bus;

fail:
	free(p);
	free(bus);
	return NULL;
}
