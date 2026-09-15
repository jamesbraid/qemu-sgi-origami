#!/usr/bin/env python3
#
# xge(4) traffic: boot an OpenBSD/IP27 kernel that has the xge driver, bring
# xge0 up over the emulated Xframe adapter, lease an address by DHCP, and
# move a payload -- ASSET-GATED SCAFFOLD, skips cleanly until that kernel
# exists (see this file's own top-of-file comment below)
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
ASSET-GATED SCAFFOLD, NOT YET A RUNNABLE TEST.

What this needs: an OpenBSD/IP27 kernel build carrying `xge* at pci?` --
OpenBSD's driver for PCI vendor 0x17d5 product 0x5831, this fork's own
emulated Xframe 10G Ethernet adapter's identity (hw/net/xframe.c). Stock
OpenBSD 6.9's GENERIC-IP27 kernel -- the one test_openbsd_login.py,
test_openbsd_net_transfer.py, and this directory's other OpenBSD tests all
boot -- carries no such driver: test_xframe_pci.py proves, on a real
passing boot of that exact kernel, that it reports the device "not
configured" instead (test_xframe_pci_verdict.py's own docstring has the
full derivation). No kernel with xge(4) compiled in has been provisioned
into ORIGAMI_ASSETS as of this writing; KERNEL_REL/DISK_REL below name
where one would need to land.

This is a DROP-IN once that asset exists: xge_traffic_verdict()
(test_openbsd_xge_verdict.py, imported below) is a real, already-tested
pure function, not a stub -- see that module's own docstring for its full
contract and unit coverage. test_xge_traffic() below is real launch and
console-drive code too, patterned closely on test_openbsd_net_transfer.py's
own _drive_login()/_Console/leg-running shape (see this file's own inline
comments for exactly what is reused and what is simplified). The only
change needed to turn this into a real, running test is provisioning the
xge-enabled kernel and a disk with the usual guest tools (dhclient, nc,
sha256, dd) at the paths KERNEL_REL/DISK_REL name, or updating those
constants to wherever it lands -- nothing else here is a placeholder.
Until then, _asset_paths() resolves to None and test_xge_traffic()
skipTest()s before ever calling vm.launch(), the same way every other
asset-gated test in this directory behaves when ORIGAMI_ASSETS is unset or
incomplete.

Boots through the real stock IP27 PROM's own netboot path, the same
conversion test_openbsd_login.py and test_openbsd_net_transfer.py make:
_drive_netboot() drives lib_prom's prom_netboot_steps() through this
file's own _Console, ahead of _drive_login()'s existing login sequence,
over a second SLIRP usermode network on the onboard IOC3 -- separate
from the xframe adapter's own -netdev n1, the same split test_xframe_pci.py
keeps. Untested by the same asset gap as everything else in this file:
there is nothing to boot this against yet.

