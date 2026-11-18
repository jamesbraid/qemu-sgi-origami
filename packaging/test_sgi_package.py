#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check packaging/sgi-package.py's naming, flags, library check and archives."""

import importlib.machinery
import importlib.util
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_LOADER = importlib.machinery.SourceFileLoader(
    'sgi_package', str(HERE / 'sgi-package.py'))
_SPEC = importlib.util.spec_from_loader('sgi_package', _LOADER)
pkg = importlib.util.module_from_spec(_SPEC)
_LOADER.exec_module(pkg)


class Naming(unittest.TestCase):
    def test_tag_gives_label(self):
        self.assertEqual(pkg.parse_tag('sgi-origami-11.1.50-1', '11.1.50'),
                         '11.1.50-1')

    def test_bad_tags_are_refused(self):
        for tag in ('sgi-11.1.50-1', 'sgi-origami-11.1.50', 'v11.1.0',
                    'sgi-origami-11.1.50-1-rc'):
            with self.assertRaises(pkg.PackageError, msg=tag):
                pkg.parse_tag(tag, '11.1.50')

    def test_tag_must_match_the_tree(self):
        with self.assertRaises(pkg.PackageError):
            pkg.parse_tag('sgi-origami-11.2.50-1', '11.1.50')

    def test_archive_names(self):
        names = {
            ('linux', 'x86_64'): 'linux-x86_64.tar.gz',
            ('linux', 'arm64'): 'linux-arm64.tar.gz',
            ('macos', 'arm64'): 'macos-arm64.tar.gz',
            ('macos', 'x86_64'): 'macos-x86_64.tar.gz',
            ('windows', 'x86_64'): 'windows-x86_64.zip',
            ('windows', 'arm64'): 'windows-arm64.zip',
        }
        self.assertEqual(set(names), set(pkg.PLATFORMS))
        for (name, arch), suffix in names.items():
            self.assertEqual(pkg.archive_name('11.1.50-1', name, arch),
                             f'sgi-origami-11.1.50-1-{suffix}')

    def test_development_label(self):
        self.assertEqual(pkg.development_label('11.1.50', 'abcdef0123'),
                         '11.1.50-gabcdef0')


class Flags(unittest.TestCase):
    def test_every_platform_gets_the_common_list(self):
        for name, _arch in pkg.PLATFORMS:
            flags = pkg.configure_flags(name)
            self.assertIn('--target-list=mips64-softmmu', flags)
            self.assertIn('--enable-chd', flags)
        self.assertIn('--enable-alsa', pkg.configure_flags('linux'))
        self.assertIn('--enable-vmnet', pkg.configure_flags('macos'))
        self.assertIn('--enable-dsound', pkg.configure_flags('windows'))
        self.assertNotIn('--enable-dsound', pkg.configure_flags('linux'))

    def test_architecture_flags_apply_to_that_architecture_only(self):
        self.assertIn('--disable-werror',
                      pkg.configure_flags('windows', 'arm64'))
        self.assertNotIn('--disable-werror',
                         pkg.configure_flags('windows', 'x86_64'))
        self.assertNotIn('--disable-werror', pkg.configure_flags('windows'))

    def test_rust_follows_rust_platforms(self):
        for name in {n for n, _ in pkg.PLATFORMS}:
            self.assertEqual('--enable-rust' in pkg.configure_flags(name),
                             name in pkg.RUST_PLATFORMS, name)


class Runtime(unittest.TestCase):
    def test_clang_environment_has_llvm_runtime(self):
        with mock.patch.dict('os.environ', {'MSYSTEM': 'CLANGARM64'}):
            names = [p for _, p in pkg.runtime_packages()]
        self.assertIn('libc++', names)
        self.assertNotIn('libstdc++', names)
        with mock.patch.dict('os.environ', {'MSYSTEM': 'MINGW64'}):
            names = [p for _, p in pkg.runtime_packages()]
        self.assertIn('libstdc++', names)


class SubprojectLicences(unittest.TestCase):
    def test_rust_crate_licences_are_found_through_the_wrap(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp)
            crate = source / 'subprojects/anyhow-1.0.98'
            crate.mkdir(parents=True)
            (crate / 'LICENSE-MIT').write_text('mit')
            (crate / 'Cargo.toml').write_text('')
            (source / 'subprojects/anyhow-1-rs.wrap').write_text(
                '[wrap-file]\ndirectory = anyhow-1.0.98\n')
            build = source / 'build'
            (build / 'meson-info').mkdir(parents=True)
            (build / 'meson-info/intro-projectinfo.json').write_text(
                '{"subprojects": [{"name": "anyhow-1-rs"}]}')
            old, pkg.SOURCE = pkg.SOURCE, source
            try:
                found = pkg.used_subproject_licences(build)
            finally:
                pkg.SOURCE = old
        self.assertEqual([(p.name, n) for p, n in found],
                         [('LICENSE-MIT', 'anyhow-1.0.98.LICENSE-MIT')])


