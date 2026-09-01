#!/usr/bin/env python3
#
# IRIX eg (Alteon Tigon) network transfer: boot the installed 6.5.30 disk on
# the real IP27 PROM, log in, bring up the eg adapter, pull a payload over it
# by TFTP, and check it byte-for-byte -- with eg's own received-packet
# counter as the device-level proof the frames traversed its receive ring
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
This is the IRIX counterpart of test_openbsd_net_transfer.py, and it is
IRIX on purpose. The fork's receive-return end-of-frame fix
("hw/net/tigon: mark receive-return descriptors end-of-frame") is
IRIX-eg-specific: IRIX's eg driver silently frees any received frame whose
receive-return descriptor lacks the end-of-frame flag, incrementing no
counter, which is exactly the "eg transmits but never receives, ping 100%
loss, Ipkts stuck at zero" signature. OpenBSD's ti(4) does not check that
flag, so a green OpenBSD-over-eg transfer would prove nothing about this
fix; only IRIX exercises the receive path the fix repairs. So this test
drives real IRIX and asserts eg's own Ipkts climbs over the transfer.

What it proves beyond a ping: a 128 KiB random payload is moved over eg as
~256 back-to-back frames through the receive ring (RRQ and per-block ACKs
outbound, the data blocks inbound), and the downloaded copy matches the
served bytes by an independently-computed POSIX cksum and byte count. At
the device level the eg model moves opaque Ethernet frames and cannot tell
TCP from UDP; a byte-exact bulk transfer through its rings is therefore a
complete exercise of the device's data path, and a far heavier one than a
single-frame ping -- which over user-mode SLIRP is unreliable anyway. The
register and ring contract, including the receive end-of-frame flag itself,
is exercised separately by tests/qtest/sn0-tigon-test.c.

How the boot differs from the OpenBSD login test: IRIX here boots the real
IP27 PROM (-bios), not a direct -kernel, so the drive first answers the
System Maintenance Menu with "1" (Start System) to boot the local SCSI
disk, then logs in as root (no password on the installed image), answers
the TERM prompt, and switches the login csh to /bin/sh so the rest of the
drive can use one Bourne command/response idiom. This is the same real PROM
and SLIRP wiring test_irix_miniroot.py uses, with an installed disk added
at SCSI id 1 and the maintenance-menu answer changed from the command
monitor to Start System.

Why the transfer is TFTP: the installed IRIX base image has no nc(1) and no
HTTP-capable ftp(1), so the host-server-and-nc shape test_openbsd_net_transfer
uses does not port. QEMU's own SLIRP stack has a built-in TFTP server
(tftp=), so the payload is served from a local directory with no daemon to
write and fetched by IRIX's own tftp(1) -- a real UDP transfer over eg,
end to end, checked byte-for-byte.

The network back end is placed only on the eg (tigon) device, at a real
Origin 200 PCI option slot; the machine's on-board IOC3 Ethernet is left
with no back end (-nic none), so the only configured interface is eg0 and
the transfer can only have crossed eg.

eg_net_verdict() (test_irix_eg_net_verdict.py, imported below) is the pure
PASS/FAIL contract, testable without a boot; see its docstring for why each
clause is scored the way it is.

