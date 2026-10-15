/*
 * Verilated verilog-pcie example_core_pcie connected to a pcie_int_blk.
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
 * The core is Verilated in C++ (not SystemC) mode and clocked from a
 * SystemC thread. Every cycle the inputs are driven with the clock low,
 * the valid / ready handshakes are sampled after the combinational logic
 * has settled and then the rising edge is evaluated.
 *
 * Expects the default example_core_pcie parameters: TLP_DATA_WIDTH 256,
 * TLP_HDR_WIDTH 128, TLP_SEG_COUNT 1, TX_SEQ_NUM_COUNT 1.
 *
 * When built with VM_TRACE (Makefile VPCIE_TRACE=1) the whole RTL
 * hierarchy can be dumped to an FST file, see trace() and
 * trace_from_args(). The file is closed on exit (including the exit when
 * QEMU disconnects) and on SIGINT / SIGTERM.
 */
#ifndef EXAMPLE_CORE_PCIE_RTL_H__
#define EXAMPLE_CORE_PCIE_RTL_H__

#include <deque>
#include <string>
#include <vector>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "systemc.h"

#include "verilated.h"
#include "Vexample_core_pcie.h"
#if VM_TRACE
#include "verilated_fst_c.h"
#endif

#include "pcie-int-blk.h"

class example_core_pcie_rtl : public sc_core::sc_module
{
public:
	SC_HAS_PROCESS(example_core_pcie_rtl);

	example_core_pcie_rtl(sc_core::sc_module_name name, pcie_int_blk& blk,
			sc_core::sc_time clk_period =
				sc_core::sc_time(4, sc_core::SC_NS)) :
		sc_module(name),
		m_blk(blk),
		m_clk_period(clk_period)
	{
#if VM_TRACE
		m_ctx.traceEverOn(true);
#endif
		m_top = new Vexample_core_pcie(&m_ctx, "core");

		SC_THREAD(clk_thread);
	}

	~example_core_pcie_rtl()
	{
		std::vector<example_core_pcie_rtl *>& v = traced();

		for (size_t i = 0; i < v.size(); i++) {
			if (v[i] == this) {
				v.erase(v.begin() + i);
				break;
			}
		}
		trace_close();
		m_top->final();
		delete m_top;
	}

	//
	// Dumps the whole RTL hierarchy to the FST file 'file' from simulation
	// time 'start' on.
	//
	void trace(const char *file,
		   sc_core::sc_time start = sc_core::SC_ZERO_TIME)
	{
#if VM_TRACE
		trace_close();
		m_tfp = new VerilatedFstC;
		m_top->trace(m_tfp, 99);
		m_tfp->open(file);
		m_trace_start = start;
		m_trace_file = file;
		register_exit_handlers(this);
		std::cout << name() << ": tracing to " << file;
		if (start != sc_core::SC_ZERO_TIME) {
			std::cout << " from " << start;
		}
		std::cout << std::endl;
#else
		SC_REPORT_WARNING(name(), "built without tracing "
				  "(VPCIE_TRACE=0), +trace ignored");
#endif
	}

	//
	// +trace[=file.fst]   trace to file.fst, by default to <binary>.fst
	//                     next to the binary (pcie/verilog-pcie/)
	// +trace-start=<us>   start dumping at <us> microseconds
	//
	void trace_from_args(int argc, char *argv[])
	{
		std::string file;
		sc_core::sc_time start = sc_core::SC_ZERO_TIME;

		for (int i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "+trace")) {
				file = default_trace_file(argv[0]);
			} else if (!strncmp(argv[i], "+trace=", 7)) {
				file = argv[i] + 7;
			} else if (!strncmp(argv[i], "+trace-start=", 13)) {
				start = sc_core::sc_time(
					strtod(argv[i] + 13, NULL),
					sc_core::SC_US);
			}
		}
		if (!file.empty()) {
			trace(file.c_str(), start);
		}
	}

	static std::string default_trace_file(const char *argv0)
	{
		char buf[PATH_MAX];
		ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
		std::string exe = (n > 0) ? std::string(buf, n) :
					    std::string(argv0);

		return exe + ".fst";
	}

	void trace_close()
	{
#if VM_TRACE
		if (m_tfp) {
			m_tfp->close();
			delete m_tfp;
			m_tfp = NULL;
			std::cout << name() << ": closed "
				  << m_trace_file << std::endl;
		}
#endif
	}

