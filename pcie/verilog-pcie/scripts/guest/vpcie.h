/*
 * Shared code of the verilog-pcie example design guest DMA tests.
 *
 * The tests only need register access to the BARs (rd32 / wr32) and a host
 * buffer with known DMA addresses, so the same checks run with physical
 * addresses (sysfs BAR mmap + /proc/self/pagemap) and with IOMMU translated
 * addresses (VFIO).
 *
 * Register map of the example design's DMA engine (BAR0), see verilog-pcie
 * example/common/rtl/example_core.v:
 *   0x000 control / status, bit 0 DMA enable, bits 9:8 busy
 *   0x008 interrupt enable (bit 0 read done, bit 1 write done)
 *   0x100 DMA read (host -> card RAM), 0x200 DMA write (card RAM -> host):
 *     +0x00 / +0x04  host DMA address low / high
 *     +0x08 / +0x0c  card RAM address low / high
 *     +0x10          length (bytes)
 *     +0x14          tag, writing it starts the transfer
 *     +0x18          status: [31] done (cleared by the read), [27:24] error,
 *                    [15:0] tag
 *
 * AT chicken bit (pcie/verilog-pcie/rtl/at-chicken-bit.patch): tag bit 8
 * set (e.g. 0x1AA) marks the host address as virtual, the DMA engine sends
 * untranslated requests (AT = 00) that the IOMMU translates. Clear (0xAA)
 * marks it physical, the requests are translated (AT = 10) and bypass the
 * IOMMU.
 */
#ifndef VPCIE_H
#define VPCIE_H

#include <stddef.h>
#include <stdint.h>

#define VPCIE_VENDOR		0x1234
#define VPCIE_DEVICE		0x0001
#define VPCIE_PAGE		4096UL
#define VPCIE_CARD_RAM		0x0	/* card RAM address used by the tests */

#define VPCIE_DMA_RD		0x100	/* host -> card RAM */
#define VPCIE_DMA_WR		0x200	/* card RAM -> host */

#define VPCIE_TAG_VIRT		0x100	/* AT chicken bit */

/* Status error codes of verilog-pcie's DMA engine (rtl/dma_if_pcie_*.v) */
#define VPCIE_DMA_ERR_CPL_UR	10
#define VPCIE_DMA_TIMEOUT	(-1)

struct vpcie_dev {
	uint32_t (*rd32)(struct vpcie_dev *d, int bar, uint32_t off);
	void (*wr32)(struct vpcie_dev *d, int bar, uint32_t off, uint32_t val);
};

struct vpcie_test {
	struct vpcie_dev *dev;
	uint8_t *buf;
	size_t size;
	/* DMA address of buf[off] as seen by the device */
	uint64_t (*dma_addr)(struct vpcie_test *t, size_t off);
	/* Interrupts received so far, NULL: not checked */
	unsigned int (*irq_count)(struct vpcie_test *t);
	void *priv;
	/*
	 * 1: DMA addresses are contiguous over the whole buffer (IOMMU),
	 * 0: only within a page (physical addresses)
	 */
	int contiguous;
	/* VPCIE_TAG_VIRT: untranslated requests, 0: translated */
	uint32_t tag_flags;
	int verbose;
	uint32_t tag;
	int errors;
};

/* Exits on errors, like the rest of the helpers */
void vpcie_die(const char *fmt, ...)
	__attribute__((noreturn, format(printf, 1, 2)));

/* Fills in the BDF of the end-point (first 1234:0001 if bdf is NULL) */
void vpcie_find_device(const char *bdf, char *out, size_t len);
/* Driver bound to the device, "" if none */
void vpcie_driver(const char *bdf, char *out, size_t len);
/* IOMMU group number, -1 if none; type ("DMA", "identity", ...) */
int vpcie_iommu_group(const char *bdf, char *type, size_t len);

/* Anonymous, locked, faulted in buffer */
uint8_t *vpcie_alloc_buf(size_t size);
uint64_t vpcie_virt_to_phys(const void *va);

/* DMAR fault lines in the kernel log, -1 if it can't be read */
int vpcie_dmar_faults(int print_from);

const char *vpcie_dma_error_name(int err);

void vpcie_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int vpcie_check(struct vpcie_test *t, int cond, const char *fmt, ...)
	__attribute__((format(printf, 3, 4)));

/*
 * Starts a DMA (VPCIE_DMA_RD / VPCIE_DMA_WR) with t->tag_flags | flags in
 * the tag and waits for it. Returns the error field of the status (0 ok),
 * VPCIE_DMA_TIMEOUT on timeout.
 */
int vpcie_dma(struct vpcie_test *t, uint32_t base, uint64_t host_addr,
	      uint32_t card_addr, uint32_t len);

void vpcie_setup(struct vpcie_test *t, int irq);
void vpcie_test_bar2(struct vpcie_test *t);
/*
 * host buf[src..] -> card RAM -> host buf[dst..], checks the copied data
 * and that nothing else in the buffer changed.
 */
void vpcie_test_copy(struct vpcie_test *t, size_t src, size_t dst,
		     uint32_t len);
/* BAR2 + the copies that fit the buffer, returns t->errors */
int vpcie_run(struct vpcie_test *t, int irq);

/* Prints "<name> PASS/FAIL", returns the exit code */
int vpcie_summary(const char *name, int errors);

#endif
