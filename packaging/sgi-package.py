#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Build the SGI release programs and pack them into one archive per platform.

The archive holds everything needed to run the SGI machines and nothing
else, in a fixed layout with no wrapping directory:

    bin/qemu-system-mips64   the emulator
    bin/qemu-img             disk image tool
    bin/qemu-sgi-machine-init
                             builds a new machine's storage
    bin/*.dll                Windows only: the MinGW runtime DLLs
    share/qemu/keymaps/      keyboard maps
    licenses/                QEMU's licences, one file per library built in,
                             and on Windows the MinGW runtime's and
                             Rust's

and is named sgi-origami-<version>-<platform>.tar.gz (Windows: .zip), where
<version> is the release tag without its sgi-origami- prefix. Without --tag
the build is a development build labelled with the commit instead.

The C libraries QEMU needs (GLib, pixman, SDL 2, GnuTLS) come from vcpkg,
built statically from the manifest in this directory at the commit named in
vcpkg-commit. Each platform builds natively:

    linux    inside the image built from debian-bookworm.docker
    macos    on an Arm or Intel Mac with Xcode's tools, Homebrew's ninja,
             pkgconf and python, and rustc, cargo and bindgen for QEMU's
             Rust devices
    windows  in an MSYS2 MINGW64 shell with the mingw-w64-x86_64 toolchain,
             pkgconf, ninja, python, git, rust, rust-bindgen and clang
             installed. QEMU cannot build with MSVC, so vcpkg uses its MinGW
             triplet there.

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

# (OS, architecture) -> (archive suffix, vcpkg triplet, executable suffix)
PLATFORMS = {
    ('linux', 'x86_64'): ('linux-x86_64', 'x64-linux-release', ''),
    ('linux', 'arm64'): ('linux-arm64', 'arm64-linux-origami', ''),
    ('macos', 'arm64'): ('macos-arm64', 'arm64-osx-origami', ''),
    ('macos', 'x86_64'): ('macos-x86_64', 'x64-osx-origami', ''),
    ('windows', 'x86_64'): ('windows-x86_64', 'x64-mingw-origami', '.exe'),
    ('windows', 'arm64'): ('windows-arm64', 'arm64-mingw-origami', '.exe'),
}

# Extra vcpkg build environment per platform: nettle's -fpic overflows the
# GOT of QEMU's executable on arm64 Linux, and its configure reads CCPIC.
VCPKG_ENV = {
    ('linux', 'arm64'): {'CCPIC': '-fPIC'},
    # GnuTLS's arm64 assembly does not assemble for Windows.
    ('windows', 'arm64'): {'enable_hardware_acceleration': 'no'},
}

# Executables in the archive, as paths in the build directory.
PROGRAMS = ['qemu-system-mips64', 'qemu-img', 'tools/sgi/qemu-sgi-machine-init']

TAG_RE = re.compile(r'^sgi-origami-(\d+\.\d+\.\d+)-(\d+)$')

# Libraries the host must provide, because they sit in front of its drivers
# and services. Anything else an executable needs has to be in the archive.
LINUX_HOST_LIBS = re.compile(
    r'^(ld-linux-x86-64|ld-linux-aarch64|'
    r'lib(c|m|dl|rt|pthread|gcc_s|X11|X11-xcb|xcb|'
    r'xkbcommon|wayland-[a-z]+|udev|asound|pulse))\.so')
MACOS_HOST_PREFIXES = ('/usr/lib/', '/System/Library/')
WINDOWS_HOST_DLLS = re.compile(
    r'^(advapi32|bcrypt|bcryptprimitives|cfgmgr32|combase|comctl32|comdlg32|'
    r'crypt32|d3d11|d3d9|dnsapi|dsound|dwmapi|dxgi|gdi32|imm32|iphlpapi|'
    r'kernel32|msvcrt|ntdll|ole32|oleaut32|setupapi|shell32|shlwapi|user32|'
    r'userenv|uxtheme|version|winmm|ws2_32|winhttp|secur32|rpcrt4|ucrtbase|'
    r'mswsock|hid|dinput8|usp10|wldap32|normaliz|ncrypt)\.dll$|'
    r'^(api|ext)-ms-win-')

LICENSE_FILES = [
    ('LICENSE', 'qemu.LICENSE'),
    ('COPYING', 'qemu.COPYING'),
    ('COPYING.LIB', 'qemu.COPYING.LIB'),
    ('LICENSE.origami', 'qemu.LICENSE.origami'),
    ('LICENSE.origami.paths', 'qemu.LICENSE.origami.paths'),
    ('hw/mips/sgi/models/LICENSE', 'qemu.sgi-models.LICENSE'),
]
# Platforms whose archive carries the Rust devices, built with the "rust" list
# of configure-flags.json.
RUST_PLATFORMS = ('macos', 'windows')

LICENSE_NAME = re.compile(r'^(LICEN[CS]E|COPYING|COPYRIGHT)', re.I)
# The licence texts of the Rust crates the GPU back end links, kept current by
# scripts/ci/sgi-rust-licences.py.
GPU_NOTICES = 'rust/sgi-gpu/THIRD-PARTY-NOTICES.txt'


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


def host_architecture():
    machine = host_platform_module.machine().lower()
    return {'amd64': 'x86_64', 'aarch64': 'arm64'}.get(machine, machine)


def configure_flags(name, arch=None):
    """The configure arguments of platform `name`, from the shared list.

    `arch` adds the list for that architecture, if the file has one.
    """
    with open(HERE / 'configure-flags.json', encoding='utf-8') as f:
        flags = json.load(f)
    extra = flags['rust'] if name in RUST_PLATFORMS else []
    return (flags['common'] + flags[name] + extra
            + flags.get(f'{name}-{arch}', []))


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


def archive_name(label, name, arch):
    suffix = PLATFORMS[name, arch][0]
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


def install_libraries(tool, triplet, install_root, binary_cache, extra_env):
    """Build the manifest's libraries; return the triplet's install prefix."""
    binary_cache.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, VCPKG_DISABLE_METRICS='1',
               VCPKG_DEFAULT_BINARY_CACHE=str(binary_cache), **extra_env)
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
    if name == 'windows':
        # bindgen does not look for libclang.dll on PATH, where MSYS2 keeps it.
        env.setdefault('LIBCLANG_PATH', str(toolchain_prefix() / 'bin'))
    return env


def meson_tool(build):
    """The command that runs the Meson configure made for `build`."""
    for candidate in ('pyvenv/bin/meson', 'pyvenv/Scripts/meson.exe',
                      'pyvenv/Scripts/meson'):
        if (build / candidate).exists():
            return [build / candidate]
    # MSYS2's Python lays its virtual environment out as bin/ with no meson
    # launcher a native process can start, but the interpreter runs Meson.
    for python in ('pyvenv/bin/python.exe', 'pyvenv/Scripts/python.exe'):
        if (build / python).exists():
            return [build / python, '-m', 'mesonbuild.mesonmain']
    raise PackageError(f'no meson in {build}/pyvenv; did configure fail?')


def build_qemu(name, arch, build, env, pkgversion, jobs):
    build.mkdir(parents=True, exist_ok=True)
    configure = [SOURCE / 'configure', *configure_flags(name, arch)]
    if name == 'windows':
        # Link the libraries statically, MSYS2's included: left alone,
        # Meson resolves -lgnutls to MSYS2's import library and the program
        # imports its DLL instead of vcpkg's pinned build.
        configure.append('--static')
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
    run([*meson, 'configure', f'-Dpkgversion={pkgversion}', build], env=env)
    # Meson keeps an extracted subproject until told to refresh it, so a
    # source tree that built before a libslirp patch changed would keep the
    # old copy.
    if glob.glob(str(SOURCE / 'subprojects/libslirp*/meson.build')):
        run([*meson, 'subprojects', 'update', '--reset', '--sourcedir', SOURCE,
             'libslirp'], cwd=build, env=env)
    exe = '.exe' if name == 'windows' else ''
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
        directories = glob.glob(str(SOURCE / 'subprojects' / (sub + '*')))
        # A Rust crate's wrap is named for its API version (anyhow-1-rs) and
        # unpacks to its full version (anyhow-1.0.98), which the wrap names.
        wrap = SOURCE / 'subprojects' / (sub + '.wrap')
        if wrap.is_file():
            match = re.search(r'^directory\s*=\s*(\S+)', wrap.read_text(
                encoding='utf-8'), re.M)
            if match:
                directories.append(str(SOURCE / 'subprojects' / match.group(1)))
        for directory in sorted(set(directories)):
            directory = Path(directory)
            if not directory.is_dir() or directory.name == 'packagefiles':
                continue
            for entry in sorted(directory.iterdir()):
                if entry.is_file() and LICENSE_NAME.match(entry.name):
                    found.append((entry, f'{directory.name}.{entry.name}'))
    return found


