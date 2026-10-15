/*
 * Shared code of the verilog-pcie example design guest DMA tests, see
 * vpcie.h.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vpcie.h"

#define DMA_TIMEOUT_S	20	/* co-simulation is slow */

void vpcie_die(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(2);
}

void vpcie_log(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
}

int vpcie_check(struct vpcie_test *t, int cond, const char *fmt, ...)
{
	va_list ap;

	printf("  %s: ", cond ? "ok  " : "FAIL");
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
	fflush(stdout);
	if (!cond)
		t->errors++;
	return cond;
}

static unsigned int read_hex(const char *path)
{
	unsigned int v = 0;
	FILE *f = fopen(path, "r");

	if (f) {
		if (fscanf(f, "%x", &v) != 1)
			v = 0;
		fclose(f);
	}
	return v;
}

void vpcie_find_device(const char *bdf, char *out, size_t len)
{
	struct dirent *de;
	char path[PATH_MAX];
	DIR *d;

	if (bdf) {
		snprintf(out, len, "%s%s", strchr(bdf, ':') &&
			 strchr(strchr(bdf, ':') + 1, ':') ? "" : "0000:", bdf);
		return;
	}

	d = opendir("/sys/bus/pci/devices");
	if (!d)
		vpcie_die("can't read /sys/bus/pci/devices");
	out[0] = '\0';
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/vendor",
			 de->d_name);
		if (read_hex(path) != VPCIE_VENDOR)
			continue;
		snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/device",
			 de->d_name);
		if (read_hex(path) != VPCIE_DEVICE)
			continue;
		/* readdir order is arbitrary, keep the lowest BDF */
		if (!out[0] || strcmp(de->d_name, out) < 0)
			snprintf(out, len, "%s", de->d_name);
	}
	closedir(d);
	if (!out[0])
		vpcie_die("no %04x:%04x device found", VPCIE_VENDOR,
			  VPCIE_DEVICE);
}

static void link_name(const char *path, char *out, size_t len)
{
	char buf[PATH_MAX];
	ssize_t n = readlink(path, buf, sizeof(buf) - 1);
	const char *base;

	out[0] = '\0';
	if (n < 0)
		return;
	buf[n] = '\0';
	base = strrchr(buf, '/');
	snprintf(out, len, "%s", base ? base + 1 : buf);
}

void vpcie_driver(const char *bdf, char *out, size_t len)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/driver", bdf);
	link_name(path, out, len);
}

int vpcie_iommu_group(const char *bdf, char *type, size_t len)
{
	char path[PATH_MAX], grp[64];
	FILE *f;

	snprintf(type, len, "?");
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/iommu_group",
		 bdf);
	link_name(path, grp, sizeof(grp));
	if (!grp[0])
		return -1;

	snprintf(path, sizeof(path), "/sys/kernel/iommu_groups/%s/type", grp);
	f = fopen(path, "r");
	if (f) {
		if (fgets(type, len, f))
			type[strcspn(type, "\n")] = '\0';
		fclose(f);
	}
	return atoi(grp);
}

uint8_t *vpcie_alloc_buf(size_t size)
{
	uint8_t *buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

	if (buf == MAP_FAILED)
		vpcie_die("mmap of %zu bytes failed: %s", size,
			  strerror(errno));
	if (mlock(buf, size))
		vpcie_die("mlock failed: %s (run as root)", strerror(errno));
	memset(buf, 0, size);
	return buf;
}

uint64_t vpcie_virt_to_phys(const void *va)
{
	uintptr_t v = (uintptr_t)va;
	uint64_t e;
	int fd;

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0)
		vpcie_die("can't open /proc/self/pagemap: %s",
			  strerror(errno));
	if (pread(fd, &e, 8, (v / VPCIE_PAGE) * 8) != 8)
		vpcie_die("can't read /proc/self/pagemap");
	close(fd);

	if (!(e >> 63))
		vpcie_die("page at %#lx not present", (unsigned long)v);
	e &= (1ULL << 55) - 1;
	if (!e)
		vpcie_die("no PFN in /proc/self/pagemap, run as root");
	return e * VPCIE_PAGE + v % VPCIE_PAGE;
}

static int contains_nocase(const char *s, const char *sub)
{
	return strcasestr(s, sub) != NULL;
}

