tests = []

tests += tcg_tests['multiarch-linux-user']['tests']

tests += {
  'hello-mips.c': {
    'cflags': ['-mno-abicalls', '-fno-PIC', '-fno-stack-protector', '-mabi=32',
               '-nostdlib']
  },
}

tests += {
  'registers-plugin32.S': {
    'cflags': ['-mno-abicalls', '-fno-PIC', '-mabi=32', '-nostdlib'],
    'plugin_test': {'plugin': 'registers', 'args': ['mips-registers=on']},
  },
}

tests += {
  'registers-plugin32.S': {
    'gdb_test': ['--test', files('../multiarch/gdbstub/registers.py')],
    'env_var': 'MIPS_REGISTER_FIXTURE=1',
  },
}

if 'qemu-mips' in emulators
  tcg_tests += {
    'mips-linux-user': {
      'cc': 'mips-linux-gnu-gcc',
      'cc_dockerfile': 'debian-all-test-cross',
      'cc_docker_host_arch': ['aarch64', 'x86_64'],
      'cc_feat_cflags': cc_feat_cflags,
      'folder': 'mips',
      'gdb_arch': 'mips',
      'qemu': emulators['qemu-mips'],
      'tests': tests,
    }
  }
endif
