#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Boot IRIX on each SGI machine family from a release archive and check hinv.

The release archives (packaging/sgi-package.py) carry the emulator and no
firmware. This script fetches the PROM image and the IRIX disk of one boot
from the public catalogue, checks both against the catalogue's SHA256SUMS,
creates the machine's stores with the archive's own qemu-sgi-machine-init,
starts the archive's qemu-system-mips64 with plain command-line arguments (no
launcher in between), waits for the IRIX login on the serial console, logs in
and compares the output of hinv with what the machine must report.

    sgi-boot-matrix.py matrix [--os linux|macos|windows]
    sgi-boot-matrix.py boot --archive FILE --id ID --work DIR [--results FILE]
    sgi-boot-matrix.py summary RESULTS
    sgi-boot-matrix.py check

`matrix` prints the boots as the JSON a workflow matrix reads: every boot on
Linux, one per family on macOS and Windows. `check` validates the data file.
What each boot expects is in sgi-boot-matrix.json, next to this file.

It uses only the Python standard library, so it runs unchanged on the Linux,
macOS and Windows runners. Exit status 1 means the boot or a check failed.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tarfile
import time
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
DATA = HERE / 'sgi-boot-matrix.json'
EXE = '.exe' if os.name == 'nt' else ''

# The O2's published PROM is an installer image: a 256-byte header that starts
# "PROM" and holds the payload length at offset 12 (big-endian), then the flash
# contents. The machine takes the finished 512 KiB array. See
# docs/system/mips/sgi/interchange.rst.
O2_HEADER_BYTES = 256
O2_FLASH_BYTES = 524288


def load_data(path=DATA):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def boots_for(data, os_name):
    """The boots one OS runs: all on Linux, one per family elsewhere."""
    boots = data['boots']
    if os_name == 'linux':
        return boots
    return [b for b in boots if b.get('representative')]


def matrix(data, os_name):
    return {'include': [{'id': b['id'], 'family': b['family'],
                         'title': b['title']}
                        for b in boots_for(data, os_name)]}


def check_data(data):
    """Problems with the data file, as a list of messages."""
    problems = []
    objects = data['objects']
    for name, obj in objects.items():
        if not re.fullmatch(r'[0-9a-f]{64}', obj['sha256']):
            problems.append(f'{name}: sha256 is not 64 hex digits')
    families = {f['id']: f for f in data['families']}
    for fid, family in families.items():
        for key in ('disk', 'prom', 'io_prom'):
            if key in family and family[key] not in objects:
                problems.append(f'family {fid}: {key} {family[key]!r} is not '
                                'in objects')
    seen = set()
    for boot in data['boots']:
        if boot['id'] in seen:
            problems.append(f"duplicate boot {boot['id']}")
        seen.add(boot['id'])
        if boot['family'] not in families:
            problems.append(f"boot {boot['id']}: unknown family "
                            f"{boot['family']}")
        if not boot.get('hinv'):
            problems.append(f"boot {boot['id']}: no hinv expectations")
        for rx in [r.lstrip('!') for r in boot.get('hinv', [])] + families.get(
                boot['family'], {}).get('hinv', []):
            try:
                re.compile(rx)
            except re.error as e:
                problems.append(f"boot {boot['id']}: bad pattern {rx!r}: {e}")
    for fid in families:
        reps = [b for b in data['boots']
                if b['family'] == fid and b.get('representative')]
        if len(reps) != 1:
            problems.append(f'family {fid}: {len(reps)} representative boots, '
                            'expected 1')
    for skip in data.get('skipped', []):
        if not skip.get('reason'):
            problems.append(f"skipped {skip['id']}: no reason")
    return problems


def parse_sha256sums(text):
    """SHA256SUMS lines are 'HASH  PATH' (sha256sum's text or binary mark)."""
    sums = {}
    for line in text.splitlines():
        m = re.fullmatch(r'([0-9a-fA-F]{64}) [ *](.+)', line.strip())
        if m:
            sums[m.group(2)] = m.group(1).lower()
    return sums


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def download(url, dest, retries=4):
    last = None
    for attempt in range(retries):
        try:
            print(f'downloading {url} (attempt {attempt + 1})', flush=True)
            with urllib.request.urlopen(url, timeout=120) as r, \
                    open(dest, 'wb') as f:
                shutil.copyfileobj(r, f, 1 << 20)
            return
        except OSError as e:
            last = e
            print(f'download failed: {e}', flush=True)
            time.sleep(5 * (attempt + 1))
    raise RuntimeError(f'cannot download {url}: {last}')


