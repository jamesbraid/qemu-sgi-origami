#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Check packaging/sgi-package.py's naming, flags, library check and archives."""

import importlib.machinery
import importlib.util
import tarfile
import tempfile
import unittest
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
        self.assertEqual(pkg.archive_name('11.1.50-1', 'linux'),
                         'sgi-origami-11.1.50-1-linux-x86_64.tar.gz')
        self.assertEqual(pkg.archive_name('11.1.50-1', 'macos'),
                         'sgi-origami-11.1.50-1-macos-arm64.tar.gz')
        self.assertEqual(pkg.archive_name('11.1.50-1', 'windows'),
                         'sgi-origami-11.1.50-1-windows-x86_64.zip')

    def test_development_label(self):
        self.assertEqual(pkg.development_label('11.1.50', 'abcdef0123'),
                         '11.1.50-gabcdef0')


class Flags(unittest.TestCase):
    def test_every_platform_gets_the_common_list(self):
        for name in pkg.PLATFORMS:
            flags = pkg.configure_flags(name)
            self.assertIn('--target-list=mips64-softmmu', flags)
            self.assertIn('--enable-chd', flags)
        self.assertIn('--enable-alsa', pkg.configure_flags('linux'))
        self.assertIn('--enable-vmnet', pkg.configure_flags('macos'))
        self.assertIn('--enable-dsound', pkg.configure_flags('windows'))
        self.assertNotIn('--enable-dsound', pkg.configure_flags('linux'))


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
        self.assertTrue(pkg.is_host_library('macos',
                                            '/usr/lib/libSystem.B.dylib'))
        self.assertFalse(pkg.is_host_library('macos',
                                             '/opt/homebrew/lib/libz.dylib'))
        self.assertTrue(pkg.is_host_library('windows', 'KERNEL32.dll'))
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


if __name__ == '__main__':
    unittest.main()
