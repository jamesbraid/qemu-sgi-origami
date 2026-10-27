slow_qtests = {
  'ahci-test': 150,
  'aspeed_smc-test': 360,
  'bios-tables-test' : 910,
  'cdrom-test' : 610,
  'device-introspect-test' : 720,
  'ide-test' : 120,
  'migration-test' : 480,
  'npcm7xx_pwm-test': 300,
  'npcm7xx_watchdog_timer-test': 120,
  'qmp-cmd-test' : 120,
  'qom-test' : 900,
  'stm32l4x5_usart-test' : 600,
  'test-hmp' : 240,
  'pxe-test': 610,
  'prom-env-test': 360,
  'boot-serial-test': 360,
  'qos-test': 120,
  'vmgenid-test': 610,
  # SGI qtests: three times the longest of six four-process thorough runs
  # on a 32-thread workstation at load average 25 to 290, counting a run
  # killed at its budget as that long, 2026-10-05. Tests that fit three
  # times inside the 60s default have no entry.
  'bcm570x-test': 180,
  'octane-mgras-test': 440,
  'octane-test': 500,
  # psitech-rad4-test's 92 cases, with the RAD4 DMA engine's, took 94.5s
  # under debug_tcg at load 145 on 32 threads, 2026-10-05; three times that.
  'psitech-rad4-test': 300,
  # sgi-graphics-pipes-test starts a fresh QEMU for nearly every case, as
  # sgi-heads-test before it did, whose 20 subtests took 65-70s at load
  # 100-150 on 32 threads, 2026-10-23, and overran the default.
  'sgi-graphics-pipes-test': 300,
  # sgi-catalogue-launch-test builds every catalogue offering once, and
  # again with a pipe where the topology takes one, about seventy machines.
  'sgi-catalogue-launch-test': 300,
  # Alone it took 55.6 s at load average 118, 2026-10-24 (11 s on a quiet
  # host), and overran the default in full runs; three times the loaded
  # figure, rounded up.
  'sgi-heart-test': 180,
  # CI run 13888 exhausted the 60s default at 41/54 cases; a two-suite retry
  # passed all 54 in 25.76s. Keep three times the exhausted CI deadline.
  'sgi-ioc4-test': 180,
  'sgi-ioc3-eth-test': 500,
  # Alone it took 36.5 s with the host at load average 118, 2026-10-24
  # (18 s on a quiet host); three times the loaded figure, rounded up.
  'sgi-kona-read-defense-test': 120,
  # 45.6 s alone at load average 118, 2026-10-24 (30 s on a quiet host);
  # three times the loaded figure, rounded up.
  'sgi-machine-storage-test': 150,
  'sgi-mgras-board-test': 670,
  'sgi-mgras-fifo-test': 190,
  'sgi-mgras-gl-draw-test': 760,
  'sgi-mgras-gl-test': 740,
  'sgi-mgras-migration-gl-test': 320,
  'sgi-mgras-migration-glstate-test': 550,
  'sgi-mgras-migration-test': 150,
  'sgi-mgras-pixels-test': 330,
  'sgi-mgras-texture-test': 1050,
  'sgi-migration-roundtrip-test': 170,
  'sgi-pci-expansion-ni-test': 2050,
  # The expanded PIC suite passed in 86s locally; CI reached 80/85 at 60s.
  'sgi-pic-test': 300,
  # Two full-machine migration cases used 43 s of the 60 s default in CI;
  # the 27-case router binary had completed only 12 cases at that limit.
  'sgi-sn1-router-test': 300,
  'sgi-replay-test': 330,
  # It starts every catalogue topology at its largest offering. 64.77s in a
  # parallel run of thirty SGI qtests at a load average near 60; budget
  # three times that, rounded up. An earlier, unmethodical figure of 171s
  # alone at load 100 had set it to 360.
  'sgi-synthetic-serial-test': 200,
  # Routed cases expand the builder suite beyond its previous budget.
  'sn-builder-test': 1650,
  'sn0-address-map-test': 110,
  'sn0-bridge-identity-test': 80,
  'sn0-bridge-test': 290,
  'sn0-ds1685-test': 210,
  'sn0-fabric-test': 460,
  'sn0-ioc3-parallel-test': 200,
  'sn0-ioc3-test': 550,
  'sn0-isp12160-test': 250,
  'sn0-menet-test': 150,
  'sn0-migration-test': 190,
  'sn0-module-management-test': 500,
  'sn0-module-domain-test': 600,
  'sn0-mscsi-test': 130,
  'sn0-mttcg-test': 390,
  # The whole binary, which only a named run uses: its groups below are
  # what the qtest suites run. It took 1109 to 1152 s in full runs 49961,
  # 50154 and 50244.
  'sn0-origin2000-kona-3d-test': 3600,
  # sn0-origin2000-kona-3d-test's groups: three times each one's time in a
  # four-process thorough run of all nine in the CI image with Mesa
  # required, 2026-10-18.
  'sn0-origin2000-kona-3d-test-draw': 1420,
  'sn0-origin2000-kona-3d-test-index': 1350,
  'sn0-origin2000-kona-3d-test-read': 1360,
  'sn0-origin2000-kona-3d-test-renderer': 1600,
  'sn0-origin2000-kona-3d-test-renderer-cpu': 580,
  'sn0-origin2000-kona-3d-test-renderer-cpu-fragment': 420,
  'sn0-origin2000-kona-3d-test-renderer-fragment': 950,
  'sn0-origin2000-kona-3d-test-state': 180,
  # The texture group ran 645 s as one test and overran 840 s in a busy
  # full run, so it is split in two. Alone at load average 85 to 118,
  # 2026-10-24: texture 375 s, texture-formats 173 s; three times each,
  # rounded up.
  'sn0-origin2000-kona-3d-test-texture': 1140,
  'sn0-origin2000-kona-3d-test-texture-formats': 530,
  'sn0-origin2000-kona-bringup-test': 230,
  'sn0-origin2000-kona-config-test': 120,
  # With the suspended-command checks it took 43-64 s alone, and 99 s in a
  # full parallel run with the saved-input and kernel-framer tests; 300
  # keeps three times that.
  'sn0-origin2000-kona-context-test': 300,
  # The whole binary, which only a named run uses: its groups below are
  # what the qtest suites run.
  'sn0-origin2000-kona-fill-test': 900,
  # sn0-origin2000-kona-fill-test's groups: three times each one's slowest
  # of three runs in the CI image at four processes beside the KONA pixel,
  # context, video and DMA qtests, 2026-10-19.
  'sn0-origin2000-kona-fill-test-box': 320,
  'sn0-origin2000-kona-fill-test-line': 240,
  'sn0-origin2000-kona-fill-test-point': 250,
  'sn0-origin2000-kona-fill-test-semantic': 240,
  # 376.6 s alone at load average 94, 2026-10-24 (302 s on a quiet host);
  # three times the loaded figure, rounded up.
  'sn0-origin2000-kona-pixel-test': 1140,
  'sn0-origin2000-kona-xdma-test': 70,
  'sn0-origin2000-pflash-test': 80,
  'sn0-origin2000-test': 430,
  'sn0-pci-carrier-test': 70,
  'sn0-pci-shoehorn-test': 100,
  'sn0-qlogic-isp-test': 80,
  'sn0-qtest-cpu-test': 240,
  'sn0-r10k-perf-migration-test': 110,
  # Router and L1 start a machine per case; bedrock-flash programs a flash
  # part word by word. Their time follows the builder. 3x the longest of
  # the 15 merge-queue full-suite runs 44454 to 45080: 243 s, 75 s and 58 s.
  'sn0-router-test': 730,
  'sn0-tigon-test': 320,
  # Routed RAM boundary and MTTCG cases start multiple machines.
  'sn1-address-map-test': 1800,
  'sn1-bedrock-flash-test': 175,
  'sn1-bedrock-md-test': 160,
  'sn1-cross-node-test': 180,
  'sn1-option-tigon-test': 600,
  'sn1-directory-state-test': 160,
  'sn1-fuel-test': 250,
  'sn1-io8-scsi-test': 160,
  # CI run 13888 exhausted 225s at 122/129 cases; a two-suite retry passed
  # all 129 in 188.13s. Keep three times the exhausted CI deadline.
  'sn1-l1-test': 675,
  # About three times its slowest full eight-process VPro run: 42-130 s,
  # and one that passed 100 s and was stopped.
  'sn1-odyssey-query-test': 400,
  # The whole binary, which only a named run uses: its groups below are
  # what the qtest suites run.
  'sn1-odyssey-test': 3400,
  # sn1-odyssey-test's groups: three times the longest of two four-process
  # thorough runs of all eleven on a 32-thread workstation at load average
  # 155 to 290 and one full run on the builders (actions run 11999),
  # 2026-10-07.
  'sn1-odyssey-test-board': 230,
  'sn1-odyssey-test-context': 320,
  # The hardware cursor cases: 2.3 s alone on a 32-thread workstation,
  # 2026-10-27, with room for a loaded builder.
  'sn1-odyssey-test-cursor': 60,
  'sn1-odyssey-test-fifo': 510,
  'sn1-odyssey-test-gl': 500,
  # The profile and identity cases, split from board: three times the
  # longest of two four-process thorough runs of every group, 22 s at load
  # average 90 to 105, 2026-10-24.
  'sn1-odyssey-test-profile': 70,
  'sn1-odyssey-test-texture': 430,
  'sn1-odyssey-test-texture-1d-clamp': 640,
  'sn1-odyssey-test-texture-1d-stored': 570,
  'sn1-odyssey-test-texture-1d-wide': 710,
  'sn1-odyssey-test-texture-2d-clamp': 620,
  'sn1-odyssey-test-texture-2d-stored': 630,
  'sn1-odyssey-test-texture-3d': 690,
  'sn1-xbridge-chip-test': 190,
  'xframe-test': 70,
  # Not yet re-measured: both run several times slower under meson's
  # MALLOC_PERTURB_ than alone, which is still being investigated.
  'sn0-baseio-test': 1090,
  'sn0-hub-test': 415,
  # Each holds a 120s socket deadline; the budget must outlast it so a
  # stalled socket reports itself rather than a killed binary.
  'sn0-elsc-test': 730,
  'sn0-ioc3-serial-test': 240,
  # Each gives a test subprocess 120s; the budget is that plus three times
  # the longest of five runs (alone, four- and 16-process) on 2026-10-05,
  # so a hung child reports itself rather than a killed binary.
  'ds1742-test': 140,
  'ics1890-test': 160,
  'sn1-pci-expansion-module-test': 140,
  'sn1-xbridge-test': 160,
}

