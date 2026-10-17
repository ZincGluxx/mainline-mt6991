// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/vmalloc.h>
#ifdef CONFIG_COMPAT
#include <linux/compat.h>
#endif
#include "ccci_config.h"
#include "ccci_common_config.h"
#include "ccci_bm.h"
#include "port_proxy.h"
#include "port_char.h"
#include "port_smem.h"
#include "ccci_fsm.h"

#define MAX_QUEUE_LENGTH 32

unsigned int port_char_dev_poll(struct file *fp,
	struct poll_table_struct *poll)
{
	struct port_t *port = fp->private_data;
	unsigned int mask = 0;
	int md_state = ccci_fsm_get_md_state();

	CCCI_DEBUG_LOG(0, CHAR, "poll on %s\n", port->name);
	poll_wait(fp, &port->rx_wq, poll);
	/* TODO: lack of poll wait for Tx */
	if (!skb_queue_empty(&port->rx_skb_list))
		mask |= POLLIN | POLLRDNORM;
	if (port_write_room_to_md(port) > 0)
		mask |= POLLOUT | POLLWRNORM;
	if (port->rx_ch == CCCI_UART1_RX &&
	    md_state != READY &&
		md_state != EXCEPTION) {
		/* notify MD logger to save its log
		 * before md_init kills it
		 */
		mask |= POLLERR;
		CCCI_NORMAL_LOG(0, CHAR,
			"poll error for MD logger at state %d,mask=%d\n",
			md_state, mask);
	}

	return mask;
}

static const struct file_operations char_dev_fops = {
	.owner = THIS_MODULE,
	.open = &port_dev_open, /*use default API*/
	.read = &port_dev_read, /*use default API*/
	.write = &port_dev_write, /*use default API*/
	.release = &port_dev_close,/*use default API*/
	.unlocked_ioctl = &port_dev_ioctl,/*use default API*/
#ifdef CONFIG_COMPAT
	.compat_ioctl = &port_dev_compat_ioctl,/*use default API*/
#endif
	.poll = &port_char_dev_poll,/*use port char self API*/
	.mmap = &port_dev_mmap,
};

static void port_char_md_state_notify(struct port_t *port, unsigned int state)
{
	struct sk_buff *skb = NULL;
	unsigned long flags;

	switch (state) {
	case GATED:
		/* when MD is stopped, the skb list of ccci_fs should be clean */
		if (port->rx_ch == CCCI_FS_RX)
			CCCI_NORMAL_LOG(0, FSM, "[%s]cccifs + GATED\n", __func__);

		if (port->flags & PORT_F_CLEAN) {
			spin_lock_irqsave(&port->rx_skb_list.lock, flags);
			while ((skb = __skb_dequeue(&port->rx_skb_list)) != NULL)
				ccci_free_skb(skb);
			spin_unlock_irqrestore(&port->rx_skb_list.lock, flags);
		}

		break;
	default:
		break;
	};
}

static int port_char_init(struct port_t *port)
{
	struct cdev *dev = NULL;
	int ret = 0;
	int tty_ret = 0;

	CCCI_DEBUG_LOG(0, CHAR,
		"char port %s is initializing\n", port->name);
	port->rx_length_th = MAX_QUEUE_LENGTH;
	port->skb_from_pool = 1;
	port->interception = 0;
	if (port->flags & PORT_F_WITH_CHAR_NODE) {
		dev = kmalloc(sizeof(struct cdev), GFP_KERNEL);
		if (unlikely(!dev)) {
			CCCI_ERROR_LOG(0, CHAR,
				"alloc char dev fail!!\n");
			return -1;
		}
		cdev_init(dev, &char_dev_fops);
		dev->owner = THIS_MODULE;
		ret = cdev_add(dev, MKDEV(port->major,
				port->minor_base + port->minor), 1);
		ret = ccci_register_dev_node(port->name, port->major,
				port->minor_base + port->minor);
		port->flags |= PORT_F_ADJUST_HEADER;
	}

	if (port->rx_ch == CCCI_UART2_RX) {
		/* Expose the AT port as a standard tty (/dev/ttyCCCI0) on top
		 * of this same port_t; the char node keeps working as before.
		 */
		tty_ret = ccci_tty_port_register(port);
		if (tty_ret)
			CCCI_ERROR_LOG(0, CHAR,
				"register tty front-end for %s fail, ret=%d\n",
				port->name, tty_ret);
	}

	return ret;
}

#if IS_ENABLED(CONFIG_MTK_ECCCI_C2K_USB)
static usb_upstream_buffer_cb_t usb_upstream_cb;
void ccci_c2k_set_usb_callback(usb_upstream_buffer_cb_t callback)
{
	usb_upstream_cb = callback;
}
EXPORT_SYMBOL(ccci_c2k_set_usb_callback);

