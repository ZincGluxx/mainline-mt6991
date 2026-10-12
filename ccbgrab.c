/*
 * ccbgrab - CCB control-header init + DHL capture for the MT6991 mainline port.
 *
 * Usage:
 *   ccbgrab status            dump the 20 CCB control headers + smem sizes
 *   ccbgrab init              write the control headers stock ccci_mdinit writes
 *   ccbgrab watch [seconds]   poll the DL ring indices for changes
 *   ccbgrab grab <file> [s]   drain available DHL pages into <file>
 *
 * Contracts decoded from libccci_util.so / mtk-ccci-userspace mdinit.py:
 *   control headers live in SMEM_USER_RAW_CCB_CTRL, 0x40 per ccb_configs[]
 *   entry; the DHL data region is the concatenation of [dl][ul] per entry.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <signal.h>
#include <setjmp.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define IOC_MAGIC		'C'
#define CCCI_IOC_SMEM_BASE	_IOR(IOC_MAGIC, 48, unsigned int)
#define CCCI_IOC_SMEM_LEN	_IOR(IOC_MAGIC, 49, unsigned int)
#define CCCI_IOC_SMEM_TX_NOTIFY	_IOW(IOC_MAGIC, 50, unsigned int)
#define CCCI_IOC_SMEM_RX_POLL	_IOR(IOC_MAGIC, 51, unsigned int)
#define CCCI_IOC_SMEM_GET_STATE	_IOR(IOC_MAGIC, 53, unsigned int)
#define CCCI_IOC_GET_CCB_CONFIG_LENGTH _IOR(IOC_MAGIC, 63, unsigned int)
#define CCCI_IOC_GET_CCB_CONFIG	_IOWR(IOC_MAGIC, 64, struct ccci_ccb_config)
#define CCCI_IOC_CCB_CTRL_INFO	_IOWR(IOC_MAGIC, 71, struct ccb_ctrl_info)
#define CCCI_IOC_GET_MD_STATE	_IOR(IOC_MAGIC, 1, unsigned int)

#define CCB_GUARD_BAND_E	0xEEFF0011u
#define PAGE_MAGIC		0x22222222u

#define CTRL_PATH		"/dev/ccci_ccb_ctrl"
#define DHL_PATH		"/dev/ccci_ccb_dhl"
#define MD_MONITOR_PATH		"/dev/ccci_ccb_md_monitor"
#define META_PATH		"/dev/ccci_ccb_meta"

#define MAX_CONFIGS		64
#define MAX_BUFFERS		64

struct ccb_ctrl_info {
	unsigned int user_id;
	unsigned int ctrl_offset;
	unsigned int ctrl_addr;
	unsigned int ctrl_length;
};

struct ccci_ccb_config {
	unsigned int user_id;
	unsigned char core_id;
	unsigned int dl_page_size;
	unsigned int ul_page_size;
	unsigned int dl_buff_size;
	unsigned int ul_buff_size;
};

struct buffer_header {
	unsigned int dl_guard_band;
	unsigned int dl_alloc_index;
	unsigned int dl_free_index;
	unsigned int dl_read_index;
	unsigned int dl_write_index;
	unsigned int dl_page_size;
	unsigned int dl_data_buffer_size;
	unsigned int dl_guard_band_e;
	unsigned int ul_guard_band;
	unsigned int ul_alloc_index;
	unsigned int ul_free_index;
	unsigned int ul_read_index;
	unsigned int ul_write_index;
	unsigned int ul_page_size;
	unsigned int ul_data_buffer_size;
	unsigned int ul_guard_band_e;
};

struct ccb {
	int ctrl_fd;
	int dhl_fd;
	unsigned int ctrl_offset;
	unsigned int ctrl_length;
	unsigned int ctrl_addr;
	void *ctrl;
	size_t ctrl_map;
	void *dhl;
	size_t dhl_len;
	unsigned int ncfg;
	struct ccci_ccb_config cfg[MAX_CONFIGS];
	unsigned int state;
};

static void on_alrm(int s) { (void)s; }

static const char *phase = "startup";
static const void *g_ctrl;
static size_t g_ctrl_map;
static unsigned int g_ncfg, g_i, g_ctrl_off, g_ctrl_len;
static sigjmp_buf bus_jmp;
static volatile sig_atomic_t bus_armed;

static void on_bus(int sig, siginfo_t *si, void *uc)
{
	char b[256];
	int n;

	(void)uc;
	if (bus_armed) {
		bus_armed = 0;
		siglongjmp(bus_jmp, 1);
	}
	n = snprintf(b, sizeof(b),
		     "BUS ERROR sig=%d si_addr=%p phase=%s\n"
		     "  ctrl=%p off=%u len=%u map=%zu ncfg=%u i=%u hdr_off=%zu\n",
		     sig, si->si_addr, phase, g_ctrl, g_ctrl_off, g_ctrl_len,
		     g_ctrl_map, g_ncfg, g_i,
		     (size_t)(g_ctrl_off + g_i) * 0x40);
	if (n > 0) {
		ssize_t w = write(2, b, (size_t)n);
		(void)w;
	}
	_exit(132);
}

static void install_bus(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = on_bus;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
}

static void die(const char *msg)
{
	perror(msg);
	exit(1);
}

static int open_ctrl(void)
{
	int fd = open(CTRL_PATH, O_RDWR);
	if (fd < 0)
		die("open " CTRL_PATH);
	return fd;
}

static void read_config(int fd, struct ccb *c)
{
	unsigned int n = 0;

	if (ioctl(fd, CCCI_IOC_GET_CCB_CONFIG_LENGTH, &n) < 0)
		die("GET_CCB_CONFIG_LENGTH");
	if (n > MAX_CONFIGS) {
		fprintf(stderr, "config count %u too large\n", n);
		exit(1);
	}
	c->ncfg = n;
	for (unsigned int i = 0; i < n; i++) {
		struct ccci_ccb_config cb;
		memset(&cb, 0, sizeof(cb));
		cb.user_id = i;
		if (ioctl(fd, CCCI_IOC_GET_CCB_CONFIG, &cb) < 0) {
			fprintf(stderr, "GET_CCB_CONFIG[%u]: %s\n", i,
				strerror(errno));
			exit(1);
		}
		c->cfg[i] = cb;
	}
}

static void ctrl_info(int fd, unsigned int user, struct ccb_ctrl_info *out)
{
	memset(out, 0, sizeof(*out));
	out->user_id = user;
	if (ioctl(fd, CCCI_IOC_CCB_CTRL_INFO, out) < 0) {
		fprintf(stderr, "CCB_CTRL_INFO(user=%u): %s\n", user,
			strerror(errno));
		exit(1);
	}
}

static int map_ctrl(struct ccb *c, const char *tag);

/*
 * Registers the CCB user (side effect: kernel sets the smem port state to
 * CCB_USER_OK) and maps both the control and the DHL data regions.
 */