qtests_generic = [
  'cdrom-test',
  'device-introspect-test',
  'machine-none-test',
  'qmp-test',
  'qmp-cmd-test',
  'qom-test',
]
if have_hmp
  qtests_generic += [
    'test-hmp',
  ]
endif
qtests_generic += [
  'qos-test',
  'readconfig-test',
  'netdev-socket',
]
if enable_modules
  qtests_generic += [ 'modules-test' ]
endif
if config_all_devices.has_key('CONFIG_MEMORY_BUFFER_TESTDEV')
  qtests_generic += [ 'memory-buffer-test' ]
endif

qtests_pci = \
  (config_all_devices.has_key('CONFIG_AIC7880_SCSI_PCI') ? ['aic7880-test'] : []) +        \
  (config_all_devices.has_key('CONFIG_VGA') ? ['display-vga-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_IVSHMEM_DEVICE') ? ['ivshmem-test'] : [])

qtests_cxl = \
  (config_all_devices.has_key('CONFIG_CXL') ? ['cxl-test'] : [])

# FIXME: Get rid of get_option('default_devices') here and check
#        for the availability of the default NICs in the tests
qtests_filter = \
  (get_option('default_devices') and slirp.found() ? ['test-netfilter'] : []) + \
  (get_option('default_devices') and host_os != 'windows' ? ['test-filter-buffer'] : []) + \
  (get_option('default_devices') and host_os != 'windows' ? ['test-filter-mirror'] : []) + \
  (get_option('default_devices') and host_os != 'windows' ? ['test-filter-redirector'] : [])

