#!/usr/bin/env python3
#
# OpenBSD/IP27 network transfer: netboot the installed 6.9 disk through the
# stock PROM WITH a network back end, log in, lease an address by DHCP, and
# move a payload three ways over the guest's own network interface, each
# leg checked by SHA-256 and byte count
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Boot OpenBSD/sgi 6.9 through the stock IP27 PROM, require an authenticated
root shell, then check DHCP and three network transfers. The guest reads a
host HTTP payload, writes to a host TCP sink, and receives a forwarded-port
payload. Each transfer checks byte count and SHA-256 on both sides.

The test uses 128 MiB because the current Origin 200 model does not back the
installed guest's memory map correctly above that size. The installed disk
is opened with -snapshot. The network backend serves the kernel by TFTP and
carries the transfer traffic.

The ordered root-device, login, and shell checks use the same transcript as
the network checks, so both outcomes need one guest boot.
"""

import hashlib
import http.server
import json
import os
import re
import secrets
import socket
import socketserver
import threading
import time

from qemu_test import (QemuSystemTest, skipBigDataTest, skipIfMissingEnv,
                       skipSlowTest)
from qemu_test.utils import get_usernet_hostfwd_port

from lib_net import (LEASE_HANDSHAKE, OPENBSD_LOGIN_STAGES,
                     OPENBSD_TRIPWIRES, Console,
                     HarnessError, lease_established, login_steps,
                     ordered_stages, summarise_pcap)
from lib_prom import (FIRMWARE_REL, MACHINES, prom_netboot_steps,
                      require_assets)


# QEMU_TEST_SGI_ASSET_DIR points directly at the asset directory.
KERNEL_REL = os.path.join('openbsd', '6.9', 'bsd.IP27')
DISK_REL = os.path.join('openbsd', 'spike', 'root.img')

# The TFTP bootfile name the PROM's netboot command asks the SLIRP back
# end for -- test_xframe_pci.py's own identical constant, kept equal
# because both tests boot the same kernel through the same netboot path.
NETBOOT_BOOTFILE = 'bsd.IP27'

MEMORY_MIB = 128
ROOT_PASSWORD = "origami123"

# net-transfer-diag's own constants for the --origin case (project
# checkout): SLIRP's default subnet, its own gateway, and the port its
# inbound leg has the guest listen on.
GATEWAY = "10.0.2.2"
GUEST_ADDR = "10.0.2.15"
GUEST_LISTEN_PORT = 7000
IFACE = "iec0"
HOME = "/root"

# net-transfer-diag's own default payload size for each leg.
PAYLOAD_BYTES = 65536

PROM_STEPS = prom_netboot_steps(NETBOOT_BOOTFILE)
LOGIN_STEPS = login_steps(PROM_STEPS,
                          ROOT_PASSWORD, as_bytes=True)

# The DHCP lease and transfers have their own deadlines after login.
LOGIN_BUDGET_SECONDS = 840


class _Console(Console):
    """lib_net's console driver, watching for the three faults an
    OpenBSD/IP27 guest shows when it wedges rather than answers."""

    TRIPWIRES = tuple((re.compile(rx.pattern.encode()), why)
                      for why, rx in OPENBSD_TRIPWIRES)


# The three transfers, in the order they run and are reported.
LEG_NAMES = ("read", "write", "inbound")


def net_transfer_verdict(transcript, dhcp_counts, legs):
    """PASS iff the DHCP lease's four-message handshake was seen on the
    wire AND all three legs were attempted and match their recorded
    SHA-256 + byte count. FAIL if the lease never appears, any leg's
    digest or byte count mismatches, or a leg was never attempted at all
    (a missing guest tool, or the guest never reaching that point) --
    each is reported as its own distinct problem, never collapsed into a
    single "it did not work".

    transcript: the captured guest console text for the whole run (boot
      through the three legs). Scanned only for trip wires -- a guest
      that wedged partway through is a defect this must catch even if,
      by coincidence, every leg dict below still looks complete.
    dhcp_counts: a dict of DHCP message name -> count, as
      lib_net.summarise_pcap()'s own "dhcp" key reports.
    legs: a dict with keys "read", "write", "inbound". Each value is
      either falsy (the leg was never attempted) or a dict with keys
      "attempted" (bool), and when attempted, "host_sha256", "host_bytes",
      "guest_sha256", "guest_bytes" -- the digest and byte count computed
      independently by each side of the run, this Python process (host_*)
      and the guest's own shell (guest_*). Naming them by which side
      measured them, rather than by which one is "expected", is
      deliberate: for the read and inbound legs the host originates the
      payload and the guest's own report is what is being checked, but
      for the write leg it is the other way around -- the guest builds
      the payload and reports on it first, and the host's own receipt is
      what is being checked. A leg dict may also carry "exit_code" (int
      or None): when present and non-zero, the leg fails even if its
      digest and byte count happen to match, because ftp(1) exits 0
      having written a zero-length file if the server answers and says
      nothing, and nc(1) exits 0 when the connection is accepted and the
      payload is dropped. Absent or None means no exit-code check applies
      to that leg (the inbound leg's nc -l runs backgrounded and prints
      no exit status this harness waits on). A leg dict may also carry
      "reason" (str), used only for the problem message when the leg was
      not attempted.

    Returns {"verdict": "PASS"|"FAIL", "problems": [...], "lease_ok":
    bool, "legs_ok": {name: bool}}.
    """
    problems = ordered_stages(transcript, OPENBSD_LOGIN_STAGES)
    problems.extend(why for why, rx in OPENBSD_TRIPWIRES
                    if rx.search(transcript))

    lease_ok = lease_established(dhcp_counts)
    if not lease_ok:
        seen = sorted(name for name in LEASE_HANDSHAKE
                      if dhcp_counts.get(name))
        problems.append(
            "the DHCP lease was never acquired -- its four-message "
            "handshake (DHCPDISCOVER, DHCPOFFER, DHCPREQUEST, DHCPACK) "
            "did not all appear on the wire (saw: "
            + (", ".join(seen) if seen else "nothing") + ")")

    legs_ok = {}
    for name in LEG_NAMES:
        leg = legs.get(name)
        if not leg or not leg.get("attempted"):
            reason = (leg or {}).get("reason")
            problems.append(
                f"the guest never reached the {name} leg"
                + (f" ({reason})" if reason else ""))
            legs_ok[name] = False
            continue
        exit_code = leg.get("exit_code")
        exit_ok = exit_code is None or exit_code == 0
        digest_ok = leg["guest_sha256"] == leg["host_sha256"]
        size_ok = leg["guest_bytes"] == leg["host_bytes"]
        if not exit_ok:
            problems.append(
                f"{name} leg: its own command exited {exit_code}, not 0")
        if not digest_ok:
            problems.append(
                f"{name} leg: digest mismatch (host measured "
                f"{leg['host_sha256']}, guest reported "
                f"{leg['guest_sha256']})")
        if not size_ok:
            problems.append(
                f"{name} leg: byte count mismatch (host measured "
                f"{leg['host_bytes']}, guest reported "
                f"{leg['guest_bytes']})")
        legs_ok[name] = exit_ok and digest_ok and size_ok

    return {
        "verdict": "PASS" if not problems else "FAIL",
        "problems": problems,
        "lease_ok": lease_ok,
        "legs_ok": legs_ok,
    }


def _drive_login(con, budget):
    """Answer the kernel's device questions and log in on this console."""
    deadline = time.monotonic() + budget
    def left(cap):
        return max(1.0, min(cap, deadline - time.monotonic()))
    for pattern, response in LOGIN_STEPS[:len(PROM_STEPS)]:
        con.expect(pattern, left(budget), what=f"{pattern!r}")
        con.send(response.rstrip("\n"))
    root = con.expect(rb"root device\s*:|root on sd0a\b", left(budget),
                      what="root device selection")
    if root.group().startswith(b"root device"):
        con.send("sd0a")
        con.expect(rb"swap device[^:]*:", left(budget),
                   what="the swap device question")
        con.send("")
    for pattern, response in LOGIN_STEPS[len(PROM_STEPS):]:
        con.expect(pattern, left(budget), what=f"{pattern!r}")
        con.send(response.rstrip("\n"))
    con.expect(rb"# ", left(120), what="a root shell")