Scope: one transfer leg, not test_openbsd_net_transfer.py's three -- this
test's own job is proving xge(4) carries a payload correctly over this
fork's emulated adapter, not re-proving that three-leg shape, which
test_openbsd_net_transfer.py already covers for the onboard iec0 adapter.
The single leg here is a write leg: the guest generates content neither
side can predict, sends it to a host TCP sink over nc(1), and the host's
own independently-measured hash and byte count are what
xge_traffic_verdict() checks against what the guest itself reported --
same reasoning test_openbsd_net_transfer.py's own _write_leg() docstring
gives for checking a hash-and-size match rather than trusting an exit
code alone.
"""

import hashlib
import os
import re
import socket
import socketserver
import threading
import time

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)

from lib_prom import FIRMWARE_REL, prom_netboot_steps, resolve_asset

from test_openbsd_net_transfer_verdict import summarise_pcap

from test_openbsd_xge_verdict import TRIPWIRES, xge_traffic_verdict


# Where an xge-enabled kernel and disk would need to be provisioned into
# ORIGAMI_ASSETS -- these do NOT exist yet (see this module's own
# docstring). Distinct paths from test_openbsd_login.py's own
# assets/openbsd/6.9/ -- this is a different kernel build, not the stock
# one every other OpenBSD test here boots.
KERNEL_REL = os.path.join('assets', 'openbsd', 'xge', 'bsd.IP27')
DISK_REL = os.path.join('assets', 'openbsd', 'xge', 'root.img')

# The boot file this test's own SLIRP TFTP server would serve the kernel
# under, once it exists -- see prom_netboot_steps()'s own docstring for
# why this need not match the kernel's name on disk.
NETBOOT_BOOTFILE = 'bsd.IP27'
NETBOOT_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)

MEMORY_MIB = 128
ROOT_PASSWORD = "origami123"

IFACE = "xge0"
GATEWAY = "10.0.2.2"
HOME = "/root"
PAYLOAD_BYTES = 65536

# Same ordered device questions test_openbsd_login.py's own LOGIN_STEPS
# and test_openbsd_net_transfer.py's own LOGIN_STEPS answer, reproduced
# here rather than imported -- SELF-CONTAINED per the same brief those two
# files' own docstrings name, and the same duplication they already make
# of each other's identical sequence.
LOGIN_STEPS = (
    (rb"root device\s*:", "sd0a\n"),
    (rb"swap device[^:]*:", "\n"),
    (rb"login:", "root\n"),
    (rb"[Pp]assword:", ROOT_PASSWORD + "\n"),
)

# Not yet measured against a real boot of this not-yet-provisioned kernel
# (see this module's own docstring); reuses test_openbsd_login.py's own
# measured 840s login budget as a conservative starting point the same
# reasoning test_xframe_pci.py's own BOOT_TIMEOUT_SECONDS gives.
LOGIN_BUDGET_SECONDS = 840


class _Harness(Exception):
    """Something on this test's own side of the run went wrong -- a
    timeout, or a value the guest never printed. Same split
    test_openbsd_net_transfer.py's own _Harness/_GuestFault keeps (see
    that file's own docstring)."""


class _GuestFault(_Harness):
    """The guest itself fell over. Same split test_openbsd_net_transfer.py
    keeps."""


class _Console:
    """Send a line to the guest console, wait for a pattern in what comes
    back. Reproduced from test_openbsd_net_transfer.py's own _Console --
    SELF-CONTAINED per this port's brief, the same duplication that file
    already makes of test_openbsd_login.py's own console-driving idiom."""

    TRIPWIRES = tuple((rx, why) for why, rx in TRIPWIRES)

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

    def run(self, cmd, tag, timeout=120):
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        m = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                        what=f"{tag} to finish")
        return int(m.group(1))

    def two_values(self, cmd, tag, pattern, timeout=120):
        pos = len(self.buf)
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        end = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                          what=f"{tag} to finish")
        rx = re.compile(pattern)
        m = rx.search(bytes(self.buf[pos:end.start()]))
        if not m:
            raise _Harness(f"{tag} printed no digest and size matching "
                           f"{rx.pattern!r}")
        return m.group(1).decode(), int(m.group(2))

    def capture_value(self, cmd, tag, pattern, timeout=120):
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


def _drive_netboot(con, budget):
    """Answer the System Maintenance Menu and command monitor to netboot
    the kernel, the same three-step dialogue prom_netboot_steps() builds
    for every netboot test in this directory -- driven here through this
    file's own _Console rather than lib_prom.drive_console(), since this
    file's whole login/command drive is already its own console idiom
    (see this module's own docstring)."""
    deadline = time.monotonic() + budget
    left = lambda cap: max(1.0, min(cap, deadline - time.monotonic()))
    for pattern, response in NETBOOT_STEPS:
        con.expect(pattern.encode(), left(budget), what=pattern)
        con.send(response.rstrip("\n"))