static int c2k_req_push_to_usb(struct port_t *port, struct sk_buff *skb)
{
	int read_len, read_count, ret = 0;
	int c2k_ch_id;
	int ppp_rx_ch = CCCI_C2K_PPP_RX;

	if (port->rx_ch == ppp_rx_ch)
		c2k_ch_id = DATA_PPP_CH_C2K-1;
	else if (port->rx_ch == CCCI_MD_LOG_RX)
		c2k_ch_id = MDLOG_CH_C2K-2;
	else {
		ret = -ENODEV;
		CCCI_ERROR_LOG(0, CHAR,
			"Err: wrong ch_id(%d) from usb bypass\n", port->rx_ch);
		return ret;
	}

	/* calculate available data */
	read_len = skb->len - sizeof(struct ccci_header);
	/* remove CCCI header */
	skb_pull(skb, sizeof(struct ccci_header));

retry_push:
	/* push to usb: USB_RAWBULK_READY: requires C2K USB driver */
	if (!usb_upstream_cb) {
		CCCI_ERROR_LOG(0, CHAR,
				"usb upstream callback is not set\n");
		return -EINVAL;
	}
	read_count = usb_upstream_cb(c2k_ch_id, skb->data, read_len);

	CCCI_DEBUG_LOG(0, CHAR,
		"data push to usb bypass (ch%d)(%d)\n",
		port->rx_ch, read_count);

	if (read_count > 0) {
		skb_pull(skb, read_count);
		read_len -= read_count;
		if (read_len > 0)
			goto retry_push;
		else if (read_len == 0)
			ccci_free_skb(skb);
		else if (read_len < 0)
			CCCI_ERROR_LOG(0, CHAR,
				"read_len error, check why come here\n");
	} else {
		CCCI_NORMAL_LOG(0, CHAR, "usb buf full\n");
		msleep(20);
		goto retry_push;
	}

	return read_len;

}
#endif

/*
 * mainline runs no ccci_mdinit, so nothing answers the MD's FS requests on
 * ch14 and the modem stalls there during boot.  Reply framing mirrors what
 * the vendor daemon writes back, see doc/CCCI_FS_RE_HANDOFF.md section 7:
 *
 *   [ccci header][u32 op | 0xffff0000][u32 n_entry][u32 len_i][data_i]...
 *
 * data[0] keeps only the low 31 bits of the request, data[1] is the total
 * frame length, channel becomes CCCI_FS_TX and seq/assert/reserved are
 * echoed.
 */
#define CCCI_FS_OP_OPEN		0x1001
#define CCCI_FS_OP_CLOSE	0x1005
#define CCCI_FS_OP_GET_SIZE	0x1009
#define CCCI_FS_OP_FIND_FIRST	0x1012
#define CCCI_FS_OP_FIND_NEXT	0x1013
#define CCCI_FS_OP_UNLOCK_ALL	0x1016
#define CCCI_FS_OP_SHUT_DOWN	0x1017
#define CCCI_FS_OP_RESTORE	0x1021
#define CCCI_FS_OP_CMPT_WRITE	0x1024
#define CCCI_FS_OP_GET_INFO	0x1025
#define CCCI_FS_OP_CMPT_READ	0x1026

#define CCCI_FS_MAX_REQ_TLV	4
#define CCCI_FS_MAX_RSP_TLV	5
#define FS_ZERO_DATA_LEN	4096
#define FS_PATH_MAX		64
#define FS_HANDLE_MAX		1000
#define FS_ERR_NOT_FOUND	((u32)-9)
/* 0x1026 reply is hdr(16)+8+{8,4,4,nread,4} TLVs = 64 + ALIGN(nread,4), and
 * ccci_alloc_skb() rejects anything above SKB_4K (CCCI_MTU+128 = 3584).
 * Stock mdinit is capped even lower by port_dev_write()'s
 * CCCI_MTU + header_len (3456+20), so the MD already has to cope with a
 * short nread and re-issue at off += nread.
 */
#define FS_READ_MAX_DATA	3408

struct fs_tlv {
	u32 len;
	const void *data;
};

/*
 * mdinit maps ENOENT to -9 (see its errno switch: 2 -> -9, 13 -> -16,
 * 24 -> -5, else -18), and the MD forwards that straight through as the
 * FS_OP_OPEN handle / CMPT_READ status.  The device's own nv_mini_dump
 * confirms which paths are absent on real hardware:
 *
 *   [path:Z:\FACTORY4952155F.log][ret:4097]FS_OP_OPEN[hd:-9] @202
 *   [path:Z:\NVRA56C1012.log][ret:4097]CMPTR fail[fs_ret:-9][handle:-1]@977
 *   [path:X:\nv_config][ret:4097]FS_OP_OPEN[hd:-9] @202
 *
 * X:\DEV_INFO does exist (protect1/protect2:/md/DEV_INFO, 16 bytes, both
 * images identical) and the MD reads it to feed SST_Verify_HRID, so the
 * responder has to hand back the real bytes instead of zeros.  Same for
 * X:\MTBT_000 (LID 0xF00A) and the rest of the drive, and for Z:\ (nvdata)
 * with its NVRAM LID records (LID 0xF008 lives in
 * Z:\NVRAM\NVD_IMEI\NV0S_000; serving zeros there made its recovery read
 * fail with 4224 and assert).  X:/Z:/T: are served live from the device's
 * own mounted partitions -- there is no embedded fallback image, so a path
 * that is missing on the partition answers -9 exactly like stock.
 */