def fetch_objects(data, names, cache):
    """Download each named object, verify it against the data file's pin and
    the catalogue's SHA256SUMS, and return {name: Path}."""
    cache.mkdir(parents=True, exist_ok=True)
    sums_path = cache / 'SHA256SUMS'
    download(data['dist'] + 'SHA256SUMS', sums_path)
    sums = parse_sha256sums(sums_path.read_text(encoding='utf-8'))
    out = {}
    for name in names:
        obj = data['objects'][name]
        listed = sums.get(obj['path'])
        if listed != obj['sha256']:
            raise RuntimeError(f"{obj['path']}: SHA256SUMS lists {listed}, "
                               f"the data file pins {obj['sha256']}")
        dest = cache / obj['path'].replace('/', '__')
        if not dest.exists() or sha256_of(dest) != obj['sha256']:
            download(data['dist'] + obj['path'], dest)
        got = sha256_of(dest)
        if got != obj['sha256']:
            raise RuntimeError(f"{obj['path']}: sha256 {got}, expected "
                               f"{obj['sha256']}")
        out[name] = dest
    return out


def o2_flash_from_installer(src, dest):
    """Strip the installer header and pad with 0xff to the flash size."""
    raw = Path(src).read_bytes()
    if raw[:4] != b'PROM':
        raise RuntimeError('the O2 PROM image does not start with PROM')
    length = int.from_bytes(raw[12:16], 'big')
    payload = raw[O2_HEADER_BYTES:O2_HEADER_BYTES + length]
    if len(payload) != length or length > O2_FLASH_BYTES:
        raise RuntimeError(f'O2 PROM payload length {length} does not fit')
    Path(dest).write_bytes(payload + b'\xff' * (O2_FLASH_BYTES - length))


def extract(archive, dest):
    """Unpack a release archive and return its bin directory.

    Needs Python 3.12 or later for the tar extraction filter."""
    shutil.rmtree(dest, ignore_errors=True)
    dest.mkdir(parents=True)
    archive = Path(archive)
    if archive.suffix == '.zip':
        with zipfile.ZipFile(archive) as z:
            z.extractall(dest)
    else:
        with tarfile.open(archive, 'r:gz') as t:
            t.extractall(dest, filter='data')
    bindir = dest / 'bin'
    for tool in ('qemu-system-mips64', 'qemu-sgi-machine-init'):
        if not (bindir / (tool + EXE)).exists():
            raise RuntimeError(f'{archive.name} has no bin/{tool}{EXE}')
    return bindir


def qemu_path(path):
    """A path inside a -drive or -blockdev value: commas are doubled."""
    return str(path).replace(',', ',,')


def free_port():
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def qemu_arguments(boot, stores, disk, port, bindir):
    """The qemu-system-mips64 command line of one boot.

    `stores` maps each store name qemu-sgi-machine-init made to its file.
    Each is a block node bound through the machine property of the same name.
    """
    machine = boot['machine']
    for name in stores:
        machine += f',{name}={name}'
    extra = boot.get('machine_options', '')
    if extra:
        machine += ',' + extra.format(flash=qemu_path(
            stores.get('node0-flash', '')))
    args = [str(bindir / ('qemu-system-mips64' + EXE)),
            '-nodefaults',
            '-L', str(bindir.parent / 'share'),
            '-accel', boot.get('accel') or (
                'tcg,thread=multi' if boot['smp'] > 1 else 'tcg'),
            '-M', machine,
            '-smp', str(boot['smp']),
            '-m', str(boot['memory'])]
    if boot.get('cpu'):
        args += ['-cpu', boot['cpu']]
    if boot.get('icount'):
        args += ['-icount', boot['icount'], '-rtc', 'clock=vm']
    for name, path in stores.items():
        args += ['-blockdev', f'driver=raw,node-name={name},'
                 f'file.driver=file,file.filename={qemu_path(path)}']
    # The CHD is compressed, so QEMU writes to a diff beside it; overlay=on
    # keeps that diff instead of recompressing the whole disk at exit.
    args += ['-drive', f'if=none,id=disk0,file={qemu_path(disk)},'
             'format=chd,overlay=on',
             '-device', f"scsi-hd,bus={boot['scsi_bus']},"
             f"scsi-id={boot.get('scsi_id', 1)},lun=0,drive=disk0"
             + (',' + boot['device_options'] if boot.get('device_options')
                else ''),
             '-display', 'none', '-audio', 'none', '-monitor', 'none',
             '-nic', 'none']
    # The console is the Nth serial line; those before it have no backend.
    for _ in range(boot.get('console_line', 0)):
        args += ['-serial', 'none']
    args += ['-serial', f'tcp:127.0.0.1:{port},server=on,wait=off']
    return args


class Timeout(Exception):
    pass


