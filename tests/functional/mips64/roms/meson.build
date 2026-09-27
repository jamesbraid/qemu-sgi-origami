# SPDX-License-Identifier: GPL-2.0-or-later

sgi_rom_cc = find_program('mips64-linux-gnuabi64-gcc', required: false)
sgi_rom_objcopy = find_program('mips64-linux-gnuabi64-objcopy', required: false)
sgi_roms_available = sgi_rom_cc.found() and sgi_rom_objcopy.found()
test_mips64_system_extra_deps = {}
if sgi_roms_available
  sgi_rom_flags = ['-mabi=64', '-march=mips3', '-EB', '-msym32', '-msoft-float', '-G0', '-mno-abicalls', '-fno-pic', '-fno-builtin', '-ffreestanding', '-fno-common', '-fno-stack-protector', '-fno-asynchronous-unwind-tables', '-Os', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter']
  sgi_rom_flags += ['-I' + (meson.current_source_dir() / 'lib')]
  sgi_rom_lib = files('lib/crt0.S', 'lib/exc.S', 'lib/rom.c')
  sgi_rom_headers = files('lib/rom.h', 'lib/exc-offsets.h', 'lib/rom.ld',
                          'lib/probe-symbols.h')
  sgi_rom_targets = []
  foreach name : ['addrspace', 'branchlikely', 'bridgebus', 'buserror', 'bwinnode', 'cacheecc', 'cachefaults', 'cacheops', 'cachetags', 'cp0misc', 'cpuid', 'diagregs', 'exceptions', 'foldback', 'fpucond', 'framemask', 'hubint', 'icountslot', 'intchain', 'intdeliver', 'isadecode', 'llsc', 'llscexc', 'llscscale', 'mips4user', 'pcachedata', 'pcachetags', 'perfcount', 'r14000_cache', 'r14000_exceptions', 'r14000_identity', 'r14000_perf', 'scachedata', 'scachetags', 'sext', 'timer', 'timerstall', 'tlb', 'tlbconflict', 'tlbshadow', 'tlbspan', 'tlbuncached', 'watch', 'xcontext', 'xkphys', 'xtlbvector']
    elf = custom_target('sgi-rom-' + name + '-elf',
      input: files(name + '.c'),
      output: name + '.elf',
      command: [sgi_rom_cc, sgi_rom_flags, '-nostdlib', '-no-pie',
                '-Wl,--no-warn-mismatch', '-Wl,--build-id=none',
                '-T', meson.current_source_dir() / 'lib/rom.ld',
                sgi_rom_lib, '@INPUT@', '-o', '@OUTPUT@'],
      depend_files: [sgi_rom_lib, sgi_rom_headers],
      build_by_default: false)
    sgi_rom_targets += custom_target('sgi-rom-' + name,
      input: elf, output: name + '.bin',
      command: [sgi_rom_objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
      build_by_default: false)
  endforeach
  alias_target('sgi-roms', sgi_rom_targets)
  test_mips64_system_extra_deps = {'sgi_roms': sgi_rom_targets}
endif