static const char * const fs_absent[] = {
	"X:\\nv_config",
};

static const u8 fs_zero_data[FS_ZERO_DATA_LEN];
static atomic_t fs_handle_seq = ATOMIC_INIT(0);
static char fs_handle_path[FS_HANDLE_MAX + 1][FS_PATH_MAX];
static u32 fs_handle_size[FS_HANDLE_MAX + 1];
static u32 fs_pos[FS_HANDLE_MAX + 1];
static u8 fs_handle_used[FS_HANDLE_MAX + 1];
static struct file *fs_handle_file[FS_HANDLE_MAX + 1];
static u8 fs_vfs_buf[FS_READ_MAX_DATA];
static u32 fs_cont_cnt;
unsigned long ccci_fs_rx_jiffies;

static void fs_handle_file_close(u32 h)
{
	if (h == 0 || h > FS_HANDLE_MAX)
		return;
	if (fs_handle_file[h]) {
		filp_close(fs_handle_file[h], NULL);
		fs_handle_file[h] = NULL;
	}
}

/*
 * fs_handle_used[] marks a slot as live; without it the plain modulo
 * allocator below wrapped every MD session (~2700 allocations per cycle
 * against FS_HANDLE_MAX=1000 and only ~84 closes) and handed MD a handle
 * whose slot had been re-bound to an unrelated file.
 *
 * Called from fsm_routine_start(): the MD is dead at that point, so every
 * handle it may still be holding is stale.
 */
void ccci_fs_handles_reset(void)
{
	unsigned int i;

	for (i = 1; i <= FS_HANDLE_MAX; i++)
		fs_handle_file_close(i);
	memset(fs_handle_path, 0, sizeof(fs_handle_path));
	memset(fs_handle_size, 0, sizeof(fs_handle_size));
	memset(fs_pos, 0, sizeof(fs_pos));
	memset(fs_handle_used, 0, sizeof(fs_handle_used));
	atomic_set(&fs_handle_seq, 0);
	fs_cont_cnt = 0;
}

static void fs_path_ascii(const u8 *utf16, u32 len, char *out, u32 outsz)
{
	u32 i, n = 0;

	for (i = 0; i + 1 < len && n + 1 < outsz; i += 2) {
		if (!utf16[i] && !utf16[i + 1])
			break;
		out[n++] = utf16[i + 1] ? '?' : (char)utf16[i];
	}
	out[n] = '\0';
}

static bool fs_path_ok(const char *p)
{
	return (p[0] == 'X' || p[0] == 'Z') && p[1] == ':' && p[2] == '\\';
}

static int fs_is_absent(const char *path)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fs_absent); i++)
		if (!strcmp(fs_absent[i], path))
			return 1;
	return 0;
}

static u32 fs_handle_alloc(const char *path)
{
	u32 h, i;

	for (i = 0; i < FS_HANDLE_MAX; i++) {
		h = ((u32)atomic_inc_return(&fs_handle_seq) % FS_HANDLE_MAX) + 1;
		if (!fs_handle_used[h])
			break;
	}
	if (i == FS_HANDLE_MAX) {
		h = ((u32)atomic_inc_return(&fs_handle_seq) % FS_HANDLE_MAX) + 1;
		CCCI_NORMAL_LOG(0, CHAR,
				"FS handle table full, reusing hd=%u\n", h);
	}
	fs_handle_file_close(h);
	fs_handle_used[h] = 1;
	fs_handle_size[h] = 0;
	fs_pos[h] = 0;
	if (path)
		strscpy(fs_handle_path[h], path, sizeof(fs_handle_path[h]));
	else
		fs_handle_path[h][0] = '\0';
	return h;
}

static void fs_handle_release(u32 h)
{
	if (h <= FS_HANDLE_MAX) {
		fs_handle_file_close(h);
		fs_handle_used[h] = 0;
		fs_handle_path[h][0] = '\0';
		fs_handle_size[h] = 0;
		fs_pos[h] = 0;
	}
}

/* ---- live-partition backend (what ccci_mdinit does in Android) -------- */

