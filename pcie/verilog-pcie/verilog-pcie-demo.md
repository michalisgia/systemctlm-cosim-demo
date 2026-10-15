# verilog-pcie example design as a co-simulated PCIe end-point

## Overview

This demo connects a QEMU x86 host to the
[verilog-pcie](https://github.com/alexforencich/verilog-pcie) example
design (`example_core_pcie`, a DMA engine with control registers, an AXI
RAM and MSI-X), Verilated and running as a PCIe end-point.

```
QEMU (x86, q35)                          SystemC
remote-port-pcie-root-port  ═ remote-port ═ remoteport_tlm_pci_ep
                                            └ pcie_root_port      (TLM <-> TLPs)
                                               ═ TLPs ═
                                            pcie_int_blk          (models the PCIe hard IP)
                                               ═ raw TLP streams ═
                                            example_core_pcie_rtl (Verilated RTL)
```

* `pcie-int-blk.h` stands in for the PCIe integrated block: it answers
  configuration requests from its own config space (IDs, BARs, MSI-X and
  PCIe capabilities), decodes BARs and hands memory TLPs and completions
  to the user logic as raw TLPs. It exports the configuration state the
  core needs (`bus_num`, MPS, MRRS, extended tags, RCB, MSI-X
  enable / mask) and returns TX sequence numbers.
* `example-core-pcie-rtl.h` clocks the Verilated `example_core_pcie` (250
  MHz by default) and converts between the block's TLP queues and the
  core's generic TLP interface (`rx_req`, `tx_cpl`, `rx_cpl`, `tx_rd_req`,
  `tx_wr_req`, `tx_msix_wr_req`). The Xilinx specific `pcie_us_if`
  (RQ/RC/CQ/CC) is not used.
* `verilog-pcie-cfg.h` sets IDs, BARs and the MSI-X layout to match the
  Alveo example's PCIe IP configuration (1234:0001, BAR0/2 16 MiB, BAR4
  64 KiB, 32 MSI-X vectors with the table at BAR4 0x0 and PBA at 0x8000).

## Build

verilog-pcie is the `verilog-pcie` submodule (a fork of
alexforencich/verilog-pcie with Verilator fixes, `git submodule update
--init verilog-pcie`), override with `VERILOG_PCIE_DIR=...`. The RTL is
Verilated in C++ mode by these targets, independently of
`HAVE_VERILOG_VERILATOR` (tested with Verilator 5.006). Four files get
the AT chicken bit (see below), they are copied to `obj_dir_vpcie/rtl/`
and patched there, the verilog-pcie tree is not modified.

```
$ make SYSTEMC=/path/to/systemc-2.3.3/ \
       pcie/verilog-pcie/verilog-pcie-demo \
       pcie/verilog-pcie/test-example-core-pcie \
       pcie/verilog-pcie/test-pcie-int-blk
```

Make sure `CXXFLAGS` selects the C++ standard the SystemC library was
built with. For example, a SystemC built as C++14 needs
`CXXFLAGS="-Wall -O2 -g -std=c++14"`; a mismatch shows up as undefined
`sc_api_version_2_3_3_cxx2017...` references when linking.

## Tests without QEMU

* `test-pcie-int-blk`: `pcie_root_port` + `pcie_int_blk` + a small C++
  user logic model. Covers config space, unaligned / multi DW MMIO and
  upstream requests with partial byte enables.
* `test-example-core-pcie`: `pcie_root_port` + `pcie_int_blk` + the
  Verilated core. Runs the register sequence of verilog-pcie's
  `test_fpga_core.py` (BAR2 RAM, DMA read / write, immediate write),
  MSI-X delivery and unaligned DMA in both directions. A probe on the
  root port's DMA socket checks the AT chicken bit: tags without bit 8
  must reach host memory as translated requests, tags with it as
  untranslated ones.

Both print `PASSED: 0 error(s)` on success.

## Running with QEMU

Build Xilinx QEMU and prepare an Ubuntu cloud image and user-data image
as described in [cpm-qdma-demos.md](../versal/cpm-qdma-demos.md).

The quickest way is `scripts/run-demo.sh`, which starts QEMU with the
guest console on the terminal (quit with Ctrl-a x) and the end-point in
the background (log in `verilog-pcie-demo.log`). The disk images are used
in snapshot mode unless `SNAPSHOT=off`, and guest SSH is forwarded to
host port 10022 (`SSH_PORT`):

```
$ pcie/verilog-pcie/scripts/run-demo.sh /path/to/qemu-system-x86_64 \
      /path/to/jammy-server-cloudimg-amd64-disk-kvm.img /path/to/user-data.img
```

