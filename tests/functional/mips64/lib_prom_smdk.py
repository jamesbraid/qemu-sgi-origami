# The Origin 300 sMDK matrix: which standalone diagnostic runs on which
# processor population, the monitor dialogue that runs one, and the reading
# of its status row
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
sMDK is the Origin 300 generation's standalone diagnostic monitor, the
`field_diags_stand` package of Internal Support Tools 2.7 in its IRIX
6.5.17 build (monitor banner "SGI Version 6.124"). The stock IP35 PROM
netboots the monitor; at its `smdk>` prompt `run <diag> <args>` fetches
one of the package's diagnostics over the network and starts it as a
process of its own, and `ds` paints a live table with one row per
diagnostic. A diagnostic reports its outcome only by overwriting its
row's status word in place: `running` becomes `Passed` or `Failed`, or
`Done` for xbg, which is judged by the row's Fail column.

smdk-matrix has one row per machine population and one column per
diagnostic; a cell is `run` or a key of REASONS saying why that population
does not run it yet. DIAGS holds each diagnostic's arguments and what it
needs served. SmdkDriver is the dialogue.

This QEMU functional-test helper implements the sMDK dialogue and
verdict rules used by these tests. The comments explain what each rule
checks.

Plain Python with no qemu_test dependency, the same rule lib_prom.py
follows: test_lib_prom_smdk.py exercises it under a bare python3.
"""

import os
import re
import shutil
import time
from dataclasses import dataclass, field
from pathlib import Path

from lib_matrix import MatrixError, matrix_rows
from lib_net import HarnessError


MANIFEST = Path(__file__).parent / 'smdk-matrix'

# Every sMDK diagnostic a row runs, in the order a row runs them: the
# field guide's own Origin 300 set (108-0264-004 Table 2-1). The package's
# diaglist adds two more, sct and CPU_Test, that the guide does not call
# for and this checkout leaves out: sct for that reason alone, and
# CPU_Test because its master compares processor 1's result before
# processor 1 has finished, and why the two processors run at different
# rates on the model is not established. The router, IO7 and GSN programs
# decline for want of hardware the machine has no model of, and iocfg and
# topology are utilities with no verdict, so none of those has a column
# either.
DIAG_ORDER = ('nmem', 'ndir', 'scct', 'System_Level_Test', 'io8bt', 'xbg')

RUN = 'run'

# Why a population does not run a diagnostic yet. The manifest cites these
# by key; every population runs every diagnostic today.
REASONS = {}

# Declared SHA-256 identities for the supplied sMDK package files.
PACKAGE_DIR = os.path.join('assets', 'boot', 'smdk')
PACKAGE_SHA256 = {
    'smdk':
        '4dc74ee3f5cb17dba2d086dd866632c9b1a9a2a99278ab1b8dc5e9aada67c272',
    'iocfg':
        '874d6608a974aef1505715ad358f7252cbd19b928373d134063acd0fc6aac430',
    'nmem':
        '09a10ceaa9f373a0b9bd3bb2c2267f9f166ac6dae45ac3aebd99fec017d43865',
    'ndir':
        '1211e6e5e6c0adc35fdfeb75a7013fa3f0252cc5f2e66597cf744ad8c6161fee',
    'scct':
        '9500bcd0ea8bd54d794f0d99f1261dc048ea70611e5f72bfc45782c21b1c9e0c',
    'System_Level_Test':
        '19c5a938a38b6f1eba8bef89bbece47ba730069a092a2ed19e8bd8a4fd4ad2fa',
    'SLT_Cmd_Option_File':
        '7ee906870d7ec923b2d5f699dc593222dbe443abf00ac5a3d84c82ac381119e7',
    'io8bt':
        '88ff20115b2b7897589a5f0e4d83d78bab02e25ae971a01f262b13622b13326d',
    'xbg':
        '340c250c7156c0af210b01e9c03c65dc3cbc40323bcbbaac427adb2c427fce11',
}


@dataclass(frozen=True)
class SmdkDiag:
    """How one diagnostic is run.

    args are what `run` passes it. prime_iocfg runs iocfg once more first:
    the monitor's own discovery pass at boot leaves the widget table io8bt
    and xbg read empty, and a second iocfg pass fills it. fetch names a
    package file the diagnostic fetches for itself once running, and
    passes the end pass written into the served copy of that file.
    pin_cpus holds each vCPU thread to a host CPU of its own for the run
    (lib_host_cpus.py says why).
    """
    args: tuple = ()
    prime_iocfg: bool = False
    fetch: str = None
    passes: int = None
    pin_cpus: bool = False


DIAGS = {
    'nmem': SmdkDiag(),
    'ndir': SmdkDiag(),
    'scct': SmdkDiag(),
    # The package's option file asks for passes 1 to 50000000 and never
    # finishes; one pass reaches Passed in about two minutes.
    'System_Level_Test': SmdkDiag(fetch='SLT_Cmd_Option_File', passes=1),
    # The default sections, USB included, and one pass (-e 1): without it
    # io8bt loops pass after pass. The default mask used to exclude USB
    # (-d 5) for a HcFmNumber check that contradicted OpenHCI 1.0a; that is
    # fixed on trunk (jamesb/qemu#915). Its serial receive-timer check
    # times the IOC3 timer against a 10 ms CP0 Count wait, and a vCPU
    # thread left waiting for a host core stretches that window and
    # fails it: the verdict counts with the vCPUs pinned.
    'io8bt': SmdkDiag(args=('-e', '1'), prime_iocfg=True, pin_cpus=True),
    # The register and array-RAM sections, as the package's diaglist runs
    # it; the others need an Alteon Gigabit Ethernet card.
    'xbg': SmdkDiag(args=('-S', '0x3'), prime_iocfg=True),
}


def package_files(row):
    """The package files row's diagnostics need, monitor first."""
    names = ['smdk', 'iocfg']
    for diag in DIAG_ORDER:
        if row.runs(diag):
            names.append(diag)
            if DIAGS[diag].fetch:
                names.append(DIAGS[diag].fetch)
    return names