private:
	enum {
		DATA_DW = 8,		// TLP_DATA_WIDTH / 32
		HDR_DW = 4,		// TLP_HDR_WIDTH / 32
		RST_CYCLES = 16,
	};

	typedef pcie_int_blk::tlp tlp;

	//
	// TLP being streamed into the core
	//
	struct rx_stream {
		bool active;
		tlp t;
		unsigned int pos;	// payload byte offset of the beat

		rx_stream() : active(false), pos(0) {}
	};

	//
	// TLP being collected from the core
	//
	struct tx_stream {
		bool active;
		tlp t;

		tx_stream() : active(false) {}
	};

	pcie_int_blk& m_blk;
	sc_core::sc_time m_clk_period;

	VerilatedContext m_ctx;
	Vexample_core_pcie *m_top;

	rx_stream m_rx_req;
	rx_stream m_rx_cpl;

	tx_stream m_tx_cpl;
	tx_stream m_tx_rd_req;
	tx_stream m_tx_wr_req;
	tx_stream m_tx_msix;

	std::deque<uint32_t> m_rd_seq;
	std::deque<uint32_t> m_wr_seq;

#if VM_TRACE
	VerilatedFstC *m_tfp = NULL;
	sc_core::sc_time m_trace_start;
	std::string m_trace_file;
