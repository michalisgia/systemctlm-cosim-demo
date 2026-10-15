/*
 * pcie_int_blk configuration matching the verilog-pcie Alveo example design.
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
 */
#ifndef VERILOG_PCIE_CFG_H__
#define VERILOG_PCIE_CFG_H__

#include <string.h>

#include "pcie-int-blk.h"

//
// Same IDs, BARs and MSI-X layout as the Alveo example's PCIe IP
// (example/Alveo/fpga/ip/pcie4_uscale_plus_0.tcl), the example driver
// binds to 1234:0001.
//
static inline pcie_int_blk::config verilog_pcie_example_cfg()
{
	pcie_int_blk::config cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.vendor_id = 0x1234;
	cfg.device_id = 0x0001;
	cfg.class_code = 0x058000;	// Memory controller
	cfg.subsys_vendor_id = 0x10ee;
	cfg.subsys_id = 0x90c8;

	// BAR0: control registers (BAR0_APERTURE = 24)
	cfg.bars[0] = { 24, true, true };
	// BAR2: AXI RAM (BAR2_APERTURE = 24)
	cfg.bars[2] = { 24, true, true };
	// BAR4: MSI-X table / PBA (BAR4_APERTURE = 16)
	cfg.bars[4] = { 16, true, true };

	cfg.msix_table_size = 32;
	cfg.msix_table_bir = 4;
	cfg.msix_table_offset = 0x0;
	cfg.msix_pba_bir = 4;
	cfg.msix_pba_offset = 0x8000;

	// 512 bytes
	cfg.max_payload_supported = 2;

	return cfg;
}

#endif
