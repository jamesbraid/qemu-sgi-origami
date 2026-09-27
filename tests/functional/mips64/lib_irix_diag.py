# The IRIX online-diagnostic matrix: which check runs on which IOC3 flavour,
# the manifest that says so, and the verdicts and parsers its boots use
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
irix-diag-matrix is a table with one row per machine flavour we model an
IOC3 on and one column per check. A cell is either `run` or the key of a
reason in REASONS saying why the flavour cannot run that check, so every
gap in coverage is written down rather than silently absent. The flavour's
launch arguments and the device names IRIX gives it on the diagnostics
disk live in FLAVOURS below; the manifest refers to them by row id.

Plain Python with no qemu_test dependency, the same rule lib_prom.py
follows: test_lib_irix_diag.py exercises the parser and the verdicts
under a bare python3.
"""

import calendar
import os
import re
import struct
import time
from dataclasses import dataclass, field
from pathlib import Path

import lib_origin300
from lib_matrix import MatrixError, matrix_rows
from lib_net import GuestFault, HarnessError
from lib_prom import strip_ansi, verdict


MANIFEST = Path(__file__).parent / 'irix-diag-matrix'

# Column order of irix-diag-matrix after its fixed fields, and the order a
# boot runs them in: shell checks first, then the diagnostics, then the
# checks that reboot and relaunch the machine.
CHECKS = (
    'prom',          # PROM power-on diagnostics and configuration
    'inventory',     # hinv lists the flavour's processors, memory, IOC3s
    'ethernet',      # each IOC3 Ethernet: station address and a ping
    'serial',        # each IOC3 serial port that is not the console
    'mediaio',       # the MediaIO IOC3's two serial ports
    'rad1',          # RAD1 playback to a host wav file
    'keyboard',      # host keystrokes read back from an IOC3 PS/2 port
    'scsi_card',     # an add-in SCSI card's channels in hinv and /hw
    'olrti',         # olrti's internal real-time interrupt loopback
    'olenet',        # olenet on the base IOC3's Ethernet
    'oldisk',
    'olmem',
    'oltape',
    'olcdrom',
    'reboot',        # an IRIX reboot restarts the PROM
    'warm_boot',     # IRIX boots again after it, and keeps nvram and time
    'persistence',   # nvram and the Timekeeper clock across a full stop
    'parallel',      # parallel-port numbering from a clean ioconfig
    'timer',         # no scheduler tick lost, over every boot of the row
)

RUN = 'run'

# Why a flavour cannot run a check. The manifest cites these by key.
REASONS = {
    'no-mediaio':
        'no MediaIO: its IOC3 serial ports and RAD1 are on the BaseIO-G '
        'daughterboard only',
    'no-keyboard':
        'no IOC3 on this flavour brings out a PS/2 keyboard',
    'no-scsi-card':
        'no add-in SCSI card in this composition',
    'no-olenet':
        'olenet is not in the field_diags_irix package on root-diags.qcow2',
    'no-parallel':
        'no IOC3 on this flavour brings out a parallel port',
    'no-parallel-connector':
        'the IO8 IOC3\'s parallel function has no rear-panel connector '
        '(007-4365-002) and IRIX lists no parallel port',
    'four-node-relaunch':
        'a reboot and relaunch measured 316s (carrier) and 622s (shoehorn) '
        'more on rows already near their budget; the one-node row covers the '
        'same BaseIO Timekeeper model, and no test covers a four-node '
        'warm reset through the routers',
    'parallel-own-history':
        'the disk was installed on an Origin 200, so its parallel-port '
        'history already names this port and clearing it renumbers nothing'
}

# The composed tape identity IRIX's driver recognizes as a DAT: the
# recorded real drive's own identity matches nothing in IRIX's table and
# the extended-status ioctl it needs refuses.
TAPE_VENDOR = 'ARCHIVE'
TAPE_PRODUCT = 'Python 25601'

# The physical DIP switches with switch 1 on and the rest off: the
# diagnostic level "None", which "performs no diagnostics, and the system
# will boot as fast as possible" (SGI HMQ-380-C, Origin hardware guide,
# MSC hardware debug switches, Table 53). A relaunch uses it; the first
# boot keeps the Normal level and checks its ALL DIAGS PASSED.
QUICK_PROM_SWITCHES = 1

_ID_RE = re.compile(r'^[a-z0-9_]+$')
_FIXED = ('id', 'cpus', 'mem', 'budget')

# The flavour field a check reads; a flavour without it cannot run it.
_NEEDS = {
    'ethernet': 'ethernets',
    'serial': 'serial_ports',
    'mediaio': 'mediaio_ports',
    'keyboard': 'keyboards',
    'scsi_card': 'scsi_card',
    'olrti': 'rti_args',
    'olenet': 'olenet_vertex',
    'parallel': 'parallel',
}


@dataclass(frozen=True)
class Row:
    id: str
    cpus: int
    mem: int
    # Seconds the whole row may take, from the test's start to its report.
    budget: int
    cells: dict = field(default_factory=dict)

    def runs(self, check):
        return self.cells[check] == RUN

    def reason(self, check):
        """REASONS text for a cell that does not run, else None."""
        cell = self.cells[check]
        return None if cell == RUN else REASONS[cell]


def parse_row(path, lineno, line, flavours=None):
    """One manifest line as a Row. Raises MatrixError naming path:lineno
    on a wrong field count, a bad value, an unknown reason key, a row id
    no flavour defines, or a check marked run that the flavour cannot
    run."""
    flavours = FLAVOURS if flavours is None else flavours
    where = f'{path}:{lineno}'
    raw = [f.strip() for f in line.split('|')]
    if len(raw) != len(_FIXED) + len(CHECKS):
        raise MatrixError(
            f'{where}: {len(raw)} fields, expected '
            f'{len(_FIXED) + len(CHECKS)}')
    values = dict(zip(_FIXED + CHECKS, raw))
    if not _ID_RE.match(values['id']):
        raise MatrixError(f'{where}: bad id {values["id"]!r}')
    flavour = flavours.get(values['id'])
    if flavour is None:
        raise MatrixError(f'{where}: no flavour named {values["id"]!r}')
    ints = {}
    for name in ('cpus', 'mem', 'budget'):
        try:
            ints[name] = int(values[name])
        except ValueError:
            raise MatrixError(
                f'{where}: {name} is not an integer: {values[name]!r}'
            ) from None
        if ints[name] <= 0:
            raise MatrixError(f'{where}: {name} must be positive')
    cells = {}
    for check in CHECKS:
        cell = values[check]
        if cell != RUN and cell not in REASONS:
            raise MatrixError(
                f'{where}: {check} is neither {RUN!r} nor a known reason: '
                f'{cell!r}')
        if cell == RUN and check in _NEEDS and \
                not getattr(flavour, _NEEDS[check]):
            raise MatrixError(
                f'{where}: {check} runs, but flavour {values["id"]!r} sets '
                f'no {_NEEDS[check]}')
        cells[check] = cell
    if cells['warm_boot'] == RUN and cells['reboot'] != RUN:
        raise MatrixError(f'{where}: warm_boot runs, but reboot does not')
    # The reboot and parallel checks are read back by the relaunch.
    for check in ('reboot', 'warm_boot', 'parallel'):
        if cells[check] == RUN and cells['persistence'] != RUN:
            raise MatrixError(
                f'{where}: {check} runs, but persistence, which relaunches '
                'the machine for it, does not')
    return Row(values['id'], ints['cpus'], ints['mem'], ints['budget'],
               cells)


def parse_manifest(path=MANIFEST, flavours=None):
    rows = []
    seen = set()
    for lineno, line in matrix_rows(path):
        row = parse_row(path, lineno, line, flavours)
        if row.id in seen:
            raise MatrixError(f'{path}:{lineno}: duplicate id {row.id!r}')
        seen.add(row.id)
        rows.append(row)
    return rows


# ---------------------------------------------------------------------------
# Running a row's checks.

class CheckFailed(Exception):
    """A check's own assertion did not hold."""


