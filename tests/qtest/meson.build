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
  # Complete one-wire memory reads transfer every bit through QMP.
  'sn0-router-nic-test': 300,
  # Complete one-wire reads plus module identity and migration exceed 120s.
  'sn0-o2000-module-mgmt-test': 300,
  'qos-test': 120,
  'vmgenid-test': 610,
  # These SN0 device qtests run 50-59s under TCG; the 60s default leaves no
  # headroom and they time out under CI load. Give them the 120s slow budget.
  'sn0-pi-error-test': 120,
  # The MTTCG primitives run several thread=multi TCG cases, and the R10000
  # performance test adds live migration and a stream analysis pass. Both fit
  # under 60s unloaded but need headroom under parallel CI load.
  'sn0-mttcg-test': 120,
  'sn0-r10k-perf-migration-test': 120,
  # sn0-qtest-cpu-test runs four sequential TCG cases with 30s polling
  # deadlines. Allow startup and teardown headroom beyond those 120s.
  'sn0-qtest-cpu-test': 180,
  # The full 32-reset run took 265s under load. Keep headroom for all cycles.
  'sn0-warm-reset-test': 600,
  # IOC3 took about 106s under load; keep twice that measured wall time.
  'sn0-ioc3-test': 240,
  # Hub's 115 cases took 212s alone and exceeded 240s in the parallel suite.
  'sn0-hub-test': 600,
  'sn0-origin2000-xtalk-attach-test': 120,
  # Routed acceptance takes 44-60s; keep headroom under parallel CI load.
  'sn0-origin2000-test': 120,
  # The migration suite runs twenty sequential cases. Its intentional
  # corrupt-ELSC-stream case also starts a third destination, and the added
  # Origin 2000 responder migration brought the measured total near 60s.
  'sn0-migration-test': 120,
  # Tigon reached 60.10s in the same loaded run without a failed assertion.
  'sn0-tigon-test': 120,
  # Model and bridge have grown the same way: 57s and 62s measured solo on a
  # loaded workstation, and in CI they were the second and fourth most killed
  # tests over Aug 30 - Sep 3 (105 and 20 timeout kills at the 120s ceiling,
  # zero failing subtests). Same treatment: twice the measured wall time.
  'sn0-bridge-test': 240,
  'sn0-model-test': 240,
  # sn0-ioc3-serial-test itself finishes in 7-17s, but its own socket read/accept
  # deadline is 120s to tolerate scheduling delay (see
  # tests/qtest/sn0-ioc3-serial-test.c). This budget has to exceed that deadline,
  # not equal it: a stalled socket reaches both at the same instant, and whichever
  # wins decides whether the run reports which socket stalled or just says the
  # binary was killed. Leave room for the deadline to fire and the test to report.
  'sn0-ioc3-serial-test': 240,
  # sn0-elsc-test's AUX socket-accept deadline is likewise 120s (see
  # tests/qtest/sn0-elsc-test.c), and needs the same headroom above it for the
  # same reason.
  'sn0-elsc-test': 240,
  # sn0-isp12160-test holds a read behind a block filter in each recovery case,
  # so its runtime tracks host load: 75.9s measured at a load average near 90,
  # against meson's 60s default, and a timeout kill was seen there. The backlog,
  # recovery and unplug cases added later hold reads the same way, and the whole
  # suite then measured between 45s and 108s across repeated runs on a
  # workstation that was also building and testing other trees. 180s leaves that
  # spread the same room the 120s left the figure it was set from.
  'sn0-isp12160-test': 180,
  # sn1-bedrock-flash-test rewrites a whole 64 KiB sector one flash word at a
  # time, which is four qtest round trips per word, and starts eight further
  # QEMU processes for the rejection and migration cases.
  'sn1-bedrock-flash-test': 180,
}