/*
 * init-rootfs.c mounts the phone's own nvdata / protect1+2 / mdota
 * partitions under /mnt/vendor/ (as /newroot/mnt/vendor/... before its
 * switch_root).  FS requests are served from those files when they exist;
 * the embedded blobs stay as the per-file fallback, so the modem sees the
 * live NVRAM state instead of this image's boot-time snapshot.
 *
 * port_fs_reply() runs in the CCIF rxq worker (process context), so the
 * sleeping file IO below is fine.  Handles are serialised by the single FS
 * channel, hence the shared fs_vfs_buf.  Writes only update the in-RAM
 * accounting for now, they are not flushed to the partition yet.
 */
#define FS_VPATH_MAX	256

struct fs_vfs_map {
	const char * const *base;
	unsigned int nbase;
};

static const char * const fs_vfs_x[] = {
	"/mnt/vendor/protect_f/md", "/mnt/vendor/protect_s/md",
	"/newroot/mnt/vendor/protect_f/md", "/newroot/mnt/vendor/protect_s/md",
};
static const char * const fs_vfs_z[] = {
	"/mnt/vendor/nvdata/md", "/newroot/mnt/vendor/nvdata/md",
};
static const char * const fs_vfs_t[] = {
	"/mnt/vendor/mdota", "/newroot/mnt/vendor/mdota",
};

static const struct fs_vfs_map *fs_vfs_map(const char *path)
{
	static const struct fs_vfs_map x = { fs_vfs_x, ARRAY_SIZE(fs_vfs_x) };
	static const struct fs_vfs_map z = { fs_vfs_z, ARRAY_SIZE(fs_vfs_z) };
	static const struct fs_vfs_map t = { fs_vfs_t, ARRAY_SIZE(fs_vfs_t) };

	if (path[1] != ':' || path[2] != '\\')
		return NULL;
	switch (path[0]) {
	case 'X':
		return &x;
	case 'Z':
		return &z;
	case 'T':
		return &t;
	}
	return NULL;
}

static int fs_vfs_build(const char *path, const char *base, char *out,
			size_t outsz)
{
	const char *p = path + 3;
	size_t o;

	if (!*p)
		return -1;
	o = scnprintf(out, outsz, "%s/", base);
	if (o + 1 >= outsz)
		return -1;
	for (; *p; p++) {
		if (p[0] == '.' && p[1] == '.')
			return -1;
		if (o + 1 >= outsz)
			return -1;
		out[o++] = *p == '\\' ? '/' : *p;
	}
	out[o] = '\0';
	return 0;
}

static struct file *fs_vfs_open(const char *path, char *vp, size_t vpsz)
{
	const struct fs_vfs_map *map = fs_vfs_map(path);
	struct file *f;
	unsigned int i;

	if (!map)
		return NULL;
	for (i = 0; i < map->nbase; i++) {
		char vpath[FS_VPATH_MAX];

		if (fs_vfs_build(path, map->base[i], vpath,
				 sizeof(vpath)) != 0)
			return NULL;
		f = filp_open(vpath, O_RDONLY | O_LARGEFILE, 0);
		if (!IS_ERR(f)) {
			if (vp && vpsz)
				strscpy(vp, vpath, vpsz);
			return f;
		}
	}
	return NULL;
}

static bool fs_vfs_bind(u32 h, const char *path, char *vp, size_t vpsz)
{
	struct file *f;

	if (!h || h > FS_HANDLE_MAX)
		return false;
	if (fs_handle_file[h])
		return true;
	f = fs_vfs_open(path, vp, vpsz);
	if (!f)
		return false;
	fs_handle_file[h] = f;
	fs_handle_size[h] = (u32)i_size_read(file_inode(f));
	return true;
}

static u8 fs_src_logged[128];

/* one line per drive per boot: which store actually serves the modem */
static void fs_vfs_note(const char *path, bool live, const char *vp, u32 len)
{
	u8 idx = (u8)path[0];

	if (idx >= ARRAY_SIZE(fs_src_logged) || fs_src_logged[idx])
		return;
	if (!live && !fs_vfs_map(path))
		return;
	fs_src_logged[idx] = 1;
	if (live)
		CCCI_NORMAL_LOG(0, CHAR, "FS src %c: live file %s len=%u\n",
				path[0], vp, len);
	else
		CCCI_NORMAL_LOG(0, CHAR, "FS src %c: embedded blob len=%u\n",
				path[0], len);
}

static u32 fs_field(const u8 *data, u32 len, u32 off)
{
	if (off + sizeof(u32) > len)
		return 0;
	return *(const u32 *)(data + off);
}

static void fs_parse_req(const u8 *grp, u32 glen, const u8 **data, u32 *len,
			 u32 *nout)
{
	u32 off = 8;
	u32 n, i;

	for (i = 0; i < CCCI_FS_MAX_REQ_TLV; i++) {
		data[i] = NULL;
		len[i] = 0;
	}
	*nout = 0;
	if (glen < 8)
		return;
	n = *(const u32 *)(grp + 4);
	*nout = n;
	if (n > CCCI_FS_MAX_REQ_TLV)
		n = CCCI_FS_MAX_REQ_TLV;
	for (i = 0; i < n; i++) {
		u32 l;

		if (off + 4 > glen)
			break;
		l = *(const u32 *)(grp + off);
		off += 4;
		if (l > glen - off)
			l = glen - off;
		data[i] = grp + off;
		len[i] = l;
		off += ALIGN(l, 4);
	}
}

