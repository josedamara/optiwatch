// SPDX-License-Identifier: GPL-2.0
/*
 * xcvr_i2c_emu - emulated I2C adapters with pluggable optics behind them.
 *
 * Registers one I2C adapter per cage ("xcvr-emu cageN"). Each adapter
 * answers at the management addresses a pluggable module uses:
 *
 *   SFF-8472 (SFP)   0x50 = A0h serial ID, 0x51 = A2h diagnostics
 *   CMIS (QSFP-DD)   0x50 = lower page + upper page selected by byte 127
 *
 * The mode follows the identifier byte (A0h byte 0) of the loaded image.
 * Userspace (e.g. /dev/i2c-N through i2c-dev) sees an ordinary adapter, so
 * optiwatchd's i2c: backend runs unmodified against it.
 *
 * Module contents and faults are driven through debugfs:
 *
 *   /sys/kernel/debug/xcvr_i2c_emu/cageN/
 *       present      bool  module seated (absent: every address NACKs)
 *       a0           256 B SFP A0h / CMIS lower page + page 00h upper half
 *       a2           256 B SFP A2h
 *       pages        32 KiB CMIS upper pages 00h..FFh, 128 bytes each
 *       nack         bool  NACK every transfer (hung module)
 *       error_every  u32   fail every Nth transfer with -EIO (0 = off)
 *       xfers        u64   transfers seen (read only)
 *
 * Bytes that belong to the host (control registers the host writes) are
 * preserved when an image is reloaded through debugfs, so a simulator can
 * refresh live values without racing the host's writes.
 */
#include <linux/debugfs.h>
#include <linux/fs.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#define EMU_MAX_CAGES 8
#define PAGE_BYTES 128
#define NR_PAGES 256

static unsigned int nr_cages = 2;
module_param(nr_cages, uint, 0444);
MODULE_PARM_DESC(nr_cages, "number of emulated cages (1-8, default 2)");

enum emu_blob_id { BLOB_A0, BLOB_A2, BLOB_PAGES };

struct emu_cage;

struct emu_blob {
	struct emu_cage *cage;
	enum emu_blob_id id;
};

struct emu_cage {
	int index;
	struct i2c_adapter adap;
	struct mutex lock;      /* protects everything below */
	bool present;
	bool nack;
	u32 error_every;
	u64 xfers;
	u8 ptr[2];              /* current offset per device address */
	u8 a0[256];
	u8 a2[256];
	u8 pages[NR_PAGES][PAGE_BYTES];
	struct emu_blob blobs[3];
	struct dentry *dir;
};

static struct emu_cage *cages[EMU_MAX_CAGES];
static struct dentry *emu_root;

static bool id_is_cmis(u8 id)
{
	return id == 0x18 || id == 0x19 || id == 0x1A || id == 0x1E;
}

static bool cage_is_cmis(const struct emu_cage *c)
{
	return id_is_cmis(c->a0[0]);
}

/* CMIS latched flags clear when the host reads them. */
static bool cmis_clear_on_read(u8 page, unsigned int off)
{
	if (off < 128)
		return off == 8 || off == 9;
	return page == 0x11 && off >= 135 && off <= 152;
}

static u8 emu_read_byte(struct emu_cage *c, int dev, u8 off)
{
	u8 v;

	if (dev == 1)
		return c->a2[off];
	if (!cage_is_cmis(c) || off < 128) {
		v = c->a0[off];
		if (cage_is_cmis(c) && cmis_clear_on_read(0, off))
			c->a0[off] = 0;
		return v;
	}
	v = c->pages[c->a0[127]][off - 128];
	if (cmis_clear_on_read(c->a0[127], off))
		c->pages[c->a0[127]][off - 128] = 0;
	return v;
}