def _drive_login(con, budget):
    deadline = time.monotonic() + budget
    left = lambda cap: max(1.0, min(cap, deadline - time.monotonic()))
    for pattern, response in LOGIN_STEPS:
        con.expect(pattern, left(budget), what=f"{pattern!r}")
        con.send(response.rstrip("\n"))
    con.expect(rb"# ", left(120), what="a root shell")


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class _SinkHandler(socketserver.BaseRequestHandler):
    def handle(self):
        with open(self.server.sink_path, "wb") as f:
            while True:
                chunk = self.request.recv(65536)
                if not chunk:
                    break
                f.write(chunk)
        self.server.done.set()


class _Sink(socketserver.ThreadingTCPServer):
    """Accept one connection and keep every byte of it. Reproduced from
    test_openbsd_net_transfer.py's own _Sink."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, path):
        self.sink_path = path
        self.done = threading.Event()
        super().__init__(("127.0.0.1", 0), _SinkHandler)


class OpenbsdXge(QemuSystemTest):
    """Boots an xge-enabled OpenBSD/IP27 kernel with the emulated xframe
    adapter attached, logs in, leases an address by DHCP on xge0, runs one
    write-style transfer leg, and requires xge_traffic_verdict() to be
    PASS. See this module's own docstring for the full contract and why
    this always skips today.
    """

    def _asset_paths(self):
        assets_root = os.environ['ORIGAMI_ASSETS']
        firmware = resolve_asset(assets_root, FIRMWARE_REL)
        kernel = resolve_asset(assets_root, KERNEL_REL)
        disk = resolve_asset(assets_root, DISK_REL)
        if not (firmware and kernel and disk):
            return None
        return firmware, kernel, disk

    def _write_leg(self, con, sink, sink_port):
        src = f"{HOME}/up.bin"
        kb = PAYLOAD_BYTES // 1024
        try:
            rc = con.run(
                f"dd if=/dev/urandom of={src} bs=1024 count={kb} "
                f"2>/dev/null || "
                f"dd if=/bsd of={src} bs=1024 count={kb} "
                f"skip=$(($$ % 512))",
                "mkup", timeout=180)
            if rc != 0:
                return {"attempted": False,
                       "reason": "the guest has nothing to build a "
                                 "payload from"}
            up_hash, up_size = con.two_values(
                f"echo U $(sha256 -q {src}) $(wc -c <{src})",
                "upsum", rb"U ([0-9a-f]{64})\s+(\d+)", timeout=120)
            if up_size != PAYLOAD_BYTES:
                return {"attempted": False,
                       "reason": f"the guest built a {up_size}-byte "
                                 f"payload, not {PAYLOAD_BYTES}"}
            rc = con.run(f"nc -N {GATEWAY} {sink_port} < {src}",
                         "write", timeout=300)
        except _Harness as e:
            return {"attempted": False, "reason": str(e)}
        if not sink.done.wait(60):
            return {"attempted": False,
                   "reason": "the host sink never saw a closed "
                             "connection"}
        received_size = (os.path.getsize(sink.sink_path)
                         if os.path.exists(sink.sink_path) else 0)
        received_hash = (_sha256_file(sink.sink_path)
                         if os.path.exists(sink.sink_path)
                         else "(nothing arrived)")
        return {"attempted": True, "host_sha256": received_hash,
               "host_bytes": received_size, "guest_sha256": up_hash,
               "guest_bytes": up_size, "exit_code": rc}

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('ORIGAMI_ASSETS')
    def test_xge_traffic(self):
        paths = self._asset_paths()
        if paths is None:
            # Deliberately still a skipTest, unlike every sibling
            # asset check in this directory (self.fail() when
            # ORIGAMI_ASSETS is set but the file is missing, since
            # that names a provisioning failure -- see those tests'
            # own commits). This one is an UNBUILT CAPABILITY, not a
            # provisioning failure: no xge-enabled OpenBSD/IP27 kernel
            # exists anywhere yet for the nightly to provision, so
            # ORIGAMI_ASSETS being set can never make the asset
            # appear. Failing loudly here would make this scaffold a
            # permanent required-check red until someone builds the
            # kernel -- a real engineering task, not a CI bug.
            self.skipTest(
                'no xge-enabled kernel/disk at '
                f'<ORIGAMI_ASSETS>/{KERNEL_REL} and '
                f'<ORIGAMI_ASSETS>/{DISK_REL} -- UNBUILT CAPABILITY, not '
                'a provisioning failure: this needs an OpenBSD/IP27 '
                'kernel build carrying "xge* at pci?" (OpenBSD\'s '
                'driver for PCI vendor 0x17d5 product 0x5831, this '
                'fork\'s own emulated Xframe adapter -- see this '
                'module\'s own docstring). Stock GENERIC-IP27 (the '
                'kernel every other OpenBSD test in this directory '
                'boots) has no such driver; test_xframe_pci.py proves '
                'that on a real passing boot. No such kernel has been '
                'built yet, so ORIGAMI_ASSETS being set does not make '
                'this a CI red the way it does for this directory\'s '
                'other asset-gated tests. Provision that kernel and a '
                'disk with the usual guest tools at the above paths '
                '(or point KERNEL_REL/DISK_REL there) to run this test')
        firmware, kernel, disk = paths

        self.require_netdev('user')

        received_path = self.scratch_file('received.bin')
        sink = _Sink(received_path)
        sink_port = sink.server_address[1]
        threading.Thread(target=sink.serve_forever, daemon=True).start()

        pcap_path = self.log_file('wire.pcap')

        vm = self.get_vm(name='openbsd_xge')
        vm.set_machine('origin200,debug-leds=off')
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-bios', firmware,
            '-nic', ('user,id=net0,'
                    f'tftp={os.path.dirname(kernel)},'
                    f'bootfile={NETBOOT_BOOTFILE}'),
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0',
            '-netdev', 'user,id=n1',
            '-device', ('xframe,addr=4.0,subsystem-vendor-id=0x10a9,'
                       'netdev=n1,mac=02:00:5d:00:00:01'),
            '-object', f'filter-dump,id=fd0,netdev=n1,file={pcap_path}')
        vm.launch()

        log_path = self.log_file('openbsd-xge.log')
        con = _Console(vm.console_socket, log_path,
                      exited=lambda: vm.exitcode() is not None)
        transfer = {"attempted": False, "reason": "not reached yet"}
        transcript = ""
        try:
            try:
                _drive_netboot(con, LOGIN_BUDGET_SECONDS)
                _drive_login(con, LOGIN_BUDGET_SECONDS)
                con.run(f"dhclient {IFACE}", "dhcp", timeout=240)
                transfer = self._write_leg(con, sink, sink_port)
                con.send("halt -p")
                time.sleep(3)
                con._pump()
            except _Harness as e:
                if transfer.get("reason") == "not reached yet":
                    transfer = {"attempted": False, "reason": str(e)}
        finally:
            transcript = con.buf.decode("utf-8", errors="replace")
            con.close()
            vm.shutdown(hard=vm.exitcode() is not None)

        if not transcript:
            self.fail(
                'the guest printed nothing at all -- a run that did '
                'not start, not a boot that failed early. Check the '
                'emulator built, and that nothing ended the machine '
                'before it printed anything.')

        try:
            with open(pcap_path, 'rb') as f:
                pcap_summary = summarise_pcap(f.read())
        except OSError:
            pcap_summary = None
        dhcp_counts = (pcap_summary or {}).get('dhcp', {})

        report = xge_traffic_verdict(transcript, dhcp_counts, transfer)
        self.assertEqual(
            report['verdict'], 'PASS',
            'xge traffic problems: ' + '; '.join(report['problems']) +
            f' (console log: {log_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