class OutOfTime(CheckFailed):
    """The row's budget ran out before a check could finish."""


class NoConsole(CheckFailed):
    """The machine the remaining checks need never reached a shell."""


# Checks that only read what the consoles already hold, so they still run
# after a stop: the lost-tick count then covers every boot the row got
# through, which is when a missed Compare match matters most.
OBSERVERS = ('timer',)


def run_checks(row, run_one, resync, now=time.monotonic):
    """Run each of row's checks in CHECKS order with run_one(check) and
    return {check: (status, detail, seconds)}.

    Status is pass, fail, n/a (with the manifest's reason) or not run.
    Every check runs even after another fails, except after a guest
    fault, a spent budget, a machine with no shell, or a timed-out
    command after which resync(check) reports the shell still busy: the
    checks after those are not run, and say why, except OBSERVERS."""
    results = {}
    stop = None
    for check in CHECKS:
        if not row.runs(check):
            results[check] = ('n/a', row.reason(check), 0)
            continue
        if stop is not None and check not in OBSERVERS:
            results[check] = ('not run', stop, 0)
            continue
        began = now()
        status, detail = 'pass', ''
        try:
            run_one(check)
        except (CheckFailed, HarnessError) as exc:
            status, detail = 'fail', str(exc)
            if isinstance(exc, GuestFault):
                stop = f'the guest faulted during {check}'
            elif isinstance(exc, OutOfTime):
                stop = f"the row's budget ran out during {check}"
            elif isinstance(exc, NoConsole):
                stop = f'no shell after {check}: {exc}'
            elif isinstance(exc, HarnessError) and not resync(check):
                stop = f'the shell stopped answering after {check} timed out'
        results[check] = (status, detail, round(now() - began, 1))
    return results


# ---------------------------------------------------------------------------
# The online diagnostics: what each prints on a pass, and its failure
# spellings. Each runs as its own shell command so each gets its own verdict
# and its own time.