static u32 fs_text(const u8 *in, u32 len, char *out, u32 outsz)
{
	u32 i, n = 0;

	for (i = 0; i < len && n + 1 < outsz; i++) {
		u8 c = in[i];

		if (c == '\n' || c == '\r')
			c = '|';
		else if (c < 0x20 || c > 0x7e)
			break;
		out[n++] = c;
	}
	out[n] = '\0';
	return n;
}

static const char *fs_hex(const u8 *p, u32 len)
{
	static char hex[3 * 48 + 1];
	u32 n = len > 48 ? 48 : len;
	u32 i;

	for (i = 0; i < n; i++)
		sprintf(hex + i * 3, "%02x ", p[i]);
	hex[n * 3] = '\0';
	return hex;
}

static void fs_tlv_u32(struct fs_tlv *tlv, u32 *word, u32 val)
{
	*word = val;
	tlv->len = sizeof(u32);
	tlv->data = word;
}

static void fs_tlv_u64(struct fs_tlv *tlv, u32 *word, u32 lo, u32 hi)
{
	word[0] = lo;
	word[1] = hi;
	tlv->len = 8;
	tlv->data = word;
}

/*
 * MD's CCCI_FS_OP_Wrapper() rejects a reply whose [u32 n] field is not
 * exactly the nout the caller put in the request descriptor (ccci_fs.c
 * line 1136, returns -1001), and then asserts when any TLV is longer than
 * the out-descriptor slot it lands in (line 1230).  Both failures cost MD
 * an FS round trip that times out on the next step, which is what drove the
 * kal_adm_free double free.  nout is fixed per op and was read out of every
 * MD_FS_* call site in the modem firmware (service/hif/ncccisrv/ccci_fs).
 */
static u32 fs_nout(u32 op)
{
	switch (op) {
	case CCCI_FS_OP_GET_SIZE:
	case CCCI_FS_OP_GET_INFO:
		return 2;
	case 0x1003:
	case CCCI_FS_OP_FIND_FIRST:
	case CCCI_FS_OP_FIND_NEXT:
	case CCCI_FS_OP_CMPT_WRITE:
		return 3;
	case CCCI_FS_OP_CMPT_READ:
		return 5;
	default:
		return 1;
	}
}

/*
 * Answer the MD's FS requests the way ccci_mdinit would.  Reply framing and
 * per-op TLV layout mirror doc/CCCI_FS_RE_HANDOFF.md section 7:
 *
 *   [ccci header][u32 op | 0xffff0000][u32 n_entry][u32 len_i][u32 data_i]...
 *
 * 0x1001/0x1005/0x1021/0x1016/0x1017 -> one u32 (handle / status)
 * 0x1009 GetFileSize                  -> {4 ret, 4 size}
 * 0x1012/0x1013 FindFirst/FindNext    -> {4 ret, 52 dentry, 0 name}
 * 0x1024 CMPT_Write                   -> {8 {mask, err}, 4 handle, 4 written}
 * 0x1025                              -> {4 ret, 24 info}
 * 0x1026 CMPT_Read                    -> {8 {mask, err}, 4 attr, 4 nread,
 *                                         nread data, 4 handle}
 *
 * The reply's [u32 n] must equal the request's nout exactly and every TLV
 * must fit its out-descriptor slot or the MD's wrapper returns -1001
 * (ccci_fs.c:1136/1230) -- see fs_nout().
 *
 * Paths that do not exist on the device answer with -9 the way mdinit's
 * ENOENT mapping does, and X:\DEV_INFO is served from its real 16 bytes so
 * SST_Verify_HRID sees the HRID the device actually carries.
 */