static void ccb_setup(struct ccb *c, const char *dhl_path)
{
	struct ccb_ctrl_info info;
	unsigned int base = 0, len = 0, state = 0;

	memset(c, 0, sizeof(*c));
	c->ctrl_fd = open_ctrl();
	read_config(c->ctrl_fd, c);

	ctrl_info(c->ctrl_fd, 0, &info);
	c->ctrl_offset = info.ctrl_offset;
	c->ctrl_length = info.ctrl_length;
	c->ctrl_addr = info.ctrl_addr;

	c->ctrl_map = (c->ctrl_length + 4095ul) & ~4095ul;
	if (map_ctrl(c, "ctrl") < 0)
		die("map ccb_ctrl");

	if (dhl_path) {
		c->dhl_fd = open(dhl_path, O_RDWR);
		if (c->dhl_fd < 0)
			die(dhl_path);
		if (ioctl(c->dhl_fd, CCCI_IOC_SMEM_BASE, &base) < 0)
			die("SMEM_BASE");
		if (ioctl(c->dhl_fd, CCCI_IOC_SMEM_LEN, &len) < 0)
			die("SMEM_LEN");
		if (ioctl(c->dhl_fd, CCCI_IOC_SMEM_GET_STATE, &state) < 0)
			die("SMEM_GET_STATE");
		c->state = state;
		c->dhl_len = len;
		c->dhl = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED,
			      c->dhl_fd, 0);
		if (c->dhl == MAP_FAILED)
			die("mmap dhl");
	}
}

/*
 * Probe a fresh mapping without dying on it.  ccci's ctrl mmap logs
 * "remap succeed" yet has SIGBUS'd on first touch (0xd4000000 is carve-out
 * reserved memory, ioremap_wc'd for the kernel and not System RAM), so every
 * map has to be proven before anything walks it.
 */
static int probe_map(const void *p)
{
	volatile const unsigned char *v = p;

	bus_armed = 1;
	if (sigsetjmp(bus_jmp, 1)) {
		bus_armed = 0;
		return 0;
	}
	(void)*v;
	bus_armed = 0;
	return 1;
}

/*
 * ccci_util ioremap_wc's the same physical pages for its own use, so /dev/mem
 * at the reported PA is the fallback when the driver's remap_pfn_range is not
 * CPU-usable.  STRICT_DEVMEM blocks System RAM and allows carved-out regions,
 * which is exactly what this is.
 */
static void *map_ctrl_fallback(struct ccb *c)
{
	int fd;
	void *p;

	fd = open("/dev/mem", O_RDWR | O_SYNC | O_CLOEXEC);
	if (fd < 0)
		return MAP_FAILED;
	p = mmap(NULL, c->ctrl_map, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
		 (off_t)c->ctrl_addr);
	close(fd);
	return p;
}

static int map_ctrl(struct ccb *c, const char *tag)
{
	c->ctrl = mmap(NULL, c->ctrl_map, PROT_READ | PROT_WRITE, MAP_SHARED,
		       c->ctrl_fd, 0);
	if (c->ctrl != MAP_FAILED && probe_map(c->ctrl))
		return 0;
	if (c->ctrl != MAP_FAILED) {
		munmap(c->ctrl, c->ctrl_map);
		c->ctrl = MAP_FAILED;
	}
	printf("%s: ccci ctrl mmap unusable, trying /dev/mem at 0x%x\n",
	       tag, c->ctrl_addr);
	fflush(stdout);
	c->ctrl = map_ctrl_fallback(c);
	if (c->ctrl != MAP_FAILED && probe_map(c->ctrl))
		return 0;
	if (c->ctrl != MAP_FAILED) {
		munmap(c->ctrl, c->ctrl_map);
		c->ctrl = MAP_FAILED;
	}
	return -1;
}

static struct buffer_header *hdr_at(struct ccb *c, unsigned int buf)
{
	size_t off = (size_t)(c->ctrl_offset + buf) * 0x40;

	if (!c->ctrl || c->ctrl == MAP_FAILED ||
	    off + sizeof(struct buffer_header) > c->ctrl_map)
		return NULL;
	return (struct buffer_header *)((char *)c->ctrl + off);
}

/*
 * Buffer i of the DHL user owns [dl_buff_size][ul_buff_size] of the data
 * region, in ccb_configs[] order.
 */
static unsigned int dhl_data_offset(struct ccb *c, unsigned int buf)
{
	unsigned int off = 0;

	for (unsigned int i = 0; i < c->ncfg && i <= buf; i++) {
		if (c->cfg[i].user_id != 0)
			continue;
		if (i == buf)
			return off;
		off += c->cfg[i].dl_buff_size + c->cfg[i].ul_buff_size;
	}
	return off;
}

static unsigned int dhl_buf_count(struct ccb *c)
{
	unsigned int n = 0;

	for (unsigned int i = 0; i < c->ncfg; i++)
		if (c->cfg[i].user_id == 0)
			n++;
	return n;
}