@dataclass(frozen=True)
class Diag:
    assertions: tuple
    tripwires: tuple = ()


# Spellings any diagnostic's output must not contain.
GUEST_TRIPWIRES = (
    ('the guest panicked', 'PANIC'),
    ('the guest took a bus error', 'Bus error'),
    ('the guest took a data bus error', 'Data bus error'),
    ('the diagnostic dumped core', 'coredump'),
)

DIAGS = {
    # The disk exerciser's async-direct pattern pass over an 8 MiB file.
    'oldisk': Diag(
        (('oldisk async-direct pass',
          re.compile(r'RSLT\s+async-direct\s+PASS')),
         ('oldisk summary all passed',
          re.compile(r'META\s+TOTAL\s+1\s+0\b'))),
        (('oldisk async-direct failed', 'RSLT async-direct   FAIL'),
         ('pattern test failed', 'Patterntest Fail'))),
    # Tag RAM, L1, L2, address-pattern and moving-inversion memory.
    'olmem': Diag(
        (('olmem tagram pass', re.compile(r'RSLT\s+tagram\s+PASS')),
         ('olmem l1cache pass', re.compile(r'RSLT\s+l1cache\s+PASS')),
         ('olmem l2cache pass', re.compile(r'RSLT\s+l2cache\s+PASS')),
         ('olmem addr-pattern pass',
          re.compile(r'RSLT\s+addr-pattern\s+PASS')),
         ('olmem memaddr pass', re.compile(r'RSLT\s+memaddr\s+PASS')),
         ('olmem summary all passed',
          re.compile(r'META\s+TOTAL\s+5\s+0\b')))),
    # rw, thrash and block against the composed ARCHIVE DAT identity;
    # --tapemark --eot exclude the two subtests that need a second tape
    # node or a medium that ends.
    'oltape': Diag(
        (('oltape started', re.compile(r'Online Tape Test')),
         ('oltape rw pass', re.compile(r'RSLT\s+rw\s+PASS')),
         ('oltape thrash pass', re.compile(r'RSLT\s+thrash\s+PASS')),
         ('oltape block pass', re.compile(r'RSLT\s+block\s+PASS')),
         ('oltape summary all passed',
          re.compile(r'META\s+TOTAL\s+3\s+0\b'))),
        (('oltape rw failed', 'RSLT rw             FAIL'),
         ('oltape thrash failed', 'RSLT thrash         FAIL'),
         ('oltape block failed', 'RSLT block          FAIL'),
         ('the tape drive refused the extended-status request',
          'ioctl(mtiocgetext) failed'))),
    # Reads the generated data CD across the whole medium. olcdrom aborts
    # rather than printing RSLT FAIL when it cannot use the drive.
    'olcdrom': Diag(
        (('olcdrom started', re.compile(r'olcdrom version')),
         ('olcdrom pass', re.compile(r'RSLT\s+olcdrom\s+PASS')),
         ('olcdrom loop completed',
          re.compile(r'LOOP\s+Completed Loop\s+\d+\s+of\s+\d+'))),
        (('olcdrom failed', 'RSLT olcdrom        FAIL'),
         ('the CD-ROM drive was not ready', "doesn't appear ready"),
         ('the CD-ROM device would not open', "can't open device"),
         ('the CD-ROM medium carried no diagnostic data',
          "can't locate SGI diagnostic data"),
         ('a CD-ROM read failed', 'READ ERROR'))),
    # The internal loopback only; the external test needs a cable.
    'olrti': Diag(
        (('olrti internal pass', re.compile(r'RSLT\s+internal\s+PASS')),),
        (('olrti internal failed', 'RSLT internal       FAIL'),)),
    'olenet': Diag(
        (('olenet probe pass',
          re.compile(r'META\s+enet-probe\s+1\s+0\b')),
         ('olenet info pass', re.compile(r'META\s+enet-info\s+1\s+0\b')),
         ('olenet summary all passed',
          re.compile(r'META\s+TOTAL\s+2\s+0\b')))),
}


def diag_problems(name, output):
    """What is wrong with one diagnostic's output: each pass line it lacks,
    as "missing: <line>", and each failure spelling it contains, as
    "tripwire: <why>". Empty on a pass."""
    diag = DIAGS[name]
    return [problem if problem.startswith('tripwire: ')
            else f'missing: {problem}'
            for problem in verdict(strip_ansi(output), diag.assertions,
                                   diag.tripwires + GUEST_TRIPWIRES)]


# ---------------------------------------------------------------------------
# Parsers for shell output.

_ANSI_RE = re.compile(r'\x1b\[[0-9;]*m')

# ls -l of a character device: permissions, links, owner, group,
# "major, minor", date (three fields), path.
_DEVICE_LINE_RE = re.compile(
    r'^c\S+\s+\d+\s+\S+\s+\S+\s+(\d+),\s*(\d+)\s+\S+\s+\S+\s+\S+\s+(/\S+)'
    r'\s*$', re.MULTILINE)