Guarded by @skipBigDataTest() (the disk is multi-gigabyte) and
@skipSlowTest() (a real multiuser IRIX boot plus the transfer is minutes),
on top of the usual ORIGAMI_ASSETS resolution + skipTest() at method entry
if the PROM or installed disk is missing, matching the other asset-gated
functional tests here. The installed IRIX disk is not committed (the fork
ships no IRIX bits); a runner provisions it at
<ORIGAMI_ASSETS>/assets/irix/6.5.30/root.qcow2 the same way the firmware
and miniroot kernel are provisioned out of band, and the test skips until it
is present.
"""

import hashlib
import json
import os
import re
import socket
import subprocess
import sys
import time

# This file's own directory, so a plain `pytest test_irix_eg_net_verdict.py`
# run finds the verdict module the same way the other functional tests here
# find theirs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from test_irix_eg_net_verdict import eg_net_verdict  # noqa: E402

try:
    from qemu_test import QemuSystemTest, skipBigDataTest, skipSlowTest
except (ImportError, RuntimeError):
    QemuSystemTest = None


RELEASE = "6.5.30"

# Where ORIGAMI_ASSETS resolves the real PROM and the installed IRIX disk.
# ORIGAMI_ASSETS names a CHECKOUT (one with its own assets/ directory), not
# assets/ itself, matching test_irix_miniroot.py's FIRMWARE_REL.
FIRMWARE_REL = os.path.join('assets', 'firmware', RELEASE,
                            'ip27prom.payload.bin')
DISK_REL = os.path.join('assets', 'irix', RELEASE, 'root.qcow2')

# The release-matched PROM SHA-256, so a stale or wrong-release firmware is
# caught before a boot is spent on it. Same digest test_irix_miniroot.py and
# tools/sashlib.py's RELEASES["6.5.30"] carry.
PROM_SHA256 = (
    "c2c69bf33c375b43ec7ac511fab41ddf595be1b6838ae717e58a9a1273437fdd"
)

# 128 MiB, the same size and reasoning test_openbsd_login.py uses: at or
# below 128 MiB the guest's whole memory bank fits the one slot the machine
# backs, so the filed >128MiB memory-map defects have no address at which to
# appear. A real multiuser IRIX boot needs the headroom over the 64 MiB the
# miniroot netboot uses.
MEMORY_MIB = 128

# SLIRP defaults: the gateway (and built-in TFTP server) and the guest's
# first-lease address, the values the eg bring-up captures used.
GATEWAY = "10.0.2.2"
GUEST_ADDR = "10.0.2.15"

# The eg (tigon) device sits at this Origin 200 PCI option slot; IRIX names
# the first Gigabit Ethernet adapter there eg0 (confirmed by hinv in the eg
# bring-up captures).
EG_ADDR = 4
IFACE = "eg0"

# A random payload neither side can predict, sized to move as many
# back-to-back frames through eg's receive ring as it takes 128 KiB of
# 512-byte TFTP blocks -- about 256 -- so the receive path is exercised well
# past a single frame.
PAYLOAD_BYTES = 128 * 1024

# A real multiuser IRIX boot from the installed disk, on the real PROM, is
# minutes: the PROM's own power-on diagnostics, then the kernel, then init to
# a console login. Kept a little under the meson timeout registered for this
# test in mips64/meson.build; the transfer legs run after this budget under
# their own fixed per-step timeouts below.
LOGIN_BUDGET_SECONDS = 1200
STEP_TIMEOUT_SECONDS = 300


class _Harness(Exception):
    """This test's own side of the run went wrong -- a timeout waiting for
    the guest, or a value it never printed. Kept distinct from a guest fault
    (below) so a report names which side of the machine to look at first,
    the same split test_openbsd_net_transfer.py keeps."""


class _GuestFault(_Harness):
    """The guest itself fell over -- an IRIX panic or a PROM exception --
    rather than a harness timeout. Ends the whole run."""


class _Console:
    """Send a line to the guest console and wait for a pattern in what comes
    back, over this framework's own console socket. The command/response
    marker idiom is test_openbsd_net_transfer.py's _Console, reproduced here
    (SELF-CONTAINED, the same duplication the other functional tests make):
    every command carries its own end marker split across a quote so the
    shell's echo of the command line itself can never match it.
    """

    TRIPWIRES = (
        (re.compile(rb"PANIC"), "the guest panicked"),
        (re.compile(rb"Data bus error"), "the guest took a data bus error"),
        (re.compile(rb"\bException\b\s*<"), "the PROM took an exception"),
    )

    def __init__(self, sock, log_path, exited=None):
        self.sock = sock
        self.sock.settimeout(1.0)
        self.buf = bytearray()
        self._log = open(log_path, "ab")
        self._exited = exited if exited is not None else (lambda: False)

    def _pump(self):
        try:
            chunk = self.sock.recv(4096)
        except socket.timeout:
            chunk = None
        except OSError:
            chunk = None
        if chunk:
            self.buf += chunk
            self._log.write(chunk)
            self._log.flush()
            return
        if self._exited():
            raise _Harness(
                "the emulator exited and the console has nothing further "
                "to read")

    def expect(self, pattern, timeout, what=None):
        rx = re.compile(pattern) if isinstance(pattern, bytes) else pattern
        deadline = time.monotonic() + timeout
        start = 0
        trip_from = len(self.buf)
        while True:
            m = rx.search(self.buf, start)
            if m:
                return m
            for trx, why in self.TRIPWIRES:
                if trx.search(self.buf, trip_from):
                    raise _GuestFault(
                        f"{why} while waiting for {what or rx.pattern!r}")
            if time.monotonic() >= deadline:
                raise _Harness(
                    f"timed out after {timeout}s waiting for "
                    f"{what or rx.pattern!r}")
            start = max(0, len(self.buf) - 4096)
            self._pump()

    def send(self, text):
        self.sock.sendall(text.encode() + b"\n")

    def run(self, cmd, tag, timeout=STEP_TIMEOUT_SECONDS):
        """Send a shell command with its own end marker, return its exit
        code. The marker is split across a quote so the shell's echo of the
        command line can never match it."""
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        m = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                        what=f"{tag} to finish")
        return int(m.group(1))

    def two_values(self, cmd, tag, pattern, timeout=STEP_TIMEOUT_SECONDS):
        """Run a command and pull two values out of what it printed, from
        the same command. Two empty files share a checksum, so pulling the
        size alongside it is what catches a transfer that 'succeeded' on
        nothing."""
        pos = len(self.buf)
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        end = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                          what=f"{tag} to finish")
        rx = re.compile(pattern)
        m = rx.search(bytes(self.buf[pos:end.start()]))
        if not m:
            raise _Harness(f"{tag} printed nothing matching {rx.pattern!r}")
        return m.group(1).decode(), int(m.group(2))

    def capture_value(self, cmd, tag, pattern, timeout=STEP_TIMEOUT_SECONDS):
        """Run a command and pull one value out of what it printed."""
        pos = len(self.buf)
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        end = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                          what=f"{tag} to finish")
        rx = re.compile(pattern)
        m = rx.search(bytes(self.buf[pos:end.start()]))
        if not m:
            raise _Harness(f"{tag} printed no value matching {rx.pattern!r}")
        return m.group(1).decode()

    def close(self):
        self._log.close()


# The System Maintenance Menu answer and the login exchange, each firing
# once against the tail of what has arrived. "1" is Start System (boot the
# local SCSI disk), as against test_irix_miniroot.py's "5" for the command
# monitor. Root has no password on the installed image; the TERM prompt is
# answered with the vt100 default.
def _drive_to_shell(con, budget):
    """Drive the PROM menu, log in as root, answer TERM, and switch to
    /bin/sh. Returns once a Bourne shell is confirmed responsive."""
    deadline = time.monotonic() + budget
    left = lambda cap: max(1.0, min(cap, deadline - time.monotonic()))

    con.expect(rb"Option\?", left(budget), what="the maintenance menu")
    con.send("1")
    con.expect(rb"login:", left(budget), what="the console login prompt")
    con.send("root")
    # First login sets TERM; accept the vt100 default. The prompt is
    # answered before the shell appears.
    con.expect(rb"TERM\s*=\s*\(", left(120), what="the TERM prompt")
    con.send("")
    # The login csh prints an "<hostname> <histnum># " prompt; the history
    # number makes it distinctive enough not to match boot text.
    con.expect(rb"[0-9]# ", left(120), what="the login csh prompt")
    # One Bourne idiom for the rest of the drive; a wide PATH so the
    # /usr/etc and /usr/bsd network tools are found by name.
    con.send("exec /bin/sh")
    con.run("PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; "
            "export PATH", "shinit", timeout=int(left(120)))


def _sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _host_cksum(path):
    """The POSIX cksum CRC and byte count of a file, computed by the host's
    own cksum(1) (coreutils' default algorithm is the POSIX CRC IRIX's
    cksum implements). Returns (crc, size) as (str, int)."""
    out = subprocess.run(["cksum", path], capture_output=True, text=True,
                         check=True).stdout
    crc, size = out.split()[0:2]
    return crc, int(size)


if QemuSystemTest is not None:

    class IrixEgNet(QemuSystemTest):
        """Boots installed IRIX 6.5.30 on the real PROM, brings up eg, pulls
        a payload over it by TFTP, and requires eg_net_verdict() to be PASS.
        See this module's docstring for the full contract."""

        def _asset_paths(self):
            assets_root = os.environ.get('ORIGAMI_ASSETS')
            if not assets_root:
                return None
            prom = os.path.join(assets_root, FIRMWARE_REL)
            disk = os.path.join(assets_root, DISK_REL)
            if not (os.path.isfile(prom) and os.path.isfile(disk)):
                return None
            return prom, disk

        def _read_ipkts(self, con, tag):
            """eg0's received-packet counter from netstat -i, or None if it
            could not be read. netstat -in columns are
            Name Mtu Network Address Ipkts ... so Ipkts is field 5."""
            try:
                return int(con.capture_value(
                    "netstat -in | awk '$1 ~ /^" + IFACE +
                    "/ {print \"IPKTS=\" $5; exit}'",
                    tag, rb"IPKTS=(\d+)", timeout=60))
            except _Harness:
                return None

        def _configure_eg(self, con):
            """Bring eg0 up with the SLIRP guest address on its own subnet
            (the TFTP server at GATEWAY is directly reachable, no route
            needed). Returns True if ifconfig reports it UP."""
            con.run("hinv | grep -i ether", "hinv", timeout=60)
            con.run("egconfig -f 1 " + IFACE, "egconfig", timeout=60)
            con.run(f"ifconfig {IFACE} inet {GUEST_ADDR} "
                    "netmask 0xffffff00 up", "ifup", timeout=60)
            try:
                con.capture_value("ifconfig " + IFACE, "ifshow",
                                 rb"(UP)[,>]", timeout=60)
                return True
            except _Harness:
                return False

        def _transfer(self, con, host_crc, host_size):
            """Fetch payload.bin over eg by TFTP and report the guest's own
            cksum and size of its copy."""
            leg = {"attempted": False, "reason": "not reached",
                  "host_cksum": host_crc, "host_bytes": host_size}
            con.run("rm -f /tmp/down.bin", "rmdown", timeout=60)
            try:
                con.run("(echo binary; echo 'get payload.bin /tmp/down.bin'; "
                        "echo quit) | tftp " + GATEWAY, "tftp",
                        timeout=STEP_TIMEOUT_SECONDS)
                guest_crc, guest_size = con.two_values(
                    "cksum /tmp/down.bin", "cksum",
                    rb"(\d+)\s+(\d+)\s+/tmp/down\.bin", timeout=120)
            except _Harness as e:
                leg["reason"] = str(e)
                return leg
            leg.update({"attempted": True, "guest_cksum": guest_crc,
                       "guest_bytes": guest_size})
            return leg

        @skipBigDataTest()
        @skipSlowTest()
        def test_irix_eg_net(self):
            paths = self._asset_paths()
            if paths is None:
                self.skipTest(
                    'ORIGAMI_ASSETS is not set, or has no PROM/disk at '
                    f'<ORIGAMI_ASSETS>/{FIRMWARE_REL} and '
                    f'<ORIGAMI_ASSETS>/{DISK_REL} -- point it at a checkout '
                    'with the real 6.5.30 PROM and an installed IRIX disk '
                    'provisioned to run this test')
            prom, disk = paths

            if _sha256_file(prom) != PROM_SHA256:
                self.fail('the 6.5.30 PROM SHA-256 does not match -- a stale '
                          'or wrong-release firmware')

            self.require_netdev('user')

            # The payload the SLIRP TFTP server serves, and the host's own
            # measure of it, computed before the guest ever fetches it.
            served_dir = self.scratch_file('served')
            os.makedirs(served_dir, exist_ok=True)
            payload = os.path.join(served_dir, 'payload.bin')
            with open(payload, 'wb') as f:
                f.write(os.urandom(PAYLOAD_BYTES))
            host_crc, host_size = _host_cksum(payload)

            pcap = self.log_file('eg-wire.pcap')

            vm = self.get_vm(name='irix_eg_net')
            vm.set_machine('origin200,synthesized-firmware=off,debug-leds=off')
            vm.set_console()
            elsc_log = self.log_file('elsc-aux.log')
            vm.add_args(
                '-accel', 'tcg,thread=single',
                '-cpu', 'R10000', '-smp', '1', '-m', str(MEMORY_MIB),
                '-bios', prom,
                '-audio', 'none',
                '-chardev', f'file,id=elsc,path={elsc_log}',
                '-serial', 'none',
                '-serial', 'chardev:elsc',
                '-drive', f'if=none,id=disk0,file={disk},format=qcow2',
                '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
                '-snapshot',
                '-nic', 'none',
                '-netdev', f'user,id=egnet,tftp={served_dir}',
                '-device', f'tigon,netdev=egnet,addr={EG_ADDR}',
                '-object', f'filter-dump,id=egcap,netdev=egnet,file={pcap}')
            vm.launch()

            log_path = self.log_file('irix-eg-net.log')
            con = _Console(vm.console_socket, log_path,
                          exited=lambda: vm.exitcode() is not None)
            login_reached = False
            eg_up = False
            ipkts_before = None
            ipkts_after = None
            transfer = {"attempted": False, "reason": "not reached",
                       "host_cksum": host_crc, "host_bytes": host_size}
            try:
                try:
                    _drive_to_shell(con, LOGIN_BUDGET_SECONDS)
                    login_reached = True
                    eg_up = self._configure_eg(con)
                    ipkts_before = self._read_ipkts(con, "ipktsbefore")
                    transfer = self._transfer(con, host_crc, host_size)
                    ipkts_after = self._read_ipkts(con, "ipktsafter")
                    con.send("sync; sync")
                    time.sleep(2)
                    con._pump()
                except _Harness as e:
                    if transfer.get("reason") == "not reached":
                        transfer["reason"] = str(e)
            finally:
                transcript = con.buf.decode("utf-8", errors="replace")
                con.close()
                vm.shutdown(hard=vm.exitcode() is not None)

            if not transcript:
                self.fail(
                    'the guest printed nothing at all -- a run that did not '
                    'start, not a boot that failed early. Check the emulator '
                    'built, and that nothing ended the machine before it '
                    'printed anything.')

            rx = {"ipkts_before": ipkts_before, "ipkts_after": ipkts_after}
            report = eg_net_verdict(login_reached, eg_up, transfer, rx)

            summary_path = self.log_file('irix-eg-net-summary.json')
            with open(summary_path, 'w', encoding='utf-8') as summary_file:
                json.dump({
                    'schema': 1,
                    'login_reached': login_reached,
                    'eg_up': eg_up,
                    'rx': rx,
                    'transfer': transfer,
                    'report': report,
                }, summary_file, indent=2, sort_keys=True, default=str)
                summary_file.write('\n')

            self.assertEqual(
                report['verdict'], 'PASS',
                'IRIX eg network transfer problems: ' +
                '; '.join(report['problems']) +
                f' (full report: {summary_path}, console log: {log_path})')


if __name__ == '__main__':
    if QemuSystemTest is not None:
        QemuSystemTest.main()
