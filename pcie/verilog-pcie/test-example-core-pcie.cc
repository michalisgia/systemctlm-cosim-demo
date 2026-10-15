/*
 * End-to-end test of the Verilated verilog-pcie example_core_pcie behind
 * the pcie_int_blk and the pcie_root_port, without QEMU.
 *
 * Copyright (c) 2026
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * The register sequence follows verilog-pcie's
 * example/Alveo/fpga/tb/fpga_core/test_fpga_core.py, plus MSI-X delivery
 * and unaligned DMA transfers (exercising First/Last DW BE).
 */

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <stdio.h>
#include <stdlib.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"

#include "soc/pci/core/pcie-root-port.h"
#include "memory.h"
#include "tlm-extensions/atsattr.h"
#include "iconnect.h"

#include "pcie-int-blk.h"
#include "example-core-pcie-rtl.h"
#include "verilog-pcie-cfg.h"

using namespace sc_core;

#define BUS_NR		1
#define BAR0_BASE	0x80000000ULL
#define BAR2_BASE	0x81000000ULL
#define BAR4_BASE	0x82000000ULL

#define HOST_RAM_SZ	(1024 * 1024)
#define MEM_BASE	0x10000ULL
#define MSIX_ADDR	0xf0000ULL
#define MSIX_DATA	0x12345678

static int nr_errors;

#define TAG_VIRT	0x100	// AT chicken bit, see rtl/at-chicken-bit.patch

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
		nr_errors++;						\
	}								\
} while (0)

//
// Sits on the root port's DMA socket and counts the host memory accesses
// by address type (MSI-X writes excluded): translated requests (AT = 10)
// carry an atsattr extension with ATTR_PHYS_ADDR.
//
SC_MODULE(at_probe)
{
	tlm_utils::simple_target_socket<at_probe> tgt;
	tlm_utils::simple_initiator_socket<at_probe> init;
	unsigned int translated;
	unsigned int untranslated;

	at_probe(sc_module_name name) :
		sc_module(name), tgt("tgt"), init("init"),
		translated(0), untranslated(0)
	{
		tgt.register_b_transport(this, &at_probe::b_transport);
	}

	void reset() { translated = untranslated = 0; }

	void b_transport(tlm::tlm_generic_payload& trans, sc_time& delay)
	{
		atsattr_extension *atsattr;

		trans.get_extension(atsattr);
		if (trans.get_address() != MSIX_ADDR) {
			if (atsattr && atsattr->is_phys_addr()) {
				translated++;
			} else {
				untranslated++;
			}
		}
		init->b_transport(trans, delay);
	}
};