# ls -l of a symbolic link: its name and target.
_LINK_LINE_RE = re.compile(r'^l\S+\s.*\s(\S+) -> (\S+)\s*$', re.MULTILINE)

_GUEST_DATE_RE = re.compile(r'DATE=(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)')
GUEST_DATE_FMT = '%Y-%m-%d %H:%M:%S'


def text(data):
    """Console bytes as text with colour escapes and carriage returns
    removed. IRIX ends a kernel warning with \\n\\r, so the line after one
    would otherwise start with a carriage return."""
    return _ANSI_RE.sub('', bytes(data).decode('utf-8', errors='replace')
                        ).replace('\r', '')


def device_numbers(listing):
    """{path: (major, minor)} for every character device in ls -l
    output."""
    return {path: (int(major), int(minor))
            for major, minor, path in _DEVICE_LINE_RE.findall(listing)}


def link_targets(listing):
    """{name: target} for every symbolic link in ls -l output."""
    return dict(_LINK_LINE_RE.findall(listing))


def processor_count(hinv):
    """How many processors hinv lists: one "Processor N:" line per CPU,
    or a single "N <speed> MHZ IPnn Processors" line when all are
    alike."""
    summary = re.search(r'(?m)^(\d+) \d+ MHZ IP\d+ Processors', hinv)
    if summary:
        return int(summary.group(1))
    return len(set(re.findall(r'(?m)^Processor (\d+):', hinv)))


def od_bytes(listing):
    """The hex byte tokens od -An -tx1 printed, in order. Only lines made
    of byte tokens count, so a kernel warning printed in between adds
    nothing."""
    return [token for line in listing.splitlines()
            if re.fullmatch(r'\s*(?:[0-9a-f]{2}\s*)+', line)
            for token in line.split()]


def guest_date(output):
    """Seconds since the epoch from the DATE= line date -u printed."""
    match = _GUEST_DATE_RE.search(output)
    if match is None:
        return None
    return calendar.timegm(time.strptime(match.group(1), GUEST_DATE_FMT))


@dataclass(frozen=True)
class ClockReading:
    """A guest date and the host times just before the command was sent
    and just after its answer arrived."""
    guest: int
    sent: float
    received: float


def clock_error(before, after):
    """How far the guest clock's advance between two readings is from the
    host's, and the largest error an exact clock could still show.

    The guest printed each whole-second date at some host instant between
    sending and receiving, and the kernel took its time from the
    Timekeeper at boot, also to a whole second. So an exact clock can
    differ from the host by each reading's round trip plus a second of
    date resolution per reading plus a second for the boot-time read."""
    host = (after.sent + after.received) / 2 - \
        (before.sent + before.received) / 2
    error = (after.guest - before.guest) - host
    bound = ((before.received - before.sent) +
             (after.received - after.sent)) / 2 + 3
    return error, bound


def command_output(con, cmd, tag, timeout=None):
    """Run cmd on a lib_net console and return what it printed after the
    shell's echo of it, up to the end marker, and its exit status."""
    pos = len(con.buf)
    status = con.run(cmd, tag, timeout)
    body = text(con.buf[pos:])
    body = body.split('\n', 1)[1] if '\n' in body else ''
    # The root prompt can land in front of an output line.
    return re.sub(r'(?m)^# ', '', body), status


def drive_to_shell(con, budget, phases, menu=True, start=0):
    """Answer the System Maintenance Menu with Start System, log in as
    root and switch to /bin/sh. Returns the PROM's output up to the menu,
    and appends (phase, seconds) to phases.

    A power-on boot must reach the menu. With menu False, a PROM whose
    NVRAM says to autoboot may skip it: the login prompt then ends the PROM
    output instead. start is the console offset the boot began at, so a
    boot after a reboot cannot match the previous boot's prompts."""
    began = time.monotonic()
    deadline = began + budget

    def left(cap):
        return max(1.0, min(cap, deadline - time.monotonic()))

    def mark(name, since):
        now = time.monotonic()
        phases.append((name, round(now - since, 1)))
        return now

    first = con.expect(
        re.compile(rb'Option\?' if menu else rb'Option\?|console login:'),
        left(budget), what='the maintenance menu', start=start)
    prom = text(con.buf[start:first.start()])
    pos = first.end()
    if first.group(0) == b'Option?':
        at = mark('PROM power-on to menu', began)
        con.send('1')
        pos = con.expect(rb'login:', left(budget),
                         what='the console login prompt', start=pos).end()
        at = mark('IRIX kernel and init to login', at)
    else:
        at = mark('PROM autoboot, kernel and init to login', began)
    con.send('root')
    pos = con.expect(rb'TERM\s*=\s*\(', left(120), what='the TERM prompt',
                     start=pos).end()
    con.send('')
    # The login csh prompt is "<hostname> <history number># ".
    con.expect(rb'[0-9]# ', left(120), what='the login csh prompt',
               start=pos)
    con.send('exec /bin/sh')
    con.run('PATH=/usr/sbin:/usr/bin:/sbin:/bin:/usr/etc:/usr/bsd:$PATH; '
            'export PATH', f'shinit{start}', timeout=int(left(120)))
    mark('login to root shell', at)
    return prom


