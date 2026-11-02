contrib_plugins = [
'bbv.c',
'cache.c',
'cflow.c',
'drcov.c',
'execlog.c',
'hotblocks.c',
'hotpages.c',
'howvec.c',
'hwprofile.c',
'ips.c',
'stoptrigger.c',
'traps.c',
'uftrace.c',
]

if host_os != 'windows'
  # lockstep uses socket.h
  contrib_plugins += 'lockstep.c'
endif

if host_os == 'linux'
  # dlcall passes guest calls through to host libraries; linux-user only
  contrib_plugins += 'dlcall.c'
  # stands in for the L2 controller's kernel under qemu-ppc
  contrib_plugins += 'sgi-l2-shim.c'
endif

if 'cpp' in all_languages
  contrib_plugins += 'cpp.cpp'
endif

t = []
sgi_l2_shim = disabler()
if get_option('plugins')
  foreach i : contrib_plugins
    m = shared_module(fs.stem(i), files(i),
                      dependencies: plugins_deps)
    t += m
    if fs.stem(i) == 'sgi-l2-shim'
      sgi_l2_shim = m
    endif
  endforeach
endif
if t.length() > 0
  alias_target('contrib-plugins', t)
else
  run_target('contrib-plugins', command: [python, '-c', ''])
endif

plugin_modules += t