def stage(name, build, prefix, stage_dir, strip):
    """Lay the archive's tree out in an empty `stage_dir`."""
    exe = '.exe' if name == 'windows' else ''
    if stage_dir.exists():
        shutil.rmtree(stage_dir)
    bin_dir = stage_dir / 'bin'
    bin_dir.mkdir(parents=True)
    for program in PROGRAMS:
        target = bin_dir / (Path(program).name + exe)
        shutil.copy2(build / (program + exe), target)
        if strip:
            strip_binary(name, target)

    # QEMU looks for its data in ../share/qemu beside the executable, and
    # -vnc fails to start without a keymap found there.
    keymaps = stage_dir / 'share/qemu/keymaps'
    shutil.copytree(SOURCE / 'pc-bios/keymaps', keymaps,
                    ignore=shutil.ignore_patterns('meson.build'))

    licences = stage_dir / 'licenses'
    licences.mkdir()
    for source, target in LICENSE_FILES:
        shutil.copy2(SOURCE / source, licences / target)
    for source, target in used_subproject_licences(build):
        shutil.copy2(source, licences / target)
    copy_library_licences(prefix, licences)
    copy_gpu_notices(build, licences)

    if name == 'linux':
        for source, target in LINUX_RUNTIME_LICENCES:
            shutil.copy2(source, licences / target)
    if name == 'windows':
        origins = copy_windows_dlls(bin_dir, [prefix / 'bin'])
        copy_runtime_licences(toolchain_prefix(), origins, licences)
        copy_rust_licences(toolchain_prefix(), licences)