/* Host writes only land in registers a real module lets the host write. */
static void emu_write_byte(struct emu_cage *c, int dev, u8 off, u8 val)
{
	if (cage_is_cmis(c)) {
		if (off == 26 || off == 126 || off == 127)
			c->a0[off] = val;
		else if (off >= 128 && c->a0[127] == 0x10)
			c->pages[0x10][off - 128] = val;
		return;
	}
	if (dev == 0)
		return; /* SFP serial ID is read only */
	if (off == 110)
		c->a2[110] = (c->a2[110] & ~0x40) | (val & 0x40);
	else if (off == 127 || (off >= 128 && off <= 247))
		c->a2[off] = val;
}

static int emu_dev_index(struct emu_cage *c, u16 addr)
{
	if (addr == 0x50)
		return 0;
	if (addr == 0x51 && !cage_is_cmis(c))
		return 1;
	return -1;
}

static int emu_xfer(struct i2c_adapter *adap, struct i2c_msg *msgs, int num)
{
	struct emu_cage *c = i2c_get_adapdata(adap);
	int i, ret = num;

	mutex_lock(&c->lock);
	c->xfers++;
	if (!c->present || c->nack) {
		ret = -ENXIO;
		goto out;
	}
	if (c->error_every && (c->xfers % c->error_every) == 0) {
		ret = -EIO;
		goto out;
	}

	for (i = 0; i < num; i++) {
		struct i2c_msg *m = &msgs[i];
		int dev = emu_dev_index(c, m->addr);
		u16 j;

		if (dev < 0) {
			ret = -ENXIO;
			goto out;
		}
		if (m->flags & I2C_M_RD) {
			for (j = 0; j < m->len; j++)
				m->buf[j] = emu_read_byte(c, dev, c->ptr[dev]++);
		} else if (m->len) {
			/* First byte is the register offset, the rest is data. */
			c->ptr[dev] = m->buf[0];
			for (j = 1; j < m->len; j++)
				emu_write_byte(c, dev, c->ptr[dev]++, m->buf[j]);
		}
	}
out:
	mutex_unlock(&c->lock);
	return ret;
}

static u32 emu_func(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_EMUL;
}

static const struct i2c_algorithm emu_algo = {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 10, 0)
	.xfer = emu_xfer,
#else
	.master_xfer = emu_xfer,
#endif
	.functionality = emu_func,
};

/* debugfs blobs ------------------------------------------------------- */

static size_t blob_size(enum emu_blob_id id)
{
	return id == BLOB_PAGES ? NR_PAGES * PAGE_BYTES : 256;
}

static u8 *blob_data(struct emu_cage *c, enum emu_blob_id id)
{
	switch (id) {
	case BLOB_A0: return c->a0;
	case BLOB_A2: return c->a2;
	default:      return &c->pages[0][0];
	}
}

static ssize_t blob_read(struct file *f, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct emu_blob *b = f->private_data;
	struct emu_cage *c = b->cage;
	size_t size = blob_size(b->id);
	ssize_t ret;
	u8 *tmp = kvmalloc(size, GFP_KERNEL);

	if (!tmp)
		return -ENOMEM;
	mutex_lock(&c->lock);
	memcpy(tmp, blob_data(c, b->id), size);
	mutex_unlock(&c->lock);
	ret = simple_read_from_buffer(ubuf, count, ppos, tmp, size);
	kvfree(tmp);
	return ret;
}

/*
 * Restore host-owned registers after an image reload. Only done while a
 * module is seated: loading an image into an empty cage (before setting
 * 'present') takes every byte from the image, like inserting a new module.
 */
static void keep_host_bytes(struct emu_cage *c, enum emu_blob_id id, const u8 *old, u8 *new)
{
	if (!c->present)
		return;

	switch (id) {
	case BLOB_A0:
		if (id_is_cmis(new[0]) && new[0] == old[0]) {
			new[26] = old[26];
			new[126] = old[126];
			new[127] = old[127];
		}
		break;
	case BLOB_A2:
		new[110] = (new[110] & ~0x40) | (old[110] & 0x40);
		new[127] = old[127];
		break;
	case BLOB_PAGES:
		memcpy(new + 0x10 * PAGE_BYTES, old + 0x10 * PAGE_BYTES, PAGE_BYTES);
		break;
	}
}

