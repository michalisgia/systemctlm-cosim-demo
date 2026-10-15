/*
 * verilog-pcie example design DMA test with PHYSICAL addresses (run as root).
 *
 * No driver: the BARs are mapped through sysfs, the DMA buffer is a locked
 * anonymous mapping and its physical addresses come from /proc/self/pagemap.
 *
 * The tags are written without the AT chicken bit, so the DMA engine sends
 * translated requests (AT = 10) that bypass the IOMMU (QEMU's root port
 * with ats-translated-bypass=on). This works with the IOMMU translating the
 * device's DMA (intel_iommu=on, the default of the test image).
 *
 * When the IOMMU translates the device's DMA, the same physical addresses
 * are then sent as untranslated requests (AT = 00, tag bit 8 set): the
 * IOMMU has no mapping for them, so the read must complete with UR and the
 * write must be dropped (DMAR faults in dmesg).
 *
 * -u sends untranslated requests for the copies instead, that only works
 * without IOMMU or with an identity (passthrough) domain.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "vpcie.h"

#define BAR_MAP_SIZE	0x10000

struct sysfs_dev {
	struct vpcie_dev dev;
	volatile uint32_t *bar[3];
};

static uint32_t sysfs_rd32(struct vpcie_dev *d, int bar, uint32_t off)
{
	struct sysfs_dev *s = (struct sysfs_dev *)d;

	return s->bar[bar][off / 4];
}

static void sysfs_wr32(struct vpcie_dev *d, int bar, uint32_t off,
		       uint32_t val)
{
	struct sysfs_dev *s = (struct sysfs_dev *)d;

	s->bar[bar][off / 4] = val;
}

static void sysfs_open(struct sysfs_dev *s, const char *bdf)
{
	char path[256];
	uint16_t cmd;
	int bar, fd;

	s->dev.rd32 = sysfs_rd32;
	s->dev.wr32 = sysfs_wr32;
	for (bar = 0; bar <= 2; bar += 2) {
		void *p;

		snprintf(path, sizeof(path),
			 "/sys/bus/pci/devices/%s/resource%d", bdf, bar);
		fd = open(path, O_RDWR | O_SYNC);
		if (fd < 0)
			vpcie_die("can't open %s (run as root)", path);
		p = mmap(NULL, BAR_MAP_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
		if (p == MAP_FAILED)
			vpcie_die("can't mmap %s", path);
		close(fd);
		s->bar[bar] = p;
	}

	/* Memory space + bus master enable */
	snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/config", bdf);
	fd = open(path, O_RDWR);
	if (fd < 0 || pread(fd, &cmd, 2, 4) != 2)
		vpcie_die("can't read %s", path);
	cmd |= 0x6;
	if (pwrite(fd, &cmd, 2, 4) != 2)
		vpcie_die("can't write %s", path);
	close(fd);
}

static uint64_t phys_dma_addr(struct vpcie_test *t, size_t off)
{
	return vpcie_virt_to_phys(t->buf + off);
}

/*
 * The IOMMU translates untranslated requests: physical addresses sent
 * untranslated must not reach memory.
 */
