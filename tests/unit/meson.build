
testblock = declare_dependency(dependencies: [block], sources: 'iothread.c')

# Unit tests selected by the SGI Meson suite, listed by test name.
fork_unit_tests = [
  'test-onewire', 'test-i2c-masters', 'test-i2c-wire',
  'test-sgi-o2000-module-mgmt', 'test-sgi-o2000-module-mgmt-real',
  'test-sgi-router', 'test-sn0-fabric-routed', 'test-sn1-topology',
  'test-sn-model', 'test-sgi-prom-image', 'test-sgi-initial-flash',
  'test-sgi-machine-init', 'test-sgi-synthetic-serial',
  'test-sgi-bedrock-ni', 'test-sgi-pi-gfx',
  'test-sgi-rad1-audio-pcm', 'test-sgi-odyssey-planes',
  'test-sgi-odyssey-native-mesa',
  'test-sgi-kona-ports', 'test-sgi-kona-gl-decode', 'test-sgi-x-gc',
  'test-sgi-memory', 'test-sgi-dma', 'test-sgi-scanout', 'test-sgi-gl',
  'test-sgi-gl-vmstate', 'test-sgi-kona-texture',
  'test-sgi-renderer', 'test-sgi-renderer-gl',
  'test-sgi-render-resource', 'test-sgi-egl-display',
]

tests = {
  'check-block-qdict': [],
  'check-qdict': [],
  'check-qnum': [],
  'check-qstring': [],
  'check-qlist': [],
  'check-qnull': [],
  'check-qobject': [],
  'check-qjson': [],
  'check-json-parser': [],
  'check-qlit': [],
  'test-error-report': [],
  'test-qobject-output-visitor': [testqapi],
  'test-clone-visitor': [testqapi],
  'test-qobject-input-visitor': [testqapi],
  'test-forward-visitor': [testqapi],
  'test-string-input-visitor': [testqapi],
  'test-string-output-visitor': [testqapi],
  'test-visitor-serialization': [testqapi],
  'test-bitmap': [],
  'test-resv-mem': [],
  # all code tested by test-x86-topo is inside topology.h
  'test-x86-topo': [],
  'test-cutils': [],
  'test-div128': [],
  'test-shift128': [],
  'test-mul64': [],
  # all code tested by test-int128 is inside int128.h
  'test-int128': [],
  'rcutorture': [],
  'test-rcu-list': [],
  'test-rcu-simpleq': [],
  'test-rcu-tailq': [],
  'test-rcu-slist': [],
  'test-qdist': [],
  'test-qht': [],
  'test-qtree': [],
  'test-bitops': [],
  'test-bitcnt': [],
  'test-qgraph': ['../qtest/libqos/qgraph.c'],
  'check-qom-interface': [qom],
  'check-qom-proplist': [qom],
  'test-qemu-opts': [],
  'test-keyval': [testqapi],
  'test-onewire': ['../../hw/1wire/crc.c', '../../hw/1wire/core.c',
                   '../../hw/1wire/ds2502.c', '../../hw/1wire/ds2505.c',
                   qom, hwcore, migration],
  'test-i2c-masters': ['../../hw/i2c/core.c', '../../hw/i2c/i2c-wire.c',
                       qom, hwcore, migration, io],
  'test-i2c-wire': ['../../hw/i2c/core.c', '../../hw/i2c/i2c-wire.c',
                    qom, hwcore, migration, io],
  'test-sgi-o2000-module-mgmt': ['../../hw/core/reset.c',
                                 '../../hw/core/qdev-properties-system.c',
                                 '../../hw/core/sysbus.c',
                                 '../../hw/core/qdev-fw.c',
                                 '../../hw/core/fw-path-provider.c',
                                 '../../hw/1wire/crc.c',
                                 '../../hw/1wire/core.c',
                                 '../../hw/1wire/ds2502.c',
                                 '../../hw/1wire/ds2505.c',
                                 '../../hw/misc/sgi-board-record.c',
                                 '../../hw/i2c/core.c',
                                 '../../hw/i2c/i2c-wire.c',
                                 '../../hw/i2c/pcf8584.c',
                                 '../../hw/misc/sgi-elsc.c',
                                 '../../hw/misc/sgi-sn0-module-management.c',
                                 '../../hw/nvram/sgi-elsc-nvram.c',
                                 '../../hw/nvram/sgi-elsc-nvram-image.c',
                                 'test-sgi-o2000-module-mgmt-stubs.c',
                                 qom, hwcore, migration, io, chardev, block],
  'test-sgi-o2000-module-mgmt-real': ['../../hw/core/reset.c',
                                 '../../hw/core/qdev-properties-system.c',
                                 '../../hw/core/sysbus.c',
                                 '../../hw/core/qdev-fw.c',
                                 '../../hw/core/fw-path-provider.c',
                                 '../../hw/1wire/crc.c',
                                 '../../hw/1wire/core.c',
                                 '../../hw/1wire/ds2502.c',
                                 '../../hw/1wire/ds2505.c',
                                 '../../hw/misc/sgi-board-record.c',
                                 '../../hw/i2c/core.c',
                                 '../../hw/i2c/i2c-wire.c',
                                 '../../hw/i2c/pcf8584.c',
                                 '../../hw/misc/sgi-elsc.c',
                                 '../../hw/misc/sgi-sn0-module-management.c',
                                 '../../hw/nvram/sgi-elsc-nvram.c',
                                 '../../hw/nvram/sgi-elsc-nvram-image.c',
                                 'test-sgi-o2000-module-mgmt-stubs.c',
                                 qom, hwcore, migration, io, chardev, block],
  'test-sgi-router': ['../../hw/misc/sgi-router.c',
                      '../../hw/misc/sgi-board-record.c',
                      '../../hw/mips/sgi/net-port.c',
                      '../../hw/mips/sgi/net-cable.c',
                      '../../hw/core/reset.c',
                      '../../hw/1wire/core.c', '../../hw/1wire/crc.c',
                      '../../hw/1wire/ds2505.c',
                      qom, hwcore, migration, io],
  'test-sn0-fabric-routed': ['../../hw/misc/sgi-router.c',
                            '../../hw/misc/sgi-board-record.c',
                            '../../hw/mips/sgi/net-port.c',
                            '../../hw/mips/sgi/net-cable.c',
                            '../../hw/core/reset.c', '../../hw/1wire/core.c',
                            '../../hw/1wire/crc.c', '../../hw/1wire/ds2505.c',
                            qom, hwcore, migration, io],
  'test-sn1-topology': ['../../hw/mips/sgi/sn1-topology.c'],
  'test-sn-model': ['../../hw/mips/sgi/sn-model-runtime.c'],
  'test-sgi-prom-image': ['../../hw/mips/sgi/prom-image.c', zlib],
  'test-sgi-initial-flash': ['../../hw/mips/sgi/initial-flash.c'],
  # all code tested by test-sgi-synthetic-serial is inside its header
  'test-sgi-synthetic-serial': [],
  # all code tested by test-sgi-rad1-audio-pcm is inside sgi-rad1-audio-pcm.h
  'test-sgi-rad1-audio-pcm': [],
  'test-logging': [],
  'test-qapi-util': [],
  'test-interval-tree': [],
  'test-fifo': [],
  'test-envlist': [],
}

