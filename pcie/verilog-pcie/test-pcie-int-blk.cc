/*
 * Test of the pcie_int_blk together with the pcie_root_port, without QEMU.
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
 * The host side drives the root port's config and BAR sockets the way
 * remoteport_tlm_pci_ep does, and the root port's DMA socket goes to a
 * memory acting as host memory. A small user logic model on the TLP side
 * implements BAR0 as a RAM and issues requests towards host memory.
 */

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <stdio.h>
#include <stdlib.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"

#include "soc/pci/core/pcie-root-port.h"
#include "memory.h"
#include "iconnect.h"

#include "pcie-int-blk.h"

using namespace sc_core;

#define BUS_NR		1
#define BAR0_BASE	0x80000000ULL
#define HOST_RAM_SZ	(64 * 1024)

static int nr_errors;

#define CHECK(cond) do {						\
	if (!(cond)) {							\
		printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
		nr_errors++;						\
	}								\
} while (0)

static void tlp_put_dw(std::vector<uint8_t>& d, uint32_t v)
{
	d.push_back(v >> 24);
	d.push_back(v >> 16);
	d.push_back(v >> 8);
	d.push_back(v);
}

//
// Stand-in for the user logic: BAR0 is a RAM
//
SC_MODULE(user_logic)
{
	pcie_int_blk& blk;
	uint8_t ram[4096];

	SC_HAS_PROCESS(user_logic);
	user_logic(sc_module_name name, pcie_int_blk& blk) :
		sc_module(name), blk(blk)
	{
		memset(ram, 0, sizeof(ram));
		SC_THREAD(req_thread);
	}

	void req_thread()
	{
		while (true) {
			pcie_int_blk::tlp req = blk.rx_req.get();
			uint32_t dw1 = req.hdr_dw(1);
			uint8_t first_be = dw1 & 0xf;
			uint8_t last_be = (dw1 >> 4) & 0xf;
			unsigned int dw_len = req.dw_len();
			uint32_t addr = req.hdr_dw(req.is_4dw() ? 3 : 2) &
					0xffc;

			CHECK(req.bar_id == 0);

			if (req.has_data()) {
				const uint8_t *d = &req.data[req.hdr_len()];

				for (unsigned int i = 0; i < dw_len * 4; i++) {
					uint8_t be = 0xf;

					if (i < 4) {
						be = first_be;
					} else if (i >= (dw_len - 1) * 4) {
						be = last_be;
					}
					if (be & (1 << (i % 4))) {
						ram[(addr + i) % sizeof(ram)] =
							d[i];
					}
				}
			} else {
				send_cpld(req, addr, dw_len, first_be,
					  dw_len > 1 ? last_be : first_be);
			}
		}
	}

	void send_cpld(const pcie_int_blk::tlp& req, uint32_t addr,
			unsigned int dw_len, uint8_t first_be,
			uint8_t last_be)
	{
		unsigned int first = __builtin_ctz(first_be);
		unsigned int last = (dw_len - 1) * 4 + 31 -
					__builtin_clz(last_be);
		unsigned int byte_count = last - first + 1;
		pcie_int_blk::tlp cpl;

		tlp_put_dw(cpl.data, 0x4a000000 | dw_len);
		tlp_put_dw(cpl.data, (uint32_t) blk.bus_num() << 24 |
					byte_count);
		tlp_put_dw(cpl.data, (req.hdr_dw(1) & 0xffffff00) |
					((addr + first) & 0x7f));
		cpl.data.insert(cpl.data.end(), &ram[addr],
				&ram[addr + dw_len * 4]);
		blk.tx_cpl.push(cpl);
	}
};