qtests_i386 = \
  (slirp.found() ? ['pxe-test'] : []) + \
  qtests_filter + \
  (config_all_devices.has_key('CONFIG_ACPI_VMGENID') ? ['vmgenid-test'] : []) +             \
  (config_all_devices.has_key('CONFIG_AHCI_ICH9') and have_tools ? ['ahci-test'] : []) +    \
  (config_all_devices.has_key('CONFIG_AHCI_ICH9') ? ['tco-test'] : []) +                    \
  (config_all_devices.has_key('CONFIG_FDC_ISA') ? ['fdc-test'] : []) +                      \
  (config_all_devices.has_key('CONFIG_I440FX') ? ['fw_cfg-test'] : []) +                    \
  (config_all_devices.has_key('CONFIG_Q35') ? ['e820-test'] : []) +                          \
  (config_all_devices.has_key('CONFIG_FW_CFG_DMA') ? ['vmcoreinfo-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_Q35') ? ['dump-test'] : []) +                         \
  (igvm.found() and
   config_all_devices.has_key('CONFIG_FW_CFG_DMA') ? ['launchupdate-test'] : []) +          \
  (config_all_devices.has_key('CONFIG_I440FX') ? ['i440fx-test'] : []) +                    \
  (config_all_devices.has_key('CONFIG_I440FX') ? ['ide-test'] : []) +                       \
  (config_all_devices.has_key('CONFIG_I440FX') ? ['numa-test'] : []) +                      \
  (config_all_devices.has_key('CONFIG_I440FX') ? ['test-x86-cpuid-compat'] : []) +          \
  (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : []) +           \
  (config_all_devices.has_key('CONFIG_SGA') ? ['boot-serial-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_ISA_IPMI_KCS') ? ['ipmi-kcs-test'] : []) +            \
  (host_os == 'linux' and                                                                  \
   config_all_devices.has_key('CONFIG_ISA_IPMI_BT') and
   config_all_devices.has_key('CONFIG_IPMI_EXTERN') ? ['ipmi-bt-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_WDT_IB700') ? ['wdt_ib700-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_PVPANIC_ISA') ? ['pvpanic-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_PVPANIC_PCI') ? ['pvpanic-pci-test'] : []) +          \
  (config_all_devices.has_key('CONFIG_HDA') ? ['intel-hda-test'] : []) +                    \
  (config_all_devices.has_key('CONFIG_I82801B11') ? ['i82801b11-test'] : []) +             \
  (config_all_devices.has_key('CONFIG_IOH3420') ? ['ioh3420-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_LPC_ICH9') ? ['lpc-ich9-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_MC146818RTC') ? ['rtc-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_USB_UHCI') ? ['usb-hcd-uhci-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_USB_UHCI') and                                        \
   config_all_devices.has_key('CONFIG_USB_EHCI') ? ['usb-hcd-ehci-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_USB_XHCI_NEC') ? ['usb-hcd-xhci-test'] : []) +        \
  (config_all_devices.has_key('CONFIG_TPM_CRB') ? ['tpm-crb-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_TPM_CRB') ? ['tpm-crb-swtpm-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_TPM_TIS_ISA') ? ['tpm-tis-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_TPM_TIS_ISA') ? ['tpm-tis-swtpm-test'] : []) +        \
  (config_all_devices.has_key('CONFIG_RTL8139_PCI') ? ['rtl8139-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_E1000E_PCI_EXPRESS') ? ['fuzz-e1000e-test'] : []) +   \
  (config_all_devices.has_key('CONFIG_MEGASAS_SCSI_PCI') ? ['fuzz-megasas-test'] : []) +    \
  (config_all_devices.has_key('CONFIG_LSI_SCSI_PCI') ? ['fuzz-lsi53c895a-test'] : []) +     \
  (config_all_devices.has_key('CONFIG_VIRTIO_SCSI') ? ['fuzz-virtio-scsi-test'] : []) +     \
  (config_all_devices.has_key('CONFIG_VIRTIO_BALLOON') ? ['virtio-balloon-test'] : []) + \
  (config_all_devices.has_key('CONFIG_Q35') ? ['q35-test'] : []) +                          \
  (config_all_devices.has_key('CONFIG_SB16') ? ['fuzz-sb16-test'] : []) +                   \
  (config_all_devices.has_key('CONFIG_SDHCI_PCI') ? ['fuzz-sdcard-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_ESP_PCI') ? ['am53c974-test'] : []) +                 \
  (config_all_devices.has_key('CONFIG_AMD_IOMMU') ? ['amd-iommu-test'] : []) +             \
  (config_all_devices.has_key('CONFIG_VTD') ? ['intel-iommu-test'] : []) +                 \
  (config_all_devices.has_key('CONFIG_VTD') and
   config_all_devices.has_key('CONFIG_IOMMU_TESTDEV') ? ['iommu-intel-test', 'iommu-intel-inv-test'] : []) +      \
  (host_os != 'windows' and                                                                \
   config_all_devices.has_key('CONFIG_ACPI_ERST') ? ['erst-test'] : []) +                   \
  (config_all_devices.has_key('CONFIG_PCIE_PORT') and                                       \
   config_all_devices.has_key('CONFIG_VIRTIO_NET') and                                      \
   config_all_devices.has_key('CONFIG_Q35') and                                             \
   config_all_devices.has_key('CONFIG_VIRTIO_PCI') and                                      \
   slirp.found() ? ['virtio-net-failover'] : []) +                                          \
  (unpack_edk2_blobs and                                                                    \
   config_all_devices.has_key('CONFIG_HPET') and                                            \
   config_all_devices.has_key('CONFIG_PARALLEL') ? ['bios-tables-test'] : []) +             \
  qtests_pci +                                                                              \
  qtests_cxl +                                                                              \
  [
   'hd-geo-test',
   'boot-order-test',
   'device-plug-test',
   'drive_del-test',
   'cpu-plug-test',
   'migration-test',
  ]

if dbus_display and config_all_devices.has_key('CONFIG_VGA')
  qtests_i386 += ['dbus-display-test']
endif

dbus_daemon = find_program('dbus-daemon', required: false)
if dbus_daemon.found() and gdbus_codegen.found()
  qtests_i386 += ['dbus-vmstate-test']
  dbus_vmstate1 = custom_target('dbus-vmstate description',
                                build_by_default: true,
                                output: [ 'dbus-vmstate1.h', 'dbus-vmstate1.c'],
                                input: meson.project_source_root() / 'backends/dbus-vmstate1.xml',
                                command: [gdbus_codegen, '@INPUT@',
                                          '--interface-prefix', 'org.qemu',
                                          '--output-directory', meson.current_build_dir(),
                                          '--generate-c-code', '@BASENAME@']).to_list()
else
  dbus_vmstate1 = []
endif

qtests_x86_64 = qtests_i386 + \
  (config_all_devices.has_key('CONFIG_USB_OHCI_PCI') and \
   config_all_devices.has_key('CONFIG_I440FX') and \
   config_all_devices.has_key('CONFIG_USB_HID') ? \
    ['usb-ohci-transfer-test'] : [])