class Libraries(unittest.TestCase):
    ELF = """\
Dynamic section at offset 0x1 contains 3 entries:
  Tag        Type                         Name/Value
 0x0000000000000001 (NEEDED)             Shared library: [libasound.so.2]
 0x0000000000000001 (NEEDED)             Shared library: [libz.so.1]
"""
    PE = """\
\tDLL Name: KERNEL32.dll
\tDLL Name: libwinpthread-1.dll
"""
    MACHO = """\
bin/qemu-img:
\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)
\t/opt/homebrew/lib/libglib-2.0.0.dylib (compatibility version 1.0.0)
"""

    def test_parsers(self):
        self.assertEqual(pkg.elf_needed(self.ELF),
                         ['libasound.so.2', 'libz.so.1'])
        self.assertEqual(pkg.pe_needed(self.PE),
                         ['KERNEL32.dll', 'libwinpthread-1.dll'])
        self.assertEqual(pkg.macho_needed(self.MACHO),
                         ['/usr/lib/libSystem.B.dylib',
                          '/opt/homebrew/lib/libglib-2.0.0.dylib'])

    def test_host_libraries(self):
        self.assertTrue(pkg.is_host_library('linux', 'libasound.so.2'))
        self.assertFalse(pkg.is_host_library('linux', 'libz.so.1'))
        self.assertTrue(pkg.is_host_library('linux', 'ld-linux-aarch64.so.1'))
        self.assertTrue(pkg.is_host_library('macos',
                                            '/usr/lib/libSystem.B.dylib'))
        self.assertFalse(pkg.is_host_library('macos',
                                             '/opt/homebrew/lib/libz.dylib'))
        self.assertTrue(pkg.is_host_library('windows', 'KERNEL32.dll'))
        self.assertTrue(pkg.is_host_library('windows', 'ncrypt.dll'))
        self.assertTrue(pkg.CRYPTO_DLLS.match('libgnutls-30.dll'))
        self.assertFalse(pkg.CRYPTO_DLLS.match('libwinpthread-1.dll'))
        self.assertFalse(pkg.is_host_library('windows',
                                             'libwinpthread-1.dll'))

    def check(self, name, wanted, present=()):
        with tempfile.TemporaryDirectory() as tmp:
            bin_dir = Path(tmp)
            (bin_dir / 'qemu-img').touch()
            for lib in present:
                (bin_dir / lib).touch()
            return pkg.stray_libraries(
                name, bin_dir,
                lambda _name, path: wanted.get(path.name, []))

    def test_missing_library_is_reported(self):
        self.assertEqual(
            self.check('linux', {'qemu-img': ['libc.so.6', 'libz.so.1']}),
            ['qemu-img: libz.so.1'])

    def test_library_in_the_archive_is_followed(self):
        wanted = {'qemu-img': ['libz.so.1'], 'libz.so.1': ['libfoo.so.1']}
        self.assertEqual(self.check('linux', wanted, ['libz.so.1']),
                         ['libz.so.1: libfoo.so.1'])


