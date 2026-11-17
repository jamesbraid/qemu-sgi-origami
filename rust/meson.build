if not have_system
  subdir_done()
else
  message('Rust enabled but it is only used by system emulators.')
endif

cargo_ws = import('rust').workspace()

# cargo run through `meson devenv` must not write into the tracked target/.
meson.add_devenv(environment({'CARGO_TARGET_DIR': meson.project_build_root() / 'rust-target'}))

genrs = []

subdir('qemu-macros')

subdir('common')
subdir('bindings')
subdir('bits')

subdir('util')
subdir('bql')
subdir('migration')
subdir('qom')
subdir('chardev')
subdir('hw/core')
subdir('system')
subdir('tests')
subdir('trace')
subdir('hw')

cargo = find_program('cargo', required: false)

if cargo.found()
  run_target('rustfmt',
    command: [config_host['MESON'], 'devenv',
              '--workdir', '@CURRENT_SOURCE_DIR@',
              cargo, 'fmt'],
    depends: genrs)
endif