qtests_alpha = ['boot-serial-test'] + \
  qtests_filter + \
  (config_all_devices.has_key('CONFIG_VGA') ? ['display-vga-test'] : [])

qtests_avr = [ 'boot-serial-test' ]

qtests_hppa = \
  qtests_filter + \
  (config_all_devices.has_key('CONFIG_VGA') ? ['display-vga-test'] : [])

qtests_loongarch64 = qtests_filter + \
  (config_all_devices.has_key('CONFIG_LOONGARCH_VIRT') ? ['numa-test'] : []) + \
  (unpack_edk2_blobs ? ['bios-tables-test'] : []) + \
  ['boot-serial-test',
   'cpu-plug-test',
   'migration-test']

qtests_m68k = ['boot-serial-test'] + \
  qtests_filter

qtests_microblaze = ['boot-serial-test'] + \
  qtests_filter

qtests_mips = \
  qtests_filter + \
  (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_VGA') ? ['display-vga-test'] : [])

qtests_mipsel = qtests_mips

# Origin 200 normally selects Lucent OHCI, but a minimal device configuration
# can disable the controller while retaining the machine.
sn0_ohci_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['usb-lucent-ohci-test'] : [])
if not config_all_devices.has_key('CONFIG_USB_OHCI_PCI')
  sn0_ohci_qtests = []
endif

# The XBridge qtests use test-only XIO and PCI endpoints.  Keep the
# registration visible under the device guard, then omit tests whose fixtures
# are not present in a minimal device build. sn1-xbridge-test also carries a
# private origin300 host-error case that needs the full SN1 board, unlike
# its other cases' bare test-only head; gate that name on CONFIG_SGI_SN1 too.
sn1_xbridge_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_XBRIDGE') ?
    ['sn1-xbridge-chip-test', 'sn1-xbridge-xbow-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-xbridge-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sn1_xbridge_qtests = []
elif not config_all_devices.has_key('CONFIG_PCI')
  sn1_xbridge_qtests = ['sn1-xbridge-xbow-test']
elif not config_all_devices.has_key('CONFIG_PCI_BRIDGE') or \
     not config_all_devices.has_key('CONFIG_IOMMU_TESTDEV')
  sn1_xbridge_qtests = ['sn1-xbridge-chip-test',
                        'sn1-xbridge-xbow-test']
endif

sn1_pci_expansion_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES') and
   config_all_devices.has_key('CONFIG_IOMMU_TESTDEV') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-pci-expansion-module-test', 'sgi-pci-expansion-ni-test'] : [])

sn0_mgras_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sgi-mgras-board-test', 'sgi-mgras-fifo-test',
     'sgi-mgras-gl-test', 'sgi-mgras-gl-draw-test',
     'sgi-mgras-pixels-test',
     'sgi-mgras-texture-test', 'sgi-mgras-migration-test',
     'sgi-mgras-migration-gl-test', 'sgi-mgras-migration-glstate-test',
     'sgi-mgras-irq-test',
     'sgi-replay-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_MGRAS')
  sn0_mgras_qtests = []
endif

# The Crossbow qtest builds its own populated switch out of test-only
# XIO endpoints.  Keep the registration under the machine guard, then
# omit it where that fixture is not present in a minimal device build.
sn0_xbow_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ? ['sn0-xbow-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sn0_xbow_qtests = []
endif

sn0_router_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-router-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sn0_router_qtests = []
endif

sn0_xbow_transport_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-xbow-transport-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sn0_xbow_transport_qtests = []
endif

# sgi-baseio's presets, sgi-pci-expansion, and the XIO board base class
# share this file: none of them has a machine of its own yet, so each
# needs only its own test fixture guard rather than SGI_SN0/SGI_SN1, and
# the merged file needs both guards since it exercises fixtures built
# under each.
sgi_board_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES') ?
    ['sgi-board-test'] : [])

# Cards that ship behind a Bridge, on the libqos head: one Bridge and the
# memory a card transfers into, under -M none.  Each card carries its own
# device guard; drop the lot if the head's two devices are not configured.
sgi_bridge_card_qtests = \
  (config_all_devices.has_key('CONFIG_TSB12LV22') ? ['tsb12lv22-test'] : []) + \
  (config_all_devices.has_key('CONFIG_TIGON') ? ['tigon-test'] : []) + \
  (config_all_devices.has_key('CONFIG_XFRAME_PCI') ? ['xframe-test'] : []) + \
  (config_all_devices.has_key('CONFIG_BCM570X') ? ['bcm570x-test'] : []) + \
  (slirp.found() and config_all_devices.has_key('CONFIG_SGI_IOC3') ?
    ['sgi-ioc3-eth-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_BRIDGE') or \
   not config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES')
  sgi_bridge_card_qtests = []
endif

# The Origin 200 is a 64-bit machine, so its tests cannot be shared with the
# 32-bit mips targets, where the machine does not exist.
sn1_board_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_BIC_BOARD_INTERFACE') and
   config_all_devices.has_key('CONFIG_TEST_DEVICES') ?
    ['sgi-bic-board-interface-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_L1') ?
    ['sn1-l1-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_MD') ?
    ['sn1-bedrock-md-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES') ?
    ['sgi-command-port-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_IO') ?
    ['sn1-bedrock-io-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_LB') ?
    ['sn1-bedrock-lb-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_XB') ?
    ['sn1-bedrock-xb-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_FLASH') ?
    ['sn1-bedrock-flash-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK') ?
    ['sn1-bedrock-pi-test', 'sn1-bedrock-test', 'sn1-bedrock-link-test'] :
    []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN1_ROUTER_TEST') ?
    ['sgi-sn1-router-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-address-map-test', 'sn1-cross-node-test',
     'sn1-cpu-identity-test', 'sn1-directory-state-test',
     'sn1-io8-scsi-test',
     'sn1-pi-gfx-test', 'sn1-pi-polled-read-test',
     'sn1-fuel-test', 'sn1-fuel-usb-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_ODYSSEY') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-fuel-vpro-test'] : []) + \
  (config_all_devices.has_key('CONFIG_TIGON') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-option-tigon-test'] : [])