# ---------------------------------------------------------------------------
# The manifest.

_ID_RE = re.compile(r'^[a-z0-9_]+$')
_FIXED = ('id', 'cpus', 'budget')


@dataclass(frozen=True)
class Row:
    id: str
    cpus: int
    # Seconds the whole row may take, from the test's start to its report.
    budget: int
    cells: dict = field(default_factory=dict)

    def runs(self, diag):
        return self.cells[diag] == RUN

    def reason(self, diag):
        cell = self.cells[diag]
        return None if cell == RUN else REASONS[cell]


# The populations a row id names: one chassis with two or four
# processors, or the two-chassis pair with four in each.
POPULATIONS = {
    'origin300': (2,),
    'origin300_4cpu': (4,),
    'origin300_pair': (4, 4),
}


def parse_row(path, lineno, line):
    """One manifest line as a Row. Raises MatrixError naming path:lineno
    on a wrong field count, a bad value, a row id with no population, a
    processor count that disagrees with it, or an unknown reason key."""
    where = f'{path}:{lineno}'
    raw = [f.strip() for f in line.split('|')]
    if len(raw) != len(_FIXED) + len(DIAG_ORDER):
        raise MatrixError(f'{where}: {len(raw)} fields, expected '
                          f'{len(_FIXED) + len(DIAG_ORDER)}')
    values = dict(zip(_FIXED + DIAG_ORDER, raw))
    if not _ID_RE.match(values['id']) or values['id'] not in POPULATIONS:
        raise MatrixError(f'{where}: no population named {values["id"]!r}')
    ints = {}
    for name in ('cpus', 'budget'):
        try:
            ints[name] = int(values[name])
        except ValueError:
            raise MatrixError(
                f'{where}: {name} is not an integer: {values[name]!r}'
            ) from None
        if ints[name] <= 0:
            raise MatrixError(f'{where}: {name} must be positive')
    if ints['cpus'] != sum(POPULATIONS[values['id']]):
        raise MatrixError(f'{where}: {values["id"]} has '
                          f'{sum(POPULATIONS[values["id"]])} processors, '
                          f'not {ints["cpus"]}')
    cells = {}
    for diag in DIAG_ORDER:
        cell = values[diag]
        if cell != RUN and cell not in REASONS:
            raise MatrixError(f'{where}: {diag} is neither {RUN!r} nor a '
                              f'known reason: {cell!r}')
        cells[diag] = cell
    return Row(values['id'], ints['cpus'], ints['budget'], cells)


