#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the SGI release programs and pack them into a per-OS archive.

The archive holds everything needed to run the SGI machines and nothing
else, in a fixed layout with no wrapping directory:

    bin/qemu-system-mips64   the emulator
    bin/qemu-img             disk image tool
    bin/qemu-sgi-machine-init
                             builds a new machine's storage
    bin/*.dll                Windows only: the MinGW runtime DLLs
    share/keymaps/           keyboard maps
    licenses/                QEMU's licences and one file per library built in

and is named sgi-origami-<version>-<platform>.tar.gz (Windows: .zip), where
<version> is the release tag without its sgi-origami- prefix. Without --tag
the build is a development build labelled with the commit instead.

The C libraries QEMU needs (GLib, pixman, SDL 2) come from vcpkg, built
statically from the manifest in this directory at the commit named in
vcpkg-commit. Each platform builds natively:

    linux    inside the image built from debian-bookworm.docker
    macos    on an arm64 Mac with Xcode's tools and Homebrew's ninja,
             pkgconf and python
    windows  in an MSYS2 MINGW64 shell with the mingw-w64-x86_64 toolchain,
             pkgconf, ninja, python and git installed. QEMU cannot build
             with MSVC, so vcpkg uses its MinGW triplet there.

Everything is written under --work-dir, which a rerun reuses. The configure
arguments are in configure-flags.json, which scripts/ci/sgi-check shares.
"""

import argparse
import glob
import gzip
import json
import os
import platform as host_platform_module
import re
import shlex
import shutil
import subprocess
import sys
import tarfile
import time
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent

# platform -> (archive suffix, vcpkg triplet, executable suffix)
PLATFORMS = {
    'linux': ('linux-x86_64', 'x64-linux-release', ''),
    'macos': ('macos-arm64', 'arm64-osx-origami', ''),
    'windows': ('windows-x86_64', 'x64-mingw-origami', '.exe'),
}

# Executables in the archive, as paths in the build directory.
PROGRAMS = ['qemu-system-mips64', 'qemu-img', 'tools/sgi/qemu-sgi-machine-init']

TAG_RE = re.compile(r'^sgi-origami-(\d+\.\d+\.\d+)-(\d+)$')

# Libraries the host must provide, because they sit in front of its drivers
# and services. Anything else an executable needs has to be in the archive.
LINUX_HOST_LIBS = re.compile(
    r'^(ld-linux-x86-64|lib(c|m|dl|rt|pthread|gcc_s|X11|X11-xcb|xcb|'
    r'xkbcommon|wayland-[a-z]+|udev|asound|pulse))\.so')
MACOS_HOST_PREFIXES = ('/usr/lib/', '/System/Library/')
WINDOWS_HOST_DLLS = re.compile(
    r'^(advapi32|bcrypt|bcryptprimitives|cfgmgr32|combase|comctl32|comdlg32|'
    r'crypt32|d3d11|d3d9|dnsapi|dsound|dwmapi|dxgi|gdi32|imm32|iphlpapi|'
    r'kernel32|msvcrt|ntdll|ole32|oleaut32|setupapi|shell32|shlwapi|user32|'
    r'userenv|uxtheme|version|winmm|ws2_32|winhttp|secur32|rpcrt4|ucrtbase|'
    r'mswsock|hid|dinput8|usp10|wldap32|normaliz)\.dll$|^(api|ext)-ms-win-')

LICENSE_FILES = [
    ('LICENSE', 'qemu.LICENSE'),
    ('COPYING', 'qemu.COPYING'),
    ('COPYING.LIB', 'qemu.COPYING.LIB'),
    ('LICENSE.origami', 'qemu.LICENSE.origami'),
    ('LICENSE.origami.paths', 'qemu.LICENSE.origami.paths'),
    ('hw/mips/sgi/models/LICENSE', 'qemu.sgi-models.LICENSE'),
]
LICENSE_NAME = re.compile(r'^(LICEN[CS]E|COPYING|COPYRIGHT)', re.I)


class PackageError(Exception):
    pass


def run(cmd, **kwargs):
    print('+ ' + shlex.join(str(a) for a in cmd), flush=True)
    subprocess.run([str(a) for a in cmd], check=True, **kwargs)


def output(cmd, **kwargs):
    return subprocess.check_output([str(a) for a in cmd], text=True,
                                   **kwargs)


def host_platform():
    if sys.platform.startswith('linux'):
        name = 'linux'
    elif sys.platform == 'darwin':
        name = 'macos'
    elif sys.platform in ('win32', 'cygwin', 'msys'):
        name = 'windows'
    else:
        raise PackageError(f'unsupported host: {sys.platform}')
    return name


def check_architecture(name):
    machine = host_platform_module.machine().lower()
    wanted = 'arm64' if name == 'macos' else 'x86_64'
    if machine == 'amd64':
        machine = 'x86_64'
    if machine == 'aarch64':
        machine = 'arm64'
    if machine != wanted:
        raise PackageError(f'{name} archives are built on {wanted} hosts, '
                           f'not {machine}')


def configure_flags(name):
    """The configure arguments of platform `name`, from the shared list."""
    with open(HERE / 'configure-flags.json', encoding='utf-8') as f:
        flags = json.load(f)
    return flags['common'] + flags[name]


def source_version():
    return (SOURCE / 'VERSION').read_text(encoding='utf-8').strip()


def parse_tag(tag, version):
    """Return the release label of `tag`: its text after sgi-origami-.

    The tag names the QEMU version it was cut at, so a tag that disagrees
    with the tree's VERSION file is a mistake worth stopping for.
    """
    match = TAG_RE.match(tag)
    if not match:
        raise PackageError(f'tag {tag!r} is not sgi-origami-<VERSION>-<n>, '
                           'for example sgi-origami-11.1.50-1')
    if match.group(1) != version:
        raise PackageError(f'tag {tag} names QEMU {match.group(1)}, but this '
                           f'tree is {version}')
    return tag[len('sgi-origami-'):]


def development_label(version, commit):
    return f'{version}-g{commit[:7]}'


def archive_name(label, name):
    suffix = PLATFORMS[name][0]
    ext = 'zip' if name == 'windows' else 'tar.gz'
    return f'sgi-origami-{label}-{suffix}.{ext}'


def git_commit(override):
    if override:
        return override
    try:
        return output(['git', '-C', SOURCE, 'rev-parse', 'HEAD']).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        raise PackageError('cannot read the commit; pass --commit') from None


def source_epoch():
    if os.environ.get('SOURCE_DATE_EPOCH'):
        return int(os.environ['SOURCE_DATE_EPOCH'])
    try:
        return int(output(['git', '-C', SOURCE, 'log', '-1',
                           '--format=%ct']).strip())
    except (subprocess.CalledProcessError, FileNotFoundError, ValueError):
        raise PackageError('cannot read the commit time; set '
                           'SOURCE_DATE_EPOCH') from None


# ---------------------------------------------------------------- vcpkg --

def ensure_vcpkg(root, commit):
    """Fetch vcpkg at `commit` into `root` and bootstrap it. Return its tool."""
    tool = root / ('vcpkg.exe' if sys.platform == 'win32'
                   else 'vcpkg')
    if not (root / '.git').exists():
        root.mkdir(parents=True, exist_ok=True)
        run(['git', 'init', '-q'], cwd=root)
        run(['git', 'remote', 'add', 'origin',
             'https://github.com/microsoft/vcpkg.git'], cwd=root)
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=root, text=True,
                          capture_output=True).stdout.strip()
    moved = head != commit
    if moved:
        run(['git', 'fetch', '-q', '--depth', '1', 'origin', commit],
            cwd=root)
        run(['git', 'checkout', '-q', '--force', commit], cwd=root)
    if moved or not tool.exists():
        if tool.suffix == '.exe':
            run(['cmd', '/c', 'bootstrap-vcpkg.bat', '-disableMetrics'],
                cwd=root)
        else:
            run(['sh', 'bootstrap-vcpkg.sh', '-disableMetrics'], cwd=root)
    return tool


def install_libraries(tool, triplet, install_root, binary_cache):
    """Build the manifest's libraries; return the triplet's install prefix."""
    binary_cache.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, VCPKG_DISABLE_METRICS='1',
               VCPKG_DEFAULT_BINARY_CACHE=str(binary_cache))
    # Host tools build with the same triplet, so a MinGW build needs no MSVC
    # and Linux and macOS skip the debug half of the stock host triplets.
    run([tool, 'install', f'--triplet={triplet}', f'--host-triplet={triplet}',
         f'--x-manifest-root={HERE}', f'--x-install-root={install_root}',
         '--clean-after-build'], env=env)
    return install_root / triplet


# ----------------------------------------------------------------- QEMU --

def build_environment(name, prefix, work):
    env = dict(os.environ)
    env['MESON_PACKAGE_CACHE_DIR'] = str(work / 'meson-package-cache')
    pkgconfig = (prefix / 'lib/pkgconfig').as_posix()
    if name == 'linux':
        # Linux also needs the host's X11, ALSA, PulseAudio and udev.
        env['PKG_CONFIG_PATH'] = pkgconfig
    else:
        # vcpkg is the only source elsewhere. A plain pkg-config, searching
        # only its prefix, keeps a stray MinGW or Homebrew library out.
        env['PKG_CONFIG'] = 'pkg-config'
        env['PKG_CONFIG_LIBDIR'] = pkgconfig
    if name == 'macos':
        env['MACOSX_DEPLOYMENT_TARGET'] = '15.0'
    return env


def meson_tool(build):
    for candidate in ('pyvenv/bin/meson', 'pyvenv/Scripts/meson.exe',
                      'pyvenv/Scripts/meson'):
        if (build / candidate).exists():
            return build / candidate
    raise PackageError(f'no meson in {build}/pyvenv; did configure fail?')


def build_qemu(name, build, env, pkgversion, jobs):
    build.mkdir(parents=True, exist_ok=True)
    configure = [SOURCE / 'configure', *configure_flags(name)]
    if name == 'windows':
        # A native Windows Python cannot execute a script by its shebang,
        # and configure finds its source tree from the slashes in $0, so
        # pass a path with forward slashes.
        configure[0] = (SOURCE / 'configure').as_posix()
        configure.insert(0, 'sh')
    # Configure again when the arguments changed since the last run, which
    # a reused build directory would otherwise ignore.
    recorded = build / 'sgi-package-configure.txt'
    command = shlex.join(str(a) for a in configure)
    if (not (build / 'build.ninja').exists() or not recorded.exists()
            or recorded.read_text(encoding='utf-8') != command):
        run(configure, cwd=build, env=env)
        recorded.write_text(command, encoding='utf-8')
    meson = meson_tool(build)
    # QEMU reports this in --version. Setting it here rather than at
    # configure keeps a reused build directory in step with its label.
    run([meson, 'configure', f'-Dpkgversion={pkgversion}', build], env=env)
    # Meson keeps an extracted subproject until told to refresh it, so a
    # source tree that built before a libslirp patch changed would keep the
    # old copy.
    if glob.glob(str(SOURCE / 'subprojects/libslirp*/meson.build')):
        run([meson, 'subprojects', 'update', '--reset', '--sourcedir', SOURCE,
             'libslirp'], cwd=build, env=env)
    exe = PLATFORMS[name][2]
    run(['ninja', '-C', build, '-j', jobs,
         *[p + exe for p in PROGRAMS]], env=env)