sn0_machine_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ? ['sn0-mttcg-test',
                                                   'sn0-forward-progress-test',
                                                   'sn0-r10k-test',
                                                   'sn0-r10k-perf-migration-test',
                                                   'sn0-address-map-test',
                                                   'sn0-bridge-test',
                                                   'sn0-ioc3-test',
                                                   'sn0-ioc3-kbc-routing-test',
                                                   'sn0-ioc3-parallel-test',
                                                   'sn0-ioc3-serial-test',
                                                   'sn0-qlogic-isp-test',
                                                   'sn0-isp12160-test',
                                                   'sn0-xframe-test',
                                                   'sn0-tigon-test',
                                                   'sn0-bcm570x-test',
                                                   'psitech-rad4-test',
                                                   'psitech-rad1-test',
                                                   'sn0-hub-test',
                                                   'sn0-pi-error-test',
                                                   'sn0-pi-gfx-test',
                                                   'sn0-pio-forward-test',
                                                   'sn0-pi-link-test',
                                                   'sn0-i2c-test',
                                                   'sn0-elsc-test',
                                                   'sn0-elsc-nvram-test',
                                                   'sn0-widget-decode-test',
                                                   'sn0-bridge-invalid-addr-test',
                                                   'sn0-bridge-master-timeout-test',
                                                   'sgi-bridge-bus-test',
                                                   'sn0-bridge-flash-test',
                                                   'sn0-origin2000-test',
                                                   'sn0-baseio-test',
                                                   'sn0-bridge-identity-test',
                                                   'sn0-module-management-test',
                                                   'sn0-module-domain-test',
                                                   'sn0-origin2000-pflash-test',
                                                   'sn0-cross-node-test',
                                                   'sn0-migration-test',
                                                   'sgi-migration-roundtrip-test',
                                                   'sn0-snapshot-test',
                                                   'sn0-ds1685-test',
                                                   'sn0-fabric-test',
                                                   'sn0-hub-inbound-test',
                                                   'sn0-qtest-cpu-test',
                                                   'sgi-debug-tools-test',
                                                   'sn-builder-test',
                                                   'sgi-synthetic-serial-test',
                                                   'sn-node-population-test',
                                                   'sgi-machine-storage-test',
                                                   'sn0-pci-shoehorn-test',
                                                   'sn0-pci-carrier-test',
                                                   'sn0-menet-test',
                                                   'sn0-mscsi-test',
                                                   'sn0-gigachannel-test',
                                                   'sn0-origin2000-kona-test',
                                                   'sn0-origin2000-kona-textport-test',
                                                   'sn0-origin2000-kona-dma-test',
                                                   'sn0-origin2000-kona-bringup-test',
                                                   'sn0-origin2000-kona-config-test',
                                                   'sn0-origin2000-kona-eeprom-test',
                                                   'sn0-origin2000-kona-video-test',
                                                   'sn0-origin2000-kona-scanout-test',
                                                   'sn0-origin2000-kona-fill-test',
                                                   'sn0-origin2000-kona-pixel-test',
                                                   'sn0-origin2000-kona-glyph-test',
                                                   'sn0-origin2000-kona-context-test',
                                                   'sn0-origin2000-kona-gl-test',
                                                   'sn0-origin2000-kona-3d-test',
                                                   'sn0-origin2000-kona-xdma-test',
                                                   'sn0-origin2000-kona-pi-credit-test',
                                                   'sn0-onyx2-test',
                                                   'sn0-display-heads-test'] : [])

sgi_qtests = \
  ['ds1742-test'] + \
  (config_all_devices.has_key('CONFIG_SGI_O2_ROM') ? ['sgi-o2-rom-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_O2_TEST') and
   config_all_devices.has_key('CONFIG_MEMORY_TRANSACTION_TEST') ?
    ['sgi-o2-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES') and
   config_all_devices.has_key('CONFIG_MEMORY_TRANSACTION_TEST') ?
    ['sgi-heart-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_OCTANE') ?
    ['sgi-heart-cpu-test', 'r10000-identity-test', 'ics1890-test',
     'octane-timer-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_OCTANE') and
   config_all_devices.has_key('CONFIG_MEMORY_TRANSACTION_TEST') ?
    ['octane-test', 'octane-mgras-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_OCTANE') ?
    ['octane-vpro-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_OCTANE') and
   config_all_devices.has_key('CONFIG_SGI_SN0') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sgi-graphics-pipes-test', 'sgi-catalogue-launch-test'] : []) + \
  sn0_router_qtests + \
  sn0_ohci_qtests + \
  sgi_bridge_card_qtests + \
  sn1_board_qtests + \
  sn0_machine_qtests + \
  (config_all_devices.has_key('CONFIG_SGI_SN0') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn-promdata-window-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN0') and
   config_all_devices.has_key('CONFIG_SGI_SN1') and
   is_variable('qemu_sgi_machine_init') ?
    ['sgi-machine-init-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_ODYSSEY') and
   config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-odyssey-test', 'sn1-odyssey-query-test'] : []) + \
  sn0_xbow_qtests + \
  sn0_xbow_transport_qtests + \
  sn1_xbridge_qtests + \
  sn1_pci_expansion_qtests + \
  (config_all_devices.has_key('CONFIG_SGI_IOC4') and \
   config_all_devices.has_key('CONFIG_SGI_XBRIDGE') ? \
    ['sgi-ioc4-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_PIC') and
   config_all_devices.has_key('CONFIG_SGI_TEST_FIXTURES') and
   config_all_devices.has_key('CONFIG_PCI_BRIDGE') ?
    ['sgi-pic-test'] : []) + \
  sn0_mgras_qtests + \
  sgi_board_qtests + \
  (config_all_devices.has_key('CONFIG_SGI_O2_GBE') ?
    ['sgi-gbe-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_KONA') and
   config_all_devices.has_key('CONFIG_TEST_DEVICES') ?
    ['sgi-kona-read-defense-test'] : [])

qtests_mips64 = qtests_mips + sgi_qtests + ['qtest-debug-test'] + \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ? ['o2-r10k-cache-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_CRIME') ? ['sgi-crime-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_MACE') and
   config_all_devices.has_key('CONFIG_MEMORY_TRANSACTION_TEST') ?
    ['sgi-mace-test'] : [])

qtests_mips64el = qtests_mips

qtests_ppc = \
  qtests_filter + \
  (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : []) +            \
  (config_all_accel.has_key('CONFIG_TCG') ? ['prom-env-test'] : []) +                              \
  (config_all_accel.has_key('CONFIG_TCG') ? ['boot-serial-test'] : []) +                           \
  ['boot-order-test']

