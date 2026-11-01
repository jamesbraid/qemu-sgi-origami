#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Keep private identifiers out of this public repository.

The repository is published, and some strings must never reach it: internal
names from reverse-engineering work, working labels for private captures,
paths into private storage, hostnames. Listing those strings here would
publish them, so the list of patterns is private. It arrives in the
SGI_LEAK_DENYLIST environment variable, which the workflow fills from an
Actions secret: the list, zlib-compressed and base64-encoded. It is held in
memory only and never written to disk, so no artifact upload can pick it up.

The check scans what a change adds, never the whole tree:

  - every added line of the diff against the pull request base,
  - the names of added or renamed files,
  - the messages of the commits in the range,
  - the pull request title and body, when the event file is given.

A hit is reported as "path:line: RULE-ID (category)". The pattern text and
the matched text are never printed, because the log is as public as the
repository. The rule id is a handle: whoever holds the private list can look
it up. --tree scans every tracked file instead, to find what an older change
left behind; it is not part of the required check.

Without a reachable list (no secret, as on a fork) the check
prints a notice and exits 0, unless SGI_LEAK_REQUIRE=1, which the main
repository's workflow sets so that an outage cannot pass a leak unnoticed.
A secret that is set but does not parse, or has fewer rules than its header
promises, is always an error: a truncated list would pass what it should stop.

Exit status: 0 clean or skipped, 1 a rule matched, 2 the check itself failed.
"""

import argparse
import base64
import binascii
import fnmatch
import json
import os
import re
import subprocess
import sys
import zlib

SECRET_ENV = 'SGI_LEAK_DENYLIST'
HEADER = re.compile(r'#\s*sgi-leak-denylist\s+v1\s+rules=(\d+)\s*$')
RULE_ID = re.compile(r'[A-Za-z0-9][A-Za-z0-9_.-]*\Z')
MAX_FILE_BYTES = 4 << 20
MAX_LIST_BYTES = 8 << 20


class CheckError(Exception):
    """The check could not run; never carries pattern text."""


class Unavailable(Exception):
    """The secret is not set; a fork, or a run without secrets."""


class Rule:
    def __init__(self, rule_id, category, regex, globs):
        self.id = rule_id
        self.category = category
        self.regex = regex
        self.globs = globs
        self.tokens = set()

    def allows(self, path):
        return any(fnmatch.fnmatchcase(path, g) for g in self.globs)

    def hits(self, text):
        """True if a match is not one of the rule's allowed identifiers."""
        return any(m.group(0).lower() not in self.tokens
                   for m in self.regex.finditer(text))


def parse_denylist(text):
    """Return the rules in a v1 list, or raise CheckError naming a rule id."""
    promised = None
    rules = []
    allows = []
    seen = set()
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip('\r\n')
        if not line.strip():
            continue
        if line.startswith('#'):
            match = HEADER.match(line)
            if match and promised is None:
                promised = int(match.group(1))
            continue
        fields = line.split('\t')
        if fields[0] == '@allow':
            if len(fields) != 3:
                raise CheckError(f'denylist line {number}: bad @allow')
            allows.append((fields[1], fields[2].lower()))
            continue
        if len(fields) != 4:
            raise CheckError(f'denylist line {number}: expected 4 '
                             f'tab-separated fields, got {len(fields)}')
        rule_id, category, pattern, globs = fields
        if not RULE_ID.match(rule_id) or rule_id in seen:
            raise CheckError(f'denylist line {number}: bad or repeated '
                             'rule id')
        seen.add(rule_id)
        try:
            regex = re.compile(pattern)
        except re.error:
            # re.error text quotes the pattern, so name only the rule.
            raise CheckError(f'rule {rule_id}: pattern does not compile')
        allowed = [] if globs == '-' else [g for g in globs.split(',') if g]
        rules.append(Rule(rule_id, category, regex, allowed))
    by_id = {rule.id: rule for rule in rules}
    for rule_id, token in allows:
        if rule_id not in by_id:
            raise CheckError('@allow names a rule that does not exist')
        by_id[rule_id].tokens.add(token)
    if promised is None:
        raise CheckError('denylist has no "# sgi-leak-denylist v1 rules=N" '
                         'header')
    if promised != len(rules):
        raise CheckError(f'denylist header promises {promised} rules, '
                         f'found {len(rules)}: truncated?')
    return rules


def decode_secret(value):
    """Turn the secret's text into the list text, or raise CheckError."""
    try:
        raw = base64.b64decode(''.join(value.split()), validate=True)
        d = zlib.decompressobj()
        text = d.decompress(raw, MAX_LIST_BYTES)
        if d.unconsumed_tail:
            raise CheckError('denylist secret is implausibly large')
        return text.decode('utf-8')
    except (binascii.Error, zlib.error, UnicodeDecodeError):
        raise CheckError(f'{SECRET_ENV} is set but is not base64 of '
                         'zlib-compressed text')


def git(*args, check=True):
    done = subprocess.run(['git', *args], stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, check=False)
    if check and done.returncode:
        raise CheckError('git ' + args[0] + ' failed: '
                         + done.stderr.decode('utf-8', 'replace').strip())
    return done.stdout.decode('utf-8', 'replace')