int vpcie_dmar_faults(int print_from)
{
	int len = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
	char *buf, *line, *save;
	int n = 0;

	if (len <= 0)
		return -1;
	buf = malloc(len + 1);
	len = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, len);
	if (len < 0) {
		free(buf);
		return -1;
	}
	buf[len] = '\0';

	for (line = strtok_r(buf, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		if (!strstr(line, "DMAR") || !contains_nocase(line, "fault"))
			continue;
		if (print_from >= 0 && n >= print_from)
			vpcie_log("  dmesg: %s", line);
		n++;
	}
	free(buf);
	return n;
}

const char *vpcie_dma_error_name(int err)
{
	static const char *const names[] = {
		[0] = "0 (NONE)",
		[1] = "1 (TIMEOUT)",
		[2] = "2 (PARITY)",
		[4] = "4 (AXI_RD_SLVERR)",
		[5] = "5 (AXI_RD_DECERR)",
		[6] = "6 (AXI_WR_SLVERR)",
		[7] = "7 (AXI_WR_DECERR)",
		[8] = "8 (PCIE_FLR)",
		[9] = "9 (PCIE_CPL_POISONED)",
		[10] = "10 (PCIE_CPL_STATUS_UR)",
		[11] = "11 (PCIE_CPL_STATUS_CA)",
	};

	if (err == VPCIE_DMA_TIMEOUT)
		return "timeout";
	if (err >= 0 && err < (int)(sizeof(names) / sizeof(names[0])) &&
	    names[err])
		return names[err];
	return "?";
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t next_tag(struct vpcie_test *t)
{
	t->tag = t->tag % 0x7f + 1;
	return t->tag | t->tag_flags;
}

int vpcie_dma(struct vpcie_test *t, uint32_t base, uint64_t host_addr,
	      uint32_t card_addr, uint32_t len)
{
	struct vpcie_dev *d = t->dev;
	const char *dir = base == VPCIE_DMA_RD ? "read" : "write";
	uint32_t tag = next_tag(t);
	double end;
	uint32_t st;

	d->wr32(d, 0, base + 0x00, host_addr & 0xffffffff);
	d->wr32(d, 0, base + 0x04, host_addr >> 32);
	d->wr32(d, 0, base + 0x08, card_addr);
	d->wr32(d, 0, base + 0x0c, 0);
	d->wr32(d, 0, base + 0x10, len);
	d->wr32(d, 0, base + 0x14, tag);

	end = now() + DMA_TIMEOUT_S;
	for (;;) {
		st = d->rd32(d, 0, base + 0x18);
		if ((st >> 31) && (st & 0xffff) == tag) {
			int err = (st >> 24) & 0xf;

			if (err)
				vpcie_log("  DMA %s addr %#llx len %u tag %#x: "
					  "error %s (status %08x)", dir,
					  (unsigned long long)host_addr, len,
					  tag, vpcie_dma_error_name(err), st);
			return err;
		}
		if (now() > end) {
			vpcie_log("  DMA %s addr %#llx len %u tag %#x: timeout, "
				  "status %08x", dir,
				  (unsigned long long)host_addr, len, tag, st);
			return VPCIE_DMA_TIMEOUT;
		}
		usleep(1000);
	}
}

void vpcie_setup(struct vpcie_test *t, int irq)
{
	t->dev->wr32(t->dev, 0, 0x000, 1);		/* DMA enable */
	t->dev->wr32(t->dev, 0, 0x008, irq ? 0x3 : 0);	/* interrupts */
}

void vpcie_test_bar2(struct vpcie_test *t)
{
	static const uint32_t vals[] = { 0x44332211, 0xdeadbeef };
	unsigned int i;

	vpcie_log("BAR2 MMIO");
	for (i = 0; i < 2; i++) {
		uint32_t r;

		t->dev->wr32(t->dev, 2, 0x0, vals[i]);
		r = t->dev->rd32(t->dev, 2, 0x0);
		vpcie_check(t, r == vals[i], "BAR2 write %08x read %08x",
			    vals[i], r);
	}
}

void vpcie_test_copy(struct vpcie_test *t, size_t src, size_t dst,
		     uint32_t len)
{
	uint8_t *pattern = malloc(len), *expect = malloc(t->size);
	int ok, data, rest, err_rd, err_wr = VPCIE_DMA_TIMEOUT;
	unsigned int irq0 = 0;
	char desc[160];
	uint32_t i;

	for (i = 0; i < len; i++)
		pattern[i] = rand();
	memset(t->buf, 0xee, t->size);
	memcpy(t->buf + src, pattern, len);
	memcpy(expect, t->buf, t->size);
	memcpy(expect + dst, pattern, len);

	snprintf(desc, sizeof(desc), "len %4u  src %#06zx (dma %#llx)  "
		 "dst %#06zx (dma %#llx)", len, src,
		 (unsigned long long)t->dma_addr(t, src), dst,
		 (unsigned long long)t->dma_addr(t, dst));
	if (t->verbose)
		vpcie_log("  %s", desc);
	if (t->irq_count)
		irq0 = t->irq_count(t);

	err_rd = vpcie_dma(t, VPCIE_DMA_RD, t->dma_addr(t, src),
			   VPCIE_CARD_RAM, len);
	if (!err_rd)
		err_wr = vpcie_dma(t, VPCIE_DMA_WR, t->dma_addr(t, dst),
				   VPCIE_CARD_RAM, len);
	ok = !err_rd && !err_wr;
	data = !memcmp(t->buf + dst, pattern, len);
	rest = !memcmp(t->buf, expect, t->size);
	vpcie_check(t, ok && data && rest, "%s%s%s%s", desc,
		    ok ? "" : " (DMA error / timeout)",
		    data ? "" : " (data mismatch)",
		    !data || rest ? "" :
		    " (bytes outside the destination changed)");

	if (t->irq_count) {
		/* Interrupts are delivered asynchronously, give them time */
		double end = now() + 2;
		unsigned int n;

		while ((n = t->irq_count(t) - irq0) < 2 && now() < end)
			usleep(10000);
		vpcie_check(t, n >= 2,
			    "%u MSI-X interrupt(s) for the 2 transfers", n);
	}
	free(pattern);
	free(expect);
}

struct copy_case {
	size_t src, dst;
	uint32_t len;
};

int vpcie_run(struct vpcie_test *t, int irq)
{
	static const struct copy_case iommu_cases[] = {
		{ 0x0000, 0x2000, 256 },
		{ 0x0010, 0x2803, 1000 },	/* unaligned destination */
		{ 0x0f00, 0x2f81, 512 },	/* both cross a page boundary */
		{ 0x0000, 0x4000, 4096 },	/* whole pages */
		{ 0x0ffd, 0x5ffe, 4100 },	/* odd, crosses 2 pages */
	};
	static const struct copy_case phys_cases[] = {
		{ 0x000, 0x1000, 256 },
		{ 0x010, 0x1803, 1000 },	/* unaligned destination */
		{ 0x100, 0x1101, 3 },		/* sub-DW */
		{ 0x000, 0x1000, 4096 },	/* whole page */
	};
	const struct copy_case *c;
	size_t i, n;

	vpcie_setup(t, irq);
	vpcie_test_bar2(t);

	if (t->contiguous) {
		vpcie_log("DMA copies (%s requests, buffer contiguous in IOVA "
			  "space, scattered in physical memory)",
			  t->tag_flags & VPCIE_TAG_VIRT ? "untranslated" :
			  "translated");
		c = iommu_cases;
		n = sizeof(iommu_cases) / sizeof(iommu_cases[0]);
	} else {
		vpcie_log("DMA copies (%s requests, physical addresses, "
			  "within single pages)",
			  t->tag_flags & VPCIE_TAG_VIRT ? "untranslated" :
			  "translated");
		c = phys_cases;
		n = sizeof(phys_cases) / sizeof(phys_cases[0]);
	}
	for (i = 0; i < n; i++) {
		size_t end = (c[i].src > c[i].dst ? c[i].src : c[i].dst) +
			     c[i].len;

		if (end > t->size) {
			vpcie_log("  skip: len %u dst %#zx does not fit the %zu "
				  "byte buffer", c[i].len, c[i].dst, t->size);
			continue;
		}
		vpcie_test_copy(t, c[i].src, c[i].dst, c[i].len);
	}
	return t->errors;
}

int vpcie_summary(const char *name, int errors)
{
	printf("%s %s (%d error(s))\n", name, errors ? "FAIL" : "PASS",
	       errors);
	return errors ? 1 : 0;
}