qtest_priorities = {
  # Keep the Hub suite's scheduling order independent of its larger budget.
  'sn0-hub-test': 240,
  # These four-vCPU MTTCG tests need longer timeouts, but promoting them with
  # long SN0 device suites causes host contention and unrelated timeouts.
  'sn0-qtest-cpu-test': 60,
  'sn0-warm-reset-test': 60,
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

qtests_pci = \
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
  (slirp.found() and config_all_devices.has_key('CONFIG_SGI_IOC3') ?
    ['sgi-ioc3-eth-test'] : []) + \
  (config_all_devices.has_key('CONFIG_XFRAME_PCI') ? ['xframe-test'] : []) + \
  (config_all_devices.has_key('CONFIG_TIGON') ? ['tigon-test'] : []) + \
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
  (config_all_devices.has_key('CONFIG_USB_OHCI_PCI') ? ['usb-lucent-ohci-test'] : []) +     \
  (config_all_devices.has_key('CONFIG_USB_UHCI') ? ['usb-hcd-uhci-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_USB_UHCI') and                                        \
   config_all_devices.has_key('CONFIG_USB_EHCI') ? ['usb-hcd-ehci-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_USB_XHCI_NEC') ? ['usb-hcd-xhci-test'] : []) +        \
  (config_all_devices.has_key('CONFIG_TPM_CRB') ? ['tpm-crb-test'] : []) +                  \
  (config_all_devices.has_key('CONFIG_TPM_CRB') ? ['tpm-crb-swtpm-test'] : []) +            \
  (config_all_devices.has_key('CONFIG_TPM_TIS_ISA') ? ['tpm-tis-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_TPM_TIS_ISA') ? ['tpm-tis-swtpm-test'] : []) +        \
  (config_all_devices.has_key('CONFIG_RTL8139_PCI') ? ['rtl8139-test'] : []) +              \
  (config_all_devices.has_key('CONFIG_BCM570X') ? ['bcm570x-test'] : []) +                   \
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

qtests_x86_64 = qtests_i386

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

# The XBridge qtests use test-only Crosstalk and PCI endpoints.  Keep the
# registration visible under the device guard, then omit tests whose fixtures
# are not present in a minimal device build.
sn1_xbridge_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_XBRIDGE') ?
    ['sn1-xbridge-test', 'sn1-xbridge-chip-test',
     'sn1-xbridge-xbow-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_XTALK_TEST_TARGET')
  sn1_xbridge_qtests = []
elif not config_all_devices.has_key('CONFIG_SGI_BRIDGE_DMA_TEST')
  sn1_xbridge_qtests = ['sn1-xbridge-xbow-test']
elif not config_all_devices.has_key('CONFIG_PCI_BRIDGE') or \
     not config_all_devices.has_key('CONFIG_SGI_IOMMU_NOTIFIER_TEST') or \
     not config_all_devices.has_key('CONFIG_IOMMU_TESTDEV')
  sn1_xbridge_qtests = ['sn1-xbridge-chip-test',
                        'sn1-xbridge-xbow-test']
endif

sn0_hub_outgoing_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-origin2000-hub-outgoing-test'] : [])
if not config_all_devices.has_key('CONFIG_SN0_HUB_OUTGOING_TEST')
  sn0_hub_outgoing_qtests = []
endif

sn0_xtalk_assembly_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-origin2000-xtalk-owner-test',
     'sn0-origin2000-xtalk-attach-test'] : [])
if not config_all_devices.has_key('CONFIG_SN0_XTALK_ASSEMBLY_TEST')
  sn0_xtalk_assembly_qtests = []
endif

# The Crossbow qtest builds its own populated switch out of test-only
# Crosstalk endpoints.  Keep the registration under the machine guard, then
# omit it where that fixture is not present in a minimal device build.
sn0_xbow_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ? ['sn0-xbow-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_XTALK_TEST_TARGET')
  sn0_xbow_qtests = []
endif

sn0_router_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-router-test', 'sn0-router-nic-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_ROUTER_TEST')
  sn0_router_qtests = []
endif

sn0_hub_packet_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-origin2000-hub-packet-test'] : [])
if not config_all_devices.has_key('CONFIG_SN0_HUB_PACKET_TEST')
  sn0_hub_packet_qtests = []
endif

sn0_xbow_transport_qtests = \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ?
    ['sn0-xbow-transport-test'] : [])
if not config_all_devices.has_key('CONFIG_SGI_XBOW_TRANSPORT_TEST')
  sn0_xbow_transport_qtests = []
endif