To run the two halves by hand instead, launch QEMU with the same command
line as the cpm-qdma-demo, e.g.:

```
$ /path/to/qemu-system-x86_64                                                 \
    -M q35,accel=kvm,kernel-irqchip=split -cpu qemu64,rdtscp                  \
    -m 4G -smp 4 -enable-kvm -display none -serial mon:stdio                  \
    -device intel-iommu,intremap=on,device-iotlb=on,aw-bits=48                \
    -drive file=/path/to/jammy-server-cloudimg-amd64-disk-kvm.img             \
    -drive file=/path/to/user-data.img,format=raw                             \
    -device ioh3420,id=rootport1,slot=1                                       \
    -device remote-port-pci-adaptor,bus=rootport1,id=rp0                      \
    -machine-path /tmp/machine-x86/                                           \
    -device virtio-net-pci,netdev=net0 -netdev type=user,id=net0              \
    -device remote-port-pcie-root-port,id=rprootport,slot=0,rp-adaptor0=rp,rp-chan0=0,ats-translated-bypass=on
```

(`ats-translated-bypass` needs the QEMU patch below.)

and in a second terminal:

```
$ ./pcie/verilog-pcie/verilog-pcie-demo unix:/tmp/machine-x86/qemu-rport-_machine_peripheral_rp0_rp 10000
```

Keep the `-machine-path` short, Unix socket paths are limited to 108
characters and QEMU does not start the remote-port connection otherwise.

In the guest the end-point shows up as `02:00.0 Memory controller
[0580]: Device [1234:0001]`.

### Guest test without a driver

`scripts/qemu-smoke-test.py` boots the plain cloud image in snapshot mode
(the images are not modified), starts the demo, logs in over the serial
console, copies a static build of `scripts/guest/vpcie-phys-dma-test` into
the guest (scp) and runs it (needs python3-pexpect):

```
$ pcie/verilog-pcie/scripts/qemu-smoke-test.py \
      --qemu /path/to/qemu-system-x86_64 \
      --image /path/to/jammy-server-cloudimg-amd64-disk-kvm.img \
      --user-data /path/to/user-data.img
```

## AT chicken bit

The DMA descriptor tag selects the PCIe Address Type (AT) of the
end-point's DMA requests:

| Tag | Host address | DMA requests | IOMMU |
|---|---|---|---|
| bit 8 set, e.g. `0x1AA` | virtual / IOVA | untranslated, AT = 00 | translates it |
| bit 8 clear, e.g. `0xAA` | physical | translated, AT = 10 | bypassed |

This is the one RTL change, `rtl/at-chicken-bit.patch` (applied by the
Makefile to copies of the upstream files, `make VPCIE_AT_CHICKEN_BIT=0`
builds the unmodified RTL with AT = 00 always):

* `example_core_pcie.v`: `DMA_TAG_WIDTH` 8 -> 16, so that bit 8 reaches
  the DMA engine (status registers return tag [15:0]).
* `dma_if_pcie_rd.v` / `dma_if_pcie_wr.v`: the AT field of each read /
  write request comes from bit `AT_TAG_BIT` (8) of its descriptor's tag.
* `example_core.v`: block mode uses the block counter as tag, it sets bit
  8 so that block mode always sends untranslated requests.

Along the path to memory:

* The root port (`tlm2tlp-bridge.h`) marks AT = 10 requests with an
  `atsattr_extension` with `ATTR_PHYS_ADDR`, remote-port forwards it as
  `RP_BUS_ATTR_PHYS_ADDR`.
* QEMU's remote-port memory slave only accepts physical-address accesses
  that hit its ATS cache (addresses translated earlier with remote-port
  ATS translation requests), the RTL doesn't do ATS.
  `qemu/remote-port-ats-translated-bypass.patch` (against Xilinx QEMU
  74d70f8008) adds `ats-translated-bypass` to
  `remote-port-pcie-root-port` (default off): physical-address accesses
  that miss the ATS cache go to system memory, like translated requests
  of a device with ATS enabled on VT-d. Without it, AT = 10 requests fail
  (QEMU warns "Physical address error detected", reads complete with UR).
* The `tlp_mon` monitors show the field as `hdr.at` (0 or 2).

## IOMMU, VFIO and virtual addresses

QEMU's remote-port PCIe root port sends the end-point's untranslated DMA
through the IOMMU address space of device 02:00.0, so with the guest's
IOMMU enabled every untranslated DMA address from the RTL is translated
by QEMU's intel-iommu.

