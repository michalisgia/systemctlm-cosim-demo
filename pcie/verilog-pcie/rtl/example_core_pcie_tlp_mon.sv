// TLP monitors for verilog-pcie's example_core_pcie (waveform viewing only).
//
// Bound into example_core_pcie, the core itself is not modified. Each TLP
// interface gets a monitor instance (tlp_mon.rx_req, tlp_mon.tx_cpl, ...)
// with:
//
//   hdr    the decoded header (pcie_tlp_pkg::tlp_req_t / tlp_cpl_t). It
//          follows the header while a TLP starts (valid && sop) and holds
//          it for the remaining beats and until the next TLP.
//   start  a TLP is accepted (valid && sop && ready)
//   data   the payload beat as DWs, data[0] is the first DW of the beat
//   count  number of TLPs accepted so far
//
// Only the default single segment configuration (TLP_SEG_COUNT 1) is
// handled.

module pcie_tlp_req_mon #(
    parameter DATA_DW = 8
) (
    input  wire                  clk,
    input  wire                  rst,
    input  wire [127:0]          tlp_hdr,
    input  wire [DATA_DW*32-1:0] tlp_data,
    input  wire                  tlp_valid,
    input  wire                  tlp_sop,
    input  wire                  tlp_ready
);
    import pcie_tlp_pkg::*;

    tlp_req_t    hdr;
    tlp_req_t    hdr_q;
    logic        start;
    logic [31:0] data [DATA_DW];
    logic [31:0] count;

    assign start = tlp_valid && tlp_sop && tlp_ready;
    assign hdr = (tlp_valid && tlp_sop) ? decode_req(tlp_hdr) : hdr_q;

    for (genvar i = 0; i < DATA_DW; i++) begin : g_data
        assign data[i] = tlp_data[i*32 +: 32];
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            hdr_q <= '0;
            count <= 0;
        end else if (start) begin
            hdr_q <= hdr;
            count <= count + 1;
        end
    end
endmodule

module pcie_tlp_cpl_mon #(
    parameter DATA_DW = 8
) (
    input  wire                  clk,
    input  wire                  rst,
    input  wire [127:0]          tlp_hdr,
    input  wire [DATA_DW*32-1:0] tlp_data,
    input  wire                  tlp_valid,
    input  wire                  tlp_sop,
    input  wire                  tlp_ready
);
    import pcie_tlp_pkg::*;

    tlp_cpl_t    hdr;
    tlp_cpl_t    hdr_q;
    logic        start;
    logic [31:0] data [DATA_DW];
    logic [31:0] count;

    assign start = tlp_valid && tlp_sop && tlp_ready;
    assign hdr = (tlp_valid && tlp_sop) ? decode_cpl(tlp_hdr) : hdr_q;

    for (genvar i = 0; i < DATA_DW; i++) begin : g_data
        assign data[i] = tlp_data[i*32 +: 32];
    end

    always_ff @(posedge clk) begin
        if (rst) begin
            hdr_q <= '0;
            count <= 0;
        end else if (start) begin
            hdr_q <= hdr;
            count <= count + 1;
        end
    end
endmodule

module example_core_pcie_tlp_mon (
    input wire         clk,
    input wire         rst,

    input wire [127:0] rx_req_tlp_hdr,
    input wire [255:0] rx_req_tlp_data,
    input wire         rx_req_tlp_valid,
    input wire         rx_req_tlp_sop,
    input wire         rx_req_tlp_ready,

    input wire [127:0] tx_cpl_tlp_hdr,
    input wire [255:0] tx_cpl_tlp_data,
    input wire         tx_cpl_tlp_valid,
    input wire         tx_cpl_tlp_sop,
    input wire         tx_cpl_tlp_ready,

    input wire [127:0] tx_rd_req_tlp_hdr,
    input wire         tx_rd_req_tlp_valid,
    input wire         tx_rd_req_tlp_sop,
    input wire         tx_rd_req_tlp_ready,

    input wire [127:0] rx_cpl_tlp_hdr,
    input wire [255:0] rx_cpl_tlp_data,
    input wire         rx_cpl_tlp_valid,
    input wire         rx_cpl_tlp_sop,
    input wire         rx_cpl_tlp_ready,

    input wire [127:0] tx_wr_req_tlp_hdr,
    input wire [255:0] tx_wr_req_tlp_data,
    input wire         tx_wr_req_tlp_valid,
    input wire         tx_wr_req_tlp_sop,
    input wire         tx_wr_req_tlp_ready,

    input wire [127:0] tx_msix_wr_req_tlp_hdr,
    input wire [31:0]  tx_msix_wr_req_tlp_data,
    input wire         tx_msix_wr_req_tlp_valid,
    input wire         tx_msix_wr_req_tlp_sop,
    input wire         tx_msix_wr_req_tlp_ready
);
    // Host MMIO requests to the BARs
    pcie_tlp_req_mon rx_req (
        .clk(clk), .rst(rst),
        .tlp_hdr(rx_req_tlp_hdr), .tlp_data(rx_req_tlp_data),
        .tlp_valid(rx_req_tlp_valid), .tlp_sop(rx_req_tlp_sop),
        .tlp_ready(rx_req_tlp_ready));

    // Completions for host MMIO reads
    pcie_tlp_cpl_mon tx_cpl (
        .clk(clk), .rst(rst),
        .tlp_hdr(tx_cpl_tlp_hdr), .tlp_data(tx_cpl_tlp_data),
        .tlp_valid(tx_cpl_tlp_valid), .tlp_sop(tx_cpl_tlp_sop),
        .tlp_ready(tx_cpl_tlp_ready));

    // DMA read requests (no payload)
    pcie_tlp_req_mon tx_rd_req (
        .clk(clk), .rst(rst),
        .tlp_hdr(tx_rd_req_tlp_hdr), .tlp_data(256'd0),
        .tlp_valid(tx_rd_req_tlp_valid), .tlp_sop(tx_rd_req_tlp_sop),
        .tlp_ready(tx_rd_req_tlp_ready));

    // Completions with the DMA read data
    pcie_tlp_cpl_mon rx_cpl (
        .clk(clk), .rst(rst),
        .tlp_hdr(rx_cpl_tlp_hdr), .tlp_data(rx_cpl_tlp_data),
        .tlp_valid(rx_cpl_tlp_valid), .tlp_sop(rx_cpl_tlp_sop),
        .tlp_ready(rx_cpl_tlp_ready));

    // DMA write requests
    pcie_tlp_req_mon tx_wr_req (
        .clk(clk), .rst(rst),
        .tlp_hdr(tx_wr_req_tlp_hdr), .tlp_data(tx_wr_req_tlp_data),
        .tlp_valid(tx_wr_req_tlp_valid), .tlp_sop(tx_wr_req_tlp_sop),
        .tlp_ready(tx_wr_req_tlp_ready));

    // MSI-X interrupt writes
    pcie_tlp_req_mon #(.DATA_DW(1)) tx_msix_wr_req (
        .clk(clk), .rst(rst),
        .tlp_hdr(tx_msix_wr_req_tlp_hdr), .tlp_data(tx_msix_wr_req_tlp_data),
        .tlp_valid(tx_msix_wr_req_tlp_valid), .tlp_sop(tx_msix_wr_req_tlp_sop),
        .tlp_ready(tx_msix_wr_req_tlp_ready));
endmodule

bind example_core_pcie example_core_pcie_tlp_mon tlp_mon (.*);