# ---------------------------------------------------------------- staging --

def used_subproject_licences(build):
    """Licence files of the bundled subprojects this build configured.

    That includes a few that only the tests or other targets use; listing a
    licence the archive does not strictly need is harmless, leaving one out
    is not.
    """
    found = []
    try:
        projects = json.loads((build / 'meson-info/intro-projectinfo.json')
                              .read_text(encoding='utf-8'))
    except (OSError, ValueError) as error:
        raise PackageError('cannot read meson-info/intro-projectinfo.json '
                           f'in {build}: {error}') from None
    names = {sub['name'] for sub in projects.get('subprojects', [])}
    for sub in sorted(names):
        for directory in sorted(glob.glob(str(SOURCE / 'subprojects' /
                                              (sub + '*')))):
            directory = Path(directory)
            if not directory.is_dir() or directory.name == 'packagefiles':
                continue
            for entry in sorted(directory.iterdir()):
                if entry.is_file() and LICENSE_NAME.match(entry.name):
                    found.append((entry, f'{directory.name}.{entry.name}'))
    return found


def stage(name, build, prefix, stage_dir, strip):
    """Lay the archive's tree out in an empty `stage_dir`."""
    exe = PLATFORMS[name][2]
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    bin_dir = stage_dir / 'bin'
    bin_dir.mkdir(parents=True)
    for program in PROGRAMS:
        target = bin_dir / (Path(program).name + exe)
        shutil.copy2(build / (program + exe), target)
        if strip:
            strip_binary(name, target)

    keymaps = stage_dir / 'share/keymaps'
    shutil.copytree(SOURCE / 'pc-bios/keymaps', keymaps,
                    ignore=shutil.ignore_patterns('meson.build'))

    licences = stage_dir / 'licenses'
    licences.mkdir()
    for source, target in LICENSE_FILES:
        shutil.copy2(SOURCE / source, licences / target)
    for source, target in used_subproject_licences(build):
        shutil.copy2(source, licences / target)
    # vcpkg installs a copyright file for each library it built.
    # Its own build helpers, the vcpkg-* ports, ship nothing.
    for copyright_file in sorted(glob.glob(str(prefix / 'share/*/copyright'))):
        port = Path(copyright_file).parent.name
        if port.startswith('vcpkg-'):
            continue
        shutil.copy2(copyright_file, licences / f'{port}.copyright')

    if name == 'windows':
        copy_windows_dlls(bin_dir, [prefix / 'bin'])


