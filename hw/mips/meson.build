mips_ss = ss.source_set()
mips_ss.add(files('bootloader.c', 'mips_int.c'))
common_ss.add(when: 'CONFIG_FW_CFG_MIPS', if_true: files('fw_cfg.c'))
common_ss.add(when: 'CONFIG_LOONGSON3V', if_true: files('loongson3_bootp.c'))
mips_ss.add(when: 'CONFIG_LOONGSON3V', if_true: files('loongson3_virt.c'))
mips_ss.add(when: 'CONFIG_MALTA', if_true: files('malta.c'))
mips_ss.add(when: 'CONFIG_MIPS_CPS', if_true: files('cps.c'))

if 'CONFIG_TCG' in config_all_accel
mips_ss.add(when: 'CONFIG_JAZZ', if_true: files('jazz.c'))
# These generated model inputs are tracked QEMU sources. Origami exports them
# explicitly; qemu-build does not regenerate them during configure.
mips_ss.add(when: 'CONFIG_SGI_SN0',
            if_true: files('sgi/sn0.c', 'sgi/sn0-diag.c', 'sgi/sn0-ecoff.c', 'sgi/sn0-fabric.c',
                           'sgi/sn0-firmware.c',
                           'sgi/sn0-model.c', 'sgi/sn0-model-consumer.c',
                           'sgi/sn0-origin200.c',
                           'sgi/sn0-origin2000.c', 'sn0-origin2000-model.c',
                           'sgi/sn0-origin2000-management-model.c',
                           'sgi/sn0-origin2000-model-consumer.c',
                           'sgi/sn0-origin2000-io.c',
                           'sgi/sn0-origin2000-xtalk.c',
                           'sgi/sn0-origin2000-xtalk-model.c',
                           'sgi/sn0-fabric-routed.c',
                           'sn0-origin200-model.c'),
            if_false: files('sgi/sn0-qmp-stub.c'))
mips_ss.add(when: ['CONFIG_SGI_SN0', 'CONFIG_TEST_DEVICES'],
            if_true: files('sgi/sn0-origin2000-flash-test.c',
                           'sgi/sn0-origin2000-model-test.c'))
mips_ss.add(when: 'CONFIG_SN0_HUB_PACKET_TEST',
            if_true: files('sgi/sn0-origin2000-io-test.c'))
mips_ss.add(when: 'CONFIG_SN0_HUB_OUTGOING_TEST',
            if_true: files('sgi/sn0-origin2000-outgoing-test.c'))
mips_ss.add(when: 'CONFIG_SN0_XTALK_ASSEMBLY_TEST',
            if_true: files('sgi/sn0-origin2000-xtalk-test.c'))
mips_ss.add(when: 'CONFIG_SGI_SN1',
            if_true: files('sgi/sn1.c', 'sn1-origin300-model.c'))
subdir('sgi')
mips_ss.add(when: 'CONFIG_FULOONG', if_true: files('fuloong2e.c'))
mips_ss.add(when: 'CONFIG_MIPS_BOSTON', if_true: files('boston.c'))
endif

hw_arch += {'mips': mips_ss}
