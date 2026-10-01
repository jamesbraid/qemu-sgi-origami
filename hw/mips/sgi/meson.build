# SN machine ASIC models and machines
# Consolidated builds for hw/mips/sgi

# sn-catalogue.json is a tracked source embedded here as a C string constant,
# the same way ui/shader/meson.build turns a GLSL source file into one, so
# sn-catalogue.c parses the embedded bytes rather than reading the file at run
# time.
sn_catalogue_embed = find_program('../../../scripts/sn-catalogue-embed.py')
sn_catalogue_embed_c = custom_target('sn-catalogue-embed.c.inc',
    output: 'sn-catalogue-embed.c.inc',
    input: files('sn-catalogue.json'),
    capture: true,
    build_by_default: false,
    command: [sn_catalogue_embed, '@INPUT0@'])
# sn-catalogue.c #includes the custom_target's output, so both are added to
# the same mips_ss.add() call: meson adds a custom_target's own output
# directory to the include path of whatever target its result feeds into,
# the way ui/shader/meson.build's generated headers reach ui/shader.c
# through the shared genh list.
mips_ss.add(when: 'CONFIG_SGI_CATALOGUE',
            if_true: [files('sn-catalogue.c'), sn_catalogue_embed_c],
            if_false: files('sn-catalogue-qmp-stub.c'))
