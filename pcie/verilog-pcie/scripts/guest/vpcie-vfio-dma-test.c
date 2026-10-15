/*
 * verilog-pcie example design DMA test through VFIO with VIRTUAL addresses
 * (run as root).
 *
 * The device is bound to vfio-pci and its DMA is translated by the IOMMU.
 * A user space buffer is mapped into the device's IO address space with
 * VFIO_IOMMU_MAP_DMA, by default at an IOVA equal to the buffer's process
 * virtual address, so the device DMAs with the same (virtual) addresses as
 * the program uses. The tags have the AT chicken bit set (bit 8), the DMA
 * engine sends untranslated requests (AT = 00). The buffer is contiguous in
 * IOVA space but scattered in physical memory, the copies cross page
 * boundaries and only work if the IOMMU translates every page.
 *
 * Also checks MSI-X delivery (VFIO eventfds) and that the IOMMU blocks DMA
 * to an unmapped IOVA (the read is completed with UR, the write is dropped).
 *
 * Needs a kernel with IOMMU and VFIO support booted with intel_iommu=on,
 * and QEMU's intel-iommu with aw-bits=48 for virtual-address IOVAs (with
 * the default 39 bits use --iova).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <linux/vfio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "vpcie.h"

#define MAX_VECTORS	4

struct vfio_dev {
	struct vpcie_dev dev;
	int container, group, fd;
	int group_nr;
	uint64_t region_off[VFIO_PCI_NUM_REGIONS];
	int nr_vectors;
	int eventfd[MAX_VECTORS];
	unsigned int irqs;
	uint64_t iova;		/* IOVA of the buffer */
};

static uint32_t vfio_rd32(struct vpcie_dev *d, int bar, uint32_t off)
{
	struct vfio_dev *v = (struct vfio_dev *)d;
	uint32_t val;

	if (pread(v->fd, &val, 4, v->region_off[bar] + off) != 4)
		vpcie_die("BAR%d read at %#x failed: %s", bar, off,
			  strerror(errno));
	return val;
}

static void vfio_wr32(struct vpcie_dev *d, int bar, uint32_t off,
		      uint32_t val)
{
	struct vfio_dev *v = (struct vfio_dev *)d;

	if (pwrite(v->fd, &val, 4, v->region_off[bar] + off) != 4)
		vpcie_die("BAR%d write at %#x failed: %s", bar, off,
			  strerror(errno));
}

static void vfio_open(struct vfio_dev *v, const char *bdf)
{
	struct vfio_group_status status = { .argsz = sizeof(status) };
	char drv[64], type[64], path[64];
	uint64_t cfg;
	uint16_t cmd;
	int iommu, idx;

	v->dev.rd32 = vfio_rd32;
	v->dev.wr32 = vfio_wr32;

	vpcie_driver(bdf, drv, sizeof(drv));
	if (strcmp(drv, "vfio-pci"))
		vpcie_die("%s is bound to %s, bind it to vfio-pci first "
			  "(vpcie-bind.sh vfio)", bdf, drv[0] ? drv : "nothing");
	v->group_nr = vpcie_iommu_group(bdf, type, sizeof(type));
	if (v->group_nr < 0)
		vpcie_die("%s has no IOMMU group, boot with intel_iommu=on",
			  bdf);

	/* Container with a type 1 IOMMU */
	v->container = open("/dev/vfio/vfio", O_RDWR);
	if (v->container < 0)
		vpcie_die("can't open /dev/vfio/vfio: %s", strerror(errno));
	if (ioctl(v->container, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
		vpcie_die("unknown VFIO API version");
	iommu = VFIO_TYPE1v2_IOMMU;
	if (ioctl(v->container, VFIO_CHECK_EXTENSION, iommu) <= 0) {
		iommu = VFIO_TYPE1_IOMMU;
		if (ioctl(v->container, VFIO_CHECK_EXTENSION, iommu) <= 0)
			vpcie_die("no VFIO type 1 IOMMU support");
	}

	snprintf(path, sizeof(path), "/dev/vfio/%d", v->group_nr);
	v->group = open(path, O_RDWR);
	if (v->group < 0)
		vpcie_die("can't open %s: %s", path, strerror(errno));
	if (ioctl(v->group, VFIO_GROUP_GET_STATUS, &status))
		vpcie_die("VFIO_GROUP_GET_STATUS: %s", strerror(errno));
	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE))
		vpcie_die("IOMMU group %d is not viable, bind all its devices "
			  "to vfio-pci", v->group_nr);
	if (ioctl(v->group, VFIO_GROUP_SET_CONTAINER, &v->container))
		vpcie_die("VFIO_GROUP_SET_CONTAINER: %s", strerror(errno));
	if (ioctl(v->container, VFIO_SET_IOMMU, iommu))
		vpcie_die("VFIO_SET_IOMMU: %s", strerror(errno));

	v->fd = ioctl(v->group, VFIO_GROUP_GET_DEVICE_FD, bdf);
	if (v->fd < 0)
		vpcie_die("VFIO_GROUP_GET_DEVICE_FD: %s", strerror(errno));

	/* Regions: BARs and config space */
	for (idx = 0; idx < VFIO_PCI_NUM_REGIONS; idx++) {
		struct vfio_region_info ri = {
			.argsz = sizeof(ri), .index = idx,
		};

		if (!ioctl(v->fd, VFIO_DEVICE_GET_REGION_INFO, &ri))
			v->region_off[idx] = ri.offset;
	}

	/* Memory space + bus master enable */
	cfg = v->region_off[VFIO_PCI_CONFIG_REGION_INDEX];
	if (pread(v->fd, &cmd, 2, cfg + 4) != 2)
		vpcie_die("config read failed");
	cmd |= 0x6;
	if (pwrite(v->fd, &cmd, 2, cfg + 4) != 2)
		vpcie_die("config write failed");
}