def wav_nonzero_samples(data):
    """How many 16-bit sample words after the 44-byte header are not zero.
    QEMU's wav backend leaves the header's lengths at zero, so the data is
    read from byte 44 to the end."""
    data = data[44:]
    count = len(data) // 2
    return sum(1 for word in struct.unpack(f'<{count}h', data[:count * 2])
               if word)


# ---------------------------------------------------------------------------
# The flavours.

@dataclass(frozen=True)
class Ethernet:
    """An IOC3 Ethernet: the interface IRIX names it, the explicit station
    address QEMU gives it (QEMU never rewrites an explicit mac=, so the
    address netstat reports can only have come through the model), and
    the user-mode network behind it as its first three octets."""
    ifname: str
    mac: str
    subnet: str


@dataclass(frozen=True)
class SerialPort:
    """An IOC3 serial port that is not the console: the /dev name the disk
    gives it, the hardware-graph vertex it must be, and either the QEMU
    serial line or the -global property that carries it."""
    name: str
    dev: str
    vertex: str
    line: int = None
    prop: str = None


@dataclass(frozen=True)
class Keyboard:
    """An IOC3 PS/2 keyboard: its controller's QOM path, the PCI vertex
    whose pckb it is, and the input device IRIX names for it."""
    qom: str
    card: str
    hw_dir: str
    dev_dir: str


@dataclass(frozen=True)
class ScsiCard:
    """An add-in SCSI card: the hinv spelling of its channels, the
    controller numbers IRIX gives them, and the vertex they sit under."""
    hinv: str
    controllers: tuple
    vertex: str


@dataclass(frozen=True)
class Flavour:
    title: str
    machine: str
    nodes: int
    firmware: str = os.path.join('assets', 'firmware', '6.5.30',
                                 'ip27prom.payload.bin')
    disk: str = os.path.join('assets', 'irix', '6.5.30', 'root-diags.qcow2')
    cpu: str = 'R10000'
    # hinv -v lines, as (what, regex).
    hinv_facts: tuple = ()
    ethernets: tuple = ()
    serial_ports: tuple = ()
    mediaio_ports: tuple = ()
    keyboards: tuple = ()
    scsi_card: ScsiCard = None
    # olrti's argument naming the IOC3 each loopback runs on.
    rti_args: tuple = ()
    olenet_vertex: str = None
    # olmem tests 80% of free memory unless MEM=<MB> bounds it
    # (108-0286-007, Table 2-3).
    olmem_args: str = ''
    tape_dev: str = '/dev/rmt/tps1d5nr'
    cd_dev: str = '/dev/scsi/sc1d6l0'
    cd_hinv: str = r'CDROM: unit 6 on SCSI controller 1\b'
    prom_banner: bytes = rb'IP27 PROM SGI Version'
    # (a stale entry the disk carries, the live entry IRIX records, the
    # port IRIX names once both are gone).
    parallel: tuple = None
    extra_args: tuple = ()
    # The machine takes elsc-dip-switches (Origin 200 and 2000 only).
    dip_switches: bool = True
    origin300: bool = False


def prom_facts(flavour, row, diags=True):
    """What the PROM's configuration summary must report, as (what,
    regex). diags False leaves out the diagnostics verdict: after IRIX
    reboots the machine the PROM prints "Skipping diags as requested by
    kernel" and "NO DIAGS WERE RUN!" instead."""
    facts = (
        ('the processors and nodes',
         rf'\b{row.cpus} CPUs on {flavour.nodes} nodes found'),
        ('the enabled nodes',
         rf'No\. of NODEs enabled\s*=\s*{flavour.nodes}\b'),
        ('the enabled processors',
         rf'No\. of CPUs enabled\s*=\s*{row.cpus}\b'),
        ('the enabled memory', rf'Mem enabled\s*=\s*{row.mem} MB'),
    )
    if diags:
        facts += (('every power-on diagnostic passing', r'ALL DIAGS PASSED'),)
    return facts


_O200 = '/hw/module/1/slot/MotherBoard/node/xtalk/8/pci'
_O200_2 = '/hw/module/2/slot/MotherBoard/node/xtalk/8/pci'
_IO6 = '/hw/module/1/slot/io1/baseio/pci'
_IO8 = '/hw/module/001c01/Ibrick/xtalk/15/pci'
_SHOEHORN = '/hw/module/1/slot/io2/xtalk_pci/pci'
_CARRIER = '/hw/module/1/slot/io2/pci_xio/pci'

