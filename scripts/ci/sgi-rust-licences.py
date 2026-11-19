#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Licence check and third-party notices for the standalone Rust workspaces.

Every Cargo.lock under rust/ belongs to a standalone workspace (rust/sgi-gpu,
for one) whose crates are linked into the release archives. cargo-about,
configured by rust/about.toml, resolves each crate's licence for the
platforms that file lists, and rust/about.hbs lays the texts out.

  check   fail on a crate whose licence rust/about.toml does not accept, and
          on a stale or missing THIRD-PARTY-NOTICES.txt beside the Cargo.lock
          of a workspace the archives link (SHIPPED)
  update  rewrite those files; packaging/sgi-package.py copies them into the
          release archives
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[2]
CONFIG = SOURCE / 'rust/about.toml'
TEMPLATE = SOURCE / 'rust/about.hbs'
NOTICES = 'THIRD-PARTY-NOTICES.txt'
# The workspaces whose crates a release archive links, so ships NOTICES.
SHIPPED = ('rust/sgi-gpu',)
LICENCE_PREFIXES = ('LICENSE', 'LICENCE', 'COPYING', 'UNLICENSE')
RULE = '=' * 80 + '\n'
IRIS = 'git+https://github.com/jamesbraid/iris'


def lockfiles(root=SOURCE):
    """Return the Cargo.lock files under root/rust, sorted."""
    found = []
    for base, dirs, names in os.walk(root / 'rust'):
        dirs[:] = [d for d in dirs if d != 'target']
        if 'Cargo.lock' in names:
            found.append(Path(base) / 'Cargo.lock')
    return sorted(found)


def licence_files(package):
    """Return a crate's licence files, from its directory or, for a crate in
    a git checkout, from anywhere up to the checkout's root."""
    directory = Path(package['manifest_path']).parent
    root = directory
    parts = directory.parts
    if (package['source'] or '').startswith('git+') and 'checkouts' in parts:
        # checkouts/<repository>-<hash>/<revision>
        root = Path(*parts[:parts.index('checkouts') + 3])
    walk = directory
    while True:
        files = sorted(p for p in walk.iterdir() if p.is_file() and
                       p.name.upper().startswith(LICENCE_PREFIXES))
        if files or walk == root:
            return files
        walk = walk.parent


def first_party(package, root=SOURCE):
    """Whether a crate is ours or iris's, the only ones the private
    exemption may skip: a path crate under rust/ or one from the iris repo."""
    source = package['source']
    if source is None:
        return Path(package['manifest_path']).is_relative_to(root / 'rust')
    return source.startswith((IRIS + '?', IRIS + '#'))


def first_party_notices(manifest):
    """Return notice sections for the dependencies cargo-about skips as
    private (publish = false), which are ours or iris's and have no licence
    field. Their licence is the file in their repository."""
    data = json.loads(subprocess.run(
        ['cargo', 'metadata', '--locked', '--format-version', '1',
         '--manifest-path', str(manifest)],
        check=True, capture_output=True, text=True).stdout)
    members = set(data['workspace_members'])
    out = []
    for package in sorted(data['packages'], key=lambda p: p['name']):
        if package['id'] in members or package['publish'] != []:
            continue
        # cargo-about skips every publish = false crate, so a third-party
        # one would escape the licence list.
        if not first_party(package):
            sys.exit(f"{package['name']} {package['version']} is publish = "
                     'false, which cargo-about does not check, and is not '
                     'ours or iris\'s')
        # The repository's other licence files cover other code in it.
        files = [f for f in licence_files(package)
                 if f.stem.upper() == 'LICENSE']
        if not files:
            sys.exit(f"no licence file for {package['name']}")
        for text in files:
            out.append(f"{RULE}{package['name']}: {text.name}\n\nUsed by:\n"
                       f"  {package['name']} {package['version']}\n\n"
                       f"{text.read_text(encoding='utf-8').rstrip()}\n")
    return '\n'.join(out)


def generate(lock, about):
    """Return the notices text for the workspace of `lock`, or exit with
    cargo-about's complaint about a crate it cannot license."""
    manifest = lock.with_name('Cargo.toml')
    # Fetch first, so cargo-about reads the crates' own files offline and
    # the text does not depend on what a network answers.
    subprocess.run(['cargo', 'fetch', '--locked', '--manifest-path',
                    str(manifest)], check=True)
    with tempfile.TemporaryDirectory() as tmp:
        out = Path(tmp) / 'notices'
        try:
            subprocess.run([about, 'generate', '--fail', '--locked',
                            '--offline', '--manifest-path', str(manifest),
                            '--config', str(CONFIG), str(TEMPLATE),
                            '--output-file', str(out)], check=True)
        except FileNotFoundError:
            sys.exit(f'{about} not found: the CI image installs it, see '
                     'tests/docker/dockerfiles/debian-sgi.docker')
        except subprocess.CalledProcessError:
            sys.exit(f'{lock.relative_to(SOURCE)}: a crate has a licence '
                     'rust/about.toml does not accept, or none')
        return out.read_text(encoding='utf-8') + '\n' + first_party_notices(
            manifest)


def run(args):
    locks = lockfiles()
    if not locks:
        sys.exit('no Cargo.lock under rust/: nothing to check')
    stale = []
    for lock in locks:
        print(f'licences: {lock.relative_to(SOURCE)}', flush=True)
        text = generate(lock, args.cargo_about)
        if lock.parent.relative_to(SOURCE).as_posix() not in SHIPPED:
            continue
        notices = lock.with_name(NOTICES)
        if args.mode == 'update':
            notices.write_text(text, encoding='utf-8')
        elif (not notices.exists() or
              notices.read_text(encoding='utf-8') != text):
            stale.append(notices.relative_to(SOURCE))
    if stale:
        sys.exit(f"{' '.join(map(str, stale))} out of date: run "
                 'scripts/ci/sgi-rust-licences.py update')


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('check', 'update'))
    parser.add_argument('--cargo-about', default='cargo-about')
    run(parser.parse_args(argv))


if __name__ == '__main__':
    main()
