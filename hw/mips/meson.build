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
            if_true: files('sgi/sn0.c', 'sgi/sn0-fabric.c',
                           'sgi/sn0-fabric-routed.c'))
# These qtest-only fixtures live under tests/qtest, beside the qtest
# programs that are their only consumer, and are pulled in here because
# meson builds hw/ before tests/ so the target binary must gain them here.
mips_ss.add(when: ['CONFIG_SGI_SN0', 'CONFIG_TEST_DEVICES'],
            if_true: files('../../tests/qtest/sn0-origin2000-flash-fixture.c',
                           '../../tests/qtest/sn-builder-fault-test.c'))
# Shared by sgi-sn0-node and sgi-sn1-node; SGI_SN0 always implies SGI_SN1 is
# also selected on this fork's mips64 target, so either config would do.
mips_ss.add(when: 'CONFIG_SGI_SN0', if_true: files('sgi/sn-promdata.c'))
# The family machine base and builder over the compiled catalogue
# (qapi/sgi-sn.json, embedded and parsed by sgi/sn-catalogue.c below).
mips_ss.add(when: 'CONFIG_SGI_SN0', if_true: files('sgi/sn-machine.c'))
mips_ss.add(when: 'CONFIG_SGI_SN1',
            if_true: files('sgi/sn1.c', 'sgi/sn1-node.c',
                           'sgi/sn1-topology.c'))
# The crosstalk port interface and the two link kinds that connect it:
# shared by whichever machines build crosstalk boards or links, so each
# selects on its own Kconfig symbol rather than either machine's.
mips_ss.add(when: 'CONFIG_SGI_XTALK_PORT', if_true: files('sgi/xtalk-port.c'))
mips_ss.add(when: 'CONFIG_SGI_XTALK_LINK', if_true: files('sgi/xtalk-link.c'))
# The XIO board base class (renamed from sgi-xtalk-board, the base-io-device
# step: its onboard-only subclasses stopped existing, leaving exactly the
# removable-card scope this name states). Shared by every XIO card, so it
# selects on its own Kconfig symbol rather than either machine's.
mips_ss.add(when: 'CONFIG_SGI_XIO_BOARD', if_true: files('sgi/xio-board.c'))
# Likewise for the XIO board fixture: see the comment above.
mips_ss.add(when: 'CONFIG_SGI_XIO_BOARD_TEST',
            if_true: files('../../tests/qtest/xtalk-test-board.c'))
# sgi-baseio: the base I/O functional unit as one device with a preset per
# shipped board design (docs/designs/sn-machine-model-architecture.md,
# "Base I/O"). Every product that carries base I/O selects this, whether or
# not it also builds an XIO card: the IP29 and IO-8 presets have no card of
# their own, only a carrier that owns the chip directly.
mips_ss.add(when: 'CONFIG_SGI_BASEIO', if_true: files('sgi/baseio.c'))
# The Origin 2000/Onyx2 BaseIO XIO card: an sgi-xio-board subclass that
# creates its own Bridge and an sgi-baseio (the "baseio" preset) behind it.
mips_ss.add(when: 'CONFIG_SGI_XIO_BASEIO', if_true: files('sgi/xio-baseio.c'))
mips_ss.add(when: 'CONFIG_SGI_PCI_EXPANSION',
            if_true: files('sgi/board-pci-expansion.c'))
# Likewise for the board fixture: see the comment above.
mips_ss.add(when: 'CONFIG_SGI_BOARD_TEST',
            if_true: files('../../tests/qtest/board-test-fixture.c'))
subdir('sgi')
mips_ss.add(when: 'CONFIG_FULOONG', if_true: files('fuloong2e.c'))
mips_ss.add(when: 'CONFIG_MIPS_BOSTON', if_true: files('boston.c'))
endif

hw_arch += {'mips': mips_ss}