qtests_ppc64 = \
  qtests_ppc + \
  (config_all_devices.has_key('CONFIG_PSERIES') ? ['device-plug-test'] : []) +               \
  (config_all_devices.has_key('CONFIG_POWERNV') ? ['pnv-xscom-test'] : []) +                 \
  (config_all_devices.has_key('CONFIG_POWERNV') ? ['pnv-xive2-test'] : []) +                 \
  (config_all_devices.has_key('CONFIG_POWERNV') ? ['pnv-spi-seeprom-test'] : []) +           \
  (config_all_devices.has_key('CONFIG_POWERNV') ? ['pnv-host-i2c-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_PSERIES') ? ['numa-test'] : []) +                      \
  (config_all_devices.has_key('CONFIG_PSERIES') ? ['rtas-test'] : []) +                      \
  (slirp.found() ? ['pxe-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_USB_UHCI') ? ['usb-hcd-uhci-test'] : []) +             \
  (config_all_devices.has_key('CONFIG_USB_XHCI_NEC') ? ['usb-hcd-xhci-test'] : []) +         \
  qtests_pci + ['migration-test', 'cpu-plug-test', 'drive_del-test']

qtests_sh4 = (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : []) + \
  (config_all_devices.has_key('CONFIG_PFLASH_CFI02') and
   config_all_devices.has_key('CONFIG_R2D') ?
     ['pflash-cfi02-test'] : [])
qtests_sh4eb = (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : [])

qtests_sparc = ['prom-env-test', 'm48t59-test', 'boot-serial-test'] + \
  qtests_filter

qtests_sparc64 = \
  (config_all_devices.has_key('CONFIG_ISA_TESTDEV') ? ['endianness-test'] : []) +            \
  qtests_filter + \
  ['prom-env-test', 'boot-serial-test']

qtests_npcm7xx = \
  ['npcm7xx_adc-test',
   'npcm7xx_gpio-test',
   'npcm7xx_pwm-test',
   'npcm7xx_rng-test',
   'npcm7xx_sdhci-test',
   'npcm7xx_smbus-test',
   'npcm7xx_timer-test',
   'npcm7xx_watchdog_timer-test'] + \
   (slirp.found() ? ['npcm7xx_emc-test'] : [])
qtests_npcm8xx = \
  ['npcm_gmac-test']
qtests_aspeed = \
  ['aspeed-acry-test',
   'aspeed_gpio-test',
   'aspeed_hace-test',
   'aspeed-sbc-test',
   'aspeed_scu-test',
   'aspeed_smc-test']
qtests_aspeed64 = \
  ['ast2700-gpio-test',
   'ast2700-hace-test',
   'ast2700-sgpio-test',
   'ast2700-smc-test']

qtests_stm32l4x5 = \
  ['stm32l4x5_exti-test',
   'stm32l4x5_syscfg-test',
   'stm32l4x5_rcc-test',
   'stm32l4x5_gpio-test',
   'stm32l4x5_usart-test']

qtests_arm = \
  (config_all_devices.has_key('CONFIG_MPS2') ? ['sse-timer-test'] : []) + \
  (config_all_devices.has_key('CONFIG_CMSDK_APB_DUALTIMER') ? ['cmsdk-apb-dualtimer-test'] : []) + \
  (config_all_devices.has_key('CONFIG_CMSDK_APB_TIMER') ? ['cmsdk-apb-timer-test'] : []) + \
  (config_all_devices.has_key('CONFIG_STELLARIS') or
   config_all_devices.has_key('CONFIG_MPS2') ? ['cmsdk-apb-watchdog-test'] : []) + \
  (config_all_devices.has_key('CONFIG_ASPEED_SOC') ? qtests_aspeed : []) + \
  (config_all_devices.has_key('CONFIG_NPCM7XX') ? qtests_npcm7xx : []) + \
  (config_all_devices.has_key('CONFIG_ZYNQ') ? ['xilinx-zynq-sdhci-test'] : []) + \
  (config_all_devices.has_key('CONFIG_GENERIC_LOADER') ? ['hexloader-test'] : []) + \
  (config_all_devices.has_key('CONFIG_TPM_TIS_I2C') ? ['tpm-tis-i2c-test', 'tpm-tis-i2c-swtpm-test'] : []) + \
  (config_all_devices.has_key('CONFIG_VEXPRESS') ? ['test-arm-mptimer'] : []) + \
  (config_all_devices.has_key('CONFIG_MICROBIT') ? ['microbit-test'] : []) + \
  (config_all_devices.has_key('CONFIG_STM32L4X5_SOC') ? qtests_stm32l4x5 : []) + \
  (config_all_devices.has_key('CONFIG_FSI_APB2OPB_ASPEED') ? ['aspeed_fsi-test'] : []) + \
  (config_all_devices.has_key('CONFIG_CAN_FLEXCAN') ? ['flexcan-test'] : []) + \
  (config_all_devices.has_key('CONFIG_STM32L4X5_SOC') and
   config_all_devices.has_key('CONFIG_DM163')? ['dm163-test'] : []) + \
  ['arm-cpu-features',
   'boot-serial-test']

# TODO: once aarch64 TCG is fixed on ARM 32 bit host, make bios-tables-test unconditional
qtests_aarch64 = \
  (cpu != 'arm' and unpack_edk2_blobs ? ['bios-tables-test'] : []) +                            \
  (config_all_accel.has_key('CONFIG_TCG') and config_all_devices.has_key('CONFIG_TPM_TIS_SYSBUS') ?            \
    ['tpm-tis-device-test', 'tpm-tis-device-swtpm-test'] : []) +                                         \
  (config_all_devices.has_key('CONFIG_XLNX_ZYNQMP_ARM') ? ['xlnx-can-test', 'fuzz-xlnx-dp-test'] : []) + \
  (config_all_devices.has_key('CONFIG_XLNX_VERSAL') ? ['xlnx-canfd-test', 'xlnx-versal-trng-test'] : []) + \
  (config_all_devices.has_key('CONFIG_RASPI') ? ['bcm2835-dma-test', 'bcm2835-i2c-test'] : []) +  \
  (config_all_accel.has_key('CONFIG_TCG') and                                            \
   config_all_devices.has_key('CONFIG_TPM_TIS_I2C') ? ['tpm-tis-i2c-test', 'tpm-tis-i2c-swtpm-test'] : []) + \
  (config_all_devices.has_key('CONFIG_ASPEED_SOC') ? qtests_aspeed64 : []) + \
  (config_all_devices.has_key('CONFIG_NPCM8XX') ? qtests_npcm8xx : []) + \
  (config_all_devices.has_key('CONFIG_IOMMU_TESTDEV') and
   config_all_devices.has_key('CONFIG_ARM_VIRT') ?
   ['iommu-smmuv3-test'] : []) + \
  qtests_cxl +                                                                                  \
  ['arm-cpu-features',
   'numa-test',
   'boot-serial-test',
   'migration-test']