static int cmd_status(void)
{
	struct ccb c;
	unsigned int n;

	ccb_setup(&c, DHL_PATH);
	n = dhl_buf_count(&c);

	printf("ctrl: addr=0x%x len=%u map=%zu offset=%u\n",
	       c.ctrl_addr, c.ctrl_length, c.ctrl_map, c.ctrl_offset);
	printf("dhl : len=%zu state=%u buffers=%u\n",
	       c.dhl_len, c.state, n);
	printf("version node: ");
	fflush(stdout);
	if (system("cat /sys/kernel/ccci/version; echo") != 0)
		printf("?\n");

	printf("%-4s %-9s %-9s %-5s %-5s %-5s %-5s %-5s "
	       "r/f/w/a  %s\n",
	       "buf", "dl_page", "dl_size", "rd", "fr", "wr", "al",
	       "pg", "guard_e");

	for (unsigned int i = 0; i < c.ncfg; i++) {
		struct buffer_header *h;
		unsigned int off;

		if (c.cfg[i].user_id != 0)
			continue;
		off = dhl_data_offset(&c, i);
		h = hdr_at(&c, i);
		if (!h)
			continue;
		printf("%-4u %-9u %-9u %-5u %-5u %-5u %-5u %-5u "
		       "%u/%u/%u/%u  %08x%08x off=%u\n",
		       i, c.cfg[i].dl_page_size, c.cfg[i].dl_buff_size,
		       h->dl_read_index, h->dl_free_index,
		       h->dl_write_index, h->dl_alloc_index,
		       h->dl_page_size ? h->dl_data_buffer_size /
					 h->dl_page_size : 0,
		       h->dl_read_index, h->dl_free_index,
		       h->dl_write_index, h->dl_alloc_index,
		       h->dl_guard_band_e, h->ul_guard_band_e, off);
	}
	return 0;
}

static int cmd_raw(void)
{
	struct ccb c;
	unsigned int n;

	ccb_setup(&c, NULL);
	n = c.ncfg > MAX_BUFFERS ? MAX_BUFFERS : c.ncfg;

	printf("ctrl addr=0x%x len=%u offset=%u\n", c.ctrl_addr,
	       c.ctrl_length, c.ctrl_offset);
	for (unsigned int i = 0; i < n; i++) {
		unsigned char *p = (unsigned char *)c.ctrl + (size_t)i * 0x40;

		printf("buf%02u ", i);
		for (int b = 0; b < 0x40; b++)
			printf("%02x", p[b]);
		printf("\n");
	}
	return 0;
}

static int cmd_notify(unsigned int core)
{
	struct ccb c;
	unsigned int mask = 0xFFFFFFFF;

	ccb_setup(&c, DHL_PATH);
	if (ioctl(c.dhl_fd, CCCI_IOC_SMEM_TX_NOTIFY, &core) < 0)
		die("SMEM_TX_NOTIFY");
	printf("TX_NOTIFY core=%u sent\n", core);
	signal(SIGALRM, on_alrm);
	alarm(5);
	if (ioctl(c.dhl_fd, CCCI_IOC_SMEM_RX_POLL, &mask) < 0)
		printf("RX_POLL: %s\n", strerror(errno));
	else
		printf("RX_POLL returned (wakeup observed)\n");
	alarm(0);
	return 0;
}


static int cmd_probe(const char *path)
{
	unsigned int base = 0, len = 0, state = 0;
	int fd;
	void *m;
	unsigned long nz = 0;

	fd = open(path, O_RDWR);
	if (fd < 0)
		die(path);
	if (ioctl(fd, CCCI_IOC_SMEM_BASE, &base) < 0)
		die("SMEM_BASE");
	if (ioctl(fd, CCCI_IOC_SMEM_LEN, &len) < 0)
		die("SMEM_LEN");
	ioctl(fd, CCCI_IOC_SMEM_GET_STATE, &state);
	printf("%s base=0x%x len=%u state=%u\n", path, base, len, state);
	if (!len)
		return 0;
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		die("mmap");
	for (unsigned int i = 0; i < len; i++)
		if (((unsigned char *)m)[i])
			nz++;
	printf("  non-zero bytes: %lu\n", nz);
	printf("  first 64: ");
	for (int i = 0; i < 64 && i < (int)len; i++)
		printf("%02x", ((unsigned char *)m)[i]);
	printf("\n");

	static const unsigned int pats[] = {
		0xDDBBCCAA, 0xAABBCCDD, 0xEEFF0011, 0x1100FFEE, 0x77424343
	};
	for (unsigned int pi = 0; pi < sizeof(pats) / sizeof(pats[0]); pi++) {
		unsigned int n = 0;
		printf("  magic %08x:", pats[pi]);
		for (unsigned int i = 0; i + 4 <= len && n < 10; i += 4)
			if (*(unsigned int *)((char *)m + i) == pats[pi]) {
				printf(" %x", i);
				n++;
			}
		if (!n)
			printf(" none");
		printf(" (first %u only)\n", n);
	}

	{
		unsigned char *snap = malloc(len);
		unsigned int changed = 0, buckets[64] = { 0 };
		unsigned int lo = ~0u, hi = 0, first = ~0u;
		memcpy(snap, m, len);
		sleep(3);
		for (unsigned int i = 0; i + 4 <= len; i += 4)
			if (memcmp(snap + i, (char *)m + i, 4)) {
				changed++;
				buckets[i >> 20 < 64 ? i >> 20 : 63]++;
				if (i < lo)
					lo = i;
				if (i > hi)
					hi = i;
				if (first == ~0u)
					first = i;
			}
		printf("  changed words in 3s: %u  range %x..%x\n",
		       changed, lo, hi);
		printf("  per-1MB: ");
		for (int b = 0; b < 64; b++)
			if (buckets[b])
				printf("%dMB=%u ", b, buckets[b]);
		printf("\n");
		if (first != ~0u) {
			printf("  @%x now: ", first);
			for (int i = 0; i < 128 && first + i < len; i++)
				printf("%02x", ((unsigned char *)m)[first + i]);
			printf("\n");
		}
		free(snap);
	}
	return 0;
}