class Console:
    """A minimal expect client for the guest's serial console."""

    def __init__(self, port, log_path, proc):
        self.proc = proc
        self.buffer = ''
        self.log = open(log_path, 'ab')
        end = time.monotonic() + 120
        while True:
            try:
                self.sock = socket.create_connection(('127.0.0.1', port), 5)
                break
            except OSError:
                if proc.poll() is not None:
                    raise RuntimeError(
                        f'qemu exited with status {proc.returncode} '
                        'before the console was reachable')
                if time.monotonic() > end:
                    raise
                time.sleep(1)
        self.sock.settimeout(1.0)

    def close(self):
        self.sock.close()
        self.log.close()

    def pump(self):
        try:
            data = self.sock.recv(65536)
        except socket.timeout:
            return
        if not data:
            raise EOFError('console closed')
        self.log.write(data)
        self.log.flush()
        self.buffer += data.decode('latin-1').replace('\r', '')

    def send(self, text):
        self.sock.sendall(text.encode('latin-1'))

    def expect(self, patterns, timeout):
        """Wait for the first of `patterns` (regexes, searched in the buffer).

        Returns (index, match) and drops the buffer through the match."""
        rxs = [re.compile(p) for p in patterns]
        end = time.monotonic() + timeout
        while True:
            for i, rx in enumerate(rxs):
                m = rx.search(self.buffer)
                if m:
                    self.buffer = self.buffer[m.end():]
                    return i, m
            if self.proc.poll() is not None:
                raise RuntimeError('qemu exited with status '
                                   f'{self.proc.returncode} while waiting')
            if time.monotonic() > end:
                raise Timeout(f'waiting for {patterns!r}; console tail: '
                              f'{self.buffer[-400:]!r}')
            self.pump()


def login(con, boot, timeout):
    """Drive the PROM menu if it appears, then log in as root to a Bourne
    shell. Returns the seconds taken."""
    t0 = time.monotonic()
    end = t0 + timeout
    while True:
        i, _ = con.expect([r'Option\?', r'login: '], max(1, end - time.monotonic()))
        if i == 0:
            # System Maintenance Menu: 1 is Start System.
            con.send('1\r')
            continue
        break
    con.send('root\r')
    con.expect([r'TERM = \(vt100\) '], 180)
    con.send('\r')
    con.expect([r'# $'], 180)
    con.send('exec /bin/sh\r')
    time.sleep(2)
    return time.monotonic() - t0


def run_command(con, command, timeout=180):
    """Run a shell command; return its output (the echo stripped)."""
    con.buffer = ''
    con.send(f'{command}; echo SGI-DONE-$?\r')
    _, m = con.expect([r'SGI-DONE-(\d+)\n'], timeout)
    text = m.string[:m.start()]
    # Drop the terminal's echo of the command line and anything before it.
    echo = text.rfind('echo SGI-DONE-$?')
    if echo >= 0:
        text = text[text.find('\n', echo) + 1:]
    # The shell's next prompt arrives ahead of the output.
    text = re.sub(r'^(# )+', '', text)
    return int(m.group(1)), text


def hinv_checks(output, patterns):
    """Each expected pattern, whether hinv's output satisfied it.

    A pattern that starts with '!' must not match anywhere."""
    return {rx: (not re.search(rx[1:], output, re.M)) if rx.startswith('!')
            else bool(re.search(rx, output, re.M)) for rx in patterns}


class Results:
    def __init__(self, path, boot):
        self.path = path
        self.data = {'id': boot['id'], 'family': boot['family'],
                     'title': boot['title'], 'status': 'fail', 'error': None,
                     'checks': {}, 'login_seconds': None, 'hinv': None}
        self.t0 = time.monotonic()

    def check(self, name, ok, detail=''):
        self.data['checks'][name] = {'ok': bool(ok), 'detail': str(detail)}
        print(f"[check] {name}: {'ok' if ok else 'FAIL'} {detail}", flush=True)

    def write(self):
        checks = self.data['checks']
        self.data['duration_s'] = round(time.monotonic() - self.t0)
        if self.data['error'] is None and checks and all(
                c['ok'] for c in checks.values()):
            self.data['status'] = 'pass'
        if self.path:
            tmp = f'{self.path}.tmp'
            Path(tmp).write_text(json.dumps(self.data, indent=2) + '\n',
                                 encoding='utf-8')
            os.replace(tmp, self.path)


def stop(proc):
    if proc.poll() is None:
        proc.kill()
    try:
        proc.wait(30)
    except subprocess.TimeoutExpired:
        pass


