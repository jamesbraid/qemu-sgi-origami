#!/usr/bin/env python3
#
# The SGI CD-ROM drive through every guest that reads it: each stock PROM
# that boots from a CD, IRIX reading and mounting a CD, an IRIX install
# started from one, and the OpenBSD installer reading its sets from one
#
# SPDX-License-Identifier: BSD-3-Clause
"""Check the CD-ROM block length contract against the software that uses it.

The drive powers on at 2048-byte blocks, reports its length in a block
descriptor, takes another from MODE SELECT and returns to 2048 on reset
(docs/system/mips/sgi/hardware/scsi-cdrom.rst). The qtests check those
commands one by one; these tests check that every consumer still works
with them, so a change of power-on length or MODE SELECT handling that
suits one consumer cannot quietly break another:

- the Origin 200, Octane2 and Origin 300 stock PROMs each load sash from
  an IRIX install CD and list the miniroot directory, which lies deep in
  the CD's EFS partition, and the Octane2 PROM loads fx from the IRIX
  6.5.30 overlay CD through sash. A drive read in the wrong unit fails
  here with "execute format error" or a missing file. The O2 is not here:
  a fresh O2 PROM talks only to its graphics console until the serial
  console is chosen there. Its PROM sends MODE SELECT for 512 to every
  CD-ROM, which the qtests cover;
- IRIX 6.5.30 on the Origin 200 reads a whole SGI-labelled CD and a whole
  install CD through the raw volume device and compares each checksum
  with the image's, finds the install CD mounted by the media daemon and
  reads a distribution from it with inst;
- the Origin 200 PROM's "Install System Software" loads the miniroot from
  the install CD, and inst in the miniroot reads a distribution from it;
- the OpenBSD 6.9 installer, booted from its CD by the Octane2 PROM,
  mounts the CD and hashes its base set, which must match the release's
  signed SHA256 list. A drive that powers on at 512 fails here: OpenBSD
  trusts READ CAPACITY, and its CD is laid out in 2048-byte sectors.

The IRIX install CD is the 6.5.22 Installation Tools and Overlays CD 1,
which carries sash64 and sashARCS in its volume header. Every test needs
QEMU_TEST_SGI_ASSET_DIR.
"""

import os
import re
import struct
import zlib

import lib_origin300
from lib_prom import (BOOT_TIMEOUT_SECONDS, FIRMWARE_REL, FIRMWARE_SHA256,
                      IRIX_ACCEL, IRIX_SMP, MACHINES, drive_console,
                      launch_paused)
from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)
from qemu_test.asset import LocalAsset


CD_REL = os.path.join('media', '6.5.22', 'overlays1.image')
CD_SHA256 = 'c36d43431569902a9bdf868cb9f98ea44519a9a99e2bb68dbf1be76be0eec299'
# The POSIX cksum of the whole image, as IRIX's cksum prints it.
CD_CKSUM = '3288084986 681861120'

CD6530_REL = os.path.join('media', '6.5.30', 'overlays1.image')
CD6530_SHA256 = ('0e8b0610955686f3be551b73f17b80c6'
                 '748ed7e6ff8898dd66766898b4c8222b')
OBSD_REL = os.path.join('openbsd', '6.9', 'install69.iso')
OBSD_SHA256 = ('3d314ab2207d7da879c4a6a6cfd66667'
               'f7734c854ffa60969fd8bf4ee81976cd')
# The base set's SHA-256, from the release's signed SHA256 list.
OBSD_BASE_SHA256 = ('b71e368f9f820a3c6ca5df53fc2ba617'
                    'e9f535b23504f01448fb0e2100aa9530')

IP30_REL = os.path.join('firmware', '6.5.30', 'IP30prom.bin')
IP30_SHA256 = 'ee4722ca6f9b821ffadf156346c4d69bf5be71fc4b911cee75e36a1f51a49972'
DISK_REL = os.path.join('irix', '6.5.30', 'root.qcow2')
DISK_SHA256 = '15b4bc1992845b6242948a9e944ce39682b7decdaee1e5474579418fb3a19c12'