The cloud image's `-kvm` kernel has no IOMMU / VFIO support, so
`scripts/make-guest-image.py` creates a qcow2 overlay of the cloud image
(which is not modified) and provisions it once over SSH: Ubuntu's generic
kernel booted with the IOMMU always translating (`intel_iommu=on
iommu.passthrough=0`, group type `DMA-FQ`), the C tests of
`scripts/guest/` in `/opt/vpcie-tests` (built there, in `PATH` also for
sudo) and the verilog-pcie example driver in
`/opt/vpcie-tests/driver/example.ko`:

```
$ pcie/verilog-pcie/scripts/make-guest-image.py \
      --qemu ~/designs/github/qemu_xlnx/build/qemu-system-x86_64 \
      --base ~/Downloads/jammy-server-cloudimg-amd64-disk-kvm.img \
      --user-data ~/Downloads/user-data.img --out ~/Downloads/vpcie-guest.qcow2
$ pcie/verilog-pcie/scripts/run-demo.sh ~/designs/github/qemu_xlnx/build/qemu-system-x86_64 \
      ~/Downloads/vpcie-guest.qcow2 ~/Downloads/user-data.img
```

`--update` reinstalls and rebuilds the tests and the driver in an
existing image after changing `scripts/guest/` (and rewrites the kernel
command line). `run-demo.sh` sets `aw-bits=48` on the intel-iommu so that
process virtual addresses (47 bit) fit as IOVAs, and
`ats-translated-bypass=on` on the root port.

In the guest (as root, see `/opt/vpcie-tests/README`):

| Command | Addresses / tags | Requests |
|---|---|---|
| `vpcie-bind.sh none; vpcie-phys-dma-test` | physical, `0xNN` | translated, bypass the IOMMU |
| `vpcie-bind.sh vfio; vpcie-vfio-dma-test` | IOVA == the process virtual address (`--iova` for another IOVA), `0x1NN` | untranslated, translated by the IOMMU |
| `vpcie-bind.sh edev` | IOVAs from the DMA API, `0x1AA` / `0x155` | untranslated, translated by the IOMMU |

Both tests are C (`vpcie.c` / `vpcie.h` shared, `make` in
`/opt/vpcie-tests`, `make -C scripts/guest LDFLAGS=-static` on the host
for binaries that run on any x86_64 guest).

`vpcie-phys-dma-test` maps the BARs through sysfs and DMAs to locked
pages using physical addresses from `/proc/self/pagemap`. With the IOMMU
translating it then sends the same physical address untranslated: the
IOMMU has no mapping for it, the read gets a UR completion (DMA status
error 10, `PCIE_CPL_STATUS_UR`) and the write is dropped. `-u` uses
untranslated requests for the copies (only for a guest without IOMMU or
with `iommu=pt`).

`vpcie-vfio-dma-test` maps an 8 page buffer at IOVA == its virtual
address, the pages are scattered in physical memory and the copies
(random data, unaligned, up to 4100 bytes) cross page boundaries, so
they only pass if every page is translated. It checks MSI-X delivery
through VFIO eventfds (through interrupt remapping) and that DMA to an
unmapped IOVA is blocked (UR / dropped), the guest logs DMAR faults for
02:00.0:

```
buffer: 8 pages, va 0x7f058d739000 mapped at iova 0x7f058d739000 (iova == va)
  page 0: iova 0x7f058d739000 -> pa 0x124d72000
  page 1: iova 0x7f058d73a000 -> pa 0x124ffd000
  ...
DMA copies (untranslated requests, buffer contiguous in IOVA space, scattered in physical memory)
  ok  : len  512  src 0x0f00 (dma 0x7f058d739f00)  dst 0x2f81 (dma 0x7f058d73bf81)
  ok  : 2 MSI-X interrupt(s) for the 2 transfers
  ...
IOMMU protection (DMA to an unmapped IOVA)
  DMA read addr 0x7f058d841000 len 64 tag 0x10b: error 10 (PCIE_CPL_STATUS_UR) (status 8a00010b)
  ok  : DMA read from unmapped IOVA 0x7f058d841000 is completed with UR, DMA status error 10 (PCIE_CPL_STATUS_UR)
  ok  : DMA write to unmapped IOVA 0x7f058d841000 is dropped (posted, status error 0 (NONE), buffer unchanged)
  dmesg: DMAR: [DMA Read NO_PASID] Request device [02:00.0] fault addr 0x7f058d841000 [fault reason 0x06] PTE Read access is not set
VPCIE_VFIO_DMA_TEST PASS (0 error(s))
```

With `+trace` the decoded TLPs show the virtual addresses and AT on the
RTL's DMA interfaces (`tlp_mon.tx_rd_req.hdr.addr` = `0x7f058d739000`,
`hdr.at` = 0; physical addresses with `hdr.at` = 2).