def strip_binary(name, path):
    cmd = ['strip', '-x', path] if name == 'macos' else ['strip', path]
    run(cmd)


# ------------------------------------------------- shared library check --

def elf_needed(text):
    """Names of shared libraries from `readelf -d` output."""
    return re.findall(r'\(NEEDED\)\s+Shared library: \[([^\]]+)\]', text)


def pe_needed(text):
    """DLL names from `objdump -p` output."""
    return re.findall(r'^\s*DLL Name: (\S+)', text, re.M)


def macho_needed(text):
    """Paths to shared libraries from `otool -L`, without the file's own line."""
    paths = []
    for line in text.splitlines()[1:]:
        match = re.match(r'^\s+(\S+) \(', line)
        if match:
            paths.append(match.group(1))
    return paths


def needed_libraries(name, path):
    if name == 'linux':
        return elf_needed(output(['readelf', '-d', path]))
    if name == 'macos':
        return macho_needed(output(['otool', '-L', path]))
    return pe_needed(output(['objdump', '-p', path]))


def is_host_library(name, library):
    if name == 'linux':
        return bool(LINUX_HOST_LIBS.match(library))
    if name == 'macos':
        return library.startswith(MACOS_HOST_PREFIXES)
    return bool(WINDOWS_HOST_DLLS.match(library.lower()))


