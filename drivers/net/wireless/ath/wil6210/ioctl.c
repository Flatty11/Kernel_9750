/*
 * Copyright (c) 2014,2017 Qualcomm Atheros, Inc.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <linux/uaccess.h>

#include "wil6210.h"
#include <uapi/linux/wil6210_uapi.h>

#define wil_hex_dump_ioctl(prefix_str, buf, len) \
	print_hex_dump_debug("DBG[IOC ]" prefix_str, \
			     DUMP_PREFIX_OFFSET, 16, 1, buf, len, true)
#define wil_dbg_ioctl(wil, fmt, arg...) wil_dbg(wil, "DBG[IOC ]" fmt, ##arg)

#define WIL_PRIV_DATA_MAX_LEN	8192
#define CMD_SET_AP_WPS_P2P_IE	"SET_AP_WPS_P2P_IE"

struct wil_android_priv_data {
	char *buf;
	int used_len;
	int total_len;
};

static void __iomem *wil_ioc_addr(struct wil6210_priv *wil, u32 addr,
				  u32 size, enum wil_memio_op op)
{
	void __iomem *a;
	u32 off;

	switch (op & wil_mmio_addr_mask) {
	case wil_mmio_addr_linker:
		a = wmi_buffer(wil, cpu_to_le32(addr));
		break;
	case wil_mmio_addr_ahb:
		a = wmi_addr(wil, addr);
		break;
	case wil_mmio_addr_bar:
		a = wmi_addr(wil, addr + WIL6210_FW_HOST_OFF);
		break;
	default:
		wil_err(wil, "Unsupported address mode, op = 0x%08x\n", op);
		return NULL;
	}

	off = a - wil->csr;
	if (size >= wil->bar_size - off) {
		wil_err(wil,
			"Invalid requested block: off(0x%08x) size(0x%08x)\n",
			off, size);
		return NULL;
	}

	return a;
}

static int wil_ioc_memio_dword(struct wil6210_priv *wil, void __user *data)
{
	struct wil_memio io;
	void __iomem *a;
	bool need_copy = false;

	if (copy_from_user(&io, data, sizeof(io)))
		return -EFAULT;

	wil_dbg_ioctl(wil, "IO: addr = 0x%08x val = 0x%08x op = 0x%08x\n",
		      io.addr, io.val, io.op);

	a = wil_ioc_addr(wil, io.addr, sizeof(u32), io.op);
	if (!a) {
		wil_err(wil, "invalid address 0x%08x, op = 0x%08x\n", io.addr,
			io.op);
		return -EINVAL;
	}
	/* operation */
	switch (io.op & wil_mmio_op_mask) {
	case wil_mmio_read:
		io.val = readl_relaxed(a);
		need_copy = true;
		break;
#if defined(CONFIG_WIL6210_WRITE_IOCTL)
	case wil_mmio_write:
		writel_relaxed(io.val, a);
		wmb(); /* make sure write propagated to HW */
		break;
#endif
	default:
		wil_err(wil, "Unsupported operation, op = 0x%08x\n", io.op);
		return -EINVAL;
	}

	if (need_copy) {
		wil_dbg_ioctl(wil,
			      "IO done: addr(0x%08x) val(0x%08x) op(0x%08x)\n",
			      io.addr, io.val, io.op);
		if (copy_to_user(data, &io, sizeof(io)))
			return -EFAULT;
	}

	return 0;
}

static int wil_ioc_memio_block(struct wil6210_priv *wil, void __user *data)
{
	struct wil_memio_block io;
	void *block;
	void __iomem *a;
	int rc = 0;

	if (copy_from_user(&io, data, sizeof(io)))
		return -EFAULT;

	wil_dbg_ioctl(wil, "IO: addr = 0x%08x size = 0x%08x op = 0x%08x\n",
		      io.addr, io.size, io.op);

	/* size */
	if (io.size % 4) {
		wil_err(wil, "size is not multiple of 4:  0x%08x\n", io.size);
		return -EINVAL;
	}

	a = wil_ioc_addr(wil, io.addr, io.size, io.op);
	if (!a) {
		wil_err(wil, "invalid address 0x%08x, op = 0x%08x\n", io.addr,
			io.op);
		return -EINVAL;
	}

	block = kmalloc(io.size, GFP_USER);
	if (!block)
		return -ENOMEM;

	/* operation */
	switch (io.op & wil_mmio_op_mask) {
	case wil_mmio_read:
		wil_memcpy_fromio_32(block, a, io.size);
		wil_hex_dump_ioctl("Read  ", block, io.size);
		if (copy_to_user(io.block, block, io.size)) {
			rc = -EFAULT;
			goto out_free;
		}
		break;
#if defined(CONFIG_WIL6210_WRITE_IOCTL)
	case wil_mmio_write:
		if (copy_from_user(block, io.block, io.size)) {
			rc = -EFAULT;
			goto out_free;
		}
		wil_memcpy_toio_32(a, block, io.size);
		wmb(); /* make sure write propagated to HW */
		wil_hex_dump_ioctl("Write ", block, io.size);
		break;
#endif
	default:
		wil_err(wil, "Unsupported operation, op = 0x%08x\n", io.op);
		rc = -EINVAL;
		break;
	}

out_free:
	kfree(block);
	return rc;
}

static int wil_ioc_android(struct wil6210_priv *wil, void __user *data)
{
	int rc = 0;
	char *command;
	struct wil_android_priv_data priv_data;

	wil_dbg_ioctl(wil, "ioc_android\n");

	if (copy_from_user(&priv_data, data, sizeof(priv_data)))
		return -EFAULT;

	if (priv_data.total_len <= 0 ||
	    priv_data.total_len >= WIL_PRIV_DATA_MAX_LEN) {
		wil_err(wil, "invalid data len %d\n", priv_data.total_len);
		return -EINVAL;
	}

	command = kmalloc(priv_data.total_len + 1, GFP_KERNEL);
	if (!command)
		return -ENOMEM;

	if (copy_from_user(command, priv_data.buf, priv_data.total_len)) {
		rc = -EFAULT;
		goto out_free;
	}

	/* Make sure the command is NUL-terminated */
	command[priv_data.total_len] = '\0';

	wil_dbg_ioctl(wil, "ioc_android: command = %s\n", command);

	/* P2P not supported, but WPS is (in AP mode).
	 * Ignore those in order not to block WPS functionality
	 * in non-P2P mode.
	 */
	if (strncasecmp(command, CMD_SET_AP_WPS_P2P_IE,
			strlen(CMD_SET_AP_WPS_P2P_IE)) == 0)
		rc = 0;
	else
		rc = -ENOIOCTLCMD;

out_free:
	kfree(command);
	return rc;
}

int wil_ioctl(struct wil6210_priv *wil, void __user *data, int cmd)
{
	int ret;

	ret = wil_pm_runtime_get(wil);
	if (ret < 0)
		return ret;

	switch (cmd) {
	case WIL_IOCTL_MEMIO:
		ret = wil_ioc_memio_dword(wil, data);
		break;
	case WIL_IOCTL_MEMIO_BLOCK:
		ret = wil_ioc_memio_block(wil, data);
		break;
	case (SIOCDEVPRIVATE + 1):
		ret = wil_ioc_android(wil, data);
		break;
	default:
		wil_dbg_ioctl(wil, "Unsupported IOCTL 0x%04x\n", cmd);
		wil_pm_runtime_put(wil);
		return -ENOIOCTLCMD;
	}

	wil_pm_runtime_put(wil);

	wil_dbg_ioctl(wil, "ioctl(0x%04x) -> %d\n", cmd, ret);
	return ret;
}