if have_system or have_tools
  tests += {
    'test-qmp-event': [testqapi],
  }

  if seccomp.found()
    tests += {'test-seccomp': ['../../system/qemu-seccomp.c', seccomp]}
  endif
endif

if have_block
  tests += {
    'test-coroutine': [testblock],
    'test-aio': [testblock],
    'test-aio-multithread': [testblock],
    'test-throttle': [testblock],
    'test-thread-pool': [testblock],
    'test-hbitmap': [testblock],
    'test-bdrv-drain': [testblock],
    'test-bdrv-graph-mod': [testblock],
    'test-blockjob': [testblock],
    'test-blockjob-txn': [testblock],
    'test-block-backend': [testblock],
    'test-block-accounting': [testblock],
    'test-block-iothread': [testblock],
    'test-write-threshold': [testblock],
    'test-crypto-hash': [crypto],
    'test-crypto-hmac': [crypto],
    'test-crypto-cipher': [crypto],
    'test-crypto-akcipher': [crypto],
    'test-crypto-secret': [crypto, keyutils],
    'test-crypto-der': [crypto],
    'test-authz-simple': [authz],
    'test-authz-list': [authz],
    'test-authz-listfile': [authz],
    'test-io-task': [testblock],
    'test-io-channel-socket': ['socket-helpers.c', 'io-channel-helpers.c', io],
    'test-io-channel-file': ['io-channel-helpers.c', io],
    'test-io-channel-command': ['io-channel-helpers.c', io],
    'test-io-channel-buffer': ['io-channel-helpers.c', io],
    'test-io-channel-null': [io],
    'test-crypto-ivgen': [io],
    'test-crypto-afsplit': [io],
    'test-crypto-block': [io],
    'test-timed-average': [],
    'test-uuid': [],
  }
  if gnutls.found() and \
     tasn1.found() and \
     host_os != 'windows'
    tests += {
      'test-crypto-tlscredsx509': ['crypto-tls-x509-helpers.c',
                                   tasn1, crypto, gnutls],
      'test-crypto-tlssession': ['crypto-tls-x509-helpers.c', 'crypto-tls-psk-helpers.c',
                                 tasn1, crypto, gnutls],
      'test-io-channel-tls': ['io-channel-helpers.c', 'crypto-tls-x509-helpers.c',
                              tasn1, io, crypto, gnutls]}
  endif
  if pam.found()
    tests += {'test-authz-pam': [authz]}
  endif
  if host_os != 'windows'
    tests += {
      'test-io-channel-websock': [io],
      'test-image-locking': [testblock],
      'test-nested-aio-poll': [],
    }
  endif
  if config_host_data.get('CONFIG_REPLICATION')
    tests += {'test-replication': [testblock]}
  endif
  tests += {'test-crypto-pbkdf': [io]}
