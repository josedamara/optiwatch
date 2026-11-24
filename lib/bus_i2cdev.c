/*
 * Linux i2c-dev backend.
 *
 * Uses the I2C_RDWR ioctl so that a register read is one combined
 * transaction: a write of the offset byte followed by a repeated-start read,
 * which is what SFF-8472 and CMIS modules expect (random read).
 */
#include "xcvr/bus.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <sys/ioctl.h>

struct i2cdev_priv {
	int fd;
};

/* Adapters report a missing ACK as ENXIO or EREMOTEIO; treat both as
 * "nobody home" so the generic layer does not retry them. */
static int map_errno(int e)
{
	return (e == EREMOTEIO || e == ENXIO) ? -ENXIO : -e;
}

static int i2cdev_read(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len)
{
	struct i2cdev_priv *p = bus->priv;
	struct i2c_msg msgs[2] = {
		{ .addr = addr, .flags = 0, .len = 1, .buf = &off },
		{ .addr = addr, .flags = I2C_M_RD, .len = (uint16_t)len, .buf = buf },
	};
	struct i2c_rdwr_ioctl_data xfer = { .msgs = msgs, .nmsgs = 2 };
	if (ioctl(p->fd, I2C_RDWR, &xfer) < 0)
		return map_errno(errno);
	return 0;
}

static int i2cdev_write(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len)
{
	struct i2cdev_priv *p = bus->priv;
	uint8_t tmp[1 + 32];
	if (len > sizeof(tmp) - 1)
		return -EINVAL;
	tmp[0] = off;
	memcpy(tmp + 1, buf, len);
	struct i2c_msg msg = { .addr = addr, .flags = 0, .len = (uint16_t)(len + 1), .buf = tmp };
	struct i2c_rdwr_ioctl_data xfer = { .msgs = &msg, .nmsgs = 1 };
	if (ioctl(p->fd, I2C_RDWR, &xfer) < 0)
		return map_errno(errno);
	return 0;
}

static int i2cdev_present(xcvr_bus *bus)
{
	(void)bus;
	return -ENOTSUP; /* no MOD_ABS line through i2c-dev: probe the EEPROM instead */
}

static void i2cdev_close(xcvr_bus *bus)
{
	struct i2cdev_priv *p = bus->priv;
	if (p->fd >= 0)
		close(p->fd);
	free(p);
	free(bus);
}

static const struct xcvr_bus_ops i2cdev_ops = {
	.read = i2cdev_read,
	.write = i2cdev_write,
	.present = i2cdev_present,
	.close = i2cdev_close,
};

xcvr_bus *xcvr_bus_open_i2cdev(const char *path, char *err, size_t errlen)
{
	int fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		snprintf(err, errlen, "open %s: %s", path, strerror(errno));
		return NULL;
	}

	unsigned long funcs = 0;
	if (ioctl(fd, I2C_FUNCS, &funcs) < 0 || !(funcs & I2C_FUNC_I2C)) {
		snprintf(err, errlen, "%s: adapter does not support plain I2C transfers", path);
		close(fd);
		return NULL;
	}

	xcvr_bus *bus = calloc(1, sizeof(*bus));
	struct i2cdev_priv *p = calloc(1, sizeof(*p));
	if (!bus || !p) {
		free(bus);
		free(p);
		close(fd);
		snprintf(err, errlen, "out of memory");
		return NULL;
	}
	p->fd = fd;
	bus->ops = &i2cdev_ops;
	bus->priv = p;
	bus->max_xfer = 32;
	return bus;
}
