/*
 * Co-simulation of a QEMU x86 host with the verilog-pcie example design
 * (example_core_pcie, Verilated) as a PCIe end-point.
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
 *  QEMU ═ remote-port ═ remoteport_tlm_pci_ep ─ pcie_root_port
 *       ═ TLPs ═ pcie_int_blk ═ example_core_pcie_rtl (Verilated RTL)
 */

#define SC_INCLUDE_DYNAMIC_PROCESSES

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "systemc.h"
#include "tlm_utils/simple_initiator_socket.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/tlm_quantumkeeper.h"

#include "soc/pci/core/pcie-root-port.h"

using namespace sc_core;
using namespace sc_dt;
using namespace std;

#include "remote-port-tlm.h"
#include "remote-port-tlm-pci-ep.h"

#include "pcie-int-blk.h"
#include "example-core-pcie-rtl.h"
#include "verilog-pcie-cfg.h"

//
// Host / PCIe RC, a QEMU PCIe root port connected over Remote-port
// (see pcie/versal/cpm-qdma-demo.cc).
//
SC_MODULE(pcie_host)
{
private:
	remoteport_tlm_pci_ep rp_pci_ep;

public:
	pcie_root_port rootport;
	sc_in<bool> rst;

	pcie_host(sc_module_name name, const char *sk_descr) :
		sc_module(name),
		rp_pci_ep("rp-pci-ep", 0, 1, 0, sk_descr),
		rootport("rootport"),
		rst("rst")
	{
		rp_pci_ep.rst(rst);
		rp_pci_ep.bind(rootport);
	}
};

SC_MODULE(Top)
{
public:
	SC_HAS_PROCESS(Top);

	pcie_host host;
	pcie_int_blk pcie_blk;
	example_core_pcie_rtl core;

	sc_signal<bool> rst;

	Top(sc_module_name name, const char *sk_descr, sc_time quantum) :
		sc_module(name),
		host("host", sk_descr),
		pcie_blk("pcie-int-blk", verilog_pcie_example_cfg()),
		core("example-core-pcie", pcie_blk),
		rst("rst")
	{
		m_qk.set_global_quantum(quantum);

		// TLP link (host.rootport <-> pcie-int-blk)
		host.rootport.init_socket.bind(pcie_blk.tgt_socket);
		pcie_blk.init_socket.bind(host.rootport.tgt_socket);

		host.rst(rst);

		SC_THREAD(pull_reset);
	}

	void pull_reset(void) {
		/* Pull the reset signal.  */
		rst.write(true);
		wait(1, SC_US);
		rst.write(false);
	}

private:
	tlm_utils::tlm_quantumkeeper m_qk;
};

void usage(void)
{
	cout << "tlm socket-path sync-quantum-ns "
		"[+trace[=file.fst]] [+trace-start=<us>]" << endl;
}

int sc_main(int argc, char* argv[])
{
	Top *top;
	uint64_t sync_quantum;

	Verilated::commandArgs(argc, argv);

	if (argc < 3) {
		sync_quantum = 10000;
	} else {
		sync_quantum = strtoull(argv[2], NULL, 10);
	}

	sc_set_time_resolution(1, SC_PS);

	top = new Top("top", argv[1], sc_time((double) sync_quantum, SC_NS));

	if (argc < 3) {
		sc_start(1, SC_PS);
		sc_stop();
		usage();
		exit(EXIT_FAILURE);
	}

	top->core.trace_from_args(argc, argv);

	sc_start();
	return 0;
}