def parse_manifest(path=MANIFEST):
    rows = []
    seen = set()
    for lineno, line in matrix_rows(path):
        row = parse_row(path, lineno, line)
        if row.id in seen:
            raise MatrixError(f'{path}:{lineno}: duplicate id {row.id!r}')
        seen.add(row.id)
        rows.append(row)
    return rows


def manifest_row(row_id, path=MANIFEST):
    for lineno, line in matrix_rows(path):
        if line.split('|', 1)[0].strip() == row_id:
            return parse_row(path, lineno, line)
    raise MatrixError(f'{path} has no row {row_id!r}')


# ---------------------------------------------------------------------------
# Reading the status table. `ds` prints each row once and then paints its
# counters and, later, its new status word onto it with cursor
# addressing, so the bytes only read as the monitor left them once
# rendered as a screen.

def render_terminal(data):
    """Console bytes rendered as the screen rows a terminal would show,
    applying cursor addressing, screen clears and line erases."""
    text = bytes(data).decode('utf-8', errors='replace')
    rows = [[]]
    row = column = 0
    escape = ''

    def ensure_row(index):
        while len(rows) <= index:
            rows.append([])

    for character in text:
        if escape:
            escape += character
            if escape in ('\x1b[', '\x1b[?'):
                continue
            if '@' <= character <= '~':
                params = escape[2:-1]
                values = _csi_values(params)
                if character == 'J' and _csi_number(params, 0) == 2:
                    rows[:] = [[]]
                elif character == 'K' and _csi_number(params, 0) == 2:
                    rows[row].clear()
                if character in 'G`':
                    column = _csi_number(params, 1) - 1
                elif character == 'C':
                    column += _csi_number(params, 1)
                elif character == 'D':
                    column = max(0, column - _csi_number(params, 1))
                elif character in 'Hf':
                    row = max(0, values[0] - 1)
                    column = max(0, values[1] - 1) if len(values) > 1 else 0
                    ensure_row(row)
                elif character == 'K':
                    del rows[row][column:]
                escape = ''
            elif character in ('\x07', '\x1b'):
                escape = ''
            continue
        if character == '\x1b':
            escape = character
        elif character == '\r':
            column = 0
        elif character == '\n':
            row += 1
            column = 0
            ensure_row(row)
        elif character == '\b':
            column = max(0, column - 1)
        elif character == '\t':
            column = (column // 8 + 1) * 8
        elif ord(character) < 0x20 or character == '\x7f':
            continue
        else:
            ensure_row(row)
            current = rows[row]
            if len(current) <= column:
                current.extend(' ' * (column + 1 - len(current)))
            current[column] = character
            column += 1
    return '\n'.join(''.join(line) for line in rows) + '\n'


def _csi_values(params):
    values = []
    for value in params.lstrip('?').split(';'):
        try:
            values.append(int(value) if value else 1)
        except ValueError:
            return [1]
    return values or [1]


def _csi_number(params, default):
    return _csi_values(params)[0] or default


# "<N>  <name>  <status>", then, once painted, "Min: 0x<life> 0x<pass>
# 0x<fail> <percent>". The Fail column is four hex digits where the others
# are eight, and the percent is decimal.
_STATUS_ROW = re.compile(
    r'^[ \t]*(\d+)[ \t]+(\S+)[ \t]+(\S+)'
    r'(?:[ \t]+Min:[ \t]+0x([0-9a-fA-F]+)[ \t]+0x([0-9a-fA-F]+)'
    r'[ \t]+0x([0-9a-fA-F]+)[ \t]+(\d+))?')

# The Name column shows eleven characters: System_Level_Test is listed as
# System_Leve.
_NAME_COLUMN_WIDTH = 11

# Status words that say a diagnostic's process has finished. `running` is
# the one that says it has not; a word never seen before counts as not
# finished, so an unfamiliar spelling can only delay a verdict.
_FINISHED = frozenset({'Passed', 'Failed', 'Done'})


@dataclass(frozen=True)
class StatusRow:
    index: int
    name: str
    status: str
    life: int = None
    passes: int = None
    fails: int = None
    complete: int = None
    text: str = ''

    def lists(self, diag):
        """Whether this row is diag's, allowing for a cut name."""
        if self.name == diag:
            return True
        return (len(self.name) == _NAME_COLUMN_WIDTH
                and len(diag) > _NAME_COLUMN_WIDTH
                and diag.startswith(self.name))


def status_rows(transcript):
    """The numbered rows of the `ds` screen transcript leaves painted."""
    rows = []
    for line in render_terminal(transcript).splitlines():
        match = _STATUS_ROW.match(line)
        if match is None:
            continue
        index, name, status, life, passes, fails, complete = match.groups()
        rows.append(StatusRow(
            int(index), name, status,
            int(life, 16) if life is not None else None,
            int(passes, 16) if passes is not None else None,
            int(fails, 16) if fails is not None else None,
            int(complete) if complete is not None else None,
            line.rstrip()))
    return rows


def table_is_idle(transcript):
    """Whether every row on the painted `ds` screen has finished, which
    is also true of a table with no rows."""
    return all(row.status in _FINISHED for row in status_rows(transcript))


def row_verdict(row):
    """pass, fail or incomplete for one diagnostic's own row. xbg writes
    Done, not Passed, and Failed over it when any section failed, so Done
    is read by its Fail column."""
    if row.status == 'Passed':
        return 'pass'
    if row.status == 'Failed':
        return 'fail'
    if row.status == 'Done' and row.fails is not None:
        return 'fail' if row.fails else 'pass'
    return 'incomplete'


def verdict(transcript, name):
    """The verdict of name's own row on the last `ds` screen in
    transcript, or incomplete if no row lists it. Only name's row is
    read: a primed iocfg's finished row must not answer for the
    diagnostic that followed it."""
    rows = [row for row in status_rows(transcript) if row.lists(name)]
    return row_verdict(rows[-1]) if rows else 'incomplete'


# An option file's end pass: "-e <n>", matched with its leading dash so an
# "-e" inside another value is left alone.
_END_PASS_OPTION = re.compile(rb'(?<![\w-])-e\s+\d+')


def bound_end_pass(options, passes):
    """options with its end pass set to passes; one is appended when the
    file names none, since the diagnostic's own default is 100000000."""
    replacement = b'-e %d' % passes
    edited, count = _END_PASS_OPTION.subn(replacement, options, count=1)
    if count == 0:
        edited = options.rstrip(b'\r\n') + b' ' + replacement + b'\n'
    return edited


# ---------------------------------------------------------------------------
# The TFTP root. The user-mode network's BOOTP reply always names one boot
# file, so every fetch -- the PROM's load of the monitor, the monitor's
# discovery fetch of iocfg, each `run` and a diagnostic's own follow-on
# fetch -- reads that one entry, whatever name the console announces.

BOOT_FILE = 'smdk'


class TftpRoot:
    """A per-run TFTP root holding a copy of each package file under
    its own name, and the boot-file entry every fetch reads."""

    def __init__(self, directory, files):
        """files maps each package file's name to its path."""
        self.directory = directory
        os.makedirs(os.path.join(directory, 'package'), exist_ok=True)
        self.copies = {}
        for name, path in files.items():
            copy = os.path.join(directory, 'package', name)
            shutil.copyfile(path, copy)
            self.copies[name] = copy
        self.serve(BOOT_FILE)

    def edit(self, name, change):
        """Rewrite this run's copy of name with change(bytes)."""
        with open(self.copies[name], 'rb') as copy:
            data = copy.read()
        with open(self.copies[name], 'wb') as copy:
            copy.write(change(data))

    def serve(self, name):
        """Put name's bytes in the boot-file entry, whole: written to a
        partial file and renamed, so a racing fetch never reads half."""
        partial = os.path.join(self.directory, '.boot.partial')
        shutil.copyfile(self.copies[name], partial)
        os.replace(partial, os.path.join(self.directory, BOOT_FILE))

    def holds(self, name):
        if name not in self.copies:
            return False
        with open(os.path.join(self.directory, BOOT_FILE), 'rb') as served, \
                open(self.copies[name], 'rb') as copy:
            return served.read() == copy.read()


# ---------------------------------------------------------------------------
# The dialogue.

PROMPT = re.compile(rb'smdk>')
# Printed for every network fetch, naming the file wanted; the TFTP
# request itself always asks for the one fixed boot file name the
# user-mode network's BOOTP reply carries, so the harness keeps that one
# entry holding the right file's bytes.
FETCH_ANNOUNCEMENT = re.compile(rb'Obtaining (\S+) from server')
# The PROM's line at the end of its own load of the monitor, just before
# it jumps to it: the moment to stage iocfg for the monitor's discovery
# fetch, with the guest stopped so the fetch cannot come first.
LOAD_COMPLETE = re.compile(
    rb'\d+[^0-9+\r\n]*\+\s*\d+\s+entry:\s+0x[0-9a-fA-F]+')
# The monitor lost a processor at boot: a diagnostic then tests fewer
# processors than the machine has.
BOOT_DEGRADED = re.compile(rb'vcpu (\d+) is not responding')
# Every `ds` reply starts by clearing the screen, and its first full paint
# ends at the third cursor-home in a row; after that it repaints forever
# and never returns a prompt.
_DS_SCREEN_CLEAR = re.compile(rb'\x1b\[H\x1b\[2J')
_DS_TABLE_PAINTED = re.compile(rb'(?:\x1b\[1;1H){3}')
# ESC leaves that display; the carriage return keeps the ESC out of the
# next command's line.
_STATUS_DISPLAY_EXIT = b'\x1b\r'

# How the dialogue waits. A `run` typed at the moment smdk> reappears can
# race the previous process's teardown, so the table is first polled
# until idle; a command typed into a display that did not leave is
# swallowed, so each `run` is retyped until the monitor announces its
# fetch.
SETTLE_FLOOR_SECONDS = 0.5
SETTLE_POLL_SECONDS = 1.0
SETTLE_TIMEOUT_SECONDS = 15
COMMAND_ATTEMPTS = 4
COMMAND_ACCEPTED_SECONDS = 20
POLL_SECONDS = 5.0


class BootDegraded(HarnessError):
    """The monitor reached its prompt without every processor."""


class SmdkDriver:
    """Netboot sMDK on an Origin 300's L1 console and run diagnostics.

    con is a lib_net.Console on the L1 console, tftp the run's TftpRoot,
    and paused(action) runs action with the guest stopped. Every wait
    takes an explicit timeout in seconds."""

    def __init__(self, con, tftp, paused, sleep=time.sleep):
        self.con = con
        self.tftp = tftp
        self.paused = paused
        self.sleep = sleep
        self.pos = 0
        self.in_display = False

    def _expect(self, pattern, timeout, what):
        match = self.con.expect(pattern, timeout, what=what, start=self.pos)
        self.pos = match.end()
        return match

    def _send(self, data):
        self.con.sock.sendall(data)

    def boot(self, timeout):
        """Take the PROM menu to the command monitor, netboot the monitor,
        stage iocfg for its discovery fetch, and wait for smdk>."""
        deadline = time.monotonic() + timeout
        for prompt, answer in ((rb'Option\?', b'5'),
                               (rb'>>', b'unsetenv netaddr'),
                               (rb'>>', b'boot -f bootp()/smdk')):
            self._expect(prompt, _left(deadline), prompt.decode())
            self._send(answer + b'\n')
        start = self.pos
        wanted = re.compile(FETCH_ANNOUNCEMENT.pattern + b'|' +
                            LOAD_COMPLETE.pattern + b'|' + PROMPT.pattern)
        while True:
            match = self._expect(wanted, _left(deadline),
                                 'the monitor\'s smdk> prompt')
            if match.group(0) == b'smdk>':
                break
            if match.group(1) is None:
                self.paused(lambda: self.tftp.serve('iocfg'))
                continue
            # The fetch has already been served by the time it is
            # announced, so this can only confirm the staging was in time.
            name = match.group(1).decode('ascii', 'replace')
            if not self.tftp.holds(name):
                raise HarnessError(f'the monitor fetched {name} before it '
                                   'was staged')
        lost = BOOT_DEGRADED.search(bytes(self.con.buf[start:self.pos]))
        if lost:
            raise BootDegraded(f'the monitor reported vcpu '
                               f'{lost.group(1).decode()} not responding')

    def _leave_display(self):
        self._send(_STATUS_DISPLAY_EXIT)
        self.in_display = False

    def status_table(self, timeout):
        """Send `ds` and return the (start, end) offsets of its first full
        paint in the console buffer. The display is left up."""
        if self.in_display:
            self._leave_display()
        self._send(b'ds\r')
        self.in_display = True
        deadline = time.monotonic() + timeout
        clear = self._expect(_DS_SCREEN_CLEAR, _left(deadline),
                             'the ds screen clear')
        self._expect(_DS_TABLE_PAINTED, _left(deadline), 'the ds table')
        return clear.start(), self.pos

    def settle(self):
        """Wait until the table lists nothing still running."""
        deadline = time.monotonic() + SETTLE_TIMEOUT_SECONDS
        self.sleep(SETTLE_FLOOR_SECONDS)
        while time.monotonic() < deadline:
            try:
                start, end = self.status_table(_left(deadline))
            except HarnessError:
                break
            if table_is_idle(self.con.buf[start:end]):
                return
            self.sleep(SETTLE_POLL_SECONDS)
        raise HarnessError(f'the ds table never went idle in '
                           f'{SETTLE_TIMEOUT_SECONDS}s')

    def run(self, name, args=(), timeout=600):
        """Serve name, type `run name args` until the monitor fetches it,
        and wait for smdk> again. Returns the console offset the command
        started at."""
        deadline = time.monotonic() + timeout
        self.settle()
        start = self.pos
        self.tftp.serve(name)
        command = ' '.join(('run', name) + tuple(args)).encode('ascii')
        for attempt in range(1, COMMAND_ATTEMPTS + 1):
            self._leave_display()
            self.sleep(SETTLE_POLL_SECONDS * attempt)
            self._send(command + b'\r')
            try:
                self._expect(FETCH_ANNOUNCEMENT,
                             min(COMMAND_ACCEPTED_SECONDS, _left(deadline)),
                             f'the monitor to fetch {name}')
                break
            except HarnessError:
                if attempt == COMMAND_ATTEMPTS:
                    raise HarnessError(
                        f'the monitor never fetched {name} in '
                        f'{COMMAND_ATTEMPTS} attempts') from None
        self._expect(PROMPT, _left(deadline), f'smdk> after loading {name}')
        return start

    def serve_follow_on(self, name, since):
        """Stage name for a running diagnostic that will fetch it itself,
        with the guest stopped; refuses if the diagnostic already asked,
        since that fetch was served whatever the entry held."""
        def stage():
            for match in FETCH_ANNOUNCEMENT.finditer(self.con.buf, since):
                if match.group(1) == name.encode('ascii'):
                    raise HarnessError(f'the diagnostic fetched {name} '
                                       'before it could be staged')
            self.tftp.serve(name)
        self.paused(stage)

    def await_fetch(self, name, timeout):
        """Wait, typing nothing, until the running diagnostic fetches
        name. System_Level_Test typed at while it starts up never fetched
        its option file, where left alone it fetched it within seconds."""
        pattern = re.compile(rb'Obtaining ' + re.escape(name.encode()) +
                             rb' from server')
        self._expect(pattern, timeout, f'the diagnostic to fetch {name}')

    def poll(self, name, start, timeout):
        """Poll `ds` until name's own row reads a verdict or the time runs
        out, the first poll a poll interval after the call. Returns
        (verdict, its row's text or None)."""
        deadline = time.monotonic() + timeout
        self.sleep(min(POLL_SECONDS, timeout))
        while True:
            try:
                _clear, end = self.status_table(_left(deadline))
            except HarnessError:
                end = len(self.con.buf)
            window = self.con.buf[start:end]
            result = verdict(window, name)
            rows = [r for r in status_rows(window) if r.lists(name)]
            text = rows[-1].text if rows else None
            if result != 'incomplete' or time.monotonic() >= deadline:
                return result, text
            self.sleep(min(POLL_SECONDS, max(0, deadline - time.monotonic())))


def _left(deadline):
    """Seconds left before deadline, at least one so a wait still
    reads what has already arrived."""
    return max(1.0, deadline - time.monotonic())