//
// The host: drives config / BAR accesses into the root port
//
SC_MODULE(host)
{
	tlm_utils::simple_initiator_socket<host> cfg;
	tlm_utils::simple_initiator_socket<host> mem;
	// Backdoor into host memory
	tlm_utils::simple_initiator_socket<host> ram;

	pcie_int_blk& blk;
	user_logic& ul;

	SC_HAS_PROCESS(host);
	host(sc_module_name name, pcie_int_blk& blk, user_logic& ul) :
		sc_module(name),
		cfg("cfg"), mem("mem"), ram("ram"),
		blk(blk), ul(ul)
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

	uint32_t cfg_rd32(unsigned int fn, unsigned int off)
	{
		uint32_t v = 0;

		access(cfg, false, BUS_NR << 20 | fn << 12 | off, &v, 4);
		return v;
	}

	void cfg_wr(unsigned int off, uint32_t v, unsigned int len = 4)
	{
		access(cfg, true, BUS_NR << 20 | off, &v, len);
	}

	void test_config()
	{
		uint32_t v;

		CHECK(cfg_rd32(0, 0x00) == 0x00011234);
		CHECK(cfg_rd32(1, 0x00) == 0xffffffff);

		// BAR0: 16 MiB, 64 bit, prefetchable
		cfg_wr(0x10, 0xffffffff);
		cfg_wr(0x14, 0xffffffff);
		CHECK(cfg_rd32(0, 0x10) == 0xff00000c);
		CHECK(cfg_rd32(0, 0x14) == 0xffffffff);
		cfg_wr(0x10, BAR0_BASE);
		cfg_wr(0x14, 0);
		CHECK(cfg_rd32(0, 0x10) == (BAR0_BASE | 0xc));

		// BAR4: 64 KiB
		cfg_wr(0x20, 0xffffffff);
		CHECK(cfg_rd32(0, 0x20) == 0xffff000c);
		cfg_wr(0x20, 0x90000000);
		cfg_wr(0x24, 0);

		// Unimplemented BAR2 slot (BAR2 not configured here)
		cfg_wr(0x18, 0xffffffff);
		CHECK(cfg_rd32(0, 0x18) == 0);

		CHECK(blk.bus_num() == BUS_NR);

		// Memory space + bus master, 16 bit write
		cfg_wr(0x04, 0x0006, 2);
		CHECK((cfg_rd32(0, 0x04) & 0xffff) == 0x0006);
		CHECK(blk.bus_master_enable());

		// Capabilities: MSI-X at 0x40 -> PCIe at 0x60
		CHECK((cfg_rd32(0, 0x34) & 0xff) == 0x40);
		v = cfg_rd32(0, 0x40);
		CHECK((v & 0xffff) == 0x6011);
		CHECK((v >> 16) == 31);
		CHECK(cfg_rd32(0, 0x44) == 0x00000004);
		CHECK(cfg_rd32(0, 0x48) == 0x00008004);
		CHECK(!blk.msix_enable());
		// Byte write to the upper byte of Message Control
		v = 0x80 << 24;
		access(cfg, true, BUS_NR << 20 | 0x43, (uint8_t *) &v + 3, 1);
		CHECK(blk.msix_enable());
		CHECK(!blk.msix_mask());

		// Device Control: MPS 256B, MRRS 1024B, ext tag
		CHECK((cfg_rd32(0, 0x60) & 0xff) == 0x10);
		cfg_wr(0x68, 3 << 12 | 1 << 8 | 1 << 5, 2);
		CHECK(blk.max_payload_size() == 1);
		CHECK(blk.max_read_request_size() == 3);
		CHECK(blk.ext_tag_enable());
		cfg_wr(0x70, 1 << 3, 2);
		CHECK(blk.rcb_128b());

		// End of the extended capability list
		CHECK(cfg_rd32(0, 0x100) == 0);
	}

	void test_mmio()
	{
		uint64_t v64;
		uint32_t v;
		uint16_t v16;

		v = 0x11223344;
		access(mem, true, BAR0_BASE + 0x10, &v, 4);
		v = 0;
		access(mem, false, BAR0_BASE + 0x10, &v, 4);
		CHECK(v == 0x11223344);

		// Sub DW write, only bytes 0x12-0x13 may change
		v16 = 0xbeef;
		access(mem, true, BAR0_BASE + 0x12, &v16, 2);
		access(mem, false, BAR0_BASE + 0x10, &v, 4);
		CHECK(v == 0xbeef3344);

		// Unaligned write spanning 2 DWs (0x16 - 0x19)
		v = 0xa1b2c3d4;
		access(mem, true, BAR0_BASE + 0x16, &v, 4);
		CHECK(ul.ram[0x15] == 0 && ul.ram[0x16] == 0xd4 &&
		      ul.ram[0x19] == 0xa1 && ul.ram[0x1a] == 0);

		// Multi DW read
		v64 = 0;
		access(mem, false, BAR0_BASE + 0x10, &v64, 8);
		CHECK(v64 == 0xc3d40000beef3344ULL);

		// Unaligned read spanning 2 DWs
		v = 0;
		access(mem, false, BAR0_BASE + 0x16, &v, 4);
		CHECK(v == 0xa1b2c3d4);

		// Read that misses all BARs gets an UR
		CHECK(access(mem, false, 0xa0000000, &v, 4) !=
			tlm::TLM_OK_RESPONSE);
	}

	void test_dma()
	{
		uint8_t buf[16];
		pcie_int_blk::tlp t;
		pcie_int_blk::seq_done s;
		unsigned int i;

		memset(buf, 0xaa, sizeof(buf));
		host_ram_access(true, 0x1000, buf, sizeof(buf));

		//
		// MWr of 6 bytes at 0x1003: 3 DWs from 0x1000,
		// first BE 0x8, last BE 0x1
		//
		tlp_put_dw(t.data, 0x40000003);
		tlp_put_dw(t.data, BUS_NR << 24 | 0x11 << 8 | 0x18);
		tlp_put_dw(t.data, 0x1000);
		for (i = 0; i < 12; i++) {
			t.data.push_back(i);
		}
		t.seq = 5;
		t.has_seq = true;
		blk.tx_wr_req.push(t);

		s = blk.tx_seq_num.get();
		CHECK(!s.is_rd && s.seq == 5);
		wait(1, SC_US);

		host_ram_access(false, 0x1000, buf, sizeof(buf));
		CHECK(buf[2] == 0xaa);
		for (i = 3; i < 9; i++) {
			CHECK(buf[i] == i);
		}
		CHECK(buf[9] == 0xaa);

		//
		// MRd of 0x1001 - 0x1005: 2 DWs, first BE 0xe, last BE 0x3
		//
		t = pcie_int_blk::tlp();
		tlp_put_dw(t.data, 0x00000002);
		tlp_put_dw(t.data, BUS_NR << 24 | 0x22 << 8 | 0x3e);
		tlp_put_dw(t.data, 0x1000);
		t.seq = 6;
		t.has_seq = true;
		blk.tx_rd_req.push(t);

		s = blk.tx_seq_num.get();
		CHECK(s.is_rd && s.seq == 6);

		t = blk.rx_cpl.get();
		CHECK(t.type() == PCI::TLP::Type_Cpl && t.has_data());
		CHECK(t.dw_len() == 2);
		CHECK((t.hdr_dw(1) & 0xfff) == 5);		// byte count
		CHECK((t.hdr_dw(2) & 0x7f) == 0x01);		// lower addr
		CHECK((t.hdr_dw(2) >> 8) == (BUS_NR << 16 | 0x22)); // req/tag
		CHECK(t.data.size() == 12 + 8);
		CHECK(t.data[12] == 0xaa && t.data[15] == 3 &&
		      t.data[16] == 4 && t.data[19] == 7);

		//
		// MSI-X write, no sequence number
		//
		t = pcie_int_blk::tlp();
		tlp_put_dw(t.data, 0x40000001);
		tlp_put_dw(t.data, BUS_NR << 24 | 0x0f);
		tlp_put_dw(t.data, 0x2000);
		tlp_put_dw(t.data, 0x78563412);
		blk.tx_wr_req.push(t);
		wait(1, SC_US);
		CHECK(blk.tx_seq_num.empty());
		host_ram_access(false, 0x2000, buf, 4);
		CHECK(buf[0] == 0x78 && buf[3] == 0x12);
	}

	void host_ram_access(bool wr, uint64_t addr, uint8_t *d,
				unsigned int len)
	{
		tlm::tlm_generic_payload gp;
		sc_time delay(SC_ZERO_TIME);

		gp.set_command(wr ? tlm::TLM_WRITE_COMMAND :
				    tlm::TLM_READ_COMMAND);
		gp.set_address(addr);
		gp.set_data_ptr(d);
		gp.set_data_length(len);
		gp.set_streaming_width(len);
		gp.set_byte_enable_ptr(NULL);
		gp.set_byte_enable_length(0);
		ram->b_transport(gp, delay);
	}

	void test_thread()
	{
		wait(10, SC_NS);

		test_config();
		test_mmio();
		test_dma();

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

static pcie_int_blk::config get_config()
{
	pcie_int_blk::config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.vendor_id = 0x1234;
	cfg.device_id = 0x0001;
	cfg.class_code = 0x058000;
	cfg.bars[0] = { 24, true, true };
	cfg.bars[4] = { 16, true, true };
	cfg.msix_table_size = 32;
	cfg.msix_table_bir = 4;
	cfg.msix_table_offset = 0x0;
	cfg.msix_pba_bir = 4;
	cfg.msix_pba_offset = 0x8000;
	cfg.max_payload_supported = 1;

	return cfg;
}

int sc_main(int argc, char *argv[])
{
	pcie_root_port rootport("rootport");
	pcie_int_blk blk("pcie-int-blk", get_config());
	user_logic ul("user-logic", blk);
	memory host_ram("host-ram", SC_ZERO_TIME, HOST_RAM_SZ);
	iconnect<2, 1> host_bus("host-bus");
	host h("host", blk, ul);
	ats_tieoff ats("ats");

	host_bus.memmap(0, HOST_RAM_SZ - 1, ADDRMODE_RELATIVE, -1,
			host_ram.socket);

	h.cfg.bind(rootport.config);
	h.mem.bind(rootport.bar[0]);
	rootport.dma.bind(*host_bus.t_sk[0]);
	h.ram.bind(*host_bus.t_sk[1]);
	rootport.ats_req.bind(ats.req);
	ats.inv.bind(rootport.ats_inv);

	rootport.init_socket.bind(blk.tgt_socket);
	blk.init_socket.bind(rootport.tgt_socket);

	sc_start();

	return nr_errors ? EXIT_FAILURE : EXIT_SUCCESS;
}