qtests_s390x = \
  qtests_filter + \
  ['boot-serial-test',
   'drive_del-test',
   'device-plug-test',
   'virtio-ccw-test',
   'cpu-plug-test',
   'migration-test']

qtests_riscv32 = \
  (config_all_devices.has_key('CONFIG_SIFIVE_E_AON') ? ['sifive-e-aon-watchdog-test'] : [])

qtests_riscv64 = ['riscv-csr-test'] + \
  (unpack_edk2_blobs ? ['bios-tables-test'] : []) + \
  (config_all_devices.has_key('CONFIG_IOMMU_TESTDEV') and
   config_all_devices.has_key('CONFIG_RISCV_IOMMU') ?
   ['iommu-riscv-test'] : []) + \
  (config_all_devices.has_key('CONFIG_K230') ?
   ['k230-ddr-test', 'k230-wdt-test', 'k230-gsdma-test', 'k230-decomp-gzip-test'] : [])

qtests_hexagon = ['boot-serial-test', 'l2vic-test', 'qct-qtimer-test']

qos_test_ss = ss.source_set()
qos_test_ss.add(
  'ac97-test.c',
  'adc128d818-test.c',
  'adm1272-test.c',
  'adm1266-test.c',
  'ds1338-test.c',
  'e1000-test.c',
  'eepro100-test.c',
  'es1370-test.c',
  'lsm303dlhc-mag-test.c',
  'isl_pmbus_vr-test.c',
  'max34451-test.c',
  'megasas-test.c',
  'ne2000-test.c',
  'tulip-test.c',
  'nvme-test.c',
  'pca9552-test.c',
  'pca9554-test.c',
  'pca9555-test.c',
  'pci-test.c',
  'pcnet-test.c',
  'qlogic-isp-test.c',
  'qlogic-isp2200-test.c',
  'rs5c372-test.c',
  'scsi-tape-test.c',
  'sdhci-test.c',
  'spapr-phb-test.c',
  'tmp105-test.c',
  'emc141x-test.c',
  'usb-hcd-ohci-test.c',
  'virtio-test.c',
  'virtio-blk-test.c',
  'virtio-net-test.c',
  'virtio-rng-test.c',
  'virtio-scsi-test.c',
  'virtio-iommu-test.c',
  'vmxnet3-test.c',
  'igb-test.c',
  'ufs-test.c',
  'riscv-iommu-test.c',
)

if config_all_devices.has_key('CONFIG_VIRTIO_SERIAL')
  qos_test_ss.add(files('virtio-serial-test.c'))
endif
if config_all_devices.has_key('CONFIG_IP_OCTAL_232')
  qos_test_ss.add(files('ipoctal232-test.c'))
endif

if host_os != 'windows'
  qos_test_ss.add(files('e1000e-test.c'))
endif
if have_virtfs
  qos_test_ss.add(files('virtio-9p-test.c'))
endif
if have_vhost_user
  qos_test_ss.add(files('vhost-user-test.c'))
endif
if have_tools and have_vhost_user_blk_server
  qos_test_ss.add(files('vhost-user-blk-test.c'))
endif

tpmemu_files = ['tpm-emu.c', 'tpm-util.c', 'tpm-tests.c']

test_migration_files = [files(
  'migration/bootfile.c',
  'migration/framework.c',
  'migration/migration-qmp.c',
  'migration/migration-util.c',
  'migration/compression-tests.c',
  'migration/cpr-tests.c',
  'migration/file-tests.c',
  'migration/misc-tests.c',
  'migration/precopy-tests.c',
  'migration/postcopy-tests.c',
)]

migration_tls_files = []
if gnutls.found()
  migration_tls_files = [files('migration/tls-tests.c',
                               '../unit/crypto-tls-psk-helpers.c'), gnutls]

  if tasn1.found()
    migration_tls_files += [files('../unit/crypto-tls-x509-helpers.c'), tasn1]
  endif
endif

migration_colo_files = []
if get_option('replication').allowed()
  migration_colo_files = [files('migration/colo-tests.c')]
endif

qtests = {
  'aspeed-acry-test': [files('aspeed-acry-test.c'),
                       crypto],
  'aspeed_hace-test': [files('aspeed-hace-utils.c', 'aspeed_hace-test.c'),
                       crypto],
  'aspeed-sbc-test': [files('aspeed-sbc-test.c'), crypto],
  'aspeed_smc-test': files('aspeed-smc-utils.c', 'aspeed_smc-test.c'),
  'ast2700-hace-test': [files('aspeed-hace-utils.c', 'ast2700-hace-test.c'),
                        crypto],
  'ast2700-smc-test': files('aspeed-smc-utils.c', 'ast2700-smc-test.c'),
  'bios-tables-test': [io, 'boot-sector.c', 'acpi-utils.c', 'tpm-emu.c'],
  'cdrom-test': files('boot-sector.c'),
  'dbus-vmstate-test': files('migration/migration-qmp.c',
                             'migration/migration-util.c') + dbus_vmstate1 +
                       [gio],
  'erst-test': files('erst-test.c'),
  'ivshmem-test': [rt, '../../contrib/ivshmem-server/ivshmem-server.c'],
  'migration-test': test_migration_files + migration_tls_files + migration_colo_files,
  'pxe-test': files('boot-sector.c'),
  'pnv-xive2-test': files('pnv-xive2-common.c', 'pnv-xive2-flush-sync.c',
                          'pnv-xive2-nvpg_bar.c'),
  'qos-test': [chardev, io, qos_test_ss.apply({}).sources()],
  'tpm-crb-swtpm-test': [io, tpmemu_files],
  'tpm-crb-test': [io, tpmemu_files],
  'tpm-tis-swtpm-test': [io, tpmemu_files, 'tpm-tis-util.c'],
  'tpm-tis-test': [io, tpmemu_files, 'tpm-tis-util.c'],
  'tpm-tis-i2c-test': [io, tpmemu_files, 'tpm-tis-i2c-util.c', 'qtest_aspeed.c'],
  'tpm-tis-i2c-swtpm-test': [io, tpmemu_files, 'tpm-tis-i2c-util.c', 'qtest_aspeed.c'],
  'tpm-tis-device-swtpm-test': [io, tpmemu_files, 'tpm-tis-util.c'],
  'tpm-tis-device-test': [io, tpmemu_files, 'tpm-tis-util.c'],
  'virtio-net-failover': test_migration_files,
  'vmgenid-test': files('boot-sector.c', 'acpi-utils.c'),
  'netdev-socket': files('netdev-socket.c', '../unit/socket-helpers.c'),
}