# A raw medium of this many bytes: an SGI volume header naming 512-byte
# sectors, as SGI's own CDs carry, and no filesystem.
RAW_BYTES = 1024 * 1024
VOLHDR_MAGIC = 0x0be5a941

# Two of the kernels sash lists in the install CD's miniroot directory.
MINIROOT_KERNELS = ('unix.IP27', 'unix.IP30')

# A step's output ends with this marker and its exit status. Each command
# carries it as __S<n>''__ so the echoed command line does not match.
STEP_DONE = '__S{}__'


def posix_cksum(data):
    """The POSIX cksum of data: CRC-32/CKSUM over the bytes and the length."""
    crc = 0
    table = _cksum_table()
    length = len(data)
    for byte in data:
        crc = ((crc << 8) & 0xffffffff) ^ table[(crc >> 24) ^ byte]
    while length:
        crc = ((crc << 8) & 0xffffffff) ^ table[(crc >> 24) ^ (length & 0xff)]
        length >>= 8
    return f'{~crc & 0xffffffff} {len(data)}'


def _cksum_table():
    table = []
    for i in range(256):
        c = i << 24
        for _ in range(8):
            c = ((c << 1) ^ 0x04c11db7) if c & 0x80000000 else c << 1
        table.append(c & 0xffffffff)
    return table


def volume_header(sectors):
    """An SGI volume header for a medium of sectors 512-byte sectors.

    The layout is NetBSD's struct sgi_boot_block (sys/sys/bootblock.h): the
    sector size at 0x28, partitions of (blocks, first, type) from 0x138 and
    a checksum at 0x1f8 that makes the 32-bit words sum to zero. Partition
    8 is the header itself and partition 10 the whole volume.
    """
    header = bytearray(512)
    struct.pack_into('>I', header, 0, VOLHDR_MAGIC)
    struct.pack_into('>H', header, 0x28, 512)
    struct.pack_into('>III', header, 0x138 + 8 * 12, 1, 0, 0)
    struct.pack_into('>III', header, 0x138 + 10 * 12, sectors, 0, 6)
    total = sum(struct.unpack('>128I', header)) & 0xffffffff
    struct.pack_into('>I', header, 0x1f8, (-total) & 0xffffffff)
    return bytes(header)


