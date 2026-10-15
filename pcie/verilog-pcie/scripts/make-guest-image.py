#!/usr/bin/env python3
#
# Creates a guest disk image for the verilog-pcie demo with the IOMMU
# enabled and the test scripts installed.
#
# The image is a qcow2 overlay on top of the Ubuntu cloud image (which is
# not modified). It is booted once with the demo (run-demo.sh) and over SSH:
#
#  * installs the generic kernel (the cloud image's -kvm kernel has no
#    IOMMU / VFIO support), its headers and gcc
#  * boots it by default with the IOMMU translating all DMA
#    ("intel_iommu=on iommu.passthrough=0")
#  * copies the C test sources in scripts/guest/ to /opt/vpcie-tests,
#    builds them there (and adds the directory to PATH)
#  * builds the verilog-pcie example kernel driver into
#    /opt/vpcie-tests/driver/example.ko, with the AT chicken bit set in its
#    DMA tags (its DMA API addresses are IOVAs: untranslated requests)
#
# Needs network access in the guest (QEMU user networking) and
# python3-pexpect on the host.
#
# Example:
#   pcie/verilog-pcie/scripts/make-guest-image.py \
#       --qemu ~/designs/github/qemu_xlnx/build/qemu-system-x86_64 \
#       --base ~/Downloads/jammy-server-cloudimg-amd64-disk-kvm.img \
#       --user-data ~/Downloads/user-data.img \
#       --out ~/Downloads/vpcie-guest.qcow2
#
# Then: pcie/verilog-pcie/scripts/run-demo.sh <qemu> \
#           ~/Downloads/vpcie-guest.qcow2 ~/Downloads/user-data.img
#
# --update only reinstalls and rebuilds the tests and the driver and
# rewrites the kernel command line in an existing image (after changing
# scripts/guest/).
#
import argparse
import io
import os
import shlex
import subprocess
import sys
import tarfile
import tempfile
import time

import pexpect

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))

KERNEL_SETUP = r'''
set -e
export DEBIAN_FRONTEND=noninteractive
sudo -E apt-get update -qq
V=$(apt-cache depends linux-image-generic | \
    sed -n 's/.*Depends: linux-image-\(.*\)-generic$/\1/p' | head -n 1)
echo "generic kernel $V"
sudo -E apt-get install -y -qq --no-install-recommends \
    linux-image-$V-generic linux-modules-extra-$V-generic \
    linux-headers-$V-generic gcc libc6-dev make pciutils
sudo apt-get clean
echo KERNEL_SETUP_DONE
'''

SCRIPTS_SETUP = r'''
set -e
V=$(ls /lib/modules | grep generic | sort -V | tail -n 1)
V=${V%-generic}

# Boot the generic kernel with the IOMMU translating all DMA
sudo tee /etc/default/grub.d/99-vpcie.cfg > /dev/null << EOF
# verilog-pcie demo (make-guest-image.py)
GRUB_DEFAULT="Advanced options for Ubuntu>Ubuntu, with Linux $V-generic"
GRUB_CMDLINE_LINUX_DEFAULT="\$GRUB_CMDLINE_LINUX_DEFAULT intel_iommu=on iommu.passthrough=0"
# boot with the initrd (the generic kernel needs it)
GRUB_FORCE_PARTUUID=
EOF
sudo update-grub 2>&1 | grep -E "Found linux image|generic" || true

# Tests (images made before the C tests have no libc6-dev)
if ! dpkg -s libc6-dev > /dev/null 2>&1; then
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
        --no-install-recommends libc6-dev > /dev/null
    sudo apt-get clean
fi
sudo rm -rf /opt/vpcie-tests
sudo mkdir -p /opt/vpcie-tests
sudo tar -C /opt/vpcie-tests -xf /tmp/vpcie-tests.tar
sudo chmod +x /opt/vpcie-tests/*.sh
sudo make -C /opt/vpcie-tests > /tmp/tests-build.log 2>&1 || \
    { tail -20 /tmp/tests-build.log; exit 1; }
ls -la /opt/vpcie-tests/vpcie-*-test
echo 'PATH="$PATH:/opt/vpcie-tests"' | sudo tee /etc/profile.d/vpcie-tests.sh > /dev/null
echo 'Defaults secure_path="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/snap/bin:/opt/vpcie-tests"' > /tmp/vpcie-sudoers
sudo visudo -cf /tmp/vpcie-sudoers > /dev/null
sudo install -m 0440 /tmp/vpcie-sudoers /etc/sudoers.d/vpcie-tests

# Example driver for the generic kernel
cat << EOF | sudo tee /opt/vpcie-tests/driver/Makefile > /dev/null
obj-m += example.o
example-objs += example_driver.o
KVER ?= \$(shell uname -r)
all:
	make -C /lib/modules/\$(KVER)/build M=\$(CURDIR) modules
clean:
	make -C /lib/modules/\$(KVER)/build M=\$(CURDIR) clean
EOF
sudo make -C /opt/vpcie-tests/driver KVER=$V-generic > /tmp/driver-build.log 2>&1 || \
    { tail -20 /tmp/driver-build.log; exit 1; }
ls -la /opt/vpcie-tests/driver/example.ko
modinfo -k $V-generic vfio-pci | head -n 2
echo GUEST_SETUP_DONE
'''