The device shares its IOMMU group with its root port, so the group's
default domain can't be switched at run time
(`/sys/kernel/iommu_groups/*/type`, kernel 5.15). The end-point has no
ATS capability, `device-iotlb=on` is not used by it.

### Example driver

verilog-pcie's `example/Alveo/fpga/driver` binds to 1234:0001 and runs
DMA tests and benchmarks on probe. The guest image above has it built
(with the chicken bit in its tags, `vpcie-bind.sh edev`). The upstream
driver writes tags without bit 8, i.e. its DMA API addresses go out as
translated requests: that is only right without IOMMU or with `iommu=pt`.
By hand on the plain cloud image: build it inside the guest (needs the
guest's kernel headers) and load it with `insmod example.ko`.

With the jammy cloud image (kernel 5.15.0-1080-kvm) the headers are no
longer in the apt index and the 2 GB root filesystem is too small for
them, so in the guest:

```
$ sudo apt-get update && sudo apt-get install -y --no-install-recommends gcc make libc6-dev
$ sudo apt-get clean
$ P=http://archive.ubuntu.com/ubuntu/pool/main/l/linux-kvm
$ wget $P/linux-headers-5.15.0-1080-kvm_5.15.0-1080.85_amd64.deb \
       $P/linux-kvm-headers-5.15.0-1080_5.15.0-1080.85_all.deb
$ sudo dpkg -i linux-headers-5.15.0-1080-kvm_5.15.0-1080.85_amd64.deb
$ D=/usr/src/linux-kvm-headers-5.15.0-1080   # generic headers go to a tmpfs
$ sudo mkdir -p $D && sudo mount -t tmpfs -o size=512m tmpfs $D
$ dpkg-deb -x linux-kvm-headers-5.15.0-1080_5.15.0-1080.85_all.deb /dev/shm/x
$ sudo cp -a /dev/shm/x$D/. $D/
```

Copy the driver directory into the guest (it is a symlink in the
verilog-pcie tree, e.g. `tar -chf - driver | ssh ... tar -xf -`, with
`hostfwd=tcp:127.0.0.1:10022-:22` added to `-netdev`), then `make` and
`sudo insmod example.ko`.

Results against the RTL:

* BAR2 read/write, DMA to card and back (`test data matches`) and the
  immediate write pass, and all three MSI-X interrupts reach the guest
  (`lspci -vv`: `MSI-X: Enable+ Count=32`).
* The RX completion buffer tests (125 runs) pass without warnings.
* The block benchmarks use 10000 blocks and a 20 s guest timeout per run.
  Every upstream read is a remote-port round trip to QEMU (~2 ms wall
  time), so the first one (10000 1-byte reads) times out after ~8900
  reads and the driver stops the benchmarks. With `count = 100` and a 600
  s timeout all 100 read/write benchmarks (1 B to 8 KiB, all strides)
  complete without warnings; request counts follow MRRS 512 / MPS 128
  (an 8 KiB block is 16 reads or 64 writes). The reported Mbps are
  simulated time and not meaningful.

## Waveforms

The whole `example_core_pcie` hierarchy (DMA engine, AXI masters, AXI
RAM, MSI-X, TLP mux / demux, ~4500 signals) is Verilated with FST tracing
(`VPCIE_TRACE ?= 1` in the Makefile, `make VPCIE_TRACE=0 ...` builds
without it). Dumping is enabled at run time, both for the demo and for
`test-example-core-pcie`:

* `+trace` dumps to `<binary>.fst` next to the binary, i.e.
  `pcie/verilog-pcie/verilog-pcie-demo.fst` or
  `pcie/verilog-pcie/test-example-core-pcie.fst` (overwritten on each
  run). `+trace=<file.fst>` dumps to another file.
* `+trace-start=<us>` only dumps from that simulation time on.

```
$ EP_ARGS=+trace pcie/verilog-pcie/scripts/run-demo.sh <qemu> <disk.img> <user-data.img>
$ ./pcie/verilog-pcie/test-example-core-pcie +trace
$ gtkwave pcie/verilog-pcie/verilog-pcie-demo.fst pcie/verilog-pcie/scripts/verilog-pcie.gtkw
```

The file is closed when the end-point exits: when QEMU quits or
disconnects, on Ctrl-C / SIGTERM, or at the end of the test. A killed
(SIGKILL) end-point leaves an unreadable file.

### Decoded TLPs

With tracing enabled, `rtl/example_core_pcie_tlp_mon.sv` is bound into
the core (SystemVerilog `bind`, the verilog-pcie sources are not
modified). It decodes the header of each TLP interface into the packed
structs of `rtl/pcie_tlp_pkg.sv`, every field is its own signal and the
`fmt_type` / completion `status` enums carry their names in the FST:

| Monitor (`core.example_core_pcie.tlp_mon.`) | Interface | Struct |
|---|---|---|
| `rx_req` | host MMIO requests to the BARs | `tlp_req_t` |
| `tx_cpl` | completions for MMIO reads | `tlp_cpl_t` |
| `tx_rd_req` | DMA read requests | `tlp_req_t` |
| `rx_cpl` | completions with DMA read data | `tlp_cpl_t` |
| `tx_wr_req` | DMA write requests | `tlp_req_t` |
| `tx_msix_wr_req` | MSI-X writes | `tlp_req_t` |

`tlp_req_t`: `fmt_type`, `length_dw` (1..1024), `tc`, `attr`, `th`, `td`,
`ep`, `at`, `requester_id`, `tag` (10 bit), `first_be`, `last_be`,
`addr` (64 bit byte address). `tlp_cpl_t`: `fmt_type`, `length_dw`,
`tc`, `attr`, `td`, `ep`, `completer_id`, `status`, `bcm`, `byte_count`
(1..4096), `requester_id`, `tag`, `lower_addr`.

Each monitor has `hdr` (follows the header while a TLP starts and holds
it for its remaining beats and until the next TLP, all zero before the
first TLP), `start` (TLP accepted), `data[]` (the beat as DWs) and
`count` (TLPs accepted so far). `scripts/verilog-pcie.gtkw` shows them
grouped per interface, the raw ports are in a collapsed group at the end.
If GTKWave shows `fmt_type` as a number, select it and use
Data Format > Enum (FST enum tables).

Example, the guest DMA test with an unaligned 256 byte write (offset 3,
MPS 128) as seen by the monitors:

```
tx_rd_req  MRD_64 len=64  tag=0   be=f/f addr=0x11c08a000
rx_cpl     CPLD   len=64  tag=0   byte_count=256  lower_addr=0x00
tx_wr_req  MWR_64 len=32  tag=0   be=8/f addr=0x11c08a800
tx_wr_req  MWR_64 len=32  tag=0   be=f/f addr=0x11c08a880
tx_wr_req  MWR_64 len=1   tag=0   be=7/0 addr=0x11c08a900
```

Booting the Ubuntu guest and running `vpcie-phys-dma-test` with `+trace`
covers ~11 ms of simulated time and gives a ~10 MB file (the core is
clocked at 250 MHz from time 0, the guest's first access to the device
is at ~9-10 ms).

## Root port fixes

Using a TLP level end-point other than `PCIeController` required fixes in
`libsystemctlm-soc/tlm-bridges/tlm2tlp-bridge.h`:

* Upstream memory writes honour First / Last DW BE.
* Completions for upstream memory reads carry the correct Byte Count and
  Lower Address (needed by verilog-pcie's DMA read engine).
* Downstream sub DW / unaligned MMIO gets correct byte enables and byte
  lanes, multi DW completions are no longer truncated to one DW and
  unaligned reads crossing a DW boundary return the right bytes.
* The request header is no longer byte swapped in place, completion
  matching only worked when the receiver swapped the buffer back (as
  `PCIeController` does).
* DMA that fails in the host (e.g. an IOMMU fault) no longer aborts the
  simulation: reads are completed with a UR completion (no data), writes
  are dropped with a warning.
* Requests with AT = Translated are passed on as physical addresses
  (`atsattr_extension` `ATTR_PHYS_ADDR`), see the AT chicken bit.
* Message TLPs from the end-point no longer abort the simulation.
  Assert/Deassert_INTx are dropped (INTx is not propagated to QEMU),
  other messages give a warning. libpcie sends Deassert_INTA when the
  host toggles the Interrupt Disable bit, which made the cpm4/cpm5 QDMA
  demos abort while an Ubuntu 22.04 guest enumerated the device (with
  the original bridge too).

## Limitations

* The root port processes upstream requests one at a time and a read is
  completed with a single CplD (no RCB splitting).
* Only memory requests and type 0 configuration requests are supported,
  no I/O or message TLPs, no INTx.
* The core is clocked continuously, also when idle.
* Upstream reads are slow in wall time (one remote-port round trip each),
  long DMA benchmarks hit the example driver's 20 s timeout.
* No ATS: the end-point has no ATS capability and sends no translation
  requests, translated requests rely on QEMU's `ats-translated-bypass`,
  which doesn't check that ATS is enabled for the device.