def _sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


class _QuietHTTPHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass


def _start_http(directory):
    """Serve @directory over HTTP on an OS-assigned loopback port. Ports
    net-transfer-diag's own start_http(): "NO DAEMON WE HAD TO WRITE" --
    a stock library HTTP server, fetched by the guest's own ftp(1)."""
    def handler(*args, **kwargs):
        return _QuietHTTPHandler(*args, directory=str(directory), **kwargs)
    srv = socketserver.ThreadingTCPServer(("127.0.0.1", 0), handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


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
    """Accept one connection and keep every byte of it. Ports
    net-transfer-diag's own Sink: deliberately not a protocol -- the
    guest opens a socket, writes a file and closes it."""

    allow_reuse_address = True
    daemon_threads = True

    def __init__(self, path):
        self.sink_path = path
        self.done = threading.Event()
        super().__init__(("127.0.0.1", 0), _SinkHandler)


class OpenbsdNetTransfer(QemuSystemTest):
    """Netboots the installed OpenBSD 6.9 disk through the stock PROM
    with a network back end, logs in, leases an address by DHCP, runs
    the three transfer legs, and requires net_transfer_verdict() to be
    PASS. See this module's own docstring for the full contract.
    """

    def _probe_tools(self, con):
        """Which of the guest programs the three legs need are
        actually present -- asked, not assumed. Ports net-transfer-diag's
        own probe: a missing program and a broken network fail
        identically further down, and the first ramdisk run of that
        tool reported an inbound-transfer failure for a guest that
        simply had no nc to listen with."""
        tools = {}
        for t in ("ftp", "nc", "sha256", "dd", "dhclient"):
            rc = con.run(f"command -v {t} >/dev/null 2>&1", f"have{t}")
            tools[t] = (rc == 0)
        return tools

    def _lease(self, con, tools):
        """Lease an address by DHCP; if that does not produce one,
        configure the static GUEST_ADDR by hand instead. Always
        returns the address now actually configured on IFACE.

        Ports net-transfer-diag's own --static fallback: without it,
        a guest whose lease failed is left with no address at all,
        so the three legs that follow would fail for a reason that
        has nothing to do with the network path they actually test --
        and a broken lease and a broken transfer would arrive as one
        indistinguishable red. With it, the legs still get a genuine
        attempt over the same address the lease would have produced
        (GUEST_ADDR is SLIRP's own default first-lease address, so
        this is not a different configuration, only a different way
        of reaching it) -- and the lease's own PASS/FAIL is decided
        later, off the wire capture (net_transfer_verdict()'s own
        job), never from this command's exit code or from which path
        configured the address (see that function's docstring for
        why: "dhclient printed an address" is a weaker claim than
        the exchange that produced it)."""
        leased = None
        if tools.get("dhclient"):
            try:
                rc = con.run(f"dhclient {IFACE}", "dhcp", timeout=240)
                if rc == 0:
                    leased = con.capture_value(
                        f"ifconfig {IFACE}", "leaseaddr",
                        rb"inet (\d+\.\d+\.\d+\.\d+)", timeout=60)
            except HarnessError:
                leased = None
        if leased:
            return leased
        try:
            con.run(f"ifconfig {IFACE} inet {GUEST_ADDR} "
                    f"netmask 255.255.255.0 up", "cfg", timeout=60)
            con.run(f"route -qn add default {GATEWAY} 2>/dev/null || "
                    f"route -qn change default {GATEWAY}", "route",
                    timeout=60)
        except HarnessError:
            pass
        return GUEST_ADDR

    def _read_leg(self, con, tools, http_port, down_hash):
        if not (tools.get("ftp") and tools.get("sha256")):
            missing = [t for t in ("ftp", "sha256") if not tools.get(t)]
            return {"attempted": False,
                   "reason": f"the guest has no {', '.join(missing)}"}
        try:
            rc = con.run(
                f"ftp -V -o {HOME}/down.bin "
                f"http://{GATEWAY}:{http_port}/payload.bin",
                "read", timeout=300)
            got, size = con.two_values(
                f"echo D $(sha256 -q {HOME}/down.bin) "
                f"$(wc -c <{HOME}/down.bin)",
                "readsum", rb"D ([0-9a-f]{64})\s+(\d+)", timeout=120)
        except HarnessError as e:
            return {"attempted": False, "reason": str(e)}
        # The host originates this payload (down_hash/PAYLOAD_BYTES,
        # computed before the guest ever fetches it); the guest's own
        # ftp+sha256 report on its local copy is what is being checked.
        # exit_code: net-transfer-diag's own read_leg() verdict is
        # `rc == 0 and <digest> and <size>`, never digest-and-size
        # alone -- ftp(1) can exit non-zero having still written
        # something matchable, and that must still fail the leg.
        return {"attempted": True, "host_sha256": down_hash,
               "host_bytes": PAYLOAD_BYTES, "guest_sha256": got,
               "guest_bytes": size, "exit_code": rc}

    def _write_leg(self, con, tools, sink, sink_port):
        needed = ("nc", "sha256", "dd")
        if not all(tools.get(t) for t in needed):
            missing = [t for t in needed if not tools.get(t)]
            return {"attempted": False,
                   "reason": f"the guest has no {', '.join(missing)}"}
        src = f"{HOME}/up.bin"
        kb = PAYLOAD_BYTES // 1024
        try:
            rc = con.run(
                f"dd if=/dev/urandom of={src} bs=1024 count={kb} "
                f"2>/dev/null || "
                f"dd if=/dev/arandom of={src} bs=1024 count={kb} "
                f"2>/dev/null || "
                f"dd if=/dev/random of={src} bs=1024 count={kb} "
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
        except HarnessError as e:
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
        # Reversed from the read leg: here the GUEST originates the
        # payload and reports on it first (up_hash/up_size, before
        # sending), and the host's own independent measurement of
        # what actually arrived (received_hash/received_size) is what
        # is being checked -- guest_sha256/guest_bytes always name
        # what the guest itself reported, host_sha256/host_bytes
        # always name what this Python process independently
        # measured, regardless of which side originated the payload
        # (see net_transfer_verdict()'s own docstring for why).
        # exit_code: net-transfer-diag's own write_leg() verdict is
        # `rc == 0 and <digest> and <size>`, never digest-and-size
        # alone -- see the read leg's own comment above.
        return {"attempted": True, "host_sha256": received_hash,
               "host_bytes": received_size, "guest_sha256": up_hash,
               "guest_bytes": up_size, "exit_code": rc}

    def _inbound_leg(self, con, tools, fwd_port, in_payload, in_hash):
        needed = ("nc", "sha256")
        if not all(tools.get(t) for t in needed):
            missing = [t for t in needed if not tools.get(t)]
            return {"attempted": False,
                   "reason": f"the guest has no {', '.join(missing)}"}
        con.send(f'(nc -l {GUEST_LISTEN_PORT} > {HOME}/in.bin; '
                 f'echo "I""N $(sha256 -q {HOME}/in.bin) '
                 f'$(wc -c <{HOME}/in.bin)") &')
        time.sleep(3)
        pushed = False
        last_err = None
        for _ in range(10):
            try:
                with socket.create_connection(
                        ("127.0.0.1", fwd_port), timeout=20) as s:
                    s.sendall(in_payload)
                    s.shutdown(socket.SHUT_WR)
                pushed = True
                break
            except OSError as e:
                last_err = e
                time.sleep(3)
        if not pushed:
            return {"attempted": False,
                   "reason": f"never got a connection through the "
                             f"forwarded port ({last_err})"}
        try:
            m = con.expect(rb"IN ([0-9a-f]{64})\s+(\d+)", 180,
                           what="the inbound digest")
        except HarnessError as e:
            return {"attempted": False, "reason": str(e)}
        # The host originates this payload (in_hash/PAYLOAD_BYTES,
        # computed before pushing it through the forwarded port); the
        # guest's own nc-l+sha256 report on what it received is what
        # is being checked.
        return {"attempted": True, "host_sha256": in_hash,
               "host_bytes": PAYLOAD_BYTES,
               "guest_sha256": m.group(1).decode(),
               "guest_bytes": int(m.group(2))}

    @skipBigDataTest()
    @skipSlowTest()
    @skipIfMissingEnv('QEMU_TEST_SGI_ASSET_DIR')
    def test_openbsd_net_transfer(self):
        assets = require_assets(self, FIRMWARE_REL, KERNEL_REL, DISK_REL)
        firmware, kernel = assets[FIRMWARE_REL], assets[KERNEL_REL]
        disk = assets[DISK_REL]

        self.require_netdev('user')

        # Content neither side can predict, so a hash that matches
        # cannot have matched by accident (net-transfer-diag's own
        # reasoning).
        served_dir = self.scratch_file('served')
        os.makedirs(served_dir, exist_ok=True)
        down_bytes = secrets.token_bytes(PAYLOAD_BYTES)
        with open(os.path.join(served_dir, 'payload.bin'), 'wb') as f:
            f.write(down_bytes)
        down_hash = _sha256_bytes(down_bytes)

        in_bytes = secrets.token_bytes(PAYLOAD_BYTES)
        in_hash = _sha256_bytes(in_bytes)

        received_path = self.scratch_file('received.bin')

        http_srv = _start_http(served_dir)
        http_port = http_srv.server_address[1]
        sink = _Sink(received_path)
        sink_port = sink.server_address[1]
        threading.Thread(target=sink.serve_forever, daemon=True).start()

        pcap_path = self.log_file('wire.pcap')

        vm = self.get_vm(name='openbsd_net_transfer')
        vm.set_machine(MACHINES['origin200'])
        vm.set_console()
        vm.add_args(
            '-m', str(MEMORY_MIB), '-snapshot',
            '-bios', firmware,
            '-drive', f'if=none,id=hd0,file={disk},format=raw',
            '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,drive=hd0',
            '-nic', ('user,id=n1,'
                     f'tftp={os.path.dirname(kernel)},'
                     f'bootfile={NETBOOT_BOOTFILE},'
                     'hostfwd=tcp:127.0.0.1:0-'
                     f'{GUEST_ADDR}:{GUEST_LISTEN_PORT}'),
            '-object', f'filter-dump,id=fd0,netdev=n1,file={pcap_path}')
        vm.launch()

        fwd_port = get_usernet_hostfwd_port(vm)
        self.assertIsNotNone(
            fwd_port, 'no forwarded port was reported by "info '
                      'usernet" -- the inbound leg has nowhere to '
                      'push through')

        log_path = self.log_file('openbsd-net-transfer.log')
        con = _Console(vm.console_socket, log_path,
                      exited=lambda: vm.exitcode() is not None)
        legs = {name: {"attempted": False, "reason": "not reached yet"}
               for name in ("read", "write", "inbound")}
        transcript = ""
        leased_addr = None
        try:
            try:
                _drive_login(con, LOGIN_BUDGET_SECONDS)
                tools = self._probe_tools(con)
                leased_addr = self._lease(con, tools)
                legs["read"] = self._read_leg(
                    con, tools, http_port, down_hash)
                legs["write"] = self._write_leg(con, tools, sink, sink_port)
                legs["inbound"] = self._inbound_leg(
                    con, tools, fwd_port, in_bytes, in_hash)
                con.send("halt -p")
                time.sleep(3)
                con._pump()
            except HarnessError as e:
                # A harness-level stop (a timeout, or a guest fault)
                # anywhere in the sequence above leaves every leg not
                # yet assigned still reading its initial "not reached
                # yet" placeholder, which is exactly correct: those
                # legs genuinely never ran.
                for name, leg in legs.items():
                    if leg.get("reason") == "not reached yet":
                        legs[name] = {"attempted": False, "reason": str(e)}
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

        report = net_transfer_verdict(transcript, dhcp_counts, legs)

        summary_path = self.log_file('net-transfer-summary.json')
        with open(summary_path, 'w', encoding='utf-8') as summary_file:
            json.dump({
                'schema': 1,
                'leased_addr': leased_addr,
                'dhcp_counts': dhcp_counts,
                'legs': legs,
                'report': report,
            }, summary_file, indent=2, sort_keys=True, default=str)
            summary_file.write('\n')

        self.assertEqual(
            report['verdict'], 'PASS',
            'OpenBSD network transfer problems: ' +
            '; '.join(report['problems']) +
            f' (full report: {summary_path}, console log: {log_path})')


if __name__ == '__main__':
    QemuSystemTest.main()