#endif

	//
	// Open traces are closed at exit(), the remote-port socket code
	// exits when QEMU disconnects. SIGINT / SIGTERM request an exit from
	// the clock thread (between two dumps), a second signal exits
	// immediately.
	//
	static std::vector<example_core_pcie_rtl *>& traced()
	{
		static std::vector<example_core_pcie_rtl *> v;
		return v;
	}

	static volatile sig_atomic_t& stop_req()
	{
		static volatile sig_atomic_t s = 0;
		return s;
	}

	static void close_all_traces()
	{
		for (example_core_pcie_rtl *r : traced()) {
			r->trace_close();
		}
		traced().clear();
	}

	static void on_signal(int sig)
	{
		if (stop_req()) {
			_exit(EXIT_FAILURE);
		}
		stop_req() = 1;
	}

	static void register_exit_handlers(example_core_pcie_rtl *r)
	{
		static bool done = false;

		traced().push_back(r);
		if (!done) {
			atexit(close_all_traces);
			signal(SIGINT, on_signal);
			signal(SIGTERM, on_signal);
			done = true;
		}
	}

	void trace_dump(sc_core::sc_time t)
	{
#if VM_TRACE
		if (m_tfp && t >= m_trace_start) {
			// The model's time precision is 1 ps
			m_tfp->dump((uint64_t) (t.to_seconds() * 1e12 + 0.5));
		}
#endif
	}

	static uint32_t le32(const uint8_t *d)
	{
		return d[0] | d[1] << 8 | d[2] << 16 | (uint32_t) d[3] << 24;
	}

	static void put_le32(std::vector<uint8_t>& d, uint32_t v)
	{
		d.push_back(v);
		d.push_back(v >> 8);
		d.push_back(v >> 16);
		d.push_back(v >> 24);
	}

	//
	// tlp_hdr[127:96] holds DW0, a 3 DW header leaves [31:0] as zero
	//
	static void set_hdr(VlWide<HDR_DW>& hdr, const tlp& t)
	{
		unsigned int nr_dw = t.hdr_len() / 4;

		for (unsigned int i = 0; i < HDR_DW; i++) {
			hdr[HDR_DW - 1 - i] = (i < nr_dw) ? t.hdr_dw(i) : 0;
		}
	}

	static void get_hdr(tlp& t, const VlWide<HDR_DW>& hdr)
	{
		unsigned int nr_dw;

		t.data.clear();
		for (unsigned int i = 0; i < HDR_DW; i++) {
			uint32_t v = hdr[HDR_DW - 1 - i];

			t.data.push_back(v >> 24);
			t.data.push_back(v >> 16);
			t.data.push_back(v >> 8);
			t.data.push_back(v);
		}

		nr_dw = t.hdr_len() / 4;
		t.data.resize(nr_dw * 4);
	}

	static unsigned int payload_len(const tlp& t)
	{
		return t.has_data() ? t.dw_len() * 4 : 0;
	}

	//
	// Drives one beat of the TLP at the head of 'q' (if any)
	//
	void drive_rx(rx_stream& s, pcie_int_blk::tlp_queue<tlp>& q,
			VlWide<DATA_DW>& data, CData& strb,
			VlWide<HDR_DW>& hdr, CData& valid, CData& sop,
			CData& eop)
	{
		unsigned int len;

		if (!s.active && !q.empty()) {
			s.t = q.pop();
			s.active = true;
			s.pos = 0;
		}

		if (!s.active) {
			valid = 0;
			return;
		}

		len = payload_len(s.t);

		set_hdr(hdr, s.t);
		strb = 0;
		for (unsigned int i = 0; i < DATA_DW; i++) {
			unsigned int off = s.pos + i * 4;

			if (off < len) {
				data[i] = le32(&s.t.data[s.t.hdr_len() + off]);
				strb |= 1 << i;
			} else {
				data[i] = 0;
			}
		}
		valid = 1;
		sop = s.pos == 0;
		eop = s.pos + DATA_DW * 4 >= len;
	}

	static void rx_handshake(rx_stream& s, CData valid, CData ready,
				 CData eop)
	{
		if (valid && ready) {
			s.pos += DATA_DW * 4;
			if (eop) {
				s.active = false;
			}
		}
	}

	//
	// Collects one beat, returns true when the TLP is complete
	//
	static bool tx_beat(tx_stream& s, const VlWide<HDR_DW>& hdr,
			    const uint32_t *data, unsigned int nr_dw,
			    unsigned int strb, CData sop, CData eop)
	{
		if (sop) {
			get_hdr(s.t, hdr);
			s.active = true;
		}
		if (!s.active) {
			SC_REPORT_ERROR("example_core_pcie_rtl",
					"TLP beat without sop");
			return false;
		}

		for (unsigned int i = 0; i < nr_dw; i++) {
			if ((strb & (1 << i)) &&
			    s.t.data.size() < s.t.hdr_len() +
						payload_len(s.t)) {
				put_le32(s.t.data, data[i]);
			}
		}

		if (eop) {
			s.active = false;
			return true;
		}
		return false;
	}

	void drive_inputs()
	{
		Vexample_core_pcie *t = m_top;

		drive_rx(m_rx_req, m_blk.rx_req, t->rx_req_tlp_data,
			 t->rx_req_tlp_strb, t->rx_req_tlp_hdr,
			 t->rx_req_tlp_valid, t->rx_req_tlp_sop,
			 t->rx_req_tlp_eop);
		t->rx_req_tlp_bar_id = m_rx_req.t.bar_id;
		t->rx_req_tlp_func_num = 0;

		drive_rx(m_rx_cpl, m_blk.rx_cpl, t->rx_cpl_tlp_data,
			 t->rx_cpl_tlp_strb, t->rx_cpl_tlp_hdr,
			 t->rx_cpl_tlp_valid, t->rx_cpl_tlp_sop,
			 t->rx_cpl_tlp_eop);
		t->rx_cpl_tlp_error = 0;

		// The block queues everything, always ready
		t->tx_cpl_tlp_ready = 1;
		t->tx_rd_req_tlp_ready = 1;
		t->tx_wr_req_tlp_ready = 1;
		t->tx_msix_wr_req_tlp_ready = 1;

		//
		// TX sequence numbers, one read and one write per cycle
		//
		while (!m_blk.tx_seq_num.empty()) {
			pcie_int_blk::seq_done s = m_blk.tx_seq_num.pop();

			if (s.is_rd) {
				m_rd_seq.push_back(s.seq);
			} else {
				m_wr_seq.push_back(s.seq);
			}
		}

		t->s_axis_rd_req_tx_seq_num_valid = !m_rd_seq.empty();
		if (!m_rd_seq.empty()) {
			t->s_axis_rd_req_tx_seq_num = m_rd_seq.front();
			m_rd_seq.pop_front();
		}
		t->s_axis_wr_req_tx_seq_num_valid = !m_wr_seq.empty();
		if (!m_wr_seq.empty()) {
			t->s_axis_wr_req_tx_seq_num = m_wr_seq.front();
			m_wr_seq.pop_front();
		}

		//
		// Configuration
		//
		t->bus_num = m_blk.bus_num();
		t->ext_tag_enable = m_blk.ext_tag_enable();
		t->rcb_128b = m_blk.rcb_128b();
		t->max_read_request_size = m_blk.max_read_request_size();
		t->max_payload_size = m_blk.max_payload_size();
		t->msix_enable = m_blk.msix_enable();
		t->msix_mask = m_blk.msix_mask();
	}

	void sample_handshakes()
	{
		Vexample_core_pcie *t = m_top;

		rx_handshake(m_rx_req, t->rx_req_tlp_valid,
			     t->rx_req_tlp_ready, t->rx_req_tlp_eop);
		rx_handshake(m_rx_cpl, t->rx_cpl_tlp_valid,
			     t->rx_cpl_tlp_ready, t->rx_cpl_tlp_eop);

		if (t->tx_cpl_tlp_valid &&
		    tx_beat(m_tx_cpl, t->tx_cpl_tlp_hdr,
			    t->tx_cpl_tlp_data.data(), DATA_DW,
			    t->tx_cpl_tlp_strb, t->tx_cpl_tlp_sop,
			    t->tx_cpl_tlp_eop)) {
			m_blk.tx_cpl.push(m_tx_cpl.t);
		}

		if (t->tx_rd_req_tlp_valid) {
			if (t->tx_rd_req_tlp_sop) {
				m_tx_rd_req.t.seq = t->tx_rd_req_tlp_seq;
				m_tx_rd_req.t.has_seq = true;
			}
			if (tx_beat(m_tx_rd_req, t->tx_rd_req_tlp_hdr,
				    NULL, 0, 0, t->tx_rd_req_tlp_sop,
				    t->tx_rd_req_tlp_eop)) {
				m_blk.tx_rd_req.push(m_tx_rd_req.t);
			}
		}

		if (t->tx_wr_req_tlp_valid) {
			if (t->tx_wr_req_tlp_sop) {
				m_tx_wr_req.t.seq = t->tx_wr_req_tlp_seq;
				m_tx_wr_req.t.has_seq = true;
			}
			if (tx_beat(m_tx_wr_req, t->tx_wr_req_tlp_hdr,
				    t->tx_wr_req_tlp_data.data(), DATA_DW,
				    t->tx_wr_req_tlp_strb,
				    t->tx_wr_req_tlp_sop,
				    t->tx_wr_req_tlp_eop)) {
				m_blk.tx_wr_req.push(m_tx_wr_req.t);
			}
		}

		if (t->tx_msix_wr_req_tlp_valid) {
			uint32_t d = t->tx_msix_wr_req_tlp_data;

			m_tx_msix.t.has_seq = false;
			if (tx_beat(m_tx_msix, t->tx_msix_wr_req_tlp_hdr,
				    &d, 1, t->tx_msix_wr_req_tlp_strb,
				    t->tx_msix_wr_req_tlp_sop,
				    t->tx_msix_wr_req_tlp_eop)) {
				m_blk.tx_wr_req.push(m_tx_msix.t);
			}
		}

		if (t->status_error_uncor) {
			SC_REPORT_WARNING(name(), "uncorrectable error");
		}
		if (t->status_error_cor) {
			SC_REPORT_WARNING(name(), "correctable error");
		}
	}

	void clk_thread()
	{
		unsigned int cycle = 0;

		while (true) {
			if (stop_req()) {
				exit(EXIT_FAILURE);
			}

			m_top->clk = 0;
			m_top->rst = cycle < RST_CYCLES;
			if (!m_top->rst) {
				drive_inputs();
			}
			m_top->eval();
			trace_dump(sc_core::sc_time_stamp());

			if (!m_top->rst) {
				sample_handshakes();
			}

			m_top->clk = 1;
			m_top->eval();
			trace_dump(sc_core::sc_time_stamp() + m_clk_period / 2);

			if (cycle < RST_CYCLES) {
				cycle++;
			}
			wait(m_clk_period);
		}
	}
};

#endif