class Licences(unittest.TestCase):
    def prefix(self, tmp, ports):
        prefix = Path(tmp) / 'prefix'
        for port, text in ports.items():
            (prefix / 'share' / port).mkdir(parents=True)
            (prefix / 'share' / port / 'copyright').write_text(text)
        return prefix

    def test_vcpkg_copyright_is_copied_unless_overridden(self):
        ports = {'gmp': 'vcpkg gmp', 'nettle': 'vcpkg nettle',
                 'libunistring': 'vcpkg libunistring', 'vcpkg-cmake': 'x'}
        with tempfile.TemporaryDirectory() as tmp:
            licences = Path(tmp) / 'licences'
            licences.mkdir()
            pkg.copy_library_licences(self.prefix(tmp, ports), licences)
            self.assertEqual((licences / 'gmp.copyright').read_text(),
                             'vcpkg gmp')
            self.assertEqual((licences / 'nettle.copyright').read_bytes(),
                             (pkg.HERE / 'licenses/nettle.copyright')
                             .read_bytes())
            self.assertFalse((licences / 'vcpkg-cmake.copyright').exists())

    def test_gpu_notices_ship_only_with_the_gpu_back_end(self):
        for config, shipped in (('#define CONFIG_HAVE_SGI_GPU\n', True),
                                ('#define CONFIG_HAVE_RUST\n', False)):
            with tempfile.TemporaryDirectory() as tmp:
                build, licences = Path(tmp) / 'build', Path(tmp) / 'licences'
                build.mkdir()
                licences.mkdir()
                (build / 'config-host.h').write_text(config)
                pkg.copy_gpu_notices(build, licences)
                self.assertEqual(
                    (licences / 'sgi-gpu.THIRD-PARTY-NOTICES.txt').exists(),
                    shipped)

    def test_override_needs_its_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            licences = Path(tmp) / 'licences'
            licences.mkdir()
            with self.assertRaises(pkg.PackageError):
                pkg.copy_library_licences(self.prefix(tmp, {'gmp': 'x'}),
                                          licences)

    def test_runtime_licences_are_always_shipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            mingw = Path(tmp) / 'mingw64'
            runtime = {'libstdc++': ['COPYING3', 'COPYING.RUNTIME'],
                       'libgcc': ['COPYING3', 'COPYING.RUNTIME'],
                       'winpthreads': ['COPYING']}
            for package, texts in runtime.items():
                (mingw / 'share/licenses' / package).mkdir(parents=True)
                for text in texts:
                    (mingw / 'share/licenses' / package / text).write_text(
                        package + text)
            licences = Path(tmp) / 'licences'
            licences.mkdir()
            pkg.copy_runtime_licences(mingw, ['libgcc_s_seh-1.dll'], licences)
            self.assertEqual(sorted(p.name for p in licences.iterdir()),
                             ['libgcc.COPYING.RUNTIME', 'libgcc.COPYING3',
                              'libstdc++.COPYING.RUNTIME',
                              'libstdc++.COPYING3', 'winpthreads.COPYING'])

    def test_unknown_runtime_dll_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(pkg.PackageError):
                pkg.copy_runtime_licences(Path(tmp), ['libfoo-1.dll'],
                                          Path(tmp))

    def test_runtime_without_texts_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(pkg.PackageError):
                pkg.copy_runtime_licences(Path(tmp), [], Path(tmp))


class Archives(unittest.TestCase):
    def stage(self, tmp):
        stage = Path(tmp) / 'stage'
        (stage / 'bin').mkdir(parents=True)
        (stage / 'share/keymaps').mkdir(parents=True)
        (stage / 'bin/qemu-img').write_bytes(b'x')
        (stage / 'bin/qemu-img').chmod(0o755)
        (stage / 'share/keymaps/de').write_text('map')
        return stage

    def test_tar_has_no_wrapping_directory_and_keeps_modes(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'out.tar.gz'
            pkg.write_archive('linux', self.stage(tmp), target, 1000)
            with tarfile.open(target) as archive:
                members = {m.name: m for m in archive.getmembers()}
            self.assertIn('bin/qemu-img', members)
            self.assertEqual(members['bin/qemu-img'].mode, 0o755)
            self.assertEqual(members['share/keymaps/de'].mode, 0o644)
            self.assertEqual(members['bin/qemu-img'].uid, 0)

    def test_archive_is_reproducible(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = self.stage(tmp)
            first, second = Path(tmp) / 'a.tar.gz', Path(tmp) / 'b.tar.gz'
            pkg.write_archive('linux', stage, first, 1000)
            pkg.write_archive('linux', stage, second, 1000)
            self.assertEqual(first.read_bytes(), second.read_bytes())

    def test_zip_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            target = Path(tmp) / 'out.zip'
            pkg.write_archive('windows', self.stage(tmp), target, 0)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(sorted(archive.namelist()),
                                 ['bin/qemu-img', 'share/keymaps/de'])


class RustLicences(unittest.TestCase):
    def test_texts_are_copied_and_a_missing_directory_is_refused(self):
        with tempfile.TemporaryDirectory() as tmp:
            prefix, licences = Path(tmp) / 'mingw64', Path(tmp) / 'licenses'
            licences.mkdir()
            with self.assertRaises(pkg.PackageError):
                pkg.copy_rust_licences(prefix, licences)
            texts = prefix / 'share/doc/rustc/licenses'
            texts.mkdir(parents=True)
            (texts / 'MIT.txt').write_text('mit')
            pkg.copy_rust_licences(prefix, licences)
            self.assertEqual([p.name for p in licences.iterdir()],
                             ['rust.MIT.txt'])


if __name__ == '__main__':
    unittest.main()
