#!/usr/bin/env python3
#
# Automated QEMU smoke test of the verilog-pcie-demo.
#
# Boots an Ubuntu cloud image in Xilinx QEMU (in snapshot mode, the images
# are never modified), connects the verilog-pcie-demo as the PCIe
# end-point, logs in over the serial console, copies a static build of
# guest/vpcie-phys-dma-test into the guest (scp) and runs it there.
#
# Example:
#   pcie/verilog-pcie/scripts/qemu-smoke-test.py \
#       --qemu ~/designs/github/qemu_xlnx/build/qemu-system-x86_64 \
#       --image ~/Downloads/jammy-server-cloudimg-amd64-disk-kvm.img \
#       --user-data ~/Downloads/user-data.img
#
# Requires python3-pexpect and access to /dev/kvm. The user-data image
# must set the 'ubuntu' user's password (see cpm-qdma-demos.md).
#
import argparse
import os
import subprocess
import sys
import time

import pexpect

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--qemu", required=True)
    ap.add_argument("--image", required=True, help="qcow2 cloud image")
    ap.add_argument("--user-data", required=True)
    ap.add_argument("--password", default="pass")
    # Unix socket paths are limited to 108 characters, keep it short
    ap.add_argument("--machine-path", default="/tmp/vpcie-machine-x86")
    ap.add_argument("--demo",
                    default=os.path.join(REPO, "pcie/verilog-pcie/verilog-pcie-demo"))
    ap.add_argument("--log-dir", default=".")
    ap.add_argument("--ssh-port", default="10023")
    args = ap.parse_args()

    # Static, runs on the cloud image without a compiler
    guest = os.path.join(HERE, "guest")
    subprocess.run(["make", "-s", "-C", guest, "LDFLAGS=-static",
                    "vpcie-phys-dma-test"], check=True)
    test_bin = os.path.join(guest, "vpcie-phys-dma-test")

    sk = os.path.join(args.machine_path,
                      "qemu-rport-_machine_peripheral_rp0_rp")
    os.makedirs(args.machine_path, exist_ok=True)
    if os.path.exists(sk):
        os.unlink(sk)

    qemu_args = [
        "-M", "q35,accel=kvm,kernel-irqchip=split", "-cpu", "qemu64,rdtscp",
        "-m", "4G", "-smp", "4", "-enable-kvm", "-display", "none",
        "-serial", "mon:stdio",
        "-device", "intel-iommu,intremap=on,device-iotlb=on,aw-bits=48",
        "-drive", "file=%s,format=qcow2,snapshot=on" % args.image,
        "-drive", "file=%s,format=raw,snapshot=on" % args.user_data,
        "-device", "ioh3420,id=rootport1,slot=1",
        "-device", "remote-port-pci-adaptor,bus=rootport1,id=rp0",
        "-machine-path", args.machine_path,
        "-device", "virtio-net-pci,netdev=net0",
        "-netdev", "type=user,id=net0,hostfwd=tcp:127.0.0.1:%s-:22" %
                   args.ssh_port,
        "-device", "remote-port-pcie-root-port,id=rprootport,slot=0,"
                   "rp-adaptor0=rp,rp-chan0=0,ats-translated-bypass=on",
    ]

    qemu_log = open(os.path.join(args.log_dir, "qemu-console.log"), "w")
    q = pexpect.spawn(args.qemu, qemu_args, encoding="utf-8", timeout=600,
                      codec_errors="replace")
    q.logfile_read = qemu_log

    for _ in range(300):
        if os.path.exists(sk):
            break
        time.sleep(0.1)
    else:
        print("remote-port socket %s did not appear" % sk)
        return 1

    sc_log = open(os.path.join(args.log_dir, "systemc.log"), "w")
    sc = subprocess.Popen([args.demo, "unix:" + sk, "10000"],
                          stdout=sc_log, stderr=subprocess.STDOUT)

    passed = False
    try:
        q.expect("login:", timeout=900)
        q.sendline("ubuntu")
        q.expect("assword:")
        q.sendline(args.password)
        q.expect(r"\$ ")

        def run(cmd, timeout=120):
            q.sendline(cmd)
            q.expect(r"\$ ", timeout=timeout)
            return q.before

        run("sudo dmesg -n 1")
        print(run("lspci -nn -d 1234:0001"))

        for _ in range(10):
            s = pexpect.spawn("scp", ["-P", args.ssh_port,
                                      "-o", "StrictHostKeyChecking=no",
                                      "-o", "UserKnownHostsFile=/dev/null",
                                      test_bin, "ubuntu@127.0.0.1:/tmp/"],
                              encoding="utf-8", timeout=120)
            if s.expect(["assword:", pexpect.EOF]) == 0:
                s.sendline(args.password)
                s.expect(pexpect.EOF)
            s.close()
            if s.exitstatus == 0:
                break
            time.sleep(5)
        else:
            print("scp of %s failed" % test_bin)
            return 1

        out = run("sudo /tmp/vpcie-phys-dma-test", timeout=600)
        print(out)
        passed = "VPCIE_PHYS_DMA_TEST PASS" in out
    finally:
        # Quit QEMU (Ctrl-a x)
        q.sendcontrol("a")
        q.send("x")
        time.sleep(2)
        q.terminate(force=True)
        sc.kill()
        qemu_log.close()
        sc_log.close()

    print("SMOKE TEST", "PASSED" if passed else "FAILED")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
