# SPDX-License-Identifier: BSD-3-Clause

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
  foreach name : ['addrspace', 'branchlikely', 'bridgebus', 'buserror', 'bwinnode', 'cacheecc', 'cachefaults', 'cacheops', 'cachetags', 'cp0misc', 'cpuid', 'diagregs', 'exceptions', 'foldback', 'fpucond', 'framemask', 'hubint', 'icountslot', 'intchain', 'intdeliver', 'isadecode', 'llsc', 'llscexc', 'llscscale', 'mips4user', 'pcachedata', 'pcachetags', 'perfcount', 'r10000_remap', 'r14000_cache', 'r14000_exceptions', 'r14000_identity', 'r14000_perf', 'scachedata', 'scachetags', 'sext', 'timer', 'timerstall', 'tlb', 'tlbconflict', 'tlbshadow', 'tlbspan', 'tlbuncached', 'watch', 'xcontext', 'xkphys', 'xtlbvector']
    rom_headers = sgi_rom_headers
    if name == 'r10000_remap'
      rom_headers += files('r10000-remap.h')
    endif
    elf = custom_target('sgi-rom-' + name + '-elf',
      input: files(name + '.c'),
      output: name + '.elf',
      command: [sgi_rom_cc, sgi_rom_flags, '-nostdlib', '-no-pie',
                '-Wl,--no-warn-mismatch', '-Wl,--build-id=none',
                '-T', meson.current_source_dir() / 'lib/rom.ld',
                sgi_rom_lib, '@INPUT@', '-o', '@OUTPUT@'],
      depend_files: [sgi_rom_lib, rom_headers],
      build_by_default: false)
    rom_target = custom_target('sgi-rom-' + name,
      input: elf, output: name + '.bin',
      command: [sgi_rom_objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
      build_by_default: false)
    sgi_rom_targets += rom_target
    if name == 'timerstall'
      sgi_timerstall_rom = rom_target
    endif
  endforeach
  # The benchmark repeats each replacement many times; the correctness
  # test runs the same checks a few times over each page size.
  sgi_tlbmaint_roms = []
  sgi_tlbmaint_check_roms = []
  foreach name, config : {'4k': ['0', '4096', '65536'],
                          '16k': ['0x6000', '16384', '65536'],
                          '64k': ['0x1e000', '65536', '8192'],
                          '16m': ['0x1ffe000', '16777216', '128'],
                          '4k-check': ['0', '4096', '16'],
                          '16k-check': ['0x6000', '16384', '16'],
                          '64k-check': ['0x1e000', '65536', '16'],
                          '16m-check': ['0x1ffe000', '16777216', '16']}
    elf = custom_target('sgi-rom-tlbmaint-' + name + '-elf',
      input: files('tlbmaint.c'), output: 'tlbmaint-' + name + '.elf',
      command: [sgi_rom_cc, sgi_rom_flags,
                '-DPAGE_MASK=' + config[0], '-DPAGE_BYTES=' + config[1],
                '-DITERATIONS=' + config[2], '-nostdlib', '-no-pie',
                '-Wl,--no-warn-mismatch', '-Wl,--build-id=none',
                '-T', meson.current_source_dir() / 'lib/rom.ld',
                sgi_rom_lib, '@INPUT@', '-o', '@OUTPUT@'],
      depend_files: [sgi_rom_lib, sgi_rom_headers], build_by_default: false)
    rom = custom_target('sgi-rom-tlbmaint-' + name,
      input: elf, output: 'tlbmaint-' + name + '.bin',
      command: [sgi_rom_objcopy, '-O', 'binary', '@INPUT@', '@OUTPUT@'],
      build_by_default: false)
    if name.endswith('-check')
      sgi_tlbmaint_check_roms += rom
    else
      sgi_tlbmaint_roms += rom
    endif
  endforeach
  test_mips64_system_extra_deps += {
    'mips_tlb_performance': sgi_tlbmaint_roms,
    'mips_tlb_replacement': sgi_tlbmaint_check_roms,
  }
  alias_target('sgi-roms', sgi_rom_targets)
  test_mips64_system_extra_deps += {'sgi_roms': sgi_rom_targets}
endif