class Findings:
    def __init__(self, rules):
        self.rules = rules
        self.lines = []

    def scan(self, where, text, path=None, first_line=1):
        """Check text line by line; where is the location prefix."""
        for offset, line in enumerate(text.splitlines()):
            for rule in self.rules:
                if path is not None and rule.allows(path):
                    continue
                if rule.hits(line):
                    self.lines.append(
                        f'{where.format(line=first_line + offset)}: '
                        f'{rule.id} ({rule.category})')

    def scan_name(self, path):
        for rule in self.rules:
            if not rule.allows(path) and rule.hits(path):
                self.lines.append(f'{path}: {rule.id} ({rule.category}) '
                                  'in the file name')


def added_lines(base, head):
    """Yield (path, new line number, text) for every line the range adds."""
    fork = git('merge-base', base, head, check=False).strip()
    if not fork:
        raise CheckError(f'no merge base between {base} and {head}')
    diff = git('diff', '-U0', '--no-color', '--no-ext-diff',
               '--diff-filter=ACMR', fork, head)
    path = None
    line = 0
    for row in diff.splitlines():
        if row.startswith('+++ '):
            path = row[6:] if row.startswith('+++ b/') else None
        elif row.startswith('@@'):
            match = re.match(r'@@ -\S+ \+(\d+)', row)
            line = int(match.group(1)) if match else 0
        elif row.startswith('+') and path is not None:
            yield path, line, row[1:]
            line += 1


def scan_range(findings, base, head, event):
    fork = git('merge-base', base, head).strip()
    names = git('diff', '--name-only', '--diff-filter=ACMR', fork, head)
    for path in names.splitlines():
        findings.scan_name(path)
    for path, number, text in added_lines(base, head):
        findings.scan(f'{path}:{number}', text, path=path)
    log = git('log', '--format=%H%x00%B%x01', f'{fork}..{head}')
    for entry in log.split('\x01'):
        if '\x00' not in entry:
            continue
        sha, body = entry.strip('\n').split('\x00', 1)
        findings.scan(f'commit {sha[:12]} message line {{line}}', body)
    if event:
        try:
            with open(event, encoding='utf-8') as handle:
                pull = json.load(handle).get('pull_request') or {}
        except (OSError, ValueError):
            # The diff and messages are the gate; the PR text is a bonus.
            print('sgi-leak-check: NOTICE: event file unreadable, pull '
                  'request title and body not checked', file=sys.stderr)
            return
        findings.scan('pull request title line {line}',
                      pull.get('title') or '')
        findings.scan('pull request body line {line}', pull.get('body') or '')


def scan_tree(findings):
    for path in git('ls-files', '-z').split('\0'):
        if not path:
            continue
        findings.scan_name(path)
        try:
            if os.path.islink(path) or os.path.getsize(path) > MAX_FILE_BYTES:
                continue
            with open(path, 'rb') as handle:
                data = handle.read()
        except OSError:
            continue
        if b'\0' in data:
            continue
        findings.scan(f'{path}:{{line}}', data.decode('utf-8', 'replace'),
                      path=path)


def load_rules(args):
    if args.denylist:
        try:
            with open(args.denylist, encoding='utf-8') as handle:
                return parse_denylist(handle.read())
        except OSError:
            raise CheckError('cannot read the --denylist file')
    value = os.environ.get(SECRET_ENV, '')
    if not value.strip():
        raise Unavailable(f'{SECRET_ENV} is not set; forks and runs '
                          'without secrets do not have it')
    return parse_denylist(decode_secret(value))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--base', help='pull request base commit')
    parser.add_argument('--head', default='HEAD')
    parser.add_argument('--event', help='event JSON, for the title and body')
    parser.add_argument('--tree', action='store_true',
                        help='scan every tracked file instead of a range')
    parser.add_argument('--denylist', help='local list instead of fetching')
    args = parser.parse_args(argv)
    if not args.tree and not args.base:
        parser.error('give --base, or --tree')

    required = os.environ.get('SGI_LEAK_REQUIRE') == '1'
    try:
        try:
            rules = load_rules(args)
        except Unavailable as reason:
            if required:
                raise CheckError(f'private denylist unavailable: {reason}')
            print(f'sgi-leak-check: SKIPPED, private denylist unavailable '
                  f'({reason}); nothing was checked.')
            return 0
        findings = Findings(rules)
        if args.tree:
            scan_tree(findings)
        else:
            scan_range(findings, args.base, args.head, args.event)
    except CheckError as err:
        print(f'sgi-leak-check: ERROR: {err}', file=sys.stderr)
        return 2

    if findings.lines:
        for line in findings.lines:
            print(line)
        print(f'sgi-leak-check: {len(findings.lines)} hit(s). The rule ids '
              'name entries in the private denylist; the matched text is '
              'not printed. Reword the change to describe behaviour and '
              'cite public sources only.')
        return 1
    print(f'sgi-leak-check: clean ({len(rules)} rules)')
    return 0


if __name__ == '__main__':
    # A failure must not dump a traceback holding a pattern.
    try:
        sys.exit(main())
    except SystemExit:
        raise
    except Exception as err:
        print(f'sgi-leak-check: ERROR: unexpected {type(err).__name__}',
              file=sys.stderr)
        sys.exit(2)