static int cmd_rawdump(const char *path, const char *out)
{
	unsigned int base = 0, len = 0;
	int fd, ofd;
	void *m;

	fd = open(path, O_RDWR);
	if (fd < 0)
		die(path);
	if (ioctl(fd, CCCI_IOC_SMEM_BASE, &base) < 0)
		die("SMEM_BASE");
	if (ioctl(fd, CCCI_IOC_SMEM_LEN, &len) < 0)
		die("SMEM_LEN");
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED)
		die("mmap");
	ofd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (ofd < 0)
		die(out);
	if (write(ofd, m, len) != (ssize_t)len)
		die("write");
	close(ofd);
	printf("dumped %u bytes of %s (base 0x%x) to %s\n", len, path, base, out);
	return 0;
}

static int cmd_init(void)
{
	struct ccb c;
	unsigned int written = 0;

	ccb_setup(&c, NULL);

	for (unsigned int i = 0; i < c.ncfg; i++) {
		struct ccci_ccb_config *cf = &c.cfg[i];
		unsigned char hdr[0x40];

		memset(hdr, 0, sizeof(hdr));
		*(unsigned int *)(hdr + 0x14) = cf->dl_page_size;
		*(unsigned int *)(hdr + 0x18) = cf->dl_buff_size;
		*(unsigned int *)(hdr + 0x1c) = CCB_GUARD_BAND_E;
		*(unsigned int *)(hdr + 0x34) = cf->ul_page_size;
		*(unsigned int *)(hdr + 0x38) = cf->ul_buff_size;
		*(unsigned int *)(hdr + 0x3c) = CCB_GUARD_BAND_E;
		if ((size_t)(i + 1) * 0x40 > c.ctrl_map) {
			printf("CCB[%02u] beyond ctrl_map=%zu, stopped\n",
			       i, c.ctrl_map);
			break;
		}
		memcpy((char *)c.ctrl + (size_t)i * 0x40, hdr, sizeof(hdr));
		written++;
		printf("CCB[%02u] user=%u dl_page=%u dl_buff=%u "
		       "ul_page=%u ul_buff=%u\n",
		       i, cf->user_id, cf->dl_page_size, cf->dl_buff_size,
		       cf->ul_page_size, cf->ul_buff_size);
	}
	printf("wrote %u control headers\n", written);
	munmap(c.ctrl, c.ctrl_map);
	close(c.ctrl_fd);
	return 0;
}

static void dump_indices(struct ccb *c, unsigned int *snap, unsigned int n)
{
	struct buffer_header *h;

	for (unsigned int i = 0; i < n; i++) {
		h = hdr_at(c, i);
		if (!h)
			continue;
		if (h->dl_write_index == snap[i] && h->dl_read_index == 0)
			continue;
		printf("  buf%u rd=%u fr=%u wr=%u al=%u gu=%08x "
		       "magic=%08x/%08x\n",
		       i, h->dl_read_index, h->dl_free_index,
		       h->dl_write_index, h->dl_alloc_index,
		       h->dl_guard_band, h->dl_guard_band_e,
		       h->ul_guard_band_e);
		snap[i] = h->dl_write_index;
	}
}

static int cmd_watch(int seconds)
{
	struct ccb c;
	unsigned int n, snap[MAX_BUFFERS] = { 0 };
	time_t end;

	ccb_setup(&c, DHL_PATH);
	n = dhl_buf_count(&c);
	if (n > MAX_BUFFERS)
		n = MAX_BUFFERS;
	printf("watching %u buffers for %d s (ctrl=%zu dhl=%zu state=%u)\n",
	       n, seconds, c.ctrl_map, c.dhl_len, c.state);
	fflush(stdout);

	end = time(NULL) + seconds;
	while (time(NULL) < end) {
		dump_indices(&c, snap, n);
		usleep(100000);
	}
	return 0;
}

static int cmd_grab(const char *path, int seconds)
{
	struct ccb c;
	unsigned int n;
	FILE *out;
	time_t end;
	unsigned long total = 0, frames = 0;

	ccb_setup(&c, DHL_PATH);
	n = dhl_buf_count(&c);
	if (n > MAX_BUFFERS)
		n = MAX_BUFFERS;

	out = fopen(path, "wb");
	if (!out)
		die(path);
	printf("grabbing to %s for %d s\n", path, seconds);
	fflush(stdout);

	end = time(NULL) + seconds;
	while (time(NULL) < end) {
		unsigned int drained = 0;

		for (unsigned int i = 0; i < n; i++) {
			struct buffer_header *h = hdr_at(&c, i);
			unsigned int pg = c.cfg[i].dl_page_size;
			unsigned int npg, avail, idx;

			if (!h || !pg)
				continue;
			npg = h->dl_data_buffer_size / pg;
			if (!npg)
				continue;
			avail = h->dl_write_index - h->dl_read_index;
			if ((int)avail < 0)
				avail += npg;
			while (avail--) {
				unsigned char *page = (unsigned char *)c.dhl +
					dhl_data_offset(&c, i) +
					(size_t)pg * h->dl_read_index;
				unsigned int magic = *(unsigned int *)page;
				unsigned int len = *(unsigned int *)(page + 4);

				idx = h->dl_read_index + 1;
				if (idx >= npg)
					idx = 0;
				if (magic != PAGE_MAGIC) {
					printf("  buf%u idx=%u magic=%08x "
					       "(not a page, stop)\n",
					       i, h->dl_read_index, magic);
					break;
				}
				if (len > pg - 8) {
					printf("  buf%u idx=%u bad len %u\n",
					       i, h->dl_read_index, len);
					break;
				}
				fwrite(page + 8, 1, len, out);
				frames++;
				total += len;
				drained++;
				h->dl_read_index = idx;
				h->dl_free_index = idx;
			}
		}
		if (!drained)
			usleep(50000);
	}
	fclose(out);
	printf("wrote %lu bytes in %lu frames to %s\n", total, frames, path);
	return 0;
}

