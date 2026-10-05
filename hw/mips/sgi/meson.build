# SN machine ASIC models and machines
# Consolidated builds for hw/mips/sgi

mips_ss.add(when: 'CONFIG_SGI_SN1', if_true: files('r14000.c'))

# sgi-machines.json is a tracked source embedded here as a C string constant,
# the same way ui/shader/meson.build turns a GLSL source file into one, so
# sgi-machines.c parses the embedded bytes rather than reading the file at run
# time.
sgi_machines_embed = find_program('../../../scripts/sgi-machines-embed.py')
sgi_machines_embed_c = custom_target('sgi-machines-embed.c.inc',
    output: 'sgi-machines-embed.c.inc',
    input: files('sgi-machines.json'),
    capture: true,
    build_by_default: false,
    command: [sgi_machines_embed, '@INPUT0@'])
# sgi-machines.c #includes the custom_target's output, so both are added to
# the same mips_ss.add() call: meson adds a custom_target's own output
# directory to the include path of whatever target its result feeds into,
# the way ui/shader/meson.build's generated headers reach ui/shader.c
# through the shared genh list.
mips_ss.add(when: 'CONFIG_SGI_CATALOGUE',
            if_true: [files('sgi-machines.c', 'machine-storage.c',
                            'board-mac.c'),
                      sgi_machines_embed_c],
            if_false: files('sgi-machines-qmp-stub.c'))