# The plain BaseIO's IOC3: Ethernet at PCI device 2, its serial ports and
# real-time interrupt under pci/4.
_IO6_FACTS = (
    ('BaseIO IOC3 serial port A', r'IOC3/IOC4 serial port: tty1\b'),
    ('BaseIO IOC3 serial port B', r'IOC3/IOC4 serial port: tty2\b'),
    ('BaseIO IOC3 Ethernet',
     r'Integral Fast Ethernet: ef0, .*slot io1, pci 2\b'),
    ('BaseIO IOC3 real-time interrupt',
     r'IOC3/IOC4 external interrupts: 1\b'),
)

_TWO_ETHERNETS = (Ethernet('ef0', '08:00:69:0e:c6:01', '10.0.2'),
                  Ethernet('ef1', '08:00:69:0e:c6:02', '10.0.3'))

_O200_BOARD_FACTS = (
    ('board IOC3 serial ports', r'IOC3/IOC4 serial port: tty1\b'),
    ('board IOC3 serial port B', r'IOC3/IOC4 serial port: tty2\b'),
    ('board IOC3 Ethernet',
     r'Integral Fast Ethernet: ef0, .*module 1, slot MotherBoard, pci 2\b'),
    ('board IOC3 parallel port', r'IOC3 parallel port: plp1\b'),
    ('board IOC3 real-time interrupt', r'IOC3/IOC4 external interrupts: 1\b'),
)