SC_MODULE(host)
{
	tlm_utils::simple_initiator_socket<host> cfg;
	tlm_utils::simple_initiator_socket<host> mem;
	// Backdoor into host memory
	tlm_utils::simple_initiator_socket<host> ram;

	at_probe *probe;

	SC_HAS_PROCESS(host);
	host(sc_module_name name) :
		sc_module(name),
		cfg("cfg"), mem("mem"), ram("ram")
	{
		SC_THREAD(test_thread);
	}

	tlm::tlm_response_status access(
		tlm_utils::simple_initiator_socket<host>& sk,
		bool wr, uint64_t addr, void *data, unsigned int len)
	{
		tlm::tlm_generic_payload gp;
		sc_time delay(SC_ZERO_TIME);

		gp.set_command(wr ? tlm::TLM_WRITE_COMMAND :
				    tlm::TLM_READ_COMMAND);
		gp.set_address(addr);
		gp.set_data_ptr(reinterpret_cast<uint8_t*>(data));
		gp.set_data_length(len);
		gp.set_streaming_width(len);
		gp.set_byte_enable_ptr(NULL);
		gp.set_byte_enable_length(0);
		gp.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
		sk->b_transport(gp, delay);
		return gp.get_response_status();
	}

	void cfg_wr(unsigned int off, uint32_t v, unsigned int len = 4)
	{
		access(cfg, true, BUS_NR << 20 | off, &v, len);
	}

	uint32_t cfg_rd(unsigned int off)
	{
		uint32_t v = 0;

		access(cfg, false, BUS_NR << 20 | off, &v, 4);
		return v;
	}

	void wr32(uint64_t addr, uint32_t v)
	{
		CHECK(access(mem, true, addr, &v, 4) == tlm::TLM_OK_RESPONSE);
	}

	uint32_t rd32(uint64_t addr)
	{
		uint32_t v = 0;

		CHECK(access(mem, false, addr, &v, 4) == tlm::TLM_OK_RESPONSE);
		return v;
	}

	void ram_wr(uint64_t addr, const void *d, unsigned int len)
	{
		access(ram, true, addr, const_cast<void*>(d), len);
	}

	void ram_rd(uint64_t addr, void *d, unsigned int len)
	{
		access(ram, false, addr, d, len);
	}

	void enumerate()
	{
		CHECK(cfg_rd(0x00) == 0x00011234);

		cfg_wr(0x10, BAR0_BASE);
		cfg_wr(0x14, 0);
		cfg_wr(0x18, BAR2_BASE);
		cfg_wr(0x1c, 0);
		cfg_wr(0x20, BAR4_BASE);
		cfg_wr(0x24, 0);
		// Memory space + bus master
		cfg_wr(0x04, 0x0006, 2);
		// Device Control: MPS 256B, MRRS 512B, extended tag
		cfg_wr(0x68, 2 << 12 | 1 << 8 | 1 << 5, 2);
	}

	void setup_msix()
	{
		uint32_t zero = 0;

		// MSI-X table entry 0
		wr32(BAR4_BASE + 0x0, MSIX_ADDR);
		wr32(BAR4_BASE + 0x4, 0);
		wr32(BAR4_BASE + 0x8, MSIX_DATA);
		wr32(BAR4_BASE + 0xc, 0);
		CHECK(rd32(BAR4_BASE + 0x8) == MSIX_DATA);

		// MSI-X enable
		cfg_wr(0x40, 1u << 31);
		ram_wr(MSIX_ADDR, &zero, 4);
	}

	void dma_read(uint64_t host_addr, uint32_t ram_addr, uint32_t len,
			uint32_t tag)
	{
		wr32(BAR0_BASE + 0x100, host_addr);
		wr32(BAR0_BASE + 0x104, host_addr >> 32);
		wr32(BAR0_BASE + 0x108, ram_addr);
		wr32(BAR0_BASE + 0x110, len);
		probe->reset();
		wr32(BAR0_BASE + 0x114, tag);
		wait(2, SC_US);
		CHECK(rd32(BAR0_BASE + 0x118) == (0x80000000 | tag));
		check_at(tag);
	}

	void dma_write(uint64_t host_addr, uint32_t ram_addr, uint32_t len,
			uint32_t tag)
	{
		wr32(BAR0_BASE + 0x200, host_addr);
		wr32(BAR0_BASE + 0x204, host_addr >> 32);
		wr32(BAR0_BASE + 0x208, ram_addr);
		wr32(BAR0_BASE + 0x210, len);
		probe->reset();
		wr32(BAR0_BASE + 0x214, tag);
		wait(2, SC_US);
		CHECK(rd32(BAR0_BASE + 0x218) == (0x80000000 | (tag & 0xffff)));
		check_at(tag);
	}

	// Tag bit 8 set: untranslated requests, clear: translated
	void check_at(uint32_t tag)
	{
		if (tag & TAG_VIRT) {
			CHECK(probe->translated == 0 && probe->untranslated > 0);
		} else {
			CHECK(probe->translated > 0 && probe->untranslated == 0);
		}
	}

	void test_bar2()
	{
		wr32(BAR2_BASE, 0x44332211);
		CHECK(rd32(BAR2_BASE) == 0x44332211);
	}

	void test_dma()
	{
		static uint8_t src[1024], dst[1024];
		uint32_t msix = 0;
		unsigned int i;

		for (i = 0; i < sizeof(src); i++) {
			src[i] = i;
		}
		ram_wr(MEM_BASE, src, sizeof(src));

		// Enable DMA and interrupts
		wr32(BAR0_BASE + 0x000, 1);
		wr32(BAR0_BASE + 0x008, 0x3);

		dma_read(MEM_BASE, 0x100, 0x400, 0xaa);

		ram_rd(MSIX_ADDR, &msix, 4);
		CHECK(msix == MSIX_DATA);

		dma_write(MEM_BASE + 0x1000, 0x100, 0x400, 0x55);
		ram_rd(MEM_BASE + 0x1000, dst, sizeof(dst));
		CHECK(memcmp(src, dst, sizeof(src)) == 0);

		// Immediate write
		wr32(BAR0_BASE + 0x200, MEM_BASE + 0x1000);
		wr32(BAR0_BASE + 0x204, 0);
		wr32(BAR0_BASE + 0x208, 0x44332211);
		wr32(BAR0_BASE + 0x210, 0x4);
		wr32(BAR0_BASE + 0x214, 0x800000aa);
		wait(2, SC_US);
		CHECK(rd32(BAR0_BASE + 0x218) == 0x800000aa);
		ram_rd(MEM_BASE + 0x1000, dst, 4);
		CHECK(dst[0] == 0x11 && dst[3] == 0x44);

		// Same with the AT chicken bit: untranslated requests
		memset(dst, 0, sizeof(dst));
		ram_wr(MEM_BASE + 0x1000, dst, sizeof(dst));
		dma_read(MEM_BASE, 0x400, 0x400, TAG_VIRT | 0xaa);
		dma_write(MEM_BASE + 0x1000, 0x400, 0x400, TAG_VIRT | 0x55);
		ram_rd(MEM_BASE + 0x1000, dst, sizeof(dst));
		CHECK(memcmp(src, dst, sizeof(src)) == 0);
	}

	void test_unaligned_dma()
	{
		static uint8_t buf[256];
		unsigned int i;

		//
		// Unaligned DMA write of 13 bytes, the neighbouring bytes
		// in host memory must not change.
		//
		memset(buf, 0xee, sizeof(buf));
		ram_wr(MEM_BASE + 0x2000, buf, sizeof(buf));

		dma_write(MEM_BASE + 0x2003, 0x100, 13, 0x56);

		ram_rd(MEM_BASE + 0x2000, buf, sizeof(buf));
		CHECK(buf[2] == 0xee);
		for (i = 0; i < 13; i++) {
			CHECK(buf[3 + i] == i);
		}
		CHECK(buf[16] == 0xee);

		//
		// Unaligned DMA read of 0x21 bytes from 0x...05 into the
		// core's RAM, written back aligned.
		//
		memset(buf, 0, sizeof(buf));
		ram_wr(MEM_BASE + 0x3000, buf, sizeof(buf));

		dma_read(MEM_BASE + 0x5, 0x800, 0x21, 0xab);
		dma_write(MEM_BASE + 0x3000, 0x800, 0x21, 0x57);

		ram_rd(MEM_BASE + 0x3000, buf, sizeof(buf));
		for (i = 0; i < 0x21; i++) {
			CHECK(buf[i] == 5 + i);
		}
		CHECK(buf[0x21] == 0);
	}

	void test_thread()
	{
		// Wait for the core to come out of reset
		wait(1, SC_US);

		enumerate();
		setup_msix();
		test_bar2();
		test_dma();
		test_unaligned_dma();

		printf("%s: %d error(s)\n", nr_errors ? "FAILED" : "PASSED",
			nr_errors);
		sc_stop();
	}
};