#define RAW_DHL_PATH		"/dev/ccci_raw_dhl"
#define MD_LOG_CTRL_PATH	"/dev/ccci_md_log_ctrl"
#define MD_LOG_TTY_PATH		"/dev/ttyC1"
#define FSM_MONITOR_PATH	"/dev/ccci_monitor"

/*
 * GET_MD_STATE on a freshly opened node.  Unknown ioctls on a CCCI char port
 * fall through port_dev_ioctl()'s default arm into ccci_fsm_ioctl(), so any
 * idle port answers exactly like /dev/ccci_monitor does.
 */
static int state_from_node(const char *path, unsigned int *out)
{
	int fd = open(path, O_RDWR | O_NONBLOCK);

	if (fd < 0)
		fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return -1;
	if (ioctl(fd, CCCI_IOC_GET_MD_STATE, out) != 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

/*
 * /dev/ccci_monitor is single-open and lives in ccci_mdinit for the whole
 * system lifetime, so a second open here fails with EBUSY and used to leave
 * md_state 0.  ccci_ccb_ctrl is the one port port_dev_open() never refuses
 * (the usage_cnt check is exempt for rx_ch == CCCI_CCB_CTRL), so it is the
 * reliable answer; ccbgrab already maps it for its CCB header walk.
 *
 * The kmsg scan is last and usually loses: mdinit prints its transitions
 * around 20-30 s, and the dmesg ring has wrapped them by the time anyone
 * asks - on boot #697 the fallback saw neither "md state=" line while the
 * modem was plainly READY and answering AT.
 */
static unsigned int md_state_query(void)
{
	static const char *const nodes[] = {
		FSM_MONITOR_PATH,
		CTRL_PATH,
		RAW_DHL_PATH,
		NULL,
	};
	static int diag_done;
	char buf[8192];
	unsigned int st = 0;
	int got = 0, fd, i, diag = !diag_done;
	ssize_t r;

	diag_done = 1;
	for (i = 0; nodes[i]; i++) {
		if (state_from_node(nodes[i], &st) == 0) {
			if (diag)
				printf("GET_MD_STATE via %s\n", nodes[i]);
			return st;
		}
		if (diag)
			fprintf(stderr, "GET_MD_STATE on %s: %s\n", nodes[i],
				strerror(errno));
	}
	if (diag)
		fprintf(stderr, "GET_MD_STATE: no node worked, trying kmsg\n");

	fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	if (fd < 0)
		return 0;
	while ((r = read(fd, buf, sizeof(buf) - 1)) > 0) {
		static const char tag[] = "mdinit: md state=";
		char *p;

		buf[r] = 0;
		p = strstr(buf, tag);
		if (!p)
			continue;
		st = (unsigned int)strtoul(p + sizeof(tag) - 1, NULL, 10);
		got = 1;
	}
	close(fd);
	if (got && diag)
		printf("GET_MD_STATE via kmsg\n");
	return got ? st : 0;
}

/*
 * SPLM: the 12-byte control channel framing emdlogger uses on
 * /dev/ccci_md_log_ctrl (lib MAL_* -> SPLM_A2M_* messages).  The MD only
 * starts pushing DHL frames after SPLM_A2M_AP_LOGGING_IS_READY.
 *
 *   [0]     0x59 magic
 *   [1]     0
 *   [2..3]  payload length, LE, <= 512
 *   [4..7]  u32, always 0 from AP
 *   [8]     state (0 AP -> MD, 1/4 MD -> AP)
 *   [9]     msgid
 *   [10]    token, incrementing
 *   [11]    XOR of bytes 0..10 (so XOR of the 12 header bytes is 0)
 */
#define SPLM_HDR		12
#define SPLM_MAX_PAY		512
#define SPLM_M_IS_READY		0
#define SPLM_M_STOP		1
#define SPLM_M_RESET		5
#define SPLM_M_IS_READY_V2	9
#define SPLM_M_QUERY_VERSION	14
#define SPLM_M_LOGGING_OPTION	32

static unsigned int splm_token = 1;
static unsigned char srx[8192];
static size_t srx_len;

/*
 * Per user, per direction.  dl is MD -> AP (what the MD pushes into DHL) and
 * ul is AP -> MD (what we hand back down, SPLM included): only looking at dl
 * made every probe read all-zero even while our own frames were in flight.
 */
#define MAX_CCB_USER	32

static void dhl_probe(const char *tag)
{
	struct ccb c;
	struct ccb_ctrl_info info;
	struct {
		unsigned cfg;
		unsigned dl_r, dl_f, dl_w, dl_a;
		unsigned ul_r, ul_f, ul_w, ul_a;
	} u[MAX_CCB_USER];
	unsigned int state = 0;

	phase = "dhl-probe";
	memset(&c, 0, sizeof(c));
	memset(u, 0, sizeof(u));
	c.ctrl_fd = open(CTRL_PATH, O_RDWR);
	if (c.ctrl_fd < 0) {
		printf("%s: open %s: %s\n", tag, CTRL_PATH, strerror(errno));
		return;
	}
	memset(&info, 0, sizeof(info));
	info.user_id = 0;
	if (ioctl(c.ctrl_fd, CCCI_IOC_GET_CCB_CONFIG_LENGTH, &c.ncfg) < 0 ||
	    ioctl(c.ctrl_fd, CCCI_IOC_CCB_CTRL_INFO, &info) < 0) {
		printf("%s: ctrl ioctl: %s\n", tag, strerror(errno));
		close(c.ctrl_fd);
		return;
	}
	c.ctrl_offset = info.ctrl_offset;
	c.ctrl_length = info.ctrl_length;
	c.ctrl_map = (c.ctrl_length + 4095ul) & ~4095ul;
	if (map_ctrl(&c, tag) < 0) {
		printf("%s: ctrl map failed (ccci and /dev/mem)\n", tag);
		close(c.ctrl_fd);
		return;
	}
	read_config(c.ctrl_fd, &c);
	g_ctrl = c.ctrl;
	g_ctrl_map = c.ctrl_map;
	g_ncfg = c.ncfg;
	g_ctrl_off = c.ctrl_offset;
	g_ctrl_len = c.ctrl_length;
	printf("%s: ctrl addr=0x%x len=%u map=%zu off=%u ptr=%p ncfg=%u\n",
	       tag, info.ctrl_addr, c.ctrl_length, c.ctrl_map, c.ctrl_offset,
	       c.ctrl, c.ncfg);
	fflush(stdout);
	c.dhl_fd = open(DHL_PATH, O_RDWR);
	if (c.dhl_fd >= 0) {
		ioctl(c.dhl_fd, CCCI_IOC_SMEM_GET_STATE, &state);
		close(c.dhl_fd);
	}
	/*
	 * The ctrl mmap has been observed to hand back a mapping that raises
	 * SIGBUS on the first touch (kernel logged vma->vm_start as a kernel
	 * address).  Probe it under sigsetjmp so a dead mapping degrades to
	 * "no indices" instead of killing the whole splm run.
	 */
	bus_armed = 1;
	if (sigsetjmp(bus_jmp, 1)) {
		bus_armed = 0;
		printf("%s: ctrl mapping unusable (SIGBUS at buf %u), "
		       "skipping index walk\n", tag, g_i);
		memset(u, 0, sizeof(u));
		for (unsigned int i = 0; i < c.ncfg; i++)
			if (c.cfg[i].user_id < MAX_CCB_USER)
				u[c.cfg[i].user_id].cfg++;
	} else {
		for (unsigned int i = 0; i < c.ncfg; i++) {
			unsigned int id = c.cfg[i].user_id;
			struct buffer_header *h;

			g_i = i;
			if (id >= MAX_CCB_USER)
				continue;
			u[id].cfg++;
			h = hdr_at(&c, i);
			if (!h)
				continue;
			u[id].dl_r += h->dl_read_index;
			u[id].dl_f += h->dl_free_index;
			u[id].dl_w += h->dl_write_index;
			u[id].dl_a += h->dl_alloc_index;
			u[id].ul_r += h->ul_read_index;
			u[id].ul_f += h->ul_free_index;
			u[id].ul_w += h->ul_write_index;
			u[id].ul_a += h->ul_alloc_index;
		}
		bus_armed = 0;
		g_i = 0;
	}
	printf("%s: dhl state=%u cfg=%u ctrl_len=%u ctrl_map=%zu off=%u\n",
	       tag, state, c.ncfg, c.ctrl_length, c.ctrl_map, c.ctrl_offset);
	for (unsigned int id = 0; id < MAX_CCB_USER; id++) {
		if (!u[id].cfg)
			continue;
		printf("  user %2u: cfg=%2u dl r/f/w/a=%u/%u/%u/%u  "
		       "ul r/f/w/a=%u/%u/%u/%u\n",
		       id, u[id].cfg,
		       u[id].dl_r, u[id].dl_f, u[id].dl_w, u[id].dl_a,
		       u[id].ul_r, u[id].ul_f, u[id].ul_w, u[id].ul_a);
	}
	for (unsigned int i = 0; i < c.ncfg; i++)
		printf("  cfg%02u user=%u dl_page=%-6u dl=%-7u "
		       "ul_page=%-6u ul=%u\n",
		       i, c.cfg[i].user_id, c.cfg[i].dl_page_size,
		       c.cfg[i].dl_buff_size, c.cfg[i].ul_page_size,
		       c.cfg[i].ul_buff_size);
	fflush(stdout);
	munmap(c.ctrl, c.ctrl_map);
	close(c.ctrl_fd);
}

static int splm_send(int fd, unsigned int msgid, const void *pay,
		     unsigned int plen)
{
	unsigned char f[SPLM_HDR + SPLM_MAX_PAY];
	unsigned char ck = 0;
	unsigned int i;
	ssize_t w;

	if (plen > SPLM_MAX_PAY) {
		fprintf(stderr, "splm plen %u too long\n", plen);
		return -1;
	}
	memset(f, 0, sizeof(f));
	f[0] = 0x59;
	f[2] = (unsigned char)plen;
	f[3] = (unsigned char)(plen >> 8);
	f[9] = (unsigned char)msgid;
	f[10] = (unsigned char)splm_token;
	splm_token = (splm_token + 1) & 0xff;
	for (i = 0; i < SPLM_HDR - 1; i++)
		ck ^= f[i];
	f[11] = ck;
	if (plen && pay)
		memcpy(f + SPLM_HDR, pay, plen);

	for (i = 0; i < 64; i++) {
		w = write(fd, f, SPLM_HDR + plen);
		if (w == (ssize_t)(SPLM_HDR + plen))
			break;
		if (w < 0 && errno == EINTR)
			continue;
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			usleep(20000);
			continue;
		}
		fprintf(stderr, "splm TX id=%u: %s (w=%zd)\n", msgid,
			strerror(errno), w);
		return -1;
	}
	if (i == 64) {
		fprintf(stderr, "splm TX id=%u: never writable\n", msgid);
		return -1;
	}
	printf("splm TX id=%-2u plen=%-3u tok=%u hdr=", msgid, plen, f[10]);
	for (i = 0; i < SPLM_HDR; i++)
		printf("%02x", f[i]);
	printf(" pay=");
	for (i = 0; i < plen; i++)
		printf("%02x", f[SPLM_HDR + i]);
	printf("\n");
	fflush(stdout);
	return 0;
}

static void splm_parse(const char *tag)
{
	size_t i = 0;

	while (srx_len - i >= SPLM_HDR) {
		unsigned int plen, k;
		unsigned char ck = 0;

		if (srx[i] != 0x59) {
			while (i < srx_len && srx[i] != 0x59)
				i++;
			break;
		}
		plen = (unsigned int)srx[i + 2] |
		       ((unsigned int)srx[i + 3] << 8);
		if (plen > SPLM_MAX_PAY) {
			i++;
			continue;
		}
		if (srx_len - i < SPLM_HDR + plen)
			break;
		for (k = 0; k < SPLM_HDR; k++)
			ck ^= srx[i + k];
		if (ck) {
			fprintf(stderr, "%s: bad header checksum at %zu\n",
				tag, i);
			i++;
			continue;
		}
		printf("%s RX state=%u id=%-2u tok=%-3u plen=%-3u pay=",
		       tag, srx[i + 8], srx[i + 9], srx[i + 10], plen);
		for (k = 0; k < plen; k++)
			printf("%02x", srx[i + SPLM_HDR + k]);
		if (plen) {
			printf(" \"");
			for (k = 0; k < plen; k++) {
				unsigned char ch = srx[i + SPLM_HDR + k];

				putchar((ch >= 0x20 && ch < 0x7f) ? ch : '.');
			}
			printf("\"");
		}
		printf("\n");
		fflush(stdout);
		i += SPLM_HDR + plen;
	}
	if (i) {
		memmove(srx, srx + i, srx_len - i);
		srx_len -= i;
	}
}

static void splm_rx(int fd, int timeout_ms, const char *tag)
{
	struct pollfd p;
	unsigned char b[2048];
	time_t hard = time(NULL) + (timeout_ms + 1999) / 1000 + 2;
	int idle = timeout_ms;

	p.fd = fd;
	p.events = POLLIN;
	while (time(NULL) < hard) {
		ssize_t r;

		p.revents = 0;
		if (poll(&p, 1, idle) <= 0)
			break;
		while ((r = read(fd, b, sizeof(b))) > 0) {
			if (srx_len + (size_t)r > sizeof(srx))
				srx_len = 0;
			memcpy(srx + srx_len, b, (size_t)r);
			srx_len += (size_t)r;
			idle = 250;
		}
		if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
		    errno != EINTR)
			fprintf(stderr, "%s: read: %s\n", tag, strerror(errno));
		splm_parse(tag);
	}
	splm_parse(tag);
}