def example_driver_at_bit(path):
    """
    The example driver's DMA tags with the AT chicken bit (tag bit 8) set:
    its addresses come from the DMA API, they are IOVAs when the IOMMU
    translates (untranslated requests, AT = 00).
    """
    src = open(path).read()
    for old, new in (
            ('iowrite32(0xAA, edev->bar[0] + 0x000114);',
             'iowrite32(0x1AA, edev->bar[0] + 0x000114);'),
            ('iowrite32(0x55, edev->bar[0] + 0x000214);',
             'iowrite32(0x155, edev->bar[0] + 0x000214);'),
            ('iowrite32(0x800000AA, edev->bar[0] + 0x000214);',
             'iowrite32(0x800001AA, edev->bar[0] + 0x000214);')):
        if src.count(old) != 1:
            sys.exit('%s: "%s" not found, update example_driver_at_bit()' %
                     (path, old))
        src = src.replace(old, new)
    return ('// AT chicken bit (tag bit 8) set by make-guest-image.py\n' +
            src).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--qemu', required=True)
    ap.add_argument('--base', help='Ubuntu cloud image (qcow2)')
    ap.add_argument('--user-data', required=True)
    ap.add_argument('--out', required=True, help='overlay image to create')
    ap.add_argument('--size', default='12G')
    ap.add_argument('--password', default='pass')
    ap.add_argument('--verilog-pcie', default=os.path.join(REPO, 'verilog-pcie'))
    ap.add_argument('--ssh-port', default='10022')
    ap.add_argument('--update', action='store_true',
                    help='only reinstall the tests / driver in --out')
    args = ap.parse_args()

    out = os.path.abspath(os.path.expanduser(args.out))
    if args.update:
        if not os.path.exists(out):
            sys.exit('%s does not exist' % out)
    else:
        if not args.base:
            sys.exit('--base is needed to create an image')
        base = os.path.abspath(os.path.expanduser(args.base))
        if os.path.exists(out):
            sys.exit('%s exists, remove it first (or use --update)' % out)
        qemu_img = os.path.join(os.path.dirname(args.qemu), 'qemu-img')
        if not os.path.exists(qemu_img):
            qemu_img = 'qemu-img'
        subprocess.run([qemu_img, 'create', '-f', 'qcow2', '-F', 'qcow2',
                        '-b', base, out, args.size], check=True)

    tmp = tempfile.mkdtemp(prefix='vpcie-img-')
    key = os.path.join(tmp, 'key')
    subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', key],
                   check=True)
    ssh = ['ssh', '-q', '-i', key, '-p', args.ssh_port,
           '-o', 'StrictHostKeyChecking=no',
           '-o', 'UserKnownHostsFile=/dev/null', 'ubuntu@127.0.0.1']

    # Test sources + example driver sources
    tar = os.path.join(tmp, 'vpcie-tests.tar')
    with tarfile.open(tar, 'w') as t:
        guest = os.path.join(HERE, 'guest')
        for f in sorted(os.listdir(guest)):
            if f.endswith(('.c', '.h', '.sh')) or f in ('Makefile', 'README'):
                t.add(os.path.join(guest, f), arcname=f)
        drv = os.path.join(args.verilog_pcie, 'example', 'common', 'driver',
                           'example')
        t.add(os.path.join(drv, 'example_driver.h'),
              arcname='driver/example_driver.h')
        src = example_driver_at_bit(os.path.join(drv, 'example_driver.c'))
        ti = tarfile.TarInfo('driver/example_driver.c')
        ti.size = len(src)
        ti.mtime = int(time.time())
        t.addfile(ti, io.BytesIO(src))

    env = dict(os.environ, SNAPSHOT='off', SSH_PORT=args.ssh_port,
               LOG=os.path.join(tmp, 'endpoint.log'))
    print('booting %s (log in %s)' % (out, tmp))
    q = pexpect.spawn(os.path.join(HERE, 'run-demo.sh'),
                      [args.qemu, out, os.path.abspath(args.user_data)],
                      env=env, encoding='utf-8', timeout=1800,
                      codec_errors='replace')
    q.logfile_read = open(os.path.join(tmp, 'console.log'), 'w')
    ok = False
    try:
        q.expect('login:', timeout=1800)
        c = pexpect.spawn('ssh-copy-id', ['-i', key + '.pub', '-p',
                                          args.ssh_port, '-o',
                                          'StrictHostKeyChecking=no', '-o',
                                          'UserKnownHostsFile=/dev/null',
                                          'ubuntu@127.0.0.1'],
                          encoding='utf-8', timeout=120)
        c.expect('assword:')
        c.sendline(args.password)
        c.expect(pexpect.EOF)

        with open(tar, 'rb') as f:
            subprocess.run(ssh + ['cat > /tmp/vpcie-tests.tar'], stdin=f,
                           check=True)
        setup = SCRIPTS_SETUP if args.update else \
            KERNEL_SETUP + SCRIPTS_SETUP
        r = subprocess.run(ssh + ['bash -s'], input=setup, text=True,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(r.stdout)
        ok = 'GUEST_SETUP_DONE' in r.stdout
        subprocess.run(ssh + ['sudo poweroff'])
        q.expect(pexpect.EOF, timeout=300)
    finally:
        if q.isalive():
            q.terminate(force=True)

    if not ok:
        sys.exit('guest setup failed, see above and %s' % tmp)
    print('%s %s' % ('updated' if args.update else 'created', out))
    print('run it with:\n  %s %s %s %s' % (
        os.path.join(HERE, 'run-demo.sh'), args.qemu, out,
        os.path.abspath(args.user_data)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