FLAVOURS = {
    'origin200': Flavour(
        title='Origin 200, one chassis: the IP29 system board IOC3',
        machine='origin200,debug-leds=off', nodes=1,
        hinv_facts=_O200_BOARD_FACTS,
        ethernets=(Ethernet('ef0', '08:00:69:0e:c6:01', '10.0.2'),),
        serial_ports=(SerialPort('ioc3-b', '/dev/ttyd2', f'{_O200}/3/tty/2/d',
                                 line=1),),
        keyboards=(Keyboard('/machine/node0/baseio/ioc3/kbc', f'{_O200}/3',
                            '/hw/input', '/dev/input'),),
        rti_args=(f'-vertex {_O200}/3',),
    ),
    'origin200_dual': Flavour(
        title='Origin 200, two chassis: an IP29 system board IOC3 in each',
        machine='origin200,topology=origin200-dual,debug-leds=off', nodes=2,
        hinv_facts=_O200_BOARD_FACTS + (
            ('second chassis IOC3 serial ports',
             r'IOC3/IOC4 serial port: tty3\b'),
            ('second chassis IOC3 serial port B',
             r'IOC3/IOC4 serial port: tty4\b'),
            ('second chassis IOC3 Ethernet',
             r'Fast Ethernet: ef1, .*module 2, slot MotherBoard, pci 2\b'),
            ('second chassis IOC3 parallel port',
             r'IOC3 parallel port: plp2\b'),
            ('second chassis IOC3 real-time interrupt',
             r'IOC3/IOC4 external interrupts: 2\b'),
        ),
        ethernets=(Ethernet('ef0', '08:00:69:0e:c6:01', '10.0.2'),
                   Ethernet('ef1', '08:00:69:0e:c6:02', '10.0.3')),
        # Serial lines are six per node: port A, port B, the system
        # controller, the bring-up UART and two record lines.
        serial_ports=(
            SerialPort('ioc3-b', '/dev/ttyd2', f'{_O200}/3/tty/2/d', line=1),
            SerialPort('chassis2-a', '/dev/ttyd3', f'{_O200_2}/3/tty/1/d',
                       line=6),
            SerialPort('chassis2-b', '/dev/ttyd4', f'{_O200_2}/3/tty/2/d',
                       line=7)),
        keyboards=(
            Keyboard('/machine/node0/baseio/ioc3/kbc', f'{_O200}/3',
                     '/hw/input', '/dev/input'),
            Keyboard('/machine/node1/baseio/ioc3/kbc', f'{_O200_2}/3',
                     '/hw/input1', '/dev/input1')),
        rti_args=(f'-vertex {_O200}/3', f'-vertex {_O200_2}/3'),
    ),
    'origin2000_one_node': Flavour(
        title='Origin 2000 one-node module: the BaseIO-G, with MediaIO',
        machine='origin2000,topology=origin2000-one-node,debug-leds=off',
        nodes=1,
        hinv_facts=(
            ('board IOC3 serial ports', r'IOC3/IOC4 serial port: tty1\b'),
            ('board IOC3 serial port B', r'IOC3/IOC4 serial port: tty2\b'),
            ('MediaIO serial port A', r'IOC3/IOC4 serial port: tty3\b'),
            ('MediaIO serial port B', r'IOC3/IOC4 serial port: tty4\b'),
            ('board IOC3 Ethernet',
             r'Integral Fast Ethernet: ef0, .*slot io1, pci 2\b'),
            ('MediaIO parallel port', r'IOC3 parallel port: plp\d'),
            ('RAD1', r'Iris Audio Processor: version RAD'),
            ('board IOC3 real-time interrupt',
             r'IOC3/IOC4 external interrupts: 1\b'),
            # IRIX 6.5.30 prints the PCI identity in hex; the inventory
            # the BaseIO datasheet cites, from another release, prints
            # "vendor 4265, device 5) pci slot 7".
            ('RAD1 at PCI slot 7',
             r'vendor 0x10a9, device 0x0005\) PCI slot 7\b'),
        ),
        ethernets=(Ethernet('ef0', '08:00:69:0e:c6:01', '10.0.2'),),
        serial_ports=(
            SerialPort('ioc3-b', '/dev/ttyd2', f'{_IO6}/4/tty/2/d', line=1),),
        mediaio_ports=(
            SerialPort('mio-a', '/dev/ttyd3', f'{_IO6}/6/tty/1/d',
                       prop='sgi-xio-baseio.mio-chardev-a'),
            SerialPort('mio-b', '/dev/ttyd4', f'{_IO6}/6/tty/2/d',
                       prop='sgi-xio-baseio.mio-chardev-b')),
        keyboards=(
            Keyboard('/machine/module0/slot8/baseio/ioc3/kbc', f'{_IO6}/4',
                     '/hw/input', '/dev/input'),
            Keyboard('/machine/module0/slot8/baseio/mio-ioc3/kbc',
                     f'{_IO6}/6', '/hw/input1', '/dev/input1')),
        rti_args=(f'-vertex {_IO6}/4',),
        # Two processors over 256 MB: the default size took 920 to 1050s.
        # The PROM's power-on memory test still covers all of it.
        olmem_args='MEM=128',
        # The disk carries a parallel entry from an Origin 200 install at a
        # hardware-graph path this machine does not have, which holds unit
        # 1; IRIX records the MediaIO port behind it.
        parallel=(r'(?m)^1 \S+/MotherBoard/\S+/ecpp\s*$',
                  rf'(?m)^\d+ {_IO6}/6/ecpp\s*$', 'plp1'),
    ),
    'origin2000_pci_shoehorn': Flavour(
        title='Origin 2000 four-node module: the plain BaseIO, and a Cad '
              'Duo IOC3 on a PCI shoehorn in IO2',
        machine='origin2000,topology=origin2000-pci-shoehorn,'
                'debug-leds=off',
        nodes=4,
        hinv_facts=_IO6_FACTS + (
            ('Cad Duo Ethernet at io2, pci 1',
             r'Fast Ethernet: ef1, .*slot io2, pci 1\b'),
        ),
        ethernets=_TWO_ETHERNETS,
        serial_ports=(
            SerialPort('ioc3-b', '/dev/ttyd2', f'{_IO6}/4/tty/2/d', line=1),),
        keyboards=(
            Keyboard('/machine/module0/slot12/cad-duo/kbc', f'{_SHOEHORN}/1',
                     '/hw/input1', '/dev/input1'),),
        # The Cad Duo has no serial ports and no real-time interrupt.
        rti_args=(f'-vertex {_IO6}/4',),
        # Bounded like the one-node's, to fit the row in a nightly job.
        olmem_args='MEM=128',
    ),
    'origin2000_pci_carrier': Flavour(
        title='Origin 2000 four-node module: the plain BaseIO, and an '
              'ISP12160 and a Cad Duo IOC3 on a PCI carrier in IO2',
        machine='origin2000,topology=origin2000-pci-carrier,debug-leds=off',
        nodes=4,
        hinv_facts=_IO6_FACTS + (
            ('Cad Duo Ethernet at io2, pci 1',
             r'Fast Ethernet: ef1, .*slot io2, pci 1\b'),
        ),
        ethernets=_TWO_ETHERNETS,
        serial_ports=(
            SerialPort('ioc3-b', '/dev/ttyd2', f'{_IO6}/4/tty/2/d', line=1),),
        keyboards=(
            Keyboard('/machine/peripheral/carrier-ioc3/kbc', f'{_CARRIER}/1',
                     '/hw/input1', '/dev/input1'),),
        # The Cad Duo has no serial ports and no real-time interrupt.
        rti_args=(f'-vertex {_IO6}/4',),
        # Bounded like the one-node's, to fit the row in a nightly job.
        olmem_args='MEM=128',
        scsi_card=ScsiCard(r'SCSI controller (\d+): Version QL12160',
                           ('2', '3'), f'{_CARRIER}/0'),
        # The carrier's slots are empty; fit an ISP12160 and a Cad Duo,
        # the card the shoehorn carries, by its placeholder subsystem id
        # (docs/datasheets/sgi-cad-duo-pci-card.md in the project
        # checkout).
        extra_args=(
            '-device', 'qlogic-isp12160,bus=pci.carrier1,addr=0',
            '-device', 'sgi-ioc3,id=carrier-ioc3,bus=pci.carrier1,addr=1,'
            'subsystem-id=0xffff'),
    ),
    'origin300': Flavour(
        title='Origin 300: the IO8 IOC3',
        machine='origin300', nodes=1, cpu='R14000',
        firmware=os.path.join('assets', 'firmware', '6.5.30',
                              'ip35prom.payload.bin'),
        disk=os.path.join('assets', 'irix', '6.5.30',
                          'root-diags-origin300.qcow2'),
        hinv_facts=(
            ('IOC3 serial port A', r'IOC3/IOC4 serial port: tty3\b'),
            ('IOC3 serial port B', r'IOC3/IOC4 serial port: tty4\b'),
            ('IOC3 Ethernet',
             r'Integral Fast Ethernet: ef0, .*module 001c01, pci 4\b'),
            ('IOC3 real-time interrupt',
             r'IOC3/IOC4 external interrupts: 1\b'),
        ),
        # The IO8 has no Ethernet-address part; the address comes from the
        # brick's board record, which lib_origin300 writes with this one.
        ethernets=(Ethernet('ef0', '08:00:69:12:34:56', '10.0.2'),),
        # Line 0 is the L1, the system console; lines 1 and 2 are the
        # IOC3's ports A and B.
        serial_ports=(
            SerialPort('ioc3-a', '/dev/ttyd3', f'{_IO8}/4/tty/1/d', line=1),
            SerialPort('ioc3-b', '/dev/ttyd4', f'{_IO8}/4/tty/2/d', line=2)),
        rti_args=(f'-v {_IO8}/4',),
        olenet_vertex=f'{_IO8}/4',
        # Two processors over 512 MB: the default size took 1030 to 1250s.
        # The PROM's power-on memory test still covers all of it.
        olmem_args='MEM=128',
        prom_banner=rb'IP35 PROM SGI Version|Starting PROM Boot process',
        dip_switches=False, origin300=True,
    ),
}