static void splm_enable(int fd)
{
	unsigned int level = 3, mode = 0;
	unsigned char opt[2] = { 1, 1 };

	splm_send(fd, SPLM_M_QUERY_VERSION, NULL, 0);
	splm_rx(fd, 500, "VER");
	splm_send(fd, SPLM_M_RESET, &level, 4);
	splm_rx(fd, 500, "RST");
	splm_send(fd, SPLM_M_IS_READY, &mode, 4);
	splm_rx(fd, 500, "RDY");
	splm_send(fd, SPLM_M_LOGGING_OPTION, opt, 2);
	splm_rx(fd, 500, "OPT");
	splm_send(fd, SPLM_M_IS_READY, &mode, 4);
	splm_rx(fd, 500, "RDY2");
}

static int cmd_splm(int seconds)
{
	unsigned int st = 0;
	time_t end;
	int fd, i;

	dhl_probe("pre");
	for (i = 0; i < 50; i++) {
		phase = "state-poll";
		st = md_state_query();
		if (st == 2 || st == 3)
			break;
		usleep(200000);
	}
	printf("md state=%u after %d polls\n", st, i);
	if (st != 2 && st != 3) {
		fprintf(stderr, "MD not ready, cannot talk SPLM\n");
		return 1;
	}
	phase = "open-ctrl";
	fd = open(MD_LOG_CTRL_PATH, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", MD_LOG_CTRL_PATH,
			strerror(errno));
		return 1;
	}
	printf("using %s for SPLM\n", MD_LOG_CTRL_PATH);
	phase = "splm-enable";
	splm_enable(fd);
	usleep(500000);
	phase = "probe-post";
	dhl_probe("post");
	end = time(NULL) + seconds;
	phase = "splm-rx";
	while (time(NULL) < end)
		splm_rx(fd, 500, "SPLM");
	phase = "probe-end";
	dhl_probe("end");
	close(fd);
	return 0;
}