# The Origin 200 is a 64-bit machine, so its tests cannot be shared with the
# 32-bit mips targets, where the machine does not exist.
qtests_mips64 = \
  qtests_mips + \
  sn0_router_qtests + \
  sn0_ohci_qtests + \
  (config_all_devices.has_key('CONFIG_SGI_L1') ?
    ['sn1-l1-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_MD') ?
    ['sn1-bedrock-md-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_NI') ?
    ['sn1-bedrock-ni-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_IO') ?
    ['sn1-bedrock-io-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_LB') ?
    ['sn1-bedrock-lb-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_XB') ?
    ['sn1-bedrock-xb-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK_FLASH') ?
    ['sn1-bedrock-flash-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_BEDROCK') ?
    ['sn1-bedrock-pi-test', 'sn1-bedrock-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN1') ?
    ['sn1-address-map-test', 'sn1-bedrock-ecc-cpu-test',
     'sn1-cpu-identity-test',
     'sn1-io8-scsi-test', 'sn1-xbridge-host-error-test'] : []) + \
  (config_all_devices.has_key('CONFIG_SGI_SN0') ? ['sn0-mttcg-test',
                                                   'sn0-nmi-test',
                                                   'sn0-r10k-llsc-test',
                                                   'sn0-r10k-perf-migration-test',
                                                   'sn0-status-re-test',
                                                   'sn0-mmio-branch-test',
                                                   'sn0-address-map-test',
                                                   'sn0-bridge-test',
                                                   'sn0-firmware-test',
                                                   'sn0-ioc3-test',
                                                   'sn0-ioc3-parallel-test',
                                                   'sn0-ioc3-serial-test',
                                                   'sn0-qlogic-isp-test',
                                                   'sn0-isp12160-test',
                                                   'sn0-xframe-test',
                                                   'sn0-tigon-test',
                                                   'sn0-bcm570x-test',
                                                   'sn0-hub-test',
                                                   'sn0-pi-error-test',
                                                   'sn0-i2c-test',
                                                   'sn0-elsc-test',
                                                   'sn0-widget0-test',
                                                   'sn0-widget-decode-test',
                                                   'sn0-bridge-invalid-addr-test',
                                                   'sn0-bridge-widget-test',
                                                   'sn0-model-test',
                                                   'sn0-origin2000-test',
                                                   'sn0-o2000-module-mgmt-test',
                                                   'sn0-origin2000-pflash-test',
                                                   'sn0-smp-test',
                                                   'sn0-cross-node-test',
                                                   'sn0-warm-reset-test',
                                                   'sn0-migration-test',
                                                   'sn0-snapshot-test',
                                                   'sn0-ds1685-test',
                                                   'sn0-debug-test',
                                                   'sn0-fabric-test',
                                                   'sn0-qtest-cpu-test',
                                                   'sn0-cpu-registers-test',
                                                   'sn0-syscall-trace-test',
                                                   'sn0-debug-breakpoint-test',
                                                   'sn0-tlb-query-test'] : []) + \
  sn0_xbow_qtests + \
  sn0_xbow_transport_qtests + \
  sn1_xbridge_qtests + \
  sn0_hub_packet_qtests + sn0_hub_outgoing_qtests + \
  sn0_xtalk_assembly_qtests

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
     ['pflash-cfi02-test', 'sgi-pflash-cfi02-test'] : [])
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
  qtest_env.set('QTEST_QEMU_BINARY', './qemu-system-' + target_base)
  if have_tools and have_vhost_user_blk_server
    qtest_env.set('QTEST_QEMU_STORAGE_DAEMON_BINARY', './storage-daemon/qemu-storage-daemon')
    test_deps += [qsd]
  endif
  if have_qemu_vnc
    qtest_env.set('QTEST_QEMU_VNC_BINARY', './tools/qemu-vnc/qemu-vnc')
    test_deps += [qemu_vnc]
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

    test('qtest-@0@/@1@'.format(target_base, test),
         qtest_executables[test],
         depends: [test_deps, qtest_emulator, emulator_modules],
         env: qtest_env,
         args: test_args,
         protocol: 'tap',
         timeout: slow_qtests.get(test, 60),
         priority: qtest_priorities.get(test, slow_qtests.get(test, 60)),
         suite: ['qtest', 'qtest-' + target_base] +
                (slow_qtests.has_key(test) ? ['slow'] : []))
  endforeach
endforeach