def copy_gpu_notices(build, licences):
    """Copy the GPU back end's crate notices, if this build linked it."""
    config = (build / 'config-host.h').read_text(encoding='utf-8')
    if re.search(r'^#define CONFIG_HAVE_SGI_GPU\b', config, re.M):
        shutil.copy2(SOURCE / GPU_NOTICES,
                     licences / 'sgi-gpu.THIRD-PARTY-NOTICES.txt')


def copy_library_licences(prefix, licences):
    """Copy each library's licence texts from vcpkg's install into `licences`.

    vcpkg installs a copyright file for each library it built. Its own build
    helpers, the vcpkg-* ports, ship nothing. Where a port's file leaves out a
    licence the library is offered under, packaging/licenses/ replaces it.
    """
    ports = set()
    for copyright_file in sorted(glob.glob(str(prefix / 'share/*/copyright'))):
        port = Path(copyright_file).parent.name
        if port.startswith('vcpkg-'):
            continue
        ports.add(port)
        override = HERE / 'licenses' / f'{port}.copyright'
        source = override if override.exists() else Path(copyright_file)
        shutil.copy2(source, licences / f'{port}.copyright')
    for override in sorted((HERE / 'licenses').glob('*.copyright')):
        port = override.name[:-len('.copyright')]
        if port not in ports:
            raise PackageError(f'{override.name} replaces the licence of a '
                               'library this build did not install')


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


# The cryptography stack comes statically from vcpkg on every platform. A
# DLL of it in the Windows archive means the build linked the MSYS2 copy.
CRYPTO_DLLS = re.compile(r'^lib(gnutls|nettle|hogweed|gmp|tasn1|idn2|'
                         r'unistring|p11-kit|brotli\w*)-?[\d.]*\.dll$', re.I)


# Linux links libstdc++ statically (configure's -static-libstdc++, for the CHD
# library's C++). Debian's libstdc++-dev package in the build image carries its
# licence, the GCC runtime exception included, and names GPL-3 by path.
LINUX_RUNTIME_LICENCES = [
    ('/usr/share/doc/libstdc++-12-dev/copyright', 'libstdc++.copyright'),
    ('/usr/share/common-licenses/GPL-3', 'libstdc++.GPL-3'),
]

# DLL names of an MSYS2 toolchain's runtime and the package whose licence
# covers each: GCC's for MINGW64, LLVM's for CLANGARM64.
GCC_RUNTIME_PACKAGES = [
    (re.compile(r'^libstdc\+\+-\d+\.dll$'), 'libstdc++'),
    (re.compile(r'^libgcc_s_\w+-\d+\.dll$'), 'libgcc'),
    (re.compile(r'^libwinpthread-\d+\.dll$'), 'winpthreads'),
]
LLVM_RUNTIME_PACKAGES = [
    (re.compile(r'^libc\+\+\.dll$'), 'libc++'),
    (re.compile(r'^libunwind\.dll$'), 'libunwind'),
    (re.compile(r'^libwinpthread-\d+\.dll$'), 'libwinpthread'),
]


def runtime_packages():
    if os.environ.get('MSYSTEM', '').startswith('CLANG'):
        return LLVM_RUNTIME_PACKAGES
    return GCC_RUNTIME_PACKAGES


def runtime_licence_packages():
    """Packages whose licences ship: the runtime's, plus the static ones."""
    names = [package for _, package in runtime_packages()]
    if runtime_packages() is LLVM_RUNTIME_PACKAGES:
        names.append('compiler-rt')  # builtins linked in, with no DLL
    return names