static int cmd_mdlog(int seconds, const char *raw_out, const char *tty_out)
{
	unsigned int base = 0, len = 0, state = 0;
	int raw_fd, ctrl_fd = -1, tty_fd = -1, tty_ofd = -1;
	void *m;
	unsigned char *prev;
	unsigned char buf[4096];
	time_t end;
	unsigned long tty_total = 0, raw_changes = 0;
	unsigned long t_text = 0, t_gap = 0, t_hi = 0;
	unsigned int changed_hi = 0;

	raw_fd = open(RAW_DHL_PATH, O_RDWR);
	if (raw_fd < 0)
		die(RAW_DHL_PATH);
	if (ioctl(raw_fd, CCCI_IOC_SMEM_BASE, &base) < 0)
		die("SMEM_BASE");
	if (ioctl(raw_fd, CCCI_IOC_SMEM_LEN, &len) < 0)
		die("SMEM_LEN");
	ioctl(raw_fd, CCCI_IOC_SMEM_GET_STATE, &state);
	m = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, raw_fd, 0);
	if (m == MAP_FAILED)
		die("mmap raw_dhl");

	{
		unsigned int md_state = md_state_query();

		printf("md state=%u (2=READY 3=EXCEPTION; log ports need READY "
		       "or the MD-start gate blocks)\n", md_state);
		if (md_state == 2 || md_state == 3) {
			ctrl_fd = open(MD_LOG_CTRL_PATH, O_RDWR | O_NONBLOCK);
			if (ctrl_fd < 0)
				fprintf(stderr, "%s: %s\n", MD_LOG_CTRL_PATH,
					strerror(errno));
			tty_fd = open(MD_LOG_TTY_PATH, O_RDONLY | O_NONBLOCK);
			if (tty_fd < 0)
				fprintf(stderr, "%s: %s\n", MD_LOG_TTY_PATH,
					strerror(errno));
		} else {
			fprintf(stderr, "not opening log ports in state %u\n",
				md_state);
		}
	}
	if (ctrl_fd >= 0) {
		printf("sending SPLM enable sequence\n");
		fflush(stdout);
		splm_enable(ctrl_fd);
		usleep(500000);
		dhl_probe("after-splm");
	}
	if (tty_out) {
		tty_ofd = open(tty_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		if (tty_ofd < 0)
			die(tty_out);
	}

	prev = malloc(len);
	if (!prev)
		die("malloc");
	memcpy(prev, m, len);

	printf("mdlog: raw_dhl base=0x%x len=%u state=%u ctrl_fd=%d tty_fd=%d "
	       "window=%ds\n", base, len, state, ctrl_fd, tty_fd, seconds);
	fflush(stdout);

	end = time(NULL) + seconds;
	while (time(NULL) < end) {
		unsigned long changed = 0;
		unsigned long z_text = 0, z_gap = 0, z_hi = 0;
		unsigned int first = len, last = 0;
		ssize_t r;

		if (tty_fd >= 0) {
			while ((r = read(tty_fd, buf, sizeof(buf))) > 0) {
				tty_total += (unsigned long)r;
				if (tty_ofd >= 0 &&
				    write(tty_ofd, buf, r) != r)
					die("tty write");
				if (tty_total <= 4096)
					printf("ttyC1 RX %zd: %.*s\n", r,
					       (int)r, buf);
			}
			if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
				fprintf(stderr, "ttyC1 read: %s\n",
					strerror(errno));
		}
		if (ctrl_fd >= 0) {
			while ((r = read(ctrl_fd, buf, sizeof(buf))) > 0)
				printf("md_log_ctrl RX %zd: %.*s\n", r,
				       (int)r, buf);
		}

		for (unsigned int i = 0; i < len; i++) {
			if (((unsigned char *)m)[i] == prev[i])
				continue;
			changed++;
			if (i < 0x600000)
				z_text++;
			else if (i < 0x680000)
				z_gap++;
			else
				z_hi++;
			if (i < first)
				first = i;
			last = i;
		}
		if (changed) {
			memcpy(prev, m, len);
			raw_changes += changed;
			t_text += z_text;
			t_gap += z_gap;
			t_hi += z_hi;
			if (last > changed_hi)
				changed_hi = last;
			printf("raw_dhl changed %lu bytes in [0x%x..0x%x] "
			       "(text=%lu gap=%lu hi=%lu total %lu)\n",
			       changed, first, last, z_text, z_gap, z_hi,
			       raw_changes);
			fflush(stdout);
		}
		usleep(250000);
	}

	printf("mdlog done: raw_dhl changed=%lu hi=0x%x ttyC1 RX=%lu "
	       "(text=%lu gap=%lu hi=%lu)\n",
	       raw_changes, changed_hi, tty_total, t_text, t_gap, t_hi);
	if (raw_out) {
		int ofd = open(raw_out, O_WRONLY | O_CREAT | O_TRUNC, 0644);

		if (ofd < 0)
			die(raw_out);
		if (write(ofd, m, len) != (ssize_t)len)
			die("raw write");
		close(ofd);
		printf("dumped %u bytes to %s\n", len, raw_out);
	}
	if (tty_ofd >= 0)
		close(tty_ofd);
	free(prev);
	return 0;
}