static ssize_t blob_write(struct file *f, const char __user *ubuf, size_t count, loff_t *ppos)
{
	struct emu_blob *b = f->private_data;
	struct emu_cage *c = b->cage;
	size_t size = blob_size(b->id);
	ssize_t ret;
	u8 *tmp = kvmalloc(size, GFP_KERNEL);

	if (!tmp)
		return -ENOMEM;
	mutex_lock(&c->lock);
	memcpy(tmp, blob_data(c, b->id), size);
	ret = simple_write_to_buffer(tmp, size, ppos, ubuf, count);
	if (ret > 0) {
		keep_host_bytes(c, b->id, blob_data(c, b->id), tmp);
		memcpy(blob_data(c, b->id), tmp, size);
	}
	mutex_unlock(&c->lock);
	kvfree(tmp);
	return ret;
}

static const struct file_operations blob_fops = {
	.owner = THIS_MODULE,
	.open = simple_open,
	.read = blob_read,
	.write = blob_write,
	.llseek = default_llseek,
};

/* setup / teardown ---------------------------------------------------- */

static void emu_cage_destroy(struct emu_cage *c)
{
	if (!c)
		return;
	i2c_del_adapter(&c->adap);
	kvfree(c);
}

static struct emu_cage *emu_cage_create(int index)
{
	static const char *const names[] = { "a0", "a2", "pages" };
	struct emu_cage *c;
	char dname[16];
	int i, ret;

	c = kvzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return ERR_PTR(-ENOMEM);
	c->index = index;
	mutex_init(&c->lock);

	c->adap.owner = THIS_MODULE;
	c->adap.algo = &emu_algo;
	c->adap.nr = -1;
	snprintf(c->adap.name, sizeof(c->adap.name), "xcvr-emu cage%d", index);
	i2c_set_adapdata(&c->adap, c);
	ret = i2c_add_adapter(&c->adap);
	if (ret) {
		kvfree(c);
		return ERR_PTR(ret);
	}

	snprintf(dname, sizeof(dname), "cage%d", index);
	c->dir = debugfs_create_dir(dname, emu_root);
	debugfs_create_bool("present", 0644, c->dir, &c->present);
	debugfs_create_bool("nack", 0644, c->dir, &c->nack);
	debugfs_create_u32("error_every", 0644, c->dir, &c->error_every);
	debugfs_create_u64("xfers", 0444, c->dir, &c->xfers);
	for (i = 0; i < 3; i++) {
		c->blobs[i].cage = c;
		c->blobs[i].id = i;
		debugfs_create_file(names[i], 0644, c->dir, &c->blobs[i], &blob_fops);
	}

	dev_info(&c->adap.dev, "emulated transceiver cage %d\n", index);
	return c;
}

static int __init emu_init(void)
{
	unsigned int i;

	if (nr_cages < 1 || nr_cages > EMU_MAX_CAGES)
		return -EINVAL;

	emu_root = debugfs_create_dir("xcvr_i2c_emu", NULL);
	for (i = 0; i < nr_cages; i++) {
		struct emu_cage *c = emu_cage_create(i);

		if (IS_ERR(c)) {
			while (i--)
				emu_cage_destroy(cages[i]);
			debugfs_remove_recursive(emu_root);
			return PTR_ERR(c);
		}
		cages[i] = c;
	}
	pr_info("xcvr_i2c_emu: %u cage(s) registered\n", nr_cages);
	return 0;
}

static void __exit emu_exit(void)
{
	unsigned int i;

	/* Remove debugfs first so nothing can reach a cage being freed. */
	debugfs_remove_recursive(emu_root);
	for (i = 0; i < nr_cages; i++)
		emu_cage_destroy(cages[i]);
}

module_init(emu_init);
module_exit(emu_exit);

MODULE_AUTHOR("OptiWatch");
MODULE_DESCRIPTION("Emulated I2C adapters with SFF-8472 / CMIS transceivers for testing");
MODULE_LICENSE("GPL");
