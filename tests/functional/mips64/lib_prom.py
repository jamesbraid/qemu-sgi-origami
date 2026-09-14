# Shared helpers for the real-PROM boot tests: where the firmware image
# lives, the firmware's progress codes and the recorded boot mark, and the
# run-record helpers every netboot test writes its provenance with
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
Everything here is plain Python with no qemu_test dependency, on purpose:
the *_verdict.py companions of the boot tests in this directory import
from this module (and from the per-test lib_*.py modules beside it) under
a plain `python3` or `pytest` run, where qemu_test is not importable --
it needs MESON_BUILD_ROOT and the qemu.machine/pycotap packages meson's
own PYTHONPATH wires in. The test_*.py modules import qemu_test
unconditionally and keep only their launch code; the logic a verdict
companion pins lives here or in a lib_*.py module.
"""

import datetime
import hashlib
import os
import re
import socket
import subprocess
import time

from lib_translation_health import assert_translation_healthy


# Where ORIGAMI_ASSETS resolves the real firmware image, matching
# tools/containerlib.py's resolve_assets_root() and tools/pod-console's
# DEFAULT_PROM in the project checkout: ORIGAMI_ASSETS names a CHECKOUT
# (one with its own assets/ directory), not the assets/ directory itself --
# confirmed against tools/README.md's own description of the variable
# ("resolves asset paths (assets/firmware/...) against `path`").
FIRMWARE_REL = os.path.join('assets', 'firmware', '6.5.30',
                            'ip27prom.payload.bin')


class AssetMismatch(Exception):
    """A provisioned file is present but is not the recorded one."""


def resolve_asset(assets_root, rel_path, sha256=None):
    """The absolute path of rel_path under assets_root, or None.

    None when assets_root is falsy (ORIGAMI_ASSETS unset or empty) or
    the file is not there -- the runner may not have it provisioned
    yet, which every caller turns into a skip. Callers read
    ORIGAMI_ASSETS themselves and hand the root in, so this stays pure
    and unit-testable.

    A test that knows the recorded SHA-256 of a file may pass it as
    sha256: a file that is present but does not match raises
    AssetMismatch naming both digests -- a wrong asset must fail
    loudly, neither masquerade as an unprovisioned one nor spend a
    boot producing a mysterious failure. Only record a digest here
    that was taken from the real provisioned file.
    """
    if not assets_root:
        return None
    path = os.path.join(assets_root, rel_path)
    if not os.path.isfile(path):
        return None
    if sha256 is not None:
        digest = file_sha256(path)
        if digest != sha256:
            raise AssetMismatch(
                f'{path}: SHA-256 {digest} does not match the '
                f'recorded {sha256}')
    return path


# The firmware-boot golden mark: the progress codes a known-good boot
# reaches, embedded here because a fork test cannot read the project
# checkout at runtime. Source: tests/golden/firmware-boot-progress-mark in
# the PROJECT checkout (/home/jamesb/joy/tech/origami, not this fork) --
# its non-comment data lines, counted directly (47 codes; 0x80 and 0xff
# appear only in that file's retirement comment and are correctly excluded
# here, same as boot-gate's own read_mark() excludes them).
MARK = frozenset({
    '0x00', '0x01', '0x02', '0x03', '0x04', '0x05', '0x06', '0x0a',
    '0x0b', '0x0c', '0x0d', '0x0e', '0x0f', '0x1d', '0x1e', '0x24',
    '0x28', '0x2a', '0x2b', '0x35', '0x36', '0x38', '0x3c', '0x3d',
    '0x3e', '0x3f', '0x40', '0x41', '0x4a', '0x4b', '0x4c', '0x4d',
    '0x4f', '0x50', '0x51', '0x52', '0x53', '0x54', '0x55', '0x56',
    '0x57', '0x58', '0x59', '0x5a', '0x5b', '0x5c', '0x5d',
})


# The line shape of the Hub's debug-leds trace event, declared in
# hw/misc/trace-events as
# `sgi_hub_led(cpu, code, kind, meaning) "CPU %u: 0x%02x (%s: %s)"`.
# The one regex every progress-code reader below shares, so a change to
# the event's printf format has one place to correct.
_PROGRESS_CODE_RE = re.compile(r'sgi_hub_led CPU \d+: (0x[0-9a-f]{2}) \((\w+):')


def first_progress_code(text):
    """Return the first firmware boot-progress code in text, or None.

    The Hub's debug-leds trace event prints a line of the form
    'sgi_hub_led CPU N: 0xNN (KIND: meaning)' for every write to its
    indicator register, where KIND is 'progress', 'FAILURE' (a
    diagnosed self-test failure such as ECC or FPU), or 'undecoded'.
    A failure line has the identical shape a progress line does, so
    this only ever returns a code whose kind is explicitly 'progress'
    -- scoring a FAILURE code as progress would be a false pass for a
    cell that actually hit one -- skipping past any failure or
    undecoded line that appears first.
    """
    for match in _PROGRESS_CODE_RE.finditer(text):
        code, kind = match.group(1), match.group(2)
        if kind == 'progress':
            return code
    return None


def codes_seen(trace_text):
    """Every progress code sgi_hub_led's trace line reports, as a set.

    Matches tools/boot-gate's own CODE regex in the project checkout: every
    0xNN this trace event reports counts, regardless of KIND (progress,
    FAILURE, or undecoded) -- boot-gate's set-subset comparison was never
    kind-filtered, so this port keeps that contract rather than narrowing
    it. (Contrast first_progress_code above, which deliberately keeps
    only kind == 'progress' lines for its own, different, single-shot "did
    the boot get anywhere at all" pass condition -- a different question
    this function does not answer.)
    """
    return {code for code, _kind in _PROGRESS_CODE_RE.findall(trace_text)}


def mark_reached(seen, mark):
    """Whether seen is a superset of mark, and which mark codes are missing.

    Returns (True, []) when every code in mark also appears in seen --
    extra codes in seen beyond mark do not affect the verdict, matching
    tools/boot-gate's own subset comparison (order-independent; a boot
    that goes further than the recorded mark still passes). Otherwise
    returns (False, sorted(mark - seen)), naming exactly what is missing.

    An empty seen is the degenerate case where every mark code is
    "missing" -- boot-gate's own "a run that did not start" case. This
    function does not give that case separate wording; the caller
    (test_prom_boot's PromBoot, and boot-gate's own main()) does, the same
    way boot-gate's codes_reached() and its "not seen" check are two
    separate pieces of that tool.
    """
    missing = sorted(mark - seen)
    return not missing, missing


def prom_netboot_steps(bootfile, args=''):
    """The three console steps that take the stock PROM from the System
    Maintenance Menu to a running netbooted image: select option 5 (the
    command monitor), clear a stale leased address left over from an
    earlier boot, and issue the boot command itself. Every netboot test
    in this directory drives this same dialogue against a real IP27 or
    IP35 PROM (test_prom_sash.py's own DRIVER_STEPS is this dialogue for
    sash64, written out by hand); this is that dialogue, generalized to
    any boot file and any kernel arguments, for a caller to prepend to
    its own driver steps or feed to drive_console() directly.

    `bootfile` is the path SLIRP's tftp= root should serve it under,
    exactly as it is typed into the boot command (`boot -f
    bootp()/<bootfile>`) -- not necessarily the boot file's own name on
    disk. `args`, when given, are appended to that same command line: the
    PROM hands them to the loaded image as its kernel arguments, the
    netboot equivalent of QEMU's own `-append`.
    """
    command = f'boot -f bootp()/{bootfile}'
    if args:
        command += f' {args}'
    return (
        (r"Option\?", "5\n"),
        (r">>", "unsetenv netaddr\n"),
        (r">>", f"{command}\n"),
    )


# How much of the received stream a driver rule is matched against: the
# tail since the last rule fired, capped so a chatty guest cannot grow it
# without bound.
_TAIL_MAX = 8192


def drive_console(sock, log_path, timeout, steps, until=None,
                  reached_when=None, vm=None):
    """Answer a guest's prompts from a rule table and drive it to an end
    condition, over the framework's console socket.

    Ported from tools/serial-console's own expect loop (see that file's
    docstring for why: commands sent before the firmware is listening are
    lost, and a rule that could refire on a prompt reappearing after being
    answered would send the wrong thing next time): each (pattern,
    response) in steps fires once, the first time PATTERN matches the tail
    of what has been received since the last match. Every byte received
    is both accumulated into the returned transcript and appended to
    log_path, so a run this drove is inspectable exactly like a captured
    -serial file.

    The end condition is either `until`, a byte regex matched against the
    tail, or `reached_when`, a predicate over the whole accumulated
    transcript, for runs whose completion line is not unique (the MDK
    memory battery ends every test with the same words, so only a count
    over the whole run can name the last one). Returns (transcript,
    reached) -- reached is True once the end condition was met, False if
    the timeout, a closed socket, or a socket error ended the loop first.

    When `vm` is given (the QEMUMachine this socket belongs to), the
    first byte received is treated as the guest having demonstrably run,
    and triggers one translation-health sample
    (lib_translation_health.assert_translation_healthy) -- so a caller
    driving a boot through here gets a fast, named failure on a
    translation storm instead of quietly spending its whole timeout on a
    guest running 100 to 300 times slower than it should.
    """
    rules = [
        {"re": re.compile(pattern.encode()), "response": response.encode(),
         "fired": False}
        for pattern, response in steps
    ]
    sock.settimeout(1.0)
    deadline = time.monotonic() + timeout
    transcript = bytearray()
    tail = b""
    reached = False
    health_checked = False
    with open(log_path, "wb") as log:
        while time.monotonic() < deadline:
            try:
                chunk = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not chunk:
                break
            transcript += chunk
            log.write(chunk)
            log.flush()
            if vm is not None and not health_checked:
                health_checked = True
                assert_translation_healthy(vm)
            tail = (tail + chunk)[-_TAIL_MAX:]
            for rule in rules:
                if not rule["fired"] and rule["re"].search(tail):
                    sock.sendall(rule["response"])
                    rule["fired"] = True
                    tail = b""
            if until is not None and until.search(tail):
                reached = True
                break
            if reached_when is not None and reached_when(bytes(transcript)):
                reached = True
                break
    return transcript.decode("utf-8", errors="replace"), reached


# ---------------------------------------------------------------------------
# Run-record helpers: the provenance every netboot test writes to its
# run.json -- when it ran, what it ran, and the identity of the tree and
# the files it ran against. None of them touches the guest.
# ---------------------------------------------------------------------------

def utc_now():
    return datetime.datetime.now(datetime.UTC).isoformat().replace(
        "+00:00", "Z")


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(path):
    resolved = os.path.realpath(path)
    return {
        "path": str(path),
        "resolved_path": resolved,
        "sha256": file_sha256(resolved),
        "size_bytes": os.path.getsize(resolved),
    }


def fork_git_identity(tree):
    """This fork checkout's own commit and dirty state, the binding record
    for a functional test inside the fork itself; None on any git failure
    -- a missing identity must not read as a fabricated clean one."""
    try:
        commit = subprocess.run(
            ["git", "-C", str(tree), "rev-parse", "HEAD"],
            capture_output=True, text=True, timeout=30)
        status = subprocess.run(
            ["git", "-C", str(tree), "status", "--porcelain=v1",
             "--untracked-files=normal"],
            capture_output=True, text=True, timeout=30)
    except (OSError, subprocess.TimeoutExpired):
        return None
    if commit.returncode != 0 or status.returncode != 0:
        return None
    return {
        "path": str(tree),
        "commit": commit.stdout.strip(),
        "dirty": bool(status.stdout.strip()),
    }
