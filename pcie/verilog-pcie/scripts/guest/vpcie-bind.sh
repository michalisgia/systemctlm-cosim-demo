#!/bin/sh
#
# Binds the verilog-pcie example end-point (1234:0001) to a driver
# (run as root).
#
# Usage: vpcie-bind.sh <vfio|none|edev|show> [BDF]
#
#   vfio  vfio-pci, for vpcie-vfio-dma-test (virtual addresses, translated
#         by the IOMMU)
#   none  no driver, for vpcie-phys-dma-test (physical addresses)
#   edev  the verilog-pcie example kernel driver (example.ko, runs its DMA
#         tests and benchmarks on probe, results in dmesg)
#   show  current driver, IOMMU group and kernel command line
#
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-show}
BDF=$2

if [ -z "$BDF" ]; then
	for d in /sys/bus/pci/devices/*; do
		if [ "$(cat $d/vendor)" = 0x1234 ] && [ "$(cat $d/device)" = 0x0001 ]; then
			BDF=$(basename $d)
			break
		fi
	done
fi
[ -n "$BDF" ] || { echo "no 1234:0001 device found"; exit 1; }
case $BDF in 0000:*) ;; *) BDF=0000:$BDF ;; esac
DEV=/sys/bus/pci/devices/$BDF

current() {
	[ -e $DEV/driver ] && basename $(readlink $DEV/driver) || echo none
}

unbind() {
	if [ -e $DEV/driver ]; then
		echo $BDF > $DEV/driver/unbind
	fi
	echo > $DEV/driver_override
}

case $MODE in
vfio)
	modprobe vfio-pci
	unbind
	echo vfio-pci > $DEV/driver_override
	echo $BDF > /sys/bus/pci/drivers_probe
	;;
none)
	unbind
	;;
edev)
	unbind
	if ! lsmod | grep -q '^example '; then
		KO=$HERE/driver/example.ko
		[ -e $KO ] || { echo "$KO not built (make -C $HERE/driver)"; exit 1; }
		insmod $KO
	else
		echo $BDF > /sys/bus/pci/drivers/edev/bind
	fi
	sleep 1
	dmesg | grep -E "edev $BDF|$BDF: (edev|Interrupt)" | tail -n 40 || true
	;;
show)
	;;
*)
	sed -n '2,17p' "$0" | sed 's/^# \{0,1\}//'
	exit 1
	;;
esac

echo "$BDF driver: $(current)"
if [ -e $DEV/iommu_group ]; then
	G=$(readlink -f $DEV/iommu_group)
	echo "IOMMU group $(basename $G), type $(cat $G/type 2>/dev/null || echo ?)," \
		"devices: $(ls $G/devices | tr '\n' ' ')"
else
	echo "no IOMMU group (IOMMU disabled)"
fi
echo "kernel: $(uname -r), cmdline: $(cat /proc/cmdline)"