def boot_one(args):
    data = load_data()
    boot = next((b for b in data['boots'] if b['id'] == args.id), None)
    if boot is None:
        sys.exit(f'no boot {args.id!r}; see the matrix subcommand')
    family = next(f for f in data['families'] if f['id'] == boot['family'])
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    evidence = work / 'evidence'
    evidence.mkdir(exist_ok=True)
    results = Results(args.results, boot)
    proc = con = None
    try:
        cache = Path(args.cache).resolve() if args.cache else work / 'cache'
        wanted = [family['disk'], family['prom']]
        if boot.get('io_prom'):
            wanted.append(family['io_prom'])
        objs = fetch_objects(data, wanted, cache)
        bindir = extract(args.archive, work / 'archive')
        prom = objs[family['prom']]
        if boot.get('prom_installer') == 'o2':
            flash = work / 'o2-flash.bin'
            o2_flash_from_installer(prom, flash)
            prom = flash
        state = work / 'machine'
        shutil.rmtree(state, ignore_errors=True)
        init = [str(bindir / ('qemu-sgi-machine-init' + EXE)),
                '--machine', boot['machine'], '--boot-prom', str(prom)]
        if boot.get('io_prom'):
            init += ['--io-prom', str(objs[family['io_prom']])]
        subprocess.run(init + [str(state)], check=True)
        stores = {p.stem: p for p in sorted(state.glob('*.raw'))}
        if not stores:
            raise RuntimeError('qemu-sgi-machine-init made no stores')
        port = free_port()
        cmd = qemu_arguments(boot, stores, objs[family['disk']], port, bindir)
        print('+', ' '.join(cmd), flush=True)
        with open(evidence / f"qemu-{boot['id']}.log", 'wb') as qemu_log:
            proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                                    stdout=qemu_log, stderr=subprocess.STDOUT)
        con = Console(port, evidence / f"console-{boot['id']}.log", proc)
        try:
            seconds = login(con, boot, args.login_timeout)
        except (Timeout, EOFError, RuntimeError) as e:
            results.check('login', False, f'{type(e).__name__}: {e}')
            # The finally block writes the results first.
            sys.exit(1)
        results.data['login_seconds'] = round(seconds)
        results.check('login', True, f'root shell after {round(seconds)} s')
        status, output = run_command(con, 'hinv')
        results.data['hinv'] = output
        results.check('hinv_runs', status == 0 and bool(output.strip()),
                      f'exit status {status}')
        patterns = family.get('hinv', []) + boot['hinv']
        for rx, ok in hinv_checks(output, patterns).items():
            results.check(f'hinv: {rx}', ok, '' if ok else 'no match')
    except Exception as e:  # noqa: BLE001 - the results file records it
        results.data['error'] = f'{type(e).__name__}: {e}'
        print(f"error: {results.data['error']}", flush=True)
    finally:
        if con is not None:
            try:
                con.close()
            except OSError:
                pass
        if proc is not None:
            stop(proc)
        results.write()
    if results.data['status'] != 'pass':
        sys.exit(1)


def summary(path):
    """A Markdown summary of one results file, for the job summary."""
    try:
        r = json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return '### no results\n\nThe boot wrote no results file.\n'
    lines = [f"### {r['title']}: {r['status']} in {r.get('duration_s')} s"
             f" (login after {r.get('login_seconds')} s)", '']
    if r.get('error'):
        lines += [f"Error: {r['error']}", '']
    for name, c in r['checks'].items():
        mark = 'pass' if c['ok'] else 'FAIL'
        lines.append(f"- {mark} `{name}`" + ('' if c['ok'] else
                                              f": {c['detail']}"))
    if r.get('hinv'):
        lines += ['', '```', r['hinv'].strip(), '```']
    return '\n'.join(lines) + '\n'


def main():
    p = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = p.add_subparsers(dest='command', required=True)
    m = sub.add_parser('matrix', help='print the workflow matrix as JSON')
    m.add_argument('--os', default='linux',
                   choices=['linux', 'macos', 'windows'])
    sub.add_parser('check', help='validate the data file')
    sm = sub.add_parser('summary', help='print a results file as Markdown')
    sm.add_argument('results')
    b = sub.add_parser('boot', help='boot one machine and check hinv')
    b.add_argument('--archive', required=True,
                   help='the release archive (.tar.gz or .zip)')
    b.add_argument('--id', required=True, help='the boot id from the matrix')
    b.add_argument('--work', required=True, help='a scratch directory')
    b.add_argument('--cache', help='where to keep downloads (default: in work)')
    b.add_argument('--results', help='write the results JSON here')
    b.add_argument('--login-timeout', type=int, default=3000,
                   help='seconds to wait for the login prompt')
    args = p.parse_args()
    if args.command == 'matrix':
        print(json.dumps(matrix(load_data(), args.os)))
    elif args.command == 'summary':
        print(summary(args.results))
    elif args.command == 'check':
        problems = check_data(load_data())
        for line in problems:
            print(line)
        sys.exit(1 if problems else 0)
    else:
        boot_one(args)


if __name__ == '__main__':
    main()