static int port_fs_reply(struct port_t *port, struct sk_buff *skb)
{
	unsigned int hdr_len = sizeof(struct ccci_header);
	struct ccci_header req, *rsp;
	struct sk_buff *reply;
	struct fs_tlv tlvs[CCCI_FS_MAX_RSP_TLV];
	const u8 *rdata[CCCI_FS_MAX_REQ_TLV];
	u32 rlen[CCCI_FS_MAX_REQ_TLV];
	u32 words[8];
	const u8 *grp;
	unsigned char *payload;
	char path[FS_PATH_MAX];
	char vp[FS_VPATH_MAX];
	u32 glen, op, n = 0, total, off;
	u32 nreq, i;
	int ret;

	if (skb->len < hdr_len + 8)
		return -CCCI_ERR_DROP_PACKET;

	WRITE_ONCE(ccci_fs_rx_jiffies, jiffies);
	memcpy(&req, skb->data, hdr_len);
	grp = skb->data + hdr_len;
	glen = skb->len - hdr_len;
	op = *(u32 *)grp;
	fs_parse_req(grp, glen, rdata, rlen, &nreq);

	fs_path_ascii(rdata[0], rlen[0], path, sizeof(path));

	if (nreq > CCCI_FS_MAX_REQ_TLV ||
	    (op == CCCI_FS_OP_CMPT_WRITE && !fs_path_ok(path))) {
		if (fs_cont_cnt++ < 4 || (fs_cont_cnt & 63) == 0)
			CCCI_NORMAL_LOG(0, CHAR,
				"FS cont op=0x%x len=%u n=%u why=%u path=%s h=%s\n",
				op, glen, nreq,
				nreq > CCCI_FS_MAX_REQ_TLV ? 1 : 2, path,
				fs_hex(grp, glen));
		return 0;
	}

	words[0] = 0;
	words[1] = 0;
	words[2] = 0;
	words[3] = 0;
	words[4] = 0;

	switch (op) {
	case CCCI_FS_OP_OPEN: {
		u32 h;

		if (fs_is_absent(path)) {
			fs_tlv_u32(&tlvs[0], &words[0], FS_ERR_NOT_FOUND);
			n = 1;
			break;
		}
		h = fs_handle_alloc(path);
		if (fs_vfs_bind(h, path, vp, sizeof(vp))) {
			fs_vfs_note(path, true, vp, fs_handle_size[h]);
			if (path[0] == 'T')
				CCCI_LEGACY_DBG_LOG(0, CHAR,
					"FS open OK %s len=%u\n",
					path, fs_handle_size[h]);
		}
		fs_tlv_u32(&tlvs[0], &words[0], h);
		n = 1;
		break;
	}
	case CCCI_FS_OP_CLOSE:
		fs_handle_release(rlen[0] >= 4 ?
				  *(const u32 *)rdata[0] : 0);
		fs_tlv_u32(&tlvs[0], &words[0], 0);
		n = 1;
		break;
	case CCCI_FS_OP_GET_SIZE: {
		u32 hd = rlen[0] >= 4 ? *(const u32 *)rdata[0] : 0;
		u32 len;

		if (hd && hd <= FS_HANDLE_MAX && !fs_handle_file[hd] &&
		    fs_handle_path[hd][0])
			fs_vfs_bind(hd, fs_handle_path[hd], vp, sizeof(vp));
		if (hd && hd <= FS_HANDLE_MAX && fs_handle_file[hd]) {
			len = (u32)i_size_read(file_inode(fs_handle_file[hd]));
			len = max_t(u32, len, fs_handle_size[hd]);
		} else {
			len = hd <= FS_HANDLE_MAX ? fs_handle_size[hd] : 0;
		}

		fs_tlv_u32(&tlvs[0], &words[0], 0);
		fs_tlv_u32(&tlvs[1], &words[1], len);
		n = 2;
		if (hd <= FS_HANDLE_MAX && !fs_handle_file[hd])
			CCCI_NORMAL_LOG(0, CHAR, "FS size hd=%u len=%u path=%s\n",
					hd, len, fs_handle_path[hd]);
		break;
	}
	case CCCI_FS_OP_CMPT_WRITE: {
		u32 flags = fs_field(rdata[1], rlen[1], 0);
		u32 wlen = fs_field(rdata[1], rlen[1], 28);
		u32 handle = fs_field(rdata[1], rlen[1], 32);

		if ((flags & 1) || (handle & 0x80000000))
			handle = fs_handle_alloc(path);
		if (handle <= FS_HANDLE_MAX)
			fs_handle_size[handle] += wlen;
		fs_tlv_u64(&tlvs[0], &words[0], flags & 0x3F, 0);
		fs_tlv_u32(&tlvs[1], &words[2], handle);
		fs_tlv_u32(&tlvs[2], &words[3], wlen);
		n = 3;
		if (rlen[2] && rdata[2][0] >= 0x20 && rdata[2][0] <= 0x7e) {
			static u32 fs_wr_sample;
			char txt[256];

			if (fs_text(rdata[2], rlen[2], txt, sizeof(txt)) &&
			    (strstr(txt, "[E]") ||
			     (fs_wr_sample++ & 31) == 0))
				CCCI_NORMAL_LOG(0, CHAR, "FS wr %s: %s\n",
						path, txt);
		}
		break;
	}
	case CCCI_FS_OP_CMPT_READ: {
		u32 flags = fs_field(rdata[1], rlen[1], 0);
		u32 attr = fs_field(rdata[1], rlen[1], 16);
		u32 off = fs_field(rdata[1], rlen[1], 20);
		u32 whence = fs_field(rdata[1], rlen[1], 24);
		u32 rmax = fs_field(rdata[1], rlen[1], 32);
		u32 handle = fs_field(rdata[1], rlen[1], 40);
		const u8 *data = fs_zero_data;
		struct file *vf = NULL;
		u32 nread, pos = 0;

		if (fs_is_absent(path)) {
			fs_tlv_u64(&tlvs[0], &words[0], flags & 0x3F,
				   FS_ERR_NOT_FOUND);
			fs_tlv_u32(&tlvs[1], &words[2], attr);
			fs_tlv_u32(&tlvs[2], &words[3], 0);
			tlvs[3].len = 0;
			tlvs[3].data = fs_zero_data;
			fs_tlv_u32(&tlvs[4], &words[4], (u32)-1);
			n = 5;
			break;
		}
		if ((flags & 1) || (handle & 0x80000000))
			handle = fs_handle_alloc(path);
		if (handle > FS_HANDLE_MAX)
			handle = 0;
		if (handle && !fs_handle_file[handle])
			fs_vfs_bind(handle, path, vp, sizeof(vp));
		vf = handle ? fs_handle_file[handle] : NULL;
		if (vf) {
			loff_t fsz = i_size_read(file_inode(vf));
			loff_t fpos;
			ssize_t rd;

			if (flags & 4) {
				if (whence == 2)
					fpos = fsz - min((loff_t)off, fsz);
				else
					fpos = min((loff_t)off, fsz);
			} else {
				fpos = min((loff_t)fs_pos[handle], fsz);
			}
			if (fpos < 0)
				fpos = 0;
			rd = kernel_read(vf, fs_vfs_buf,
					 min(rmax, (u32)FS_READ_MAX_DATA),
					 &fpos);
			if (rd < 0)
				rd = 0;
			nread = min((u32)rd, (u32)FS_READ_MAX_DATA);
			fs_pos[handle] = (u32)fpos;
			data = fs_vfs_buf;
		} else {
			nread = min(rmax, (u32)FS_ZERO_DATA_LEN);
		}
		nread = min(nread, (u32)FS_READ_MAX_DATA);
		if (strncmp(path, "Z:\\NVRAM", 8) != 0)
			CCCI_LEGACY_DBG_LOG(0, CHAR,
				"FS rd %s fl=0x%x off=%u whence=%u len=%u pos=%u n=%u hd=%d\n",
				path, flags, off, whence, rmax, pos, nread,
				(int)handle);
		fs_tlv_u64(&tlvs[0], &words[0], flags & 0x3F, 0);
		fs_tlv_u32(&tlvs[1], &words[2], attr);
		fs_tlv_u32(&tlvs[2], &words[3], nread);
		tlvs[3].len = nread;
		tlvs[3].data = data;
		fs_tlv_u32(&tlvs[4], &words[4], handle);
		n = 5;
		break;
	}
	case CCCI_FS_OP_FIND_FIRST:
	case CCCI_FS_OP_FIND_NEXT:
		fs_tlv_u32(&tlvs[0], &words[0], FS_ERR_NOT_FOUND);
		tlvs[1].len = 52;
		tlvs[1].data = fs_zero_data;
		tlvs[2].len = 0;
		tlvs[2].data = fs_zero_data;
		n = 3;
		break;
	case CCCI_FS_OP_GET_INFO:
		fs_tlv_u32(&tlvs[0], &words[0], FS_ERR_NOT_FOUND);
		tlvs[1].len = 24;
		tlvs[1].data = fs_zero_data;
		n = 2;
		break;
	default: {
		char dst[FS_PATH_MAX];

		dst[0] = '\0';
		if (nreq >= 2 && rlen[1])
			fs_path_ascii(rdata[1], rlen[1], dst, sizeof(dst));
		CCCI_LEGACY_DBG_LOG(0, CHAR,
				"FS unimpl op=0x%x n=%u path=%s path2=%s\n",
				op, nreq, path, dst);
		fs_tlv_u32(&tlvs[0], &words[0], 0);
		n = fs_nout(op);
		for (i = 1; i < n; i++) {
			tlvs[i].len = 0;
			tlvs[i].data = fs_zero_data;
		}
		break;
	}
	}

	total = hdr_len + 8;
	for (i = 0; i < n; i++)
		total += sizeof(u32) + ALIGN(tlvs[i].len, 4);

	reply = ccci_alloc_skb(total, 1, 1);
	if (!reply)
		return -ENOMEM;

	rsp = (struct ccci_header *)skb_put(reply, hdr_len);
	rsp->data[0] = req.data[0] & 0x7FFFFFFF;
	rsp->data[1] = total;
	rsp->channel = port->tx_ch;
	rsp->seq_num = req.seq_num;
	rsp->assert_bit = req.assert_bit;
	rsp->reserved = req.reserved;

	payload = skb_put(reply, total - hdr_len);
	*(u32 *)(payload + 0) = op | 0xFFFF0000;
	*(u32 *)(payload + 4) = n;
	off = 8;
	for (i = 0; i < n; i++) {
		u32 len = tlvs[i].len;

		*(u32 *)(payload + off) = len;
		off += sizeof(u32);
		memcpy(payload + off, tlvs[i].data, len);
		if (ALIGN(len, 4) != len)
			memset(payload + off + len, 0, ALIGN(len, 4) - len);
		off += ALIGN(len, 4);
	}

	/*
	 * 0x1024/0x1026 carry ~99% of the traffic and cost ~11 ms of console
	 * time each on the 115200 serial port, which is most of a round trip.
	 * Keep everything else, plus the BITMAP bookkeeping that immediately
	 * precedes every MD exception.  0x1005 is a constant {0} and 0x1009
	 * is already logged by "FS size", so neither needs its own line.
	 */
	if (op == CCCI_FS_OP_CMPT_WRITE || op == CCCI_FS_OP_CMPT_READ) {
		if (strstr(path, "BITMAP"))
			CCCI_NORMAL_LOG(0, CHAR,
				"FS reply op=0x%x n=%u len=%u rv=%u d0=0x%x d1=%u p0=%u a0=%u w=%u x2=%u path=%s\n",
				op, n, total, req.reserved, req.data[0],
				req.data[1], rlen[0], words[0], words[3],
				words[2], path);
	} else if (op != CCCI_FS_OP_GET_SIZE &&
		   op != CCCI_FS_OP_CLOSE) {
		if (op == CCCI_FS_OP_OPEN || op == CCCI_FS_OP_RESTORE)
			CCCI_LEGACY_DBG_LOG(0, CHAR,
				"FS reply op=0x%x n=%u len=%u rv=%u d0=0x%x d1=%u p0=%u a0=%u w=%u x2=%u path=%s\n",
				op, n, total, req.reserved, req.data[0],
				req.data[1], rlen[0], words[0], words[3],
				words[2], path);
		else
			CCCI_LEGACY_DBG_LOG(0, CHAR,
				"FS reply op=0x%x n=%u len=%u rv=%u d0=0x%x d1=%u v0=%u r0=%u\n",
				op, n, total, req.reserved, req.data[0],
				req.data[1],
				rlen[0] >= 4 ? *(const u32 *)rdata[0] : 0,
				n >= 1 && tlvs[0].len >= 4 ?
					*(const u32 *)tlvs[0].data : 0);
	}

	ret = port_send_skb_to_md(port, reply, 1);
	if (ret) {
		CCCI_ERROR_LOG(0, CHAR, "FS reply send fail op=0x%x ret=%d\n",
			op, ret);
		ccci_free_skb(reply);
		return ret;
	}
	return 0;
}