static int map_dma(struct vfio_dev *v, void *va, uint64_t iova, size_t size)
{
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = (uintptr_t)va,
		.iova = iova,
		.size = size,
	};

	return ioctl(v->container, VFIO_IOMMU_MAP_DMA, &map);
}

static void unmap_dma(struct vfio_dev *v, uint64_t iova, size_t size)
{
	struct vfio_iommu_type1_dma_unmap unmap = {
		.argsz = sizeof(unmap), .iova = iova, .size = size,
	};

	ioctl(v->container, VFIO_IOMMU_UNMAP_DMA, &unmap);
}

static int msix_count(struct vfio_dev *v)
{
	struct vfio_irq_info info = {
		.argsz = sizeof(info), .index = VFIO_PCI_MSIX_IRQ_INDEX,
	};

	if (ioctl(v->fd, VFIO_DEVICE_GET_IRQ_INFO, &info))
		return 0;
	return info.count;
}

/* One eventfd per MSI-X vector */
static void enable_msix(struct vfio_dev *v, int count)
{
	size_t sz = sizeof(struct vfio_irq_set) + count * sizeof(int);
	struct vfio_irq_set *set = calloc(1, sz);
	int i;

	for (i = 0; i < count; i++) {
		v->eventfd[i] = eventfd(0, EFD_NONBLOCK);
		if (v->eventfd[i] < 0)
			vpcie_die("eventfd: %s", strerror(errno));
	}
	set->argsz = sz;
	set->flags = VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	set->index = VFIO_PCI_MSIX_IRQ_INDEX;
	set->start = 0;
	set->count = count;
	memcpy(set->data, v->eventfd, count * sizeof(int));
	if (ioctl(v->fd, VFIO_DEVICE_SET_IRQS, set))
		vpcie_die("VFIO_DEVICE_SET_IRQS: %s", strerror(errno));
	free(set);
	v->nr_vectors = count;
}

static void disable_msix(struct vfio_dev *v)
{
	struct vfio_irq_set set = {
		.argsz = sizeof(set),
		.flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER,
		.index = VFIO_PCI_MSIX_IRQ_INDEX,
	};
	int i;

	ioctl(v->fd, VFIO_DEVICE_SET_IRQS, &set);
	for (i = 0; i < v->nr_vectors; i++)
		close(v->eventfd[i]);
	v->nr_vectors = 0;
}

/* Interrupts received so far over all vectors */
static unsigned int msix_total(struct vpcie_test *t)
{
	struct vfio_dev *v = t->priv;
	uint64_t n;
	int i;

	for (i = 0; i < v->nr_vectors; i++) {
		if (read(v->eventfd[i], &n, sizeof(n)) == sizeof(n))
			v->irqs += n;
	}
	return v->irqs;
}

static uint64_t iova_dma_addr(struct vpcie_test *t, size_t off)
{
	struct vfio_dev *v = t->priv;

	return v->iova + off;
}

/* DMA to an IOVA that is not mapped must not reach memory */
static void test_protection(struct vpcie_test *t)
{
	struct vfio_dev *v = t->priv;
	uint64_t bad = v->iova + t->size + 0x100000;
	unsigned int (*irq_count)(struct vpcie_test *t) = t->irq_count;
	uint8_t *before = malloc(t->size);
	int err, faults0 = vpcie_dmar_faults(-1);

	vpcie_log("IOMMU protection (DMA to an unmapped IOVA)");
	t->irq_count = NULL;

	err = vpcie_dma(t, VPCIE_DMA_RD, bad, VPCIE_CARD_RAM, 64);
	vpcie_check(t, err == VPCIE_DMA_ERR_CPL_UR,
		    "DMA read from unmapped IOVA %#llx is completed with UR, "
		    "DMA status error %s", (unsigned long long)bad,
		    vpcie_dma_error_name(err));

	memset(t->buf, 0xee, t->size);
	memcpy(before, t->buf, t->size);
	err = vpcie_dma(t, VPCIE_DMA_WR, bad, VPCIE_CARD_RAM, 64);
	vpcie_check(t, err == 0 && !memcmp(t->buf, before, t->size),
		    "DMA write to unmapped IOVA %#llx is dropped (posted, "
		    "status error %s, buffer unchanged)",
		    (unsigned long long)bad, vpcie_dma_error_name(err));

	/* The engine still works afterwards */
	vpcie_test_copy(t, 0x0, 0x2000, 256);
	t->irq_count = irq_count;

	/* The fault details are logged a bit later */
	sleep(1);
	if (faults0 < 0)
		vpcie_log("  (can't read the kernel log, need root)");
	else if (vpcie_dmar_faults(faults0) <= faults0)
		vpcie_log("  (no new DMAR fault lines in dmesg)");
	free(before);
}

