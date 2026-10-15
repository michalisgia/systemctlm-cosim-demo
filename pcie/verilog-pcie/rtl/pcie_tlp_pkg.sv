// PCIe TLP header types and decode functions for waveform viewing.
//
// The verilog-pcie generic TLP interface carries the header in a 128 bit
// vector with DW0 in [127:96] (byte 0 of the TLP in [127:120]). The
// functions below decode it into packed structs, with --trace-structs
// every field shows up as its own signal in the waveforms and the enums
// are shown by name.
//
// Field layouts follow the PCIe Base Specification, section 2.2.

package pcie_tlp_pkg;

    // {Fmt, Type} of DW0 [31:24]
    typedef enum logic [7:0] {
        MRD_32   = 8'h00,
        MRD_64   = 8'h20,
        MRDLK_32 = 8'h01,
        MRDLK_64 = 8'h21,
        MWR_32   = 8'h40,
        MWR_64   = 8'h60,
        IORD     = 8'h02,
        IOWR     = 8'h42,
        CFGRD0   = 8'h04,
        CFGWR0   = 8'h44,
        CFGRD1   = 8'h05,
        CFGWR1   = 8'h45,
        MSG_RC   = 8'h30,  // routed to root complex
        MSG_ADDR = 8'h31,  // routed by address
        MSG_ID   = 8'h32,  // routed by ID
        MSG_BC   = 8'h33,  // broadcast from root complex
        MSG_LOC  = 8'h34,  // local, terminated at receiver
        MSG_GATH = 8'h35,  // gathered and routed to root complex
        MSGD_RC  = 8'h70,
        MSGD_LOC = 8'h74,
        CPL      = 8'h0A,
        CPLD     = 8'h4A,
        CPLLK    = 8'h0B,
        CPLDLK   = 8'h4B
    } tlp_fmt_type_t;

    typedef enum logic [2:0] {
        CPL_SC  = 3'b000,  // successful completion
        CPL_UR  = 3'b001,  // unsupported request
        CPL_CRS = 3'b010,  // configuration request retry status
        CPL_CA  = 3'b100   // completer abort
    } tlp_cpl_status_t;

    // Memory / IO request header (MRd, MWr, MSI-X writes)
    typedef struct packed {
        tlp_fmt_type_t fmt_type;
        logic [10:0]   length_dw;     // payload / requested DWs, 1..1024
        logic [2:0]    tc;
        logic [2:0]    attr;          // {ID based ordering, relaxed ordering, no snoop}
        logic          th;
        logic          td;
        logic          ep;
        logic [1:0]    at;
        logic [15:0]   requester_id;  // {bus, device, function}
        logic [9:0]    tag;           // {T9, T8, tag}
        logic [3:0]    first_be;
        logic [3:0]    last_be;
        logic [63:0]   addr;          // byte address of the first DW
    } tlp_req_t;

    // Completion header (Cpl, CplD)
    typedef struct packed {
        tlp_fmt_type_t   fmt_type;
        logic [10:0]     length_dw;     // payload DWs, 1..1024, 0 for Cpl
        logic [2:0]      tc;
        logic [2:0]      attr;
        logic            td;
        logic            ep;
        logic [15:0]     completer_id;
        tlp_cpl_status_t status;
        logic            bcm;
        logic [12:0]     byte_count;    // remaining bytes, 1..4096
        logic [15:0]     requester_id;
        logic [9:0]      tag;
        logic [6:0]      lower_addr;
    } tlp_cpl_t;

    function automatic logic [10:0] decode_len(logic [9:0] len);
        return (len == 10'd0) ? 11'd1024 : {1'b0, len};
    endfunction

    function automatic tlp_req_t decode_req(logic [127:0] hdr);
        tlp_req_t r;
        logic [31:0] dw0 = hdr[127:96];
        logic [31:0] dw1 = hdr[95:64];
        logic [31:0] dw2 = hdr[63:32];
        logic [31:0] dw3 = hdr[31:0];

        r.fmt_type     = tlp_fmt_type_t'(dw0[31:24]);
        r.length_dw    = decode_len(dw0[9:0]);
        r.tc           = dw0[22:20];
        r.attr         = {dw0[18], dw0[13:12]};
        r.th           = dw0[16];
        r.td           = dw0[15];
        r.ep           = dw0[14];
        r.at           = dw0[11:10];
        r.requester_id = dw1[31:16];
        r.tag          = {dw0[23], dw0[19], dw1[15:8]};
        r.last_be      = dw1[7:4];
        r.first_be     = dw1[3:0];
        // Fmt[0] set: 4 DW header with a 64 bit address
        r.addr         = dw0[29] ? {dw2, dw3[31:2], 2'b00} :
                                   {32'd0, dw2[31:2], 2'b00};
        return r;
    endfunction

    function automatic tlp_cpl_t decode_cpl(logic [127:0] hdr);
        tlp_cpl_t c;
        logic [31:0] dw0 = hdr[127:96];
        logic [31:0] dw1 = hdr[95:64];
        logic [31:0] dw2 = hdr[63:32];

        c.fmt_type     = tlp_fmt_type_t'(dw0[31:24]);
        // Cpl without data: Length is reserved
        c.length_dw    = dw0[30] ? decode_len(dw0[9:0]) : 11'd0;
        c.tc           = dw0[22:20];
        c.attr         = {dw0[18], dw0[13:12]};
        c.td           = dw0[15];
        c.ep           = dw0[14];
        c.completer_id = dw1[31:16];
        c.status       = tlp_cpl_status_t'(dw1[15:13]);
        c.bcm          = dw1[12];
        c.byte_count   = (dw1[11:0] == 12'd0) ? 13'd4096 : {1'b0, dw1[11:0]};
        c.requester_id = dw2[31:16];
        c.tag          = {dw0[23], dw0[19], dw2[15:8]};
        c.lower_addr   = dw2[6:0];
        return c;
    endfunction

endpackage
