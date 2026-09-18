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
# The format-2 machine descriptor (sn-model.h) and the family machine
# base and builder that reads it (Origin 200 so far; other products
# join at later steps).
mips_ss.add(when: 'CONFIG_SGI_SN0',
            if_true: files('sgi/sn-model.c', 'sgi/sn-machine.c'))
mips_ss.add(when: 'CONFIG_SGI_SN1',
            if_true: files('sgi/sn1.c', 'sgi/sn1-node.c',
                           'sgi/sn1-topology.c'))
# The crosstalk port interface and the two link kinds that connect it:
# shared by whichever machines build crosstalk boards or links, so each
# selects on its own Kconfig symbol rather than either machine's.
mips_ss.add(when: 'CONFIG_SGI_XTALK_PORT', if_true: files('sgi/xtalk-port.c'))
mips_ss.add(when: 'CONFIG_SGI_XTALK_LINK', if_true: files('sgi/xtalk-link.c'))
# The crosstalk board base class: shared by SN0 and SN1 I/O boards, so it
# selects on its own Kconfig symbol rather than either machine's.
mips_ss.add(when: 'CONFIG_SGI_XTALK_BOARD', if_true: files('sgi/xtalk-board.c'))
# Likewise for the crosstalk board fixture: see the comment above.
mips_ss.add(when: 'CONFIG_SGI_XTALK_BOARD_TEST',
            if_true: files('../../tests/qtest/xtalk-test-board.c'))
# The four board classes: each its own Kconfig symbol, since a product
# builds only the ones it fits (Origin 200 selects sgi-ip29-io, Origin 2000
# sgi-baseio and sgi-pci-expansion; sgi-io8 awaits the Origin 300 step).
mips_ss.add(when: 'CONFIG_SGI_IP29_IO', if_true: files('sgi/board-ip29-io.c'))
mips_ss.add(when: 'CONFIG_SGI_BASEIO', if_true: files('sgi/board-baseio.c'))
mips_ss.add(when: 'CONFIG_SGI_IO8', if_true: files('sgi/board-io8.c'))
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