endif

if have_system and opengl.found()
  renderer_gl_test = ['../../hw/display/sgi/renderer-gl.c',
                      '../../hw/display/sgi/renderer.c',
                      '../../hw/display/sgi/render-resource.c',
                      '../../hw/display/sgi/gl-context.c',
                      '../../hw/display/sgi/rasterizer.c',
                      '../../hw/display/sgi/x-gc.c',
                      '../../hw/display/sgi/board-memory.c',
                      '../../ui/sgi-renderer.c', '../../ui/egl-helpers.c',
                      '../../ui/dmabuf.c', opengl, pixman, migration, io]
  if gbm.found()
    renderer_gl_test += ['../../util/drm.c', gbm]
    tests += {
      'test-sgi-egl-display': ['../../ui/egl-helpers.c', '../../ui/egl-context.c',
                               '../../ui/dmabuf.c',
                               opengl, gbm, pixman],
    }
  endif
  tests += {'test-sgi-renderer-gl': renderer_gl_test}
  tests += {
    'test-sgi-odyssey-native-mesa': [
      '../../hw/misc/sgi-odyssey-native-mesa.c',
      '../../hw/display/sgi/board-memory.c',
      migration, io,
    ] + renderer_gl_test,
  }
endif

if have_system
  odyssey_planes_test = ['../../hw/misc/sgi-odyssey-planes.c',
                         '../../hw/display/sgi/render-resource.c']
  if opengl.found()
    odyssey_planes_test = ['../../hw/misc/sgi-odyssey-planes.c'] + renderer_gl_test
  endif
  tests += {
    'test-sgi-renderer': ['../../ui/sgi-renderer.c'],
    'ptimer-test': ['ptimer-test-stubs.c', meson.project_source_root() / 'hw/core/ptimer.c'],
    'test-sgi-bedrock-ni': ['../../hw/misc/sgi-bedrock-ni-regs.c'],
    'test-sgi-pi-gfx': ['../../hw/mips/sgi/pi-gfx.c'],
    'test-sgi-odyssey-planes': odyssey_planes_test,
    'test-sgi-kona-ports': ['../../hw/misc/sgi-command-port.c',
                            '../../hw/misc/sgi-kona-ports.c'],
    'test-sgi-kona-gl-decode': ['../../hw/misc/sgi-kona-gl-decode.c'],
    'test-sgi-x-gc': ['../../hw/display/sgi/x-gc.c',
                      '../../hw/display/sgi/board-memory.c', migration, io],
    'test-sgi-memory': ['../../hw/display/sgi/board-memory.c',
                        migration, io],
    'test-sgi-gl': ['../../hw/display/sgi/gl-context.c',
                    '../../hw/display/sgi/x-gc.c',
                    '../../hw/display/sgi/rasterizer.c',
                    '../../hw/display/sgi/board-memory.c',
                    migration, io],
    'test-sgi-render-resource': ['../../hw/display/sgi/render-resource.c'],
    'test-sgi-kona-texture': ['../../hw/misc/sgi-kona-texture.c',
                              '../../hw/misc/sgi-kona-gl-decode.c',
                              '../../hw/display/sgi/gl-context.c',
                              '../../hw/display/sgi/x-gc.c',
                              '../../hw/display/sgi/rasterizer.c',
                              '../../hw/display/sgi/board-memory.c',
                              migration, io],
    'test-sgi-gl-vmstate': ['../../hw/display/sgi/gl-context.c',
                            migration, io],
    'test-sgi-dma': ['../../hw/display/sgi/board-memory.c',
                     '../../hw/display/sgi/pixel-dma.c',
                     '../../hw/display/sgi/command-fetch.c',
                     migration, io],
    'test-sgi-scanout': ['../../hw/display/sgi/scanout.c',
                         migration, io, pixman, opengl],
    'test-iov': [],
    'test-opts-visitor': [testqapi],
    'test-xs-node': [qom],
    'test-virtio-dmabuf': [meson.project_source_root() / 'hw/display/virtio-dmabuf.c'],
    'test-qmp-cmds': [testqapi],
    'test-xbzrle': [migration],
    'test-util-sockets': ['socket-helpers.c'],
    'test-base64': [],
    'test-bufferiszero': [],
    'test-smp-parse': [qom, meson.project_source_root() / 'hw/core/machine-smp.c'],
    'test-vmstate': [migration, io],
    'test-yank': ['socket-helpers.c', qom, io, chardev],
    'test-ram-discard-manager': [
      'test-ram-discard-manager.c',
      'test-ram-discard-manager-stubs.c',
      meson.project_source_root() / 'system/ram-discard-manager.c',
      genh, qemuutil, qom
    ],
  }
  if config_host_data.get('CONFIG_INOTIFY1')
    tests += {'test-util-filemonitor': []}
  endif

  # Some tests: test-char, test-qdev-global-props, and test-qga,
  # are not runnable under TSan due to a known issue.
  # https://github.com/google/sanitizers/issues/1116
  if not get_option('tsan')
    if host_os != 'windows'
        tests += {
          'test-char': ['socket-helpers.c', 'wctablet-stubs.c',
                       '../../chardev/wctablet.c', qom, io, chardev]
        }
    endif

    tests += {
      'test-qdev-global-props': [qom, hwcore]
    }
  endif
  tests += {'test-qdev': [qom, hwcore]}