static void usage(const char *prog)
{
	printf("Usage: %s [-d BDF] [--iova ADDR] [--pages N] [--no-irq] "
	       "[--no-fault] [-s SEED] [-v]\n"
	       "  -d BDF       device (default: first 1234:0001)\n"
	       "  --iova ADDR  IOVA to map the buffer at (default: its "
	       "virtual address)\n"
	       "  --pages N    buffer size in pages (default 8)\n"
	       "  --no-irq     do not check MSI-X interrupts\n"
	       "  --no-fault   skip the unmapped IOVA test\n"
	       "  -s SEED      random seed of the test data\n"
	       "  -v           verbose\n", prog);
}

int main(int argc, char *argv[])
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "iova", required_argument, NULL, 'i' },
		{ "pages", required_argument, NULL, 'p' },
		{ "no-irq", no_argument, NULL, 'I' },
		{ "no-fault", no_argument, NULL, 'F' },
		{ "seed", required_argument, NULL, 's' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	const char *bdf_arg = NULL;
	struct vpcie_test t = { 0 };
	struct vfio_dev dev = { 0 };
	int c, irq = 1, no_fault = 0, have_iova = 0;
	unsigned int seed = time(NULL), pages = 8;
	char bdf[64];
	size_t p;

	while ((c = getopt_long(argc, argv, "d:s:vh", opts, NULL)) != -1) {
		switch (c) {
		case 'd': bdf_arg = optarg; break;
		case 'i':
			dev.iova = strtoull(optarg, NULL, 0);
			have_iova = 1;
			break;
		case 'p': pages = strtoul(optarg, NULL, 0); break;
		case 'I': irq = 0; break;
		case 'F': no_fault = 1; break;
		case 's': seed = strtoul(optarg, NULL, 0); break;
		case 'v': t.verbose = 1; break;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}
	srand(seed);
	if (!pages)
		vpcie_die("--pages must be at least 1");

	vpcie_find_device(bdf_arg, bdf, sizeof(bdf));
	vfio_open(&dev, bdf);
	printf("device %s, IOMMU group %d, BAR0 region at %#llx, seed %u\n",
	       bdf, dev.group_nr, (unsigned long long)dev.region_off[0],
	       seed);

	t.size = pages * VPCIE_PAGE;
	t.buf = vpcie_alloc_buf(t.size);
	if (!have_iova)
		dev.iova = (uintptr_t)t.buf;
	if (map_dma(&dev, t.buf, dev.iova, t.size))
		vpcie_die("VFIO_IOMMU_MAP_DMA of iova %#llx failed: %s\n"
			  "IOVA beyond the IOMMU address width? Use aw-bits=48 "
			  "on QEMU's intel-iommu or --iova 0x10000000",
			  (unsigned long long)dev.iova, strerror(errno));

	printf("buffer: %u pages, va %p mapped at iova %#llx%s\n", pages,
	       t.buf, (unsigned long long)dev.iova,
	       dev.iova == (uintptr_t)t.buf ? " (iova == va)" : "");
	for (p = 0; p < t.size; p += VPCIE_PAGE)
		printf("  page %zu: iova %#llx -> pa %#llx\n", p / VPCIE_PAGE,
		       (unsigned long long)(dev.iova + p),
		       (unsigned long long)vpcie_virt_to_phys(t.buf + p));

	if (irq) {
		int n = msix_count(&dev);

		if (n > MAX_VECTORS)
			n = MAX_VECTORS;
		enable_msix(&dev, n);
		printf("MSI-X: %d vector(s) with eventfds\n", n);
	}

	t.dev = &dev.dev;
	t.priv = &dev;
	t.dma_addr = iova_dma_addr;
	t.irq_count = irq ? msix_total : NULL;
	t.contiguous = 1;
	t.tag_flags = VPCIE_TAG_VIRT;	/* untranslated requests */
	vpcie_run(&t, irq);
	if (irq)
		dev.dev.wr32(&dev.dev, 0, 0x008, 0);
	if (!no_fault)
		test_protection(&t);

	if (irq)
		disable_msix(&dev);
	unmap_dma(&dev, dev.iova, t.size);
	return vpcie_summary("VPCIE_VFIO_DMA_TEST", t.errors);
}