//
// Tie-offs for the root port's unused ATS sockets
//
SC_MODULE(ats_tieoff)
{
	tlm_utils::simple_target_socket<ats_tieoff> req;
	tlm_utils::simple_initiator_socket<ats_tieoff> inv;

	ats_tieoff(sc_module_name name) :
		sc_module(name), req("req"), inv("inv")
	{
		req.register_b_transport(this, &ats_tieoff::b_transport);
	}

	void b_transport(tlm::tlm_generic_payload& trans, sc_time& delay)
	{
		trans.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
	}
};

int sc_main(int argc, char *argv[])
{
	Verilated::commandArgs(argc, argv);

	pcie_root_port rootport("rootport");
	pcie_int_blk blk("pcie-int-blk", verilog_pcie_example_cfg());
	example_core_pcie_rtl core("core", blk);
	memory host_ram("host-ram", SC_ZERO_TIME, HOST_RAM_SZ);
	iconnect<2, 1> host_bus("host-bus");
	host h("host");
	ats_tieoff ats("ats");
	at_probe probe("at-probe");

	host_bus.memmap(0, HOST_RAM_SZ - 1, ADDRMODE_RELATIVE, -1,
			host_ram.socket);

	h.cfg.bind(rootport.config);
	h.mem.bind(rootport.bar[0]);
	rootport.dma.bind(probe.tgt);
	probe.init.bind(*host_bus.t_sk[0]);
	h.probe = &probe;
	h.ram.bind(*host_bus.t_sk[1]);
	rootport.ats_req.bind(ats.req);
	ats.inv.bind(rootport.ats_inv);

	rootport.init_socket.bind(blk.tgt_socket);
	blk.init_socket.bind(rootport.tgt_socket);

	core.trace_from_args(argc, argv);

	sc_start();

	return nr_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
