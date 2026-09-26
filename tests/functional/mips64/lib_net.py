# The console driver the networked guest tests talk to a logged-in shell
# with, the ordered-progression scorer their verdicts are built from, and
# the wire-level DHCP counting that decides whether a lease really happened
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Every test here that moves bytes over an emulated adapter does the same
three things: drive a console to a shell, run commands through it with an
end marker so a reply can be told apart from the shell's echo of the
command, and decide afterwards whether the run got where it was supposed
to. Each of them grew its own copy of all three. This is that code, once.

Plain Python with no qemu_test dependency, the same rule lib_prom.py
follows: test_lib_net.py exercises it under a bare `python3` run.
"""

import re
import socket
import struct
import time


class HarnessError(Exception):
    """Something on the test's own side of the run went wrong -- a timeout
    waiting for the guest, or a value the guest never printed. A
    harness-level problem and a guest fault are different findings (see
    GuestFault), and collapsing them loses which side of the machine to
    look at first."""


class GuestFault(HarnessError):
    """The guest itself fell over -- its kernel debugger, a panic, or a
    bus error -- not a harness timeout. Unlike a step that merely returns
    the wrong value, this ends the whole run."""


# The three faults an OpenBSD/IP27 guest shows when it wedges rather than
# answers. Shared by the login, install and transfer runs, which all drive
# the same kernel.
OPENBSD_TRIPWIRES = (
    ("the guest fell into its kernel debugger", re.compile(r"[\r\n]+ddb>")),
    ("the guest panicked", re.compile(r"panic: ")),
    ("the guest took a bus error", re.compile(r"Trap cause = 7")),
)

# How much of the received buffer a pattern is matched against on each
# pass, so a chatty guest cannot make every search rescan the whole run.
_MATCH_WINDOW = 4096


class Console:
    """Send a line to the guest console, wait for a pattern in what comes
    back.

    A subclass names the faults it watches for in TRIPWIRES -- pairs of
    (compiled bytes regex, why) -- and the timeout its own steps default
    to in STEP_TIMEOUT. The base class watches for nothing, which is the
    right answer only for a caller that passes its own.
    """

    TRIPWIRES = ()
    STEP_TIMEOUT = 120

    def __init__(self, sock, log_path, exited=None):
        self.sock = sock
        self.sock.settimeout(1.0)
        self.buf = bytearray()
        self._log = open(log_path, "ab")
        # A zero-arg callable reporting whether the emulator process has
        # already exited (a real caller passes `lambda: vm.exitcode() is
        # not None`). Defaults to "never exited" so a caller with nothing
        # to poll -- a fixture, an offline check -- behaves exactly as if
        # this parameter did not exist, losing only the fast failure.
        self._exited = exited if exited is not None else (lambda: False)

    def _pump(self):
        """Read one chunk if the guest has sent one in the next second,
        appending it to the buffer and the log. Returns without blocking
        past that second either way.

        Raises HarnessError immediately, rather than returning quietly,
        when a read produces nothing AND the emulator process has already
        exited: once both are true there is nothing further this console
        will ever receive, so waiting out the rest of the caller's timeout
        burns the budget on a question already answered. Checked only
        after an empty read, never before one -- the process can exit
        between one chunk and the next, and bytes already in flight when
        it did must still be drained and matched.
        """
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
            raise HarnessError(
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
                    raise GuestFault(
                        f"{why} while waiting for {what or rx.pattern!r}")
            if time.monotonic() >= deadline:
                raise HarnessError(
                    f"timed out after {timeout}s waiting for "
                    f"{what or rx.pattern!r}")
            start = max(0, len(self.buf) - _MATCH_WINDOW)
            self._pump()

    def send(self, text):
        self.sock.sendall(text.encode() + b"\n")

    def _run_to_marker(self, cmd, tag, timeout):
        """Send a command with its own end marker and return (start of the
        command's own output, the marker match). The marker is split
        across a quote in the command so the shell's echo of the command
        line itself can never match it."""
        pos = len(self.buf)
        self.send(f'{cmd}; echo "MARK""-{tag}-$?"')
        end = self.expect(rb"MARK-" + tag.encode() + rb"-(\d+)", timeout,
                          what=f"{tag} to finish")
        return pos, end

    def run(self, cmd, tag, timeout=None):
        """Send a shell command and return its exit code."""
        _pos, end = self._run_to_marker(
            cmd, tag, self.STEP_TIMEOUT if timeout is None else timeout)
        return int(end.group(1))

    def two_values(self, cmd, tag, pattern, timeout=None):
        """Run a command and pull two values out of what it printed, from
        the same command. Two empty files share a checksum, so pulling the
        size alongside it is what catches a transfer that "succeeded" on
        nothing."""
        pos, end = self._run_to_marker(
            cmd, tag, self.STEP_TIMEOUT if timeout is None else timeout)
        rx = re.compile(pattern)
        m = rx.search(bytes(self.buf[pos:end.start()]))
        if not m:
            raise HarnessError(
                f"{tag} printed nothing matching {rx.pattern!r}")
        return m.group(1).decode(), int(m.group(2))

    def capture_value(self, cmd, tag, pattern, timeout=None):
        """Run a command and pull one value out of what it printed."""
        pos, end = self._run_to_marker(
            cmd, tag, self.STEP_TIMEOUT if timeout is None else timeout)
        rx = re.compile(pattern)
        m = rx.search(bytes(self.buf[pos:end.start()]))
        if not m:
            raise HarnessError(
                f"{tag} printed no value matching {rx.pattern!r}")
        return m.group(1).decode()

    def close(self):
        self._log.close()


class IrixConsole(Console):
    """Watch the PROM and IRIX guest for faults while driving a shell."""

    TRIPWIRES = (
        (re.compile(rb"PANIC"), "the guest panicked"),
        (re.compile(rb"Data bus error"), "the guest took a data bus error"),
        (re.compile(rb"\bException\b\s*<"), "the PROM took an exception"),
    )
    STEP_TIMEOUT = 300


def drive_irix_to_shell(con, budget):
    """Boot the installed IRIX disk through the PROM and enter /bin/sh."""
    deadline = time.monotonic() + budget

    def left(cap):
        return max(1.0, min(cap, deadline - time.monotonic()))

    con.expect(rb"Option\?", left(budget), what="the maintenance menu")
    con.send("1")
    con.expect(rb"login:", left(budget), what="the console login prompt")
    con.send("root")
    # The first login asks for TERM before displaying the csh prompt.
    con.expect(rb"TERM\s*=\s*\(", left(120), what="the TERM prompt")
    con.send("")
    con.expect(rb"[0-9]# ", left(120), what="the login csh prompt")
    con.send("exec /bin/sh")
    con.run("PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; "
            "export PATH", "shinit", timeout=int(left(120)))


def login_steps(netboot_steps, password, as_bytes=False):
    """The PROM netboot dialogue followed by the questions an OpenBSD/IP27
    kernel asks on its way to a root shell: the root device, the swap
    device's default, the login name, and the password.

    netboot_steps is lib_prom.prom_netboot_steps()'s own three steps, so
    the caller keeps naming its own boot file. as_bytes encodes each
    pattern, for a driver that compiles bytes patterns (Console.expect
    only compiles a pattern when it is bytes); the responses stay str
    either way.

    The swap-device answer is a bare newline taking the default, and is
    not separately asserted anywhere: reaching the login prompt already
    proves it was answered, since the kernel does not print "login:"
    without getting past it.
    """
    steps = tuple(netboot_steps) + (
        (r"root device\s*:", "sd0a\n"),
        (r"swap device[^:]*:", "\n"),
        (r"login:", "root\n"),
        (r"[Pp]assword:", password + "\n"),
    )
    if not as_bytes:
        return steps
    return tuple((pattern.encode(), response)
                 for pattern, response in steps)


def ordered_stages(transcript, stages, tripwires=()):
    """What is wrong with a transcript that was supposed to pass through
    stages in order: every trip wire that fired before the run got where
    it was going, then the name of the first stage it never reached.

    stages is a sequence of (problem message, compiled regex). Each stage
    is searched for only after the end of the one before it, so a marker
    that appears out of order does not count -- and only the FIRST stage
    not reached is reported, because everything after it is unreachable
    and naming all of them says nothing more.

    Trip wires are checked over everything before the last stage's own
    match began, or over the whole transcript when that stage was never
    reached: a wedge anywhere before real completion must fail, not only
    one that happens to sit between the last stage matched and the next.

    An empty list is a pass.
    """
    searched_from = 0
    last_start = None
    first_unreached = None
    for message, pattern in stages:
        match = pattern.search(transcript, searched_from)
        if match is None:
            first_unreached = message
            break
        searched_from, last_start = match.end(), match.start()

    trip_end = (last_start if last_start is not None and not first_unreached
                else len(transcript))
    problems = [why for why, rx in tripwires
                if rx.search(transcript, 0, trip_end)]
    if first_unreached:
        problems.append(first_unreached)
    return problems


# ---------------------------------------------------------------------------
# Wire-level DHCP counting: "dhclient printed an address" is a weaker
# claim than the four-message exchange that produced it, so a lease is
# judged from what crossed the wire, not from a guest command's exit code.

DHCP_NAMES = {1: "DHCPDISCOVER", 2: "DHCPOFFER", 3: "DHCPREQUEST",
              5: "DHCPACK", 6: "DHCPNAK", 7: "DHCPRELEASE"}

LEASE_HANDSHAKE = ("DHCPDISCOVER", "DHCPOFFER", "DHCPREQUEST", "DHCPACK")


def dhcp_kind(frame: bytes):
    """Name a single Ethernet frame's DHCP message type, if it has one.

    Requires an IPv4 frame (ethertype 0x0800) carrying UDP (protocol 17)
    between ports 67 and 68, containing the DHCP magic cookie followed by
    a type-53 option. Returns None for anything else, including a frame
    too short to hold a minimal DHCP packet.
    """
    if len(frame) < 44 or frame[12:14] != b"\x08\x00":
        return None
    ihl = (frame[14] & 0x0f) * 4
    if frame[23] != 17:                      # not UDP
        return None
    udp = 14 + ihl
    if len(frame) < udp + 8:
        return None
    sport = int.from_bytes(frame[udp:udp + 2], "big")
    dport = int.from_bytes(frame[udp + 2:udp + 4], "big")
    if {sport, dport} != {67, 68}:
        return None
    body = frame[udp + 8:]
    cookie = body.find(b"\x63\x82\x53\x63")
    if cookie < 0:
        return None
    i = cookie + 4
    while i < len(body):
        opt = body[i]
        if opt == 255:
            return None
        if opt == 0:
            i += 1
            continue
        if i + 1 >= len(body):
            return None
        length = body[i + 1]
        if opt == 53 and length >= 1:
            return DHCP_NAMES.get(body[i + 2])
        i += 2 + length
    return None


def summarise_pcap(raw: bytes):
    """Count the frames in a captured .pcap file's bytes, by who sent them
    and, for DHCP frames, by message type.

    Takes the raw bytes rather than a path so it can be exercised on a
    hand-built fixture without a capture file on disk. Returns None on an
    unrecognised magic number, {} on a file too short to hold even the
    24-byte global pcap header, otherwise {"by_src": {...}, "dhcp":
    {...}}.
    """
    if len(raw) < 24:
        return {}
    magic = raw[:4]
    if magic == b"\xa1\xb2\xc3\xd4":
        end = ">"
    elif magic == b"\xd4\xc3\xb2\xa1":
        end = "<"
    else:
        return None
    by_src, dhcp = {}, {}
    off = 24
    while off + 16 <= len(raw):
        _, _, incl, _ = struct.unpack(end + "IIII", raw[off:off + 16])
        off += 16
        frame = raw[off:off + incl]
        off += incl
        if len(frame) >= 12:
            src = ":".join("%02x" % b for b in frame[6:12])
            by_src[src] = by_src.get(src, 0) + 1
            kind = dhcp_kind(frame)
            if kind:
                dhcp[kind] = dhcp.get(kind, 0) + 1
    return {"by_src": by_src, "dhcp": dhcp}


def lease_established(dhcp_counts):
    """Whether all four of the lease's messages were seen at least once."""
    return all(dhcp_counts.get(name, 0) >= 1 for name in LEASE_HANDSHAKE)