static void test_protection(struct vpcie_test *t)
{
	uint64_t pa = t->dma_addr(t, 0);
	uint32_t flags = t->tag_flags;
	uint8_t *before = malloc(t->size);
	int err, faults0 = vpcie_dmar_faults(-1);

	vpcie_log("IOMMU protection (physical address in an untranslated "
		  "request)");
	t->tag_flags = VPCIE_TAG_VIRT;

	err = vpcie_dma(t, VPCIE_DMA_RD, pa, VPCIE_CARD_RAM, 64);
	vpcie_check(t, err == VPCIE_DMA_ERR_CPL_UR,
		    "untranslated DMA read from pa %#llx is completed with "
		    "UR, DMA status error %s", (unsigned long long)pa,
		    vpcie_dma_error_name(err));

	memset(t->buf, 0xee, t->size);
	memcpy(before, t->buf, t->size);
	err = vpcie_dma(t, VPCIE_DMA_WR, pa, VPCIE_CARD_RAM, 64);
	vpcie_check(t, err == 0 && !memcmp(t->buf, before, t->size),
		    "untranslated DMA write to pa %#llx is dropped (posted, "
		    "status error %s, buffer unchanged)",
		    (unsigned long long)pa, vpcie_dma_error_name(err));

	/* Translated requests still work */
	t->tag_flags = flags;
	vpcie_test_copy(t, 0x0, 0x1000, 256);

	/* The fault details are logged a bit later */
	sleep(1);
	if (vpcie_dmar_faults(faults0 < 0 ? 0 : faults0) <= faults0)
		vpcie_log("  (no new DMAR fault lines in dmesg)");
	free(before);
}

static void usage(const char *prog)
{
	printf("Usage: %s [-d BDF] [-u] [--no-fault] [-s SEED] [-v]\n"
	       "  -d BDF       device (default: first 1234:0001)\n"
	       "  -u           untranslated requests (AT = 00) for the copies,\n"
	       "               needs no IOMMU or an identity domain\n"
	       "  --no-fault   skip the IOMMU protection check\n"
	       "  -s SEED      random seed of the test data\n"
	       "  -v           verbose\n", prog);
}

int main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "untranslated", no_argument, NULL, 'u' },
		{ "no-fault", no_argument, NULL, 'F' },
		{ "seed", required_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const char *bdf_arg = NULL;
	char bdf[64], drv[64], type[64];
	struct vpcie_test t = { 0 };
	struct sysfs_dev dev;
	int c, grp, translating, no_fault = 0;
	unsigned int seed = time(NULL);
	size_t p;

	while ((c = getopt_long(argc, argv, "d:us:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'd': bdf_arg = optarg; break;
		case 'u': t.tag_flags = VPCIE_TAG_VIRT; break;
		case 'F': no_fault = 1; break;
		case 's': seed = strtoul(optarg, NULL, 0); break;
		case 'v': t.verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	srand(seed);

	vpcie_find_device(bdf_arg, bdf, sizeof(bdf));
	vpcie_driver(bdf, drv, sizeof(drv));
	if (drv[0])
		vpcie_die("%s is bound to %s, unbind it first "
			  "(vpcie-bind.sh none)", bdf, drv);

	grp = vpcie_iommu_group(bdf, type, sizeof(type));
	translating = grp >= 0 && strcmp(type, "identity");
	printf("device %s, ", bdf);
	if (grp >= 0)
		printf("IOMMU group %d, type %s (%s)\n", grp, type,
		       translating ? "DMA translated" : "passthrough");
	else
		printf("no IOMMU group (IOMMU disabled)\n");
	printf("copies with %s requests, seed %u\n",
	       t.tag_flags ? "untranslated (AT = 00, tag bit 8 set)" :
	       "translated (AT = 10, tag bit 8 clear)", seed);
	if (t.tag_flags && translating)
		printf("warning: the IOMMU translates untranslated requests, "
		       "physical addresses will fault\n");

	sysfs_open(&dev, bdf);

	/* Two pages, each physically contiguous only within itself */
	t.size = 2 * VPCIE_PAGE;
	t.buf = vpcie_alloc_buf(t.size);
	for (p = 0; p < t.size; p += VPCIE_PAGE)
		printf("buffer page %zu: va %p -> pa %#llx\n", p / VPCIE_PAGE,
		       t.buf + p,
		       (unsigned long long)vpcie_virt_to_phys(t.buf + p));

	t.dev = &dev.dev;
	t.dma_addr = phys_dma_addr;
	t.contiguous = 0;
	vpcie_run(&t, 0);

	if (translating && !t.tag_flags && !no_fault)
		test_protection(&t);

	return vpcie_summary("VPCIE_PHYS_DMA_TEST", t.errors);
}