def stray_libraries(name, bin_dir, needed=needed_libraries):
    """Return "file: library" for every library outside the archive.

    A library counts as inside when it sits in bin/ (it is then checked
    in turn) or is one the host provides.
    """
    problems = []
    seen = set()
    pending = [p for p in sorted(bin_dir.iterdir()) if p.is_file()
               and p.suffix.lower() in ('', '.exe', '.dll', '.so', '.dylib')]
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        for library in needed(name, path):
            local = bin_dir / library
            if local.is_file():
                pending.append(local)
            elif not is_host_library(name, library):
                problems.append(f'{path.name}: {library}')
    return sorted(set(problems))


def copy_windows_dlls(bin_dir, extra_dirs):
    """Copy the DLLs the executables need, and theirs in turn, into bin/.

    MinGW's thread library, and the C++ runtime when the toolchain's shared
    one is linked, come from the toolchain only as DLLs. They are found on
    PATH, which in an MSYS2 MINGW64 shell starts with the toolchain's bin
    directory.
    """
    search = [Path(d) for d in os.environ.get('PATH', '').split(os.pathsep)
              if d] + [Path(d) for d in extra_dirs]
    pending = sorted(bin_dir.glob('*.exe'))
    seen = set()
    while pending:
        path = pending.pop()
        if path in seen:
            continue
        seen.add(path)
        for dll in pe_needed(output(['objdump', '-p', path])):
            if is_host_library('windows', dll) or (bin_dir / dll).exists():
                continue
            for directory in search:
                candidate = directory / dll
                if candidate.is_file():
                    shutil.copy2(candidate, bin_dir / dll)
                    pending.append(bin_dir / dll)
                    break
            else:
                raise PackageError(f'{path.name} needs {dll}, which is on '
                                   'neither PATH nor in the vcpkg tree')


# ---------------------------------------------------------------- archive --