def raw_medium(path):
    """Write a volume header and reproducible bytes, RAW_BYTES in all, to
    path; return the whole image's cksum."""
    data = bytearray(volume_header(RAW_BYTES // 512))
    counter = 0
    while len(data) < RAW_BYTES:
        data += zlib.crc32(counter.to_bytes(4, 'big')).to_bytes(4, 'big')
        counter += 1
    with open(path, 'wb') as medium:
        medium.write(data)
    return posix_cksum(bytes(data))


def shell_steps(prompt, commands):
    """Console steps that run each command on a Bourne shell at prompt.

    Each command's output ends with its step marker, which the next step
    waits for. Returns (steps, final marker pattern).
    """
    steps = []
    wait = prompt
    for index, command in enumerate(commands):
        marker = STEP_DONE.format(index)
        quoted = marker[:-2] + "''" + marker[-2:]
        steps.append((wait, f'{command}; echo {quoted}$?\r'))
        wait = re.escape(marker) + r'\d'
    return steps, wait


class SgiCdrom(QemuSystemTest):
    """One test per consumer of the CD-ROM contract."""

    ASSET_CD = LocalAsset(CD_REL, CD_SHA256)
    ASSET_CD6530 = LocalAsset(CD6530_REL, CD6530_SHA256)
    ASSET_OBSD = LocalAsset(OBSD_REL, OBSD_SHA256)
    ASSET_IP27 = LocalAsset(FIRMWARE_REL, FIRMWARE_SHA256)
    ASSET_IP30 = LocalAsset(IP30_REL, IP30_SHA256)
    ASSET_IP35 = LocalAsset(lib_origin300.FIRMWARE_REL,
                            lib_origin300.FIRMWARE_SHA256)
    ASSET_DISK = LocalAsset(DISK_REL, DISK_SHA256)

    def cd_args(self, bus, target, image=None):
        image = image or self.ASSET_CD.fetch()
        name = f'cd{target}'
        return ['-drive', f'if=none,id={name},file={image},format=raw,'
                'readonly=on',
                '-device', f'scsi-cd,bus={bus},scsi-id={target},drive={name}']

    def drive(self, name, steps, until, timeout=BOOT_TIMEOUT_SECONDS):
        """Run the launched VM through steps to until; return the transcript."""
        log_path = self.log_file(f'{name}.log')
        try:
            transcript, reached = drive_console(
                self.vm.console_socket, log_path, timeout, steps,
                until=re.compile(until.encode()), vm=self.vm)
        finally:
            self.vm.shutdown(hard=self.vm.exitcode() is not None)
        self.assertTrue(reached, f'{name} did not reach {until!r} -- '
                        f'console log: {log_path}')
        return transcript

    def prom_sash(self, name, device, sash, menu='Option\\? '):
        """From the PROM menu: Command Monitor, sash from the CD, then list
        the miniroot directory on its EFS partition."""
        steps = [
            (menu, '5\r'),
            ('>> ', f'boot -f {device.format(8)}{sash}\r'),
            ('sash: ', f'ls {device.format(7)}dist/miniroot\r'),
        ]
        transcript = self.drive(name, steps, r'unix\.IP27[\s\S]*sash: ')
        for want in ('Standalone Shell',) + MINIROOT_KERNELS:
            self.assertTrue(want in transcript,
                            f'{name}: no {want!r} on the console')

    @skipBigDataTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin200_prom_boots_sash_from_cd(self):
        self.set_machine('origin200')
        self.vm.set_machine(MACHINES['origin200'])
        self.vm.set_console()
        self.vm.add_args('-cpu', 'R10000', '-m', '64',
                         '-bios', str(self.ASSET_IP27.fetch()),
                         '-audio', 'none', '-nic', 'none',
                         *self.cd_args('scsi.0', 6))
        launch_paused(self.vm)
        self.prom_sash('origin200-sash', 'dksc(0,6,{})', 'sash64')

    def octane2(self, image):
        # The Octane PROM probes targets 1 to 3 on its first controller and
        # every target on the second (docs/system/mips/sgi/ip30.rst).
        self.set_machine('octane2')
        self.vm.set_console()
        self.vm.add_args('-bios', str(self.ASSET_IP30.fetch()),
                         '-audio', 'none', '-nic', 'none',
                         *self.cd_args('scsi.1', 4, image))
        launch_paused(self.vm)

    @skipBigDataTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_octane2_prom_boots_sash_from_cd(self):
        self.octane2(self.ASSET_CD.fetch())
        self.prom_sash('octane2-sash', 'dksc(1,4,{})', 'sash64')

    @skipBigDataTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_octane2_prom_loads_fx_from_6530_cd(self):
        self.octane2(self.ASSET_CD6530.fetch())
        steps = [
            ('Option\\? ', '5\r'),
            ('>> ', 'boot -f dksc(1,4,8)sash64 dksc(1,4,7)stand/fx.64 --x\r'),
        ]
        self.drive('octane2-fx', steps, r'fx: "device-name" = ')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_openbsd_installer_reads_its_cd(self):
        self.octane2(self.ASSET_OBSD.fetch())
        steps = [
            ('Option\\? ', '2\r'),
            ('or <enter> to start: ', '\r'),
            ('Insert the installation CD-ROM, then press <enter>: ', '\r'),
            ('\\(S\\)hell\\? ', 'S\r'),
            # One short command per prompt: the installer's shell edits a
            # long pasted line badly on the serial console.
            ('# ', 'mount -r /dev/cd0a /mnt2\r'),
            ('# ', 'cd /mnt2/6.9/sgi\r'),
            ('# ', 'sha256 base69.tgz\r'),
            ('# ', 'echo __D""ONE__\r'),
        ]
        transcript = self.drive('openbsd-cd', steps, r'__DONE__')
        self.assertTrue(
            f'SHA256 (base69.tgz) = {OBSD_BASE_SHA256}' in transcript,
            'the OpenBSD installer did not read its base set from the CD: '
            f'{self.log_file("openbsd-cd.log")}')

    @skipBigDataTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_origin300_prom_boots_sash_from_cd(self):
        machine, argv = lib_origin300.launch_args(
            self.ASSET_IP35.fetch(), 2, self.workdir)
        self.set_machine('origin300')
        self.vm.set_machine(machine)
        self.vm.set_console()
        self.vm.add_args(*argv, '-audio', 'none', *self.cd_args('scsi.0', 2))
        launch_paused(self.vm)
        self.prom_sash('origin300-sash', 'dksc(0,2,{})', 'sash64')

    def irix_origin200(self, cds):
        self.set_machine('origin200')
        self.vm.set_machine(MACHINES['origin200'])
        self.vm.set_console()
        disk = self.ASSET_DISK.fetch()
        self.vm.add_args(
            '-accel', IRIX_ACCEL, '-cpu', 'R10000', '-smp', IRIX_SMP,
            '-m', '128', '-bios', str(self.ASSET_IP27.fetch()),
            '-audio', 'none', '-nic', 'user,id=net0',
            '-drive', f'if=none,id=disk0,file={disk},format=qcow2,'
            'snapshot=on',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=disk0', *cds)
        launch_paused(self.vm)

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_irix_reads_and_mounts_cds(self):
        raw = os.path.join(self.workdir, 'raw-medium.img')
        raw_cksum = raw_medium(raw)
        self.irix_origin200(self.cd_args('scsi.0', 4, raw) +
                            self.cd_args('scsi.0', 6))
        # The media daemon mounts the install CD's EFS partition at a
        # /CDROM mount point of its choosing; find it, waiting for it.
        efs = ("/sbin/mount | awk '$5 == \"efs\" && $3 ~ /CDROM/ "
               "{ print $3 }'")
        steps, done = shell_steps('# ', [
            'dd if=/dev/rdsk/dks0d4vol bs=2048 | cksum | sed "s/^/RAW= /"',
            'dd if=/dev/rdsk/dks0d6vol bs=64k | cksum | sed "s/^/EFS= /"',
            f'n=0; until [ -n "`{efs}`" -o $n -ge 60 ]; do sleep 2; '
            'n=`expr $n + 1`; done',
            f'ls `{efs}`/dist/miniroot | sed "s/^/MINIROOT= /"',
            f'printf "list\\nquit\\n" | inst -f `{efs}`/dist/sysmon_6522m',
        ])
        steps = [('Option\\? ', '1\r'), ('login: ', 'root\r'),
                 ('TERM = ', '\r'), ('\\d# ', 'exec /bin/sh\r')] + steps
        transcript = self.drive('irix-cd', steps, done)
        log = self.log_file('irix-cd.log')
        for want, what in ((f'RAW= {raw_cksum}',
                            'read the whole SGI-labelled CD'),
                           (f'EFS= {CD_CKSUM}', 'read the whole install CD'),
                           ('MINIROOT= unix.IP27', 'mounted the install CD'),
                           ('sysmon.sw.viewer',
                            'read a distribution with inst')):
            self.assertTrue(want in transcript,
                            f'IRIX never {what}: no {want!r} in {log}')

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_prom_installs_miniroot_from_cd(self):
        self.irix_origin200(self.cd_args('scsi.0', 6))
        steps = [
            ('Option\\? ', '2\r'),
            ('or <enter> to start: ', '\r'),
            ('Insert the installation CD-ROM, then press <enter>: ', '\r'),
            ('Inst> ', 'from /CDROM/dist/sysmon_6522m\r'),
        ]
        transcript = self.drive('miniroot-cd', steps,
                                r'Reading product descriptions[\s\S]*Inst> ')
        self.assertTrue(
            'Default distribution to install from: /CDROM/dist' in transcript,
            'the miniroot did not start inst from the CD')


if __name__ == '__main__':
    QemuSystemTest.main()
