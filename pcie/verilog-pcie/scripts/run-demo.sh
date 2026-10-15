#!/bin/sh
#
# Launches the verilog-pcie demo: Xilinx QEMU (x86 q35 host) with the
# Verilated verilog-pcie example_core_pcie as a PCIe end-point.
#
# QEMU runs in the foreground with the guest serial console on stdio
# (quit with Ctrl-a x), the SystemC/RTL end-point runs in the background
# and logs to verilog-pcie-demo.log.
#
# Usage:
#   pcie/verilog-pcie/scripts/run-demo.sh <qemu-system-x86_64> <disk.img> <user-data.img>
#
# Environment:
#   MACHINE_PATH  remote-port socket directory (default /tmp/vpcie-machine-x86,
#                 keep it short, Unix socket paths are limited to 108 chars)
#   SNAPSHOT      "on" (default) to not modify the disk images, "off" to keep
#                 changes
#   SSH_PORT      host port forwarded to the guest's SSH (default 10022)
#   LOG           end-point log file (default ./verilog-pcie-demo.log)
#   EP_ARGS       extra end-point arguments, e.g. EP_ARGS=+trace dumps the
#                 RTL waveforms to pcie/verilog-pcie/verilog-pcie-demo.fst
#
# The root port's ats-translated-bypass=on lets the end-point's translated
# requests (AT = 10, DMA tags without the AT chicken bit) bypass the IOMMU.
#
set -e

if [ $# -ne 3 ]; then
	sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
fi

QEMU=$1
IMAGE=$2
USER_DATA=$3
HERE=$(cd "$(dirname "$0")" && pwd)
DEMO=$HERE/../verilog-pcie-demo
MACHINE_PATH=${MACHINE_PATH:-/tmp/vpcie-machine-x86}
SNAPSHOT=${SNAPSHOT:-on}
SSH_PORT=${SSH_PORT:-10022}
LOG=${LOG:-$PWD/verilog-pcie-demo.log}
SK=$MACHINE_PATH/qemu-rport-_machine_peripheral_rp0_rp

[ -x "$DEMO" ] || { echo "$DEMO not built (make pcie/verilog-pcie/verilog-pcie-demo)"; exit 1; }

mkdir -p "$MACHINE_PATH"
rm -f "$SK"

# Start the end-point once QEMU has created the remote-port socket
(
	i=0
	while [ ! -S "$SK" ]; do
		i=$((i + 1))
		[ $i -gt 300 ] && { echo "no remote-port socket $SK" >&2; exit 1; }
		sleep 0.1
	done
	exec "$DEMO" "unix:$SK" 10000 $EP_ARGS
) > "$LOG" 2>&1 &
EP_PID=$!
# The end-point exits by itself when QEMU disconnects (closing the
# waveform file), only kill it if it is still running after that.
cleanup() {
	i=0
	while kill -0 $EP_PID 2>/dev/null && [ $i -lt 100 ]; do
		sleep 0.1
		i=$((i + 1))
	done
	kill $EP_PID 2>/dev/null || true
}
trap cleanup EXIT

echo "End-point log: $LOG, guest SSH: ssh -p $SSH_PORT ubuntu@127.0.0.1"

"$QEMU" \
	-M q35,accel=kvm,kernel-irqchip=split -cpu qemu64,rdtscp \
	-m 4G -smp 4 -enable-kvm -display none -serial mon:stdio \
	-device intel-iommu,intremap=on,device-iotlb=on,aw-bits=48 \
	-drive file="$IMAGE",snapshot=$SNAPSHOT \
	-drive file="$USER_DATA",format=raw,snapshot=$SNAPSHOT \
	-device ioh3420,id=rootport1,slot=1 \
	-device remote-port-pci-adaptor,bus=rootport1,id=rp0 \
	-machine-path "$MACHINE_PATH" \
	-device virtio-net-pci,netdev=net0 \
	-netdev type=user,id=net0,hostfwd=tcp:127.0.0.1:$SSH_PORT-:22 \
	-device remote-port-pcie-root-port,id=rprootport,slot=0,rp-adaptor0=rp,rp-chan0=0,ats-translated-bypass=on