def copy_windows_dlls(bin_dir, extra_dirs):
    """Copy the DLLs the executables need, and theirs in turn, into bin/.

    MinGW's thread library, and the C++ runtime when the toolchain's shared
    one is linked, come from the toolchain only as DLLs. They are found on
    PATH, which in an MSYS2 MINGW64 shell starts with the toolchain's bin
    directory. Return {DLL name: directory it was copied from}.
    """
    search = [Path(d) for d in os.environ.get('PATH', '').split(os.pathsep)
              if d] + [Path(d) for d in extra_dirs]
    origins = {}
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
            if CRYPTO_DLLS.match(dll):
                raise PackageError(f'{path.name} imports {dll}, so it linked '
                                   "a host copy of the cryptography stack "
                                   'instead of the static vcpkg build')
            for directory in search:
                candidate = directory / dll
                if candidate.is_file():
                    shutil.copy2(candidate, bin_dir / dll)
                    origins[dll] = directory
                    pending.append(bin_dir / dll)
                    break
            else:
                raise PackageError(f'{path.name} needs {dll}, which is on '
                                   'neither PATH nor in the vcpkg tree')
    return origins


def toolchain_prefix():
    """The MSYS2 prefix of the toolchain on PATH, such as .../mingw64."""
    compiler = shutil.which('gcc') or shutil.which('clang')
    if not compiler:
        raise PackageError('no gcc or clang on PATH; build in an MSYS2 '
                           'MINGW64 or CLANGARM64 shell')
    return Path(compiler).resolve().parents[1]


def copy_runtime_licences(prefix, dlls, licences):
    """Copy the licence texts of the MinGW runtime packages into `licences`.

    The executables link the runtime statically (configure's --static), so
    it is in them whether or not a DLL reaches bin/. Each package keeps its
    texts in share/licenses/<package> under the toolchain's prefix. The check
    is on DLL names only: a name that matches is accepted, and any other DLL
    in bin/ is refused, so no DLL ships with an unknown licence.
    """
    for dll in dlls:
        if not any(pattern.match(dll) for pattern, _ in runtime_packages()):
            raise PackageError(f'{dll} is in bin/ but is no known MinGW '
                               'runtime library, so its licence is unknown')
    for package in runtime_licence_packages():
        directory = prefix / 'share/licenses' / package
        texts = (sorted(p for p in directory.iterdir() if p.is_file())
                 if directory.is_dir() else [])
        if not texts:
            raise PackageError(f'no licence texts for {package} in '
                               f'{directory}')
        for text in texts:
            shutil.copy2(text, licences / f'{package}.{text.name}')


def copy_rust_licences(prefix, licences):
    """Copy the Rust toolchain's licence texts into `licences`.

    The executables link Rust's standard library statically. MSYS2's rust
    package keeps the texts of everything it bundles in one directory.
    """
    directory = prefix / 'share/doc/rustc/licenses'
    texts = (sorted(p for p in directory.iterdir() if p.is_file())
             if directory.is_dir() else [])
    if not texts:
        raise PackageError(f'no Rust licence texts in {directory}')
    for text in texts:
        shutil.copy2(text, licences / f'rust.{text.name}')


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
    if args.platform:
        name, arch = next(key for key, value in PLATFORMS.items()
                          if value[0] == args.platform)
    else:
        name, arch = host_platform(), host_architecture()
    if (name, arch) != (host_platform(), host_architecture()):
        raise PackageError(f'{name}-{arch} archives build natively; this '
                           f'host is {host_platform()}-'
                           f'{host_architecture()}')
    if (name, arch) not in PLATFORMS:
        raise PackageError(f'no {name} archive for {arch} hosts')
    version = source_version()
    commit = git_commit(args.commit)
    label = (parse_tag(args.tag, version) if args.tag
             else development_label(version, commit))
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    triplet = PLATFORMS[name, arch][1]

    vcpkg_commit = (HERE / 'vcpkg-commit').read_text(encoding='utf-8').strip()
    tool = ensure_vcpkg(args.vcpkg_root or work / 'vcpkg', vcpkg_commit)
    prefix = install_libraries(tool, triplet, work / 'vcpkg-installed',
                               work / 'vcpkg-binary-cache',
                               VCPKG_ENV.get((name, arch), {}))

    build = work / 'qemu-build'
    env = build_environment(name, prefix, work)
    build_qemu(name, arch, build, env, f'sgi-origami {label}', args.jobs)

    stage_dir = work / 'stage'
    stage(name, build, prefix, stage_dir, not args.no_strip)
    problems = stray_libraries(name, stage_dir / 'bin')
    if problems:
        raise PackageError('executables need libraries outside the archive:\n'
                           + '\n'.join('  ' + p for p in problems))

    target = args.output_dir.resolve() / archive_name(label, name, arch)
    write_archive(name, stage_dir, target, source_epoch())
    print(f'{target} ({target.stat().st_size} bytes)')
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--platform',
                        choices=sorted(v[0] for v in PLATFORMS.values()),
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