if vnc.found()
  gvnc = dependency('gvnc-1.0', method: 'pkg-config', required: false)
  if gvnc.found()
    qtests += {'vnc-display-test': [gvnc, keymap_targets]}
    qtests_generic += [ 'vnc-display-test' ]
    if have_qemu_vnc and dbus_display and config_all_devices.has_key('CONFIG_VGA')
      dbus_vnc_test_deps = [dbus_display1, qemu_vnc1, gio, gvnc, keymap_targets]
      if gnutls.found() and tasn1.found()
        dbus_vnc_test_deps += [files('../unit/crypto-tls-x509-helpers.c'),
                               gnutls, tasn1]
      endif
      qtests += {'dbus-vnc-test': dbus_vnc_test_deps}
      qtests_x86_64 += ['dbus-vnc-test']
    endif
  endif
endif

if dbus_display
  qtests += {'dbus-display-test': [dbus_display1, gio]}
endif

qtest_executables = {}
foreach dir : target_dirs
  if not dir.endswith('-softmmu')
    continue
  endif

  target_base = dir.split('-')[0]
  qtest_emulator = emulators['qemu-system-' + target_base]
  target_qtests = get_variable('qtests_' + target_base, []) + qtests_generic
  has_kvm = ('CONFIG_KVM' in config_all_accel and host_os == 'linux'
             and cpu == target_base and fs.exists('/dev/kvm'))

  test_deps = roms
  qtest_env = environment()
  if have_tools
    qtest_env.set('QTEST_QEMU_IMG', './qemu-img')
    test_deps += [qemu_img]
  endif
  qtest_env.set('G_TEST_DBUS_DAEMON', meson.project_source_root() / 'tests/dbus-daemon.sh')
  qtest_env.set('G_TEST_SRCDIR', meson.current_source_dir())
  qtest_env.set('QTEST_QEMU_BINARY', './qemu-system-' + target_base)
  if have_tools and have_vhost_user_blk_server
    qtest_env.set('QTEST_QEMU_STORAGE_DAEMON_BINARY', './storage-daemon/qemu-storage-daemon')
    test_deps += [qsd]
  endif
  if have_qemu_vnc
    qtest_env.set('QTEST_QEMU_VNC_BINARY', './tools/qemu-vnc/qemu-vnc')
    test_deps += [qemu_vnc]
  endif
  if is_variable('qemu_sgi_machine_init')
    qtest_env.set('QTEST_QEMU_SGI_MACHINE_INIT',
                  qemu_sgi_machine_init.full_path())
    test_deps += [qemu_sgi_machine_init]
  endif

  qtest_env.set('PYTHON', python.full_path())

  foreach test : target_qtests
    # Executables are shared across targets, declare them only the first time we
    # encounter them
    if not qtest_executables.has_key(test)
      src = [test + '.c']
      deps = [qemuutil, qos]
      if test in qtests
        # use a sourceset to quickly separate sources and deps
        test_ss = ss.source_set()
        test_ss.add(qtests[test])
        src += test_ss.all_sources()
        deps += test_ss.all_dependencies()
      endif
      qtest_executables += {
        test: executable(test, src, dependencies: deps)
      }
    endif

    test_args = ['--tap', '-k']

    if test == 'migration-test' and has_kvm
      test_args += ['--full']
    endif

    if target_base == 'mips64' and test == 'sn0-bridge-test'
      benchmark('sgi-bridge-dma-bench', qtest_executables[test],
                depends: [test_deps, qtest_emulator, emulator_modules],
                env: qtest_env,
                args: ['--tap', '-m', 'perf', '-p',
                       '/mips64/sn0/bridge/dma-speed'],
                protocol: 'tap', timeout: 0, suite: ['speed'])
    endif

    # A slow_qtests entry is a timeout budget. It puts an upstream test in
    # the slow suite, which the quick setup leaves out, but not an SGI one:
    # those stay in the quick setup and register their exhaustive cases
    # only under g_test_slow().
    test_suites = ['qtest', 'qtest-' + target_base] + \
                  (slow_qtests.has_key(test) and test not in sgi_qtests ?
                    ['slow'] : []) + \
                  (target_base == 'mips64' and test in sgi_qtests ?
                    ['sgi', 'sgi-qtest'] : [])

    # A test with a TEST-groups.txt file beside its source runs as one test
    # per group, TEST-GROUP, which passes the binary the group's GLib paths
    # with --group-path, so its groups run at once. The binary's own name
    # then runs every case, outside the qtest suites, for a named run.
    groups = {}
    if fs.is_file(test + '-groups.txt')
      foreach line : fs.read(test + '-groups.txt').split('\n')
        words = line.split()
        if words.length() == 2 and not words[0].startswith('#')
          groups += {words[0]: groups.get(words[0], []) +
                               ['--group-path', words[1]]}
        endif
      endforeach
    endif
    foreach group, paths : groups
      test('qtest-@0@/@1@-@2@'.format(target_base, test, group),
           qtest_executables[test],
           depends: [test_deps, qtest_emulator, emulator_modules],
           env: qtest_env,
           args: test_args + paths,
           protocol: 'tap',
           timeout: slow_qtests.get(test + '-' + group, 60),
           priority: slow_qtests.get(test + '-' + group, 60),
           suite: test_suites)
    endforeach

    test('qtest-@0@/@1@'.format(target_base, test),
         qtest_executables[test],
         depends: [test_deps, qtest_emulator, emulator_modules],
         env: qtest_env,
         args: test_args,
         is_parallel: not test.startswith('sgi-mgras-') and
                      test != 'sgi-replay-test',
         protocol: 'tap',
         timeout: slow_qtests.get(test, 60),
         priority: test == 'sgi-replay-test' ? 1100 :
                   slow_qtests.get(test, 60),
         suite: groups.keys().length() > 0 ? [] : test_suites)
  endforeach
endforeach
