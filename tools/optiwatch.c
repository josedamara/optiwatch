/*
 * optiwatch: command-line client for optiwatchd.
 *
 *   optiwatch show ports
 *   optiwatch show port p1
 *   optiwatch show alarms
 *   optiwatch tx-disable p1 0-3 on
 *   optiwatch port p1              (JSON, pretty printed)
 *   optiwatch --raw ports          (JSON, one line)
 *
 * The socket path comes from -s PATH, then $OPTIWATCH_SOCKET, then
 * /tmp/optiwatchd.sock.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

/* Print a JSON document with two-space indentation. */
static void pretty(const char *s)
{
	int depth = 0, in_str = 0, esc = 0;
	for (const char *p = s; *p; p++) {
		char c = *p;
		if (in_str) {
			putchar(c);
			if (esc)
				esc = 0;
			else if (c == '\\')
				esc = 1;
			else if (c == '"')
				in_str = 0;
			continue;
		}
		switch (c) {
		case '"':
			in_str = 1;
			putchar(c);
			break;
		case '{':
		case '[':
			if (p[1] == (c == '{' ? '}' : ']')) {
				printf("%c%c", c, p[1]);
				p++;
				break;
			}
			putchar(c);
			printf("\n%*s", 2 * ++depth, "");
			break;
		case '}':
		case ']':
			printf("\n%*s%c", 2 * --depth, "", c);
			break;
		case ',':
			printf(",\n%*s", 2 * depth, "");
			break;
		case ':':
			printf(": ");
			break;
		default:
			putchar(c);
		}
	}
	putchar('\n');
}

/* Extract and unescape the "text" string from a {"ok":true,"text":"..."} reply. */
static int print_text(const char *s)
{
	const char *p = strstr(s, "\"text\":\"");
	if (!p)
		return 0;
	for (p += 8; *p && *p != '"'; p++) {
		if (*p != '\\') {
			putchar(*p);
			continue;
		}
		switch (*++p) {
		case 'n': putchar('\n'); break;
		case 't': putchar('\t'); break;
		case '\0': return 1;
		default: putchar(*p);
		}
	}
	return 1;
}

int main(int argc, char **argv)
{
	const char *path = getenv("OPTIWATCH_SOCKET");
	int raw = 0;
	int i = 1;
	for (; i < argc && argv[i][0] == '-'; i++) {
		if (!strcmp(argv[i], "-s") && i + 1 < argc)
			path = argv[++i];
		else if (!strcmp(argv[i], "--raw"))
			raw = 1;
		else {
			fprintf(stderr, "usage: %s [-s SOCKET] [--raw] COMMAND [ARGS...]\n", argv[0]);
			return 2;
		}
	}
	if (!path)
		path = "/tmp/optiwatchd.sock";
	if (i >= argc) {
		fprintf(stderr,
			"usage: %s [-s SOCKET] [--raw] COMMAND [ARGS...]\n"
			"commands: ping, ports, port NAME, alarms [NAME], events [N], stats,\n"
			"          tx-disable NAME LANES on|off, show ports|port NAME|alarms\n",
			argv[0]);
		return 2;
	}

	char line[1024] = "";
	size_t used = 0;
	for (; i < argc; i++) {
		int n = snprintf(line + used, sizeof(line) - used, "%s%s", used ? " " : "", argv[i]);
		if (n < 0 || (size_t)n >= sizeof(line) - used - 1) {
			fprintf(stderr, "optiwatch: command too long\n");
			return 2;
		}
		used += (size_t)n;
	}
	line[used++] = '\n';

	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
		fprintf(stderr, "optiwatch: cannot connect to %s: %s (is optiwatchd running?)\n", path,
			strerror(errno));
		return 1;
	}
	if (write(fd, line, used) != (ssize_t)used) {
		fprintf(stderr, "optiwatch: write: %s\n", strerror(errno));
		return 1;
	}

	size_t cap = 4096, len = 0;
	char *buf = malloc(cap);
	for (;;) {
		if (len + 1024 > cap) {
			cap *= 2;
			char *nb = realloc(buf, cap);
			if (!nb) {
				free(buf);
				return 1;
			}
			buf = nb;
		}
		ssize_t n = read(fd, buf + len, cap - len - 1);
		if (n <= 0)
			break;
		len += (size_t)n;
		if (buf[len - 1] == '\n')
			break;
	}
	close(fd);
	buf[len] = '\0';
	if (len && buf[len - 1] == '\n')
		buf[--len] = '\0';

	int ok = strstr(buf, "\"ok\":true") != NULL;
	if (raw)
		puts(buf);
	else if (!(ok && print_text(buf)))
		pretty(buf);
	free(buf);
	return ok ? 0 : 1;
}