int main(int argc, char **argv)
{
	install_bus();
	if (argc < 2) {
		fprintf(stderr,
			"usage: %s status|raw|probe <dev>|rawdump <dev> <file>|init|notify [core]|watch [secs]|grab <file> [secs]|mdlog [secs] [rawfile] [ttyfile]|splm [secs]\n",
			argv[0]);
		return 2;
	}
	if (!strcmp(argv[1], "status"))
		return cmd_status();
	if (!strcmp(argv[1], "raw"))
		return cmd_raw();
	if (!strcmp(argv[1], "rawdump")) {
		if (argc < 4) { fprintf(stderr, "rawdump <dev> <file>\n"); return 2; }
		return cmd_rawdump(argv[2], argv[3]);
	}
	if (!strcmp(argv[1], "probe")) {
		if (argc < 3) { fprintf(stderr, "probe needs a dev path\n"); return 2; }
		return cmd_probe(argv[2]);
	}
	if (!strcmp(argv[1], "notify"))
		return cmd_notify(argc > 2 ? (unsigned)atoi(argv[2]) : 0);
	if (!strcmp(argv[1], "init"))
		return cmd_init();
	if (!strcmp(argv[1], "watch"))
		return cmd_watch(argc > 2 ? atoi(argv[2]) : 10);
	if (!strcmp(argv[1], "grab")) {
		if (argc < 3) {
			fprintf(stderr, "grab needs an output file\n");
			return 2;
		}
		return cmd_grab(argv[2], argc > 3 ? atoi(argv[3]) : 10);
	}
	if (!strcmp(argv[1], "splm"))
		return cmd_splm(argc > 2 ? atoi(argv[2]) : 20);
	if (!strcmp(argv[1], "mdlog"))
		return cmd_mdlog(argc > 2 ? atoi(argv[2]) : 30,
				 argc > 3 ? argv[3] : NULL,
				 argc > 4 ? argv[4] : NULL);
	fprintf(stderr, "unknown command %s\n", argv[1]);
	return 2;
}