static int port_char_recv_skb(struct port_t *port, struct sk_buff *skb)
{
	/* /dev/ttyCCCI0 has priority while it is open: the payload goes to
	 * the tty flip buffer instead of the char device Rx queue.  When the
	 * tty is closed this is a no-op and the legacy path is unchanged.
	 */
	if (port->rx_ch == CCCI_UART2_RX && ccci_tty_is_open()) {
		ccci_tty_rx_skb(port, skb);
		return 0;
	}

	if (port->rx_ch == CCCI_FS_RX) {
		if (port_fs_reply(port, skb) == 0) {
			ccci_free_skb(skb);
			return 0;
		}
	}
	if (!atomic_read(&port->usage_cnt) &&
		(port->flags&PORT_F_CLOSE_NO_DROP_PKT) == 0)
		return -CCCI_ERR_DROP_PACKET;

#if IS_ENABLED(CONFIG_MTK_ECCCI_C2K_USB)
	if (port->interception) {
		c2k_req_push_to_usb(port, skb);
		return 0;
	}
#endif
	CCCI_DEBUG_LOG(0, CHAR, "recv on %s, len=%d\n",
		port->name, port->rx_skb_list.qlen);
	return port_recv_skb(port, skb);
}

void port_char_dump_info(struct port_t *port, unsigned int flag)
{
	if (port == NULL) {
		CCCI_ERROR_LOG(0, CHAR, "%s: port==NULL\n", __func__);
		return;
	}
	if (atomic_read(&port->usage_cnt) == 0)
		return;
	if (port->flags & PORT_F_CH_TRAFFIC)
		CCCI_REPEAT_LOG(0, CHAR,
			"CHR:(%d):%dR(%d,%d,%d):%dT(%d)\n",
			port->flags, port->rx_ch,
			port->rx_skb_list.qlen,
			atomic_read(&port->rx_pkg_cnt), port->rx_drop_cnt,
			port->tx_ch, port->tx_pkg_cnt);
}
struct port_ops char_port_ops = {
	.init = &port_char_init,
	.recv_skb = &port_char_recv_skb,
	.dump_info = &port_char_dump_info,
	.md_state_notify = port_char_md_state_notify,
};

