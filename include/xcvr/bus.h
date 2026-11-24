/*
 * I2C bus abstraction for one transceiver cage.
 *
 * A bus moves bytes to and from a 7-bit device address (0x50 / 0x51 for
 * pluggable optics) and answers whether a module is seated. Backends:
 *
 *   i2c:/dev/i2c-N            Linux i2c-dev (I2C_RDWR ioctl). Works against real
 *                             hardware or the xcvr_i2c_emu kernel module.
 *   sim:/path/to.sock:PORT    Unix socket to the Python simulator (optisim).
 *   sim:HOST:TCPPORT:PORT     TCP socket to the Python simulator.
 *   mem:sfp | mem:cmis        In-process memory image (unit tests).
 *
 * The generic layer splits transfers into max_xfer-sized chunks that never
 * cross a 128-byte page boundary and retries failed transactions with a
 * short backoff. All functions return 0 on success or a negative errno.
 */
#ifndef XCVR_BUS_H
#define XCVR_BUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xcvr_bus xcvr_bus;

struct xcvr_bus_ops {
	int (*read)(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len);
	int (*write)(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len);
	/* 1 = present, 0 = absent, -ENOTSUP = backend has no presence signal (probe instead) */
	int (*present)(xcvr_bus *bus);
	void (*close)(xcvr_bus *bus);
};

struct xcvr_bus_stats {
	unsigned long reads;
	unsigned long writes;
	unsigned long bytes_read;
	unsigned long bytes_written;
	unsigned long retries;
	unsigned long errors;   /* transactions that failed after all retries */
};

struct xcvr_bus {
	const struct xcvr_bus_ops *ops;
	void *priv;
	char spec[128];
	unsigned max_xfer;      /* largest single transaction, bytes */
	unsigned retries;       /* extra attempts per transaction */
	unsigned retry_delay_us;
	struct xcvr_bus_stats stats;
};

/* Open a bus from a spec string (see above). On failure returns NULL and
 * writes a message into err. */
xcvr_bus *xcvr_bus_open(const char *spec, char *err, size_t errlen);
void xcvr_bus_close(xcvr_bus *bus);

int xcvr_bus_read(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t *buf, size_t len);
int xcvr_bus_write(xcvr_bus *bus, uint8_t addr, uint8_t off, const uint8_t *buf, size_t len);
int xcvr_bus_write_byte(xcvr_bus *bus, uint8_t addr, uint8_t off, uint8_t val);

/* 1 if a module is seated, 0 if not. Falls back to probing byte 0 at 0x50
 * when the backend has no presence signal. */
int xcvr_bus_present(xcvr_bus *bus);

/* Backend constructors, used by xcvr_bus_open(). */
xcvr_bus *xcvr_bus_open_i2cdev(const char *path, char *err, size_t errlen);
xcvr_bus *xcvr_bus_open_sim(const char *target, char *err, size_t errlen);
xcvr_bus *xcvr_bus_open_mem(const char *kind, char *err, size_t errlen);

/* Memory backend controls (unit tests). */
uint8_t *xcvr_mem_sfp_page(xcvr_bus *bus, uint8_t addr); /* 256-byte image for 0x50/0x51 */
uint8_t *xcvr_mem_cmis_page(xcvr_bus *bus, uint8_t page); /* 128-byte upper page, or lower page for 0xFF */
void xcvr_mem_set_present(xcvr_bus *bus, int present);
void xcvr_mem_fail_next(xcvr_bus *bus, unsigned n_transactions);

#ifdef __cplusplus
}
#endif

#endif /* XCVR_BUS_H */