endif

if have_ga and host_os == 'linux'
  tests += {'test-qga': ['../qtest/libqmp.c', '../qtest/libqtest.c']}
  test_deps += {'test-qga': qga}
endif

test_env = environment()
test_env.set('G_TEST_SRCDIR', meson.current_source_dir())
test_env.set('G_TEST_BUILDDIR', meson.current_build_dir())

# Runs the built tool rather than linking its sources.
if is_variable('qemu_sgi_machine_init')
  tests += {'test-sgi-machine-init': ['../../hw/rtc/m48t35-ip27.c']}
  test_deps += {'test-sgi-machine-init': qemu_sgi_machine_init}
  test_env.set('QEMU_SGI_MACHINE_INIT', qemu_sgi_machine_init.full_path())
endif

slow_tests = {
  'test-aio-multithread' : 120,
  'test-bufferiszero': 60,
  'test-crypto-block' : 300,
  'test-crypto-tlscredsx509': 90,
  'test-crypto-tlssession': 90,
  'test-replication': 60,
  'rcutorture': 30,
  'test-rcu-list': 30,
  'test-rcu-simpleq': 30,
  'test-rcu-tailq': 30,
  'test-rcu-slist': 30,
  'test-thread-pool': 30,
}

foreach test_name, extra: tests
  src = [test_name + '.c']
  deps = [qemuutil]
  if extra.length() > 0
    # use a sourceset to quickly separate sources and deps
    test_ss = ss.source_set()
    test_ss.add(extra)
    src += test_ss.all_sources()
    deps += test_ss.all_dependencies()
  endif
  test_link_args = []
  test_c_args = []
  if test_name in ['test-sgi-gl', 'test-sgi-gl-vmstate'] and meson.get_compiler('c').has_link_argument('-Wl,--wrap=g_try_realloc_n')
    test_link_args = ['-Wl,--wrap=g_try_realloc_n']
    test_c_args = ['-DSGI_GL_ALLOCATION_WRAP']
  endif
  if test_name == 'test-sgi-gl'
    test_c_args += ['-DSGI_GL_TEXTURE_TEST']
  endif
  exe = executable(test_name, src, genh, dependencies: deps,
                   link_args: test_link_args, c_args: test_c_args)

  test(test_name, exe,
       depends: test_deps.get(test_name, []),
       env: test_env,
       args: ['--tap', '-k'],
       protocol: 'tap',
       timeout: slow_tests.get(test_name, 30),
       priority: slow_tests.get(test_name, 30),
       suite: ['unit'] +
              (test_name in fork_unit_tests ? ['sgi', 'sgi-unit'] : []) +
              (slow_tests.has_key(test_name) ? ['slow'] : []))
endforeach
