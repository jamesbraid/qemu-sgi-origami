
qht_bench = executable('qht-bench',
                       sources: 'qht-bench.c',
                       dependencies: [qemuutil])

executable('qtree-bench',
           sources: 'qtree-bench.c',
           dependencies: [qemuutil])

executable('atomic_add-bench',
           sources: files('atomic_add-bench.c'),
           dependencies: [qemuutil],
           build_by_default: false)

executable('atomic64-bench',
           sources: files('atomic64-bench.c'),
           dependencies: [qemuutil],
           build_by_default: false)

benchs = {}

if have_block
  benchs += {
     'bufferiszero-bench': [],
     'dma-bswap32-bench': [],
     'benchmark-crypto-hash': [crypto],
     'benchmark-crypto-hmac': [crypto],
     'benchmark-crypto-cipher': [crypto],
     'benchmark-crypto-akcipher': [crypto],
  }
endif

foreach bench_name, deps: benchs
  bench_sources = [bench_name + '.c']
  if bench_name == 'dma-bswap32-bench'
    bench_sources += ['../../hw/misc/sgi-dma-bswap.c']
  endif
  exe = executable(bench_name, bench_sources,
                   dependencies: [qemuutil] + deps)
  benchmark(bench_name, exe,
            args: ['--tap', '-k'],
            protocol: 'tap',
            timeout: 0,
            suite: ['speed'])
endforeach