def _serial_args(flavour, paths):
    """-serial options for the flavour's ports, with null for the lines in
    between. Line 0 is the console, which the framework owns."""
    by_line = {port.line: port for port in flavour.serial_ports
               if port.line is not None}
    argv = []
    for line in range(1, max(by_line, default=0) + 1):
        port = by_line.get(line)
        if port is None:
            argv += ['-serial', 'null']
        else:
            argv += ['-chardev',
                     f'file,id={port.name},path={paths["ports"][port.name]}',
                     '-serial', f'chardev:{port.name}']
    for port in flavour.mediaio_ports:
        argv += ['-chardev',
                 f'file,id={port.name},path={paths["ports"][port.name]}',
                 '-global', f'{port.prop}={port.name}']
    return argv


def launch_args(flavour, row, assets, paths, quick_prom=False):
    """The -M value and the rest of the command line for one launch of a
    flavour. assets and paths are the test's resolved asset paths and
    scratch files. quick_prom sets the diagnostic-level DIP switches to
    None for a relaunch (see QUICK_PROM_SWITCHES)."""
    machine = flavour.machine
    argv = ['-accel', 'tcg,thread=multi', '-cpu', flavour.cpu,
            '-smp', str(row.cpus), '-m', str(row.mem)]
    if flavour.origin300:
        machine, o300 = lib_origin300.launch_args(assets['root'], row.cpus,
                                                  paths['scratch'])
        # Keep the boot flash and identity records; this module picks the
        # processor, memory and accelerator itself.
        for flag in ('-accel', '-cpu', '-smp', '-m'):
            index = o300.index(flag)
            del o300[index:index + 2]
        argv += o300
    else:
        argv += ['-bios', assets['firmware']]
        if quick_prom and flavour.dip_switches:
            machine += f',elsc-dip-switches={QUICK_PROM_SWITCHES}'
    if row.cells['rad1'] == RUN:
        argv += ['-audiodev', f'wav,id=rad1,path={paths["wav"]}',
                 '-global', 'sgi-rad1-audio.audiodev=rad1']
    else:
        argv += ['-audio', 'none']
    argv += [
        '-drive', f'if=none,id=sgi-nvram0,file={paths["nvram"]},format=raw',
        '-drive',
        f'if=none,id=sgi-nvram0-clock,file={paths["clock"]},format=raw',
        '-drive', f'if=none,id=disk0,file={paths["overlay"]},format=qcow2',
        '-device', 'scsi-hd,bus=scsi.0,scsi-id=1,lun=0,drive=disk0',
        '-drive', f'if=none,id=tape0,file={paths["tape"]},format=raw',
        '-device', 'scsi-tape,bus=scsi.1,scsi-id=5,lun=0,drive=tape0,'
        f'vendor={TAPE_VENDOR},product={TAPE_PRODUCT}',
        '-drive',
        f'if=none,id=cd0,file={assets["medium"]},format=raw,readonly=on',
        '-device', 'scsi-cd,bus=scsi.1,scsi-id=6,lun=0,drive=cd0',
    ]
    argv += _serial_args(flavour, paths)
    for index, eth in enumerate(flavour.ethernets):
        argv += ['-nic', f'user,id=net{index},mac={eth.mac},'
                 f'net={eth.subnet}.0/24']
    argv += list(flavour.extra_args)
    return machine, argv