def tree_entries(stage_dir):
    """(archive path, file path or None for a directory), sorted."""
    entries = []
    for current, dirs, files in os.walk(stage_dir):
        dirs.sort()
        rel = Path(current).relative_to(stage_dir)
        if str(rel) != '.':
            entries.append((rel.as_posix(), None))
        for file in sorted(files):
            entries.append(((rel / file).as_posix(), Path(current) / file))
    return entries


def write_archive(name, stage_dir, target, epoch):
    """Write `target`, with fixed owners and times so a rerun matches."""
    target.parent.mkdir(parents=True, exist_ok=True)
    entries = tree_entries(stage_dir)
    if name == 'windows':
        stamp = time.gmtime(max(epoch, 315532800))[:6]
        with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED,
                             compresslevel=9) as archive:
            for rel, path in entries:
                if path is None:
                    continue
                info = zipfile.ZipInfo(rel, stamp)
                info.compress_type = zipfile.ZIP_DEFLATED
                info.external_attr = (0o755 if path.suffix in ('.exe', '.dll')
                                      else 0o644) << 16
                archive.writestr(info, path.read_bytes())
        return
    with open(target, 'wb') as raw, \
            gzip.GzipFile(filename='', mode='wb', fileobj=raw, mtime=0,
                          compresslevel=9) as zipped, \
            tarfile.open(fileobj=zipped, mode='w', format=tarfile.PAX_FORMAT
                         ) as archive:
        for rel, path in entries:
            info = tarfile.TarInfo(rel)
            info.mtime = epoch
            info.uid = info.gid = 0
            info.uname = info.gname = ''
            if path is None:
                info.type = tarfile.DIRTYPE
                info.mode = 0o755
                archive.addfile(info)
                continue
            info.size = path.stat().st_size
            info.mode = 0o755 if os.access(path, os.X_OK) else 0o644
            with open(path, 'rb') as f:
                archive.addfile(info, f)


# ------------------------------------------------------------------- main --

def package(args):
    name = args.platform or host_platform()
    if name != host_platform():
        raise PackageError(f'{name} archives build natively; this host is '
                           f'{host_platform()}')
    check_architecture(name)
    version = source_version()
    commit = git_commit(args.commit)
    label = (parse_tag(args.tag, version) if args.tag
             else development_label(version, commit))
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    triplet = PLATFORMS[name][1]

    vcpkg_commit = (HERE / 'vcpkg-commit').read_text(encoding='utf-8').strip()
    tool = ensure_vcpkg(args.vcpkg_root or work / 'vcpkg', vcpkg_commit)
    prefix = install_libraries(tool, triplet, work / 'vcpkg-installed',
                               work / 'vcpkg-binary-cache')

    build = work / 'qemu-build'
    env = build_environment(name, prefix, work)
    build_qemu(name, build, env, f'sgi-origami {label}', args.jobs)

    stage_dir = work / 'stage'
    stage(name, build, prefix, stage_dir, not args.no_strip)
    problems = stray_libraries(name, stage_dir / 'bin')
    if problems:
        raise PackageError('executables need libraries outside the archive:\n'
                           + '\n'.join('  ' + p for p in problems))

    target = args.output_dir.resolve() / archive_name(label, name)
    write_archive(name, stage_dir, target, source_epoch())
    print(f'{target} ({target.stat().st_size} bytes)')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--platform', choices=sorted(PLATFORMS),
                        help='default: this host (the build is always native)')
    parser.add_argument('--tag', help='release tag, sgi-origami-<VERSION>-<n>;'
                        ' without it the archive is a development build')
    parser.add_argument('--commit', help='commit to label a development '
                        'build with (default: HEAD)')
    parser.add_argument('--work-dir', type=Path,
                        default=SOURCE / 'build-package',
                        help='build and cache directory (default: '
                        'build-package in the source tree)')
    parser.add_argument('--output-dir', type=Path,
                        help='where the archive goes (default: dist in the '
                        'work directory)')
    parser.add_argument('--vcpkg-root', type=Path,
                        help='vcpkg checkout (default: vcpkg in the work '
                        'directory)')
    parser.add_argument('--jobs', type=int, default=os.cpu_count() or 4,
                        help='parallel compile jobs')
    parser.add_argument('--no-strip', action='store_true',
                        help='keep symbols in the executables')
    args = parser.parse_args(argv)
    if args.output_dir is None:
        args.output_dir = args.work_dir / 'dist'
    try:
        return package(args)
    except PackageError as error:
        print(f'sgi-package: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
