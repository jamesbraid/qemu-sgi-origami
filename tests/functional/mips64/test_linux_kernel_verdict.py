#!/usr/bin/env python3
#
# Equivalence test: test_linux_kernel's verdict logic ports
# tools/linux-usercheck's own missing_assertions(log, cpus) contract,
# without booting anything
#
# SPDX-License-Identifier: GPL-2.0-or-later
"""
A plain unittest -- no QEMU, no qemu_test import, runnable with just
`python3 tests/functional/mips64/test_linux_kernel_verdict.py` (or
`uv run --with pytest pytest tests/functional/mips64/test_linux_kernel_verdict.py
-v`). It exercises missing_assertions(), the pure function
test_linux_kernel.py calls against a captured console transcript, over
fixture transcripts that stand in for a captured guest console log.

missing_assertions(log, cpus) is a close port of tools/linux-usercheck's
own function of the same name (project checkout), for the one- and
two-CPU cases only -- the four-CPU case is explicitly out of scope for
this test (see test_linux_kernel.py's own docstring: a known,
pre-existing stop_machine livelock at that population, unrelated to
this fork's own work).

One deliberate departure from the ported original: linux-usercheck's own
missing_assertions() only checks for a "Discovered N cpus on 1 nodes"
line at cpus == 2 (there is no analogous check at cpus == 1 in that
function, just "init handoff" and "userspace exit status"). This port
adds that discovery-line check at ONE cpu too, per this port's own task
brief, which states the one-CPU contract as exit code AND discovery
line together. This is not an invented marker: a real one-CPU capture
in the project checkout already prints exactly this line -- see
REAL_ONE_CPU_PASS_TRANSCRIPT's own provenance comment below.

The exact-match property the brief calls out -- a fatal signal must not
masquerade as a chosen exit -- falls out of the marker text itself
without any extra logic: the guest's own panic message prints the full
8-hex-digit value every time ("exitcode=0x%08x" in the kernel's own
format), with the chosen exit status in the upper byte and a fatal
signal's number in the LOWER byte (0 when no signal was involved). A
signal death therefore produces a wholly different 8-hex string, not a
prefix or suffix of the wanted one, so a plain substring search against
the full literal ("exitcode=0x00002a00" at one CPU, "0x00002b00" at
two) already rejects it -- proven directly by
MissingAssertionsTest.test_fatal_signal_transcript_fails_even_though_init_ran
below, and by test_a_near_miss_exit_code_does_not_satisfy, which checks a value that
shares digits with the wanted one but is not it.

Fixture provenance (see each fixture's own comment for the exact source
and what, if anything, was changed). Every fixture's line endings carry
a real \r before the \n, matching the raw serial console bytes the
source captures actually contain (confirmed against the source files
directly: every line ends CRLF) -- the same reason test_diag_verdict.py
converts its own captured "^M" text back into a literal \r rather than
leaving the CR out. It changes nothing about missing_assertions() itself
(none of its markers span a line ending), but keeps these fixtures
byte-faithful to what a real capture looks like.
"""

import unittest


def missing_assertions(log, cpus):
    """Which of the one- or two-CPU userspace-self-check markers never
    appeared in log, ported from tools/linux-usercheck's own function of
    the same name (project checkout) -- see this module's own docstring
    for the one departure (a discovery-line check added at one CPU) and
    why the exit-status marker alone already rejects a fatal-signal
    death. An empty return means every required marker was found, i.e.
    PASS; cpus == 4 is out of scope (see this module's docstring) and
    raises rather than silently answering a question this port does not
    cover.
    """
    if cpus not in (1, 2):
        raise ValueError(
            'missing_assertions: cpus must be 1 or 2 -- four-CPU '
            'wall-clock boot is a known-failure sentinel out of scope '
            'for this test, see this module\'s own docstring')

    exit_status = 'exitcode=0x00002b00' if cpus == 2 else 'exitcode=0x00002a00'
    checks = [('cpu discovery', f'Discovered {cpus} cpus on 1 nodes')]
    if cpus == 2:
        checks += [
            ('secondary CPU execution', 'CPU1 revision is:'),
            ('two-CPU scheduler bring-up', 'smp: Brought up 1 node, 2 CPUs'),
        ]
    checks += [
        ('init handoff', 'Run /init as init process'),
        ('userspace exit status', exit_status),
    ]
    return [description for description, marker in checks if marker not in log]


# ---------------------------------------------------------------------------
# Fixtures

REAL_ONE_CPU_PASS_TRANSCRIPT = """\
Linux version 7.2.0-rc7 (jamesb@521d96223a7d) (mips64-linux-gnuabi64-gcc (Debian \
14.2.0-13) 14.2.0, GNU ld (GNU Binutils for Debian) 2.44) #12 SMP Wed Aug 12 03:51:50 \
UTC 2026\r
Discovered 1 cpus on 1 nodes\r
************** Topology ********************\r
    00 \r
00  10 \r
printk: legacy bootconsole [early0] enabled\r
CPU0 revision is: 00000900 (R10000)\r
FPU revision is: 00000900\r
IP27: Running on node 0.\r
Node 0 has a primary CPU.\r
Node 0 has no secondary CPU.\r
Machine is in M mode.\r
Initrd not found or empty - disabling initrd\r
REPLICATION: ON nasid 0, ktext from nasid 0, kdata from nasid 0\r
Primary instruction cache 32kB, VIPT, 2-way, linesize 64 bytes.\r
Primary data cache 32kB, 2-way, VIPT, no aliases, linesize 32 bytes\r
Unified secondary cache 1024kB 2-way, linesize 128 bytes.\r
Zone ranges:\r
  Normal   [mem 0x0000000000000000-0x000000001bffffff]\r
Movable zone start for each node\r
Early memory node ranges\r
  node   0: [mem 0x0000000000000000-0x0000000003ffffff]\r
  node   0: [mem 0x0000000008000000-0x000000000bffffff]\r
  node   0: [mem 0x0000000010000000-0x0000000013ffffff]\r
  node   0: [mem 0x0000000018000000-0x000000001bffffff]\r
Initmem setup node 0 [mem 0x0000000000000000-0x000000001bffffff]\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
percpu: Embedded 19 pages/cpu s37536 r8192 d32096 u77824\r
CPU 0 clock is 180MHz.\r
printk: log buffer data + meta data: 32768 + 114688 = 147456 bytes\r
Dentry cache hash table entries: 32768 (order: 6, 262144 bytes, linear)\r
Inode-cache hash table entries: 16384 (order: 5, 131072 bytes, linear)\r
Fallback order for Node 0: 0 \r
Built 1 zonelists, mobility grouping on.  Total pages: 65536\r
Policy zone: Normal\r
mem auto-init: stack:all(zero), heap alloc:off, heap free:off\r
SLUB: HWalign=128, Order=0-3, MinObjects=0, CPUs=1, Nodes=1\r
rcu: Hierarchical RCU implementation.\r
rcu: 	RCU event tracing is enabled.\r
rcu: 	RCU restricting CPUs from NR_CPUS=64 to nr_cpu_ids=1.\r
rcu: RCU calculated value of scheduler-enlistment delay is 100 jiffies.\r
rcu: Adjusting geometry for rcu_fanout_leaf=16, nr_cpu_ids=1\r
NR_IRQS: 256\r
rcu: srcu_init: Setting srcu_struct sizes based on contention.\r
clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: \
1911260446275000 ns\r
clocksource: HUB-RT: mask: 0xfffffffffffff max_cycles: 0x127350b88, max_idle_ns: \
1763180808480 ns\r
sched_clock: 52 bits at 1250kHz, resolution 800ns, wraps every 4398046510800ns\r
Calibrating delay loop... 661.50 BogoMIPS (lpj=330752)\r
pid_max: default: 32768 minimum: 301\r
Mount-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
Mountpoint-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
VFS: Finished mounting rootfs on nullfs\r
rcu: Hierarchical SRCU implementation.\r
rcu: 	Max phase no-delay instances is 400.\r
smp: Bringing up secondary CPUs ...\r
smp: Brought up 1 node, 1 CPU\r
Memory: 240840K/262144K available (9086K kernel code, 485K rwdata, 1972K rodata, 424K \
init, 188K bss, 19976K reserved, 0K cma-reserved)\r
posixtimers hash table entries: 512 (order: 1, 8192 bytes, linear)\r
futex hash table entries: 256 (32768 bytes on 1 NUMA nodes, total 32 KiB, linear).\r
NET: Registered PF_NETLINK/PF_ROUTE protocol family\r
thermal_sys: Registered thermal governor 'step_wise'\r
xtalk:n0/8 bridge widget\r
raid6: using intx1 recovery algorithm\r
raid6: int64x1  gen()  1333 MB/s\r
raid6: int64x2  gen()  1710 MB/s\r
raid6: int64x4  gen()  1551 MB/s\r
raid6: int64x8  gen()   695 MB/s\r
raid6: using algorithm int64x2 gen() 1710 MB/s\r
raid6: .... xor() 904 MB/s, rmw enabled\r
SCSI subsystem initialized\r
pps_core: LinuxPPS API ver. 1 registered\r
pps_core: Software ver. 5.3.6 - Copyright 2005-2007 Rodolfo Giometti <giometti@linux.it>\r
PTP clock support registered\r
clocksource: Switched to clocksource HUB-RT\r
NET: Registered PF_INET protocol family\r
IP idents hash table entries: 4096 (order: 3, 32768 bytes, linear)\r
tcp_listen_portaddr_hash hash table entries: 256 (order: 0, 4096 bytes, linear)\r
Table-perturb hash table entries: 65536 (order: 6, 262144 bytes, linear)\r
TCP established hash table entries: 2048 (order: 2, 16384 bytes, linear)\r
TCP bind hash table entries: 2048 (order: 4, 65536 bytes, linear)\r
TCP: Hash tables configured (established 2048 bind 2048)\r
UDP hash table entries: 256 (order: 2, 16384 bytes, linear)\r
NET: Registered PF_UNIX/PF_LOCAL protocol family\r
RPC: Registered named UNIX socket transport module.\r
RPC: Registered udp transport module.\r
RPC: Registered tcp transport module.\r
RPC: Registered tcp-with-tls transport module.\r
RPC: Registered tcp NFSv4.1 backchannel transport module.\r
PCI: CLS 0 bytes, default 128\r
Initialise system trusted keyrings\r
workingset: timestamp_bits=56 (anon: 51) max_order=16 bucket_order=0 (anon: 0)\r
Key type asymmetric registered\r
Asymmetric key parser 'x509' registered\r
Block layer SCSI generic (bsg) driver version 0.4 loaded (major 251)\r
io scheduler mq-deadline registered\r
io scheduler kyber registered\r
xor: measuring software checksum speed\r
   32regs_prefetch :  1095 MB/sec\r
   32regs          :  1734 MB/sec\r
   8regs_prefetch  :  1438 MB/sec\r
   8regs           :  1718 MB/sec\r
xor: using function: 32regs (1734 MB/sec)\r
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled\r
loop: module loaded\r
st: Version 20160209, fixed bufsize 32768, s/g segs 256\r
Driver for 1-wire Dallas network protocol.\r
w1_master_driver w1_bus_master1: Attaching one wire slave 0b.060504030201 crc 36\r
NET: Registered PF_INET6 protocol family\r
Segment Routing with IPv6\r
In-situ OAM (IOAM) with IPv6\r
NET: Registered PF_PACKET protocol family\r
PFKEY is deprecated and scheduled to be removed in 2027, please contact the netdev \
mailing list\r
NET: Registered PF_KEY protocol family\r
xtalk-bridge xtalk-bridge.1.auto: PCI host bridge to bus 0000:00\r
pci_bus 0000:00: root bus resource [mem 0x08200000-0x08ffffff]\r
pci_bus 0000:00: root bus resource [io  0x8200000-0x8ffffff]\r
pci_bus 0000:00: root bus resource [bus 00-ff]\r
pci 0000:00:00.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:00.0: BAR 0 [io  0x8200000-0x82000ff]\r
pci 0000:00:00.0: BAR 1 [mem 0x08200000-0x082000ff]\r
pci 0000:00:01.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:01.0: BAR 0 [io  0x8400000-0x84000ff]\r
pci 0000:00:01.0: BAR 1 [mem 0x08400000-0x084000ff]\r
pci 0000:00:02.0: [10a9:0003] type 00 class 0xff0000 conventional PCI endpoint\r
pci 0000:00:02.0: BAR 0 [mem 0x08600000-0x086fffff]\r
qla1280: QLA1040 found on PCI bus 0, dev 0\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:00.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
qla1280: QLA1040 found on PCI bus 0, dev 1\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:01.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
Loading compiled-in X.509 certificates\r
Demotion targets for Node 0: null\r
Warning: unable to open an initial console.\r
Freeing unused kernel image (initmem) memory: 424K\r
This architecture does not have kernel memory protection.\r
Run /init as init process\r
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00002a00\r
---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x00002a00 ]---\r
"""

# SYNTHETIC. No archived capture in the project checkout is a genuine
# two-CPU-only boot (docs/boot-logs/ there has only one- and four-CPU
# captures at the time of this port) so this fixture is built by editing
# REAL_ONE_CPU_PASS_TRANSCRIPT above, not by inventing text from
# scratch. Exactly three edits, all traceable:
#   1. "Discovered 1 cpus on 1 nodes" -> "Discovered 2 cpus on 1 nodes".
#   2. The six-line CPU1 bring-up block (cache-size lines, "CPU 1 clock
#      is 180MHz.", "CPU1 revision is: 00000900 (R10000)", "FPU revision
#      is: 00000900") is inserted, copied VERBATIM from a real four-CPU
#      capture's own node-0 secondary-CPU bring-up --
#      docs/boot-logs/linux.boot.2026-08-20T23:24:55-07:00, lines 78-83
#      in the project checkout -- immediately after "smp: Bringing up
#      secondary CPUs ...", replacing the one-CPU transcript's own
#      "smp: Brought up 1 node, 1 CPU" with "smp: Brought up 1 node, 2
#      CPUs" (the exact marker tools/linux-usercheck's own
#      missing_assertions checks at cpus == 2).
#   3. Both "exitcode=0x00002a00" panic lines become
#      "exitcode=0x00002b00" -- 43 << 8, per tools/linux-usercheck's own
#      documented encoding for the two-CPU affinity-stress exit.
# Everything else -- the whole rest of the boot, byte for byte -- is the
# same real one-CPU capture unchanged.
SYNTHETIC_TWO_CPU_PASS_TRANSCRIPT = """\
Linux version 7.2.0-rc7 (jamesb@521d96223a7d) (mips64-linux-gnuabi64-gcc (Debian \
14.2.0-13) 14.2.0, GNU ld (GNU Binutils for Debian) 2.44) #12 SMP Wed Aug 12 03:51:50 \
UTC 2026\r
Discovered 2 cpus on 1 nodes\r
************** Topology ********************\r
    00 \r
00  10 \r
printk: legacy bootconsole [early0] enabled\r
CPU0 revision is: 00000900 (R10000)\r
FPU revision is: 00000900\r
IP27: Running on node 0.\r
Node 0 has a primary CPU.\r
Node 0 has no secondary CPU.\r
Machine is in M mode.\r
Initrd not found or empty - disabling initrd\r
REPLICATION: ON nasid 0, ktext from nasid 0, kdata from nasid 0\r
Primary instruction cache 32kB, VIPT, 2-way, linesize 64 bytes.\r
Primary data cache 32kB, 2-way, VIPT, no aliases, linesize 32 bytes\r
Unified secondary cache 1024kB 2-way, linesize 128 bytes.\r
Zone ranges:\r
  Normal   [mem 0x0000000000000000-0x000000001bffffff]\r
Movable zone start for each node\r
Early memory node ranges\r
  node   0: [mem 0x0000000000000000-0x0000000003ffffff]\r
  node   0: [mem 0x0000000008000000-0x000000000bffffff]\r
  node   0: [mem 0x0000000010000000-0x0000000013ffffff]\r
  node   0: [mem 0x0000000018000000-0x000000001bffffff]\r
Initmem setup node 0 [mem 0x0000000000000000-0x000000001bffffff]\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
percpu: Embedded 19 pages/cpu s37536 r8192 d32096 u77824\r
CPU 0 clock is 180MHz.\r
printk: log buffer data + meta data: 32768 + 114688 = 147456 bytes\r
Dentry cache hash table entries: 32768 (order: 6, 262144 bytes, linear)\r
Inode-cache hash table entries: 16384 (order: 5, 131072 bytes, linear)\r
Fallback order for Node 0: 0 \r
Built 1 zonelists, mobility grouping on.  Total pages: 65536\r
Policy zone: Normal\r
mem auto-init: stack:all(zero), heap alloc:off, heap free:off\r
SLUB: HWalign=128, Order=0-3, MinObjects=0, CPUs=1, Nodes=1\r
rcu: Hierarchical RCU implementation.\r
rcu: 	RCU event tracing is enabled.\r
rcu: 	RCU restricting CPUs from NR_CPUS=64 to nr_cpu_ids=1.\r
rcu: RCU calculated value of scheduler-enlistment delay is 100 jiffies.\r
rcu: Adjusting geometry for rcu_fanout_leaf=16, nr_cpu_ids=1\r
NR_IRQS: 256\r
rcu: srcu_init: Setting srcu_struct sizes based on contention.\r
clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: \
1911260446275000 ns\r
clocksource: HUB-RT: mask: 0xfffffffffffff max_cycles: 0x127350b88, max_idle_ns: \
1763180808480 ns\r
sched_clock: 52 bits at 1250kHz, resolution 800ns, wraps every 4398046510800ns\r
Calibrating delay loop... 661.50 BogoMIPS (lpj=330752)\r
pid_max: default: 32768 minimum: 301\r
Mount-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
Mountpoint-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
VFS: Finished mounting rootfs on nullfs\r
rcu: Hierarchical SRCU implementation.\r
rcu: 	Max phase no-delay instances is 400.\r
smp: Bringing up secondary CPUs ...\r
Primary instruction cache 32kB, VIPT, 2-way, linesize 64 bytes.\r
Primary data cache 32kB, 2-way, VIPT, no aliases, linesize 32 bytes\r
Unified secondary cache 1024kB 2-way, linesize 128 bytes.\r
CPU 1 clock is 180MHz.\r
CPU1 revision is: 00000900 (R10000)\r
FPU revision is: 00000900\r
smp: Brought up 1 node, 2 CPUs\r
Memory: 240840K/262144K available (9086K kernel code, 485K rwdata, 1972K rodata, 424K \
init, 188K bss, 19976K reserved, 0K cma-reserved)\r
posixtimers hash table entries: 512 (order: 1, 8192 bytes, linear)\r
futex hash table entries: 256 (32768 bytes on 1 NUMA nodes, total 32 KiB, linear).\r
NET: Registered PF_NETLINK/PF_ROUTE protocol family\r
thermal_sys: Registered thermal governor 'step_wise'\r
xtalk:n0/8 bridge widget\r
raid6: using intx1 recovery algorithm\r
raid6: int64x1  gen()  1333 MB/s\r
raid6: int64x2  gen()  1710 MB/s\r
raid6: int64x4  gen()  1551 MB/s\r
raid6: int64x8  gen()   695 MB/s\r
raid6: using algorithm int64x2 gen() 1710 MB/s\r
raid6: .... xor() 904 MB/s, rmw enabled\r
SCSI subsystem initialized\r
pps_core: LinuxPPS API ver. 1 registered\r
pps_core: Software ver. 5.3.6 - Copyright 2005-2007 Rodolfo Giometti <giometti@linux.it>\r
PTP clock support registered\r
clocksource: Switched to clocksource HUB-RT\r
NET: Registered PF_INET protocol family\r
IP idents hash table entries: 4096 (order: 3, 32768 bytes, linear)\r
tcp_listen_portaddr_hash hash table entries: 256 (order: 0, 4096 bytes, linear)\r
Table-perturb hash table entries: 65536 (order: 6, 262144 bytes, linear)\r
TCP established hash table entries: 2048 (order: 2, 16384 bytes, linear)\r
TCP bind hash table entries: 2048 (order: 4, 65536 bytes, linear)\r
TCP: Hash tables configured (established 2048 bind 2048)\r
UDP hash table entries: 256 (order: 2, 16384 bytes, linear)\r
NET: Registered PF_UNIX/PF_LOCAL protocol family\r
RPC: Registered named UNIX socket transport module.\r
RPC: Registered udp transport module.\r
RPC: Registered tcp transport module.\r
RPC: Registered tcp-with-tls transport module.\r
RPC: Registered tcp NFSv4.1 backchannel transport module.\r
PCI: CLS 0 bytes, default 128\r
Initialise system trusted keyrings\r
workingset: timestamp_bits=56 (anon: 51) max_order=16 bucket_order=0 (anon: 0)\r
Key type asymmetric registered\r
Asymmetric key parser 'x509' registered\r
Block layer SCSI generic (bsg) driver version 0.4 loaded (major 251)\r
io scheduler mq-deadline registered\r
io scheduler kyber registered\r
xor: measuring software checksum speed\r
   32regs_prefetch :  1095 MB/sec\r
   32regs          :  1734 MB/sec\r
   8regs_prefetch  :  1438 MB/sec\r
   8regs           :  1718 MB/sec\r
xor: using function: 32regs (1734 MB/sec)\r
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled\r
loop: module loaded\r
st: Version 20160209, fixed bufsize 32768, s/g segs 256\r
Driver for 1-wire Dallas network protocol.\r
w1_master_driver w1_bus_master1: Attaching one wire slave 0b.060504030201 crc 36\r
NET: Registered PF_INET6 protocol family\r
Segment Routing with IPv6\r
In-situ OAM (IOAM) with IPv6\r
NET: Registered PF_PACKET protocol family\r
PFKEY is deprecated and scheduled to be removed in 2027, please contact the netdev \
mailing list\r
NET: Registered PF_KEY protocol family\r
xtalk-bridge xtalk-bridge.1.auto: PCI host bridge to bus 0000:00\r
pci_bus 0000:00: root bus resource [mem 0x08200000-0x08ffffff]\r
pci_bus 0000:00: root bus resource [io  0x8200000-0x8ffffff]\r
pci_bus 0000:00: root bus resource [bus 00-ff]\r
pci 0000:00:00.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:00.0: BAR 0 [io  0x8200000-0x82000ff]\r
pci 0000:00:00.0: BAR 1 [mem 0x08200000-0x082000ff]\r
pci 0000:00:01.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:01.0: BAR 0 [io  0x8400000-0x84000ff]\r
pci 0000:00:01.0: BAR 1 [mem 0x08400000-0x084000ff]\r
pci 0000:00:02.0: [10a9:0003] type 00 class 0xff0000 conventional PCI endpoint\r
pci 0000:00:02.0: BAR 0 [mem 0x08600000-0x086fffff]\r
qla1280: QLA1040 found on PCI bus 0, dev 0\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:00.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
qla1280: QLA1040 found on PCI bus 0, dev 1\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:01.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
Loading compiled-in X.509 certificates\r
Demotion targets for Node 0: null\r
Warning: unable to open an initial console.\r
Freeing unused kernel image (initmem) memory: 424K\r
This architecture does not have kernel memory protection.\r
Run /init as init process\r
Kernel panic - not syncing: Attempted to kill init! exitcode=0x00002b00\r
---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x00002b00 ]---\r
"""

# SYNTHETIC: REAL_ONE_CPU_PASS_TRANSCRIPT with ONE edit -- both
# "exitcode=0x00002a00" occurrences become "exitcode=0x0000000b",
# modeling init dying to SIGSEGV (signal 11) rather than choosing exit
# 42: same panic message, same "Run /init as init process" line
# immediately above it (init handoff genuinely happened), but the low
# byte now carries a signal number instead of being zero. This is
# exactly the case tools/linux-usercheck's own docstring warns about --
# "a fatal signal would put its number in the byte below" -- and the
# fixture this module's brief requires: proof that a fatal-signal death
# does not pass as the chosen exit.
SYNTHETIC_FATAL_SIGNAL_TRANSCRIPT = """\
Linux version 7.2.0-rc7 (jamesb@521d96223a7d) (mips64-linux-gnuabi64-gcc (Debian \
14.2.0-13) 14.2.0, GNU ld (GNU Binutils for Debian) 2.44) #12 SMP Wed Aug 12 03:51:50 \
UTC 2026\r
Discovered 1 cpus on 1 nodes\r
************** Topology ********************\r
    00 \r
00  10 \r
printk: legacy bootconsole [early0] enabled\r
CPU0 revision is: 00000900 (R10000)\r
FPU revision is: 00000900\r
IP27: Running on node 0.\r
Node 0 has a primary CPU.\r
Node 0 has no secondary CPU.\r
Machine is in M mode.\r
Initrd not found or empty - disabling initrd\r
REPLICATION: ON nasid 0, ktext from nasid 0, kdata from nasid 0\r
Primary instruction cache 32kB, VIPT, 2-way, linesize 64 bytes.\r
Primary data cache 32kB, 2-way, VIPT, no aliases, linesize 32 bytes\r
Unified secondary cache 1024kB 2-way, linesize 128 bytes.\r
Zone ranges:\r
  Normal   [mem 0x0000000000000000-0x000000001bffffff]\r
Movable zone start for each node\r
Early memory node ranges\r
  node   0: [mem 0x0000000000000000-0x0000000003ffffff]\r
  node   0: [mem 0x0000000008000000-0x000000000bffffff]\r
  node   0: [mem 0x0000000010000000-0x0000000013ffffff]\r
  node   0: [mem 0x0000000018000000-0x000000001bffffff]\r
Initmem setup node 0 [mem 0x0000000000000000-0x000000001bffffff]\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
percpu: Embedded 19 pages/cpu s37536 r8192 d32096 u77824\r
CPU 0 clock is 180MHz.\r
printk: log buffer data + meta data: 32768 + 114688 = 147456 bytes\r
Dentry cache hash table entries: 32768 (order: 6, 262144 bytes, linear)\r
Inode-cache hash table entries: 16384 (order: 5, 131072 bytes, linear)\r
Fallback order for Node 0: 0 \r
Built 1 zonelists, mobility grouping on.  Total pages: 65536\r
Policy zone: Normal\r
mem auto-init: stack:all(zero), heap alloc:off, heap free:off\r
SLUB: HWalign=128, Order=0-3, MinObjects=0, CPUs=1, Nodes=1\r
rcu: Hierarchical RCU implementation.\r
rcu: 	RCU event tracing is enabled.\r
rcu: 	RCU restricting CPUs from NR_CPUS=64 to nr_cpu_ids=1.\r
rcu: RCU calculated value of scheduler-enlistment delay is 100 jiffies.\r
rcu: Adjusting geometry for rcu_fanout_leaf=16, nr_cpu_ids=1\r
NR_IRQS: 256\r
rcu: srcu_init: Setting srcu_struct sizes based on contention.\r
clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: \
1911260446275000 ns\r
clocksource: HUB-RT: mask: 0xfffffffffffff max_cycles: 0x127350b88, max_idle_ns: \
1763180808480 ns\r
sched_clock: 52 bits at 1250kHz, resolution 800ns, wraps every 4398046510800ns\r
Calibrating delay loop... 661.50 BogoMIPS (lpj=330752)\r
pid_max: default: 32768 minimum: 301\r
Mount-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
Mountpoint-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
VFS: Finished mounting rootfs on nullfs\r
rcu: Hierarchical SRCU implementation.\r
rcu: 	Max phase no-delay instances is 400.\r
smp: Bringing up secondary CPUs ...\r
smp: Brought up 1 node, 1 CPU\r
Memory: 240840K/262144K available (9086K kernel code, 485K rwdata, 1972K rodata, 424K \
init, 188K bss, 19976K reserved, 0K cma-reserved)\r
posixtimers hash table entries: 512 (order: 1, 8192 bytes, linear)\r
futex hash table entries: 256 (32768 bytes on 1 NUMA nodes, total 32 KiB, linear).\r
NET: Registered PF_NETLINK/PF_ROUTE protocol family\r
thermal_sys: Registered thermal governor 'step_wise'\r
xtalk:n0/8 bridge widget\r
raid6: using intx1 recovery algorithm\r
raid6: int64x1  gen()  1333 MB/s\r
raid6: int64x2  gen()  1710 MB/s\r
raid6: int64x4  gen()  1551 MB/s\r
raid6: int64x8  gen()   695 MB/s\r
raid6: using algorithm int64x2 gen() 1710 MB/s\r
raid6: .... xor() 904 MB/s, rmw enabled\r
SCSI subsystem initialized\r
pps_core: LinuxPPS API ver. 1 registered\r
pps_core: Software ver. 5.3.6 - Copyright 2005-2007 Rodolfo Giometti <giometti@linux.it>\r
PTP clock support registered\r
clocksource: Switched to clocksource HUB-RT\r
NET: Registered PF_INET protocol family\r
IP idents hash table entries: 4096 (order: 3, 32768 bytes, linear)\r
tcp_listen_portaddr_hash hash table entries: 256 (order: 0, 4096 bytes, linear)\r
Table-perturb hash table entries: 65536 (order: 6, 262144 bytes, linear)\r
TCP established hash table entries: 2048 (order: 2, 16384 bytes, linear)\r
TCP bind hash table entries: 2048 (order: 4, 65536 bytes, linear)\r
TCP: Hash tables configured (established 2048 bind 2048)\r
UDP hash table entries: 256 (order: 2, 16384 bytes, linear)\r
NET: Registered PF_UNIX/PF_LOCAL protocol family\r
RPC: Registered named UNIX socket transport module.\r
RPC: Registered udp transport module.\r
RPC: Registered tcp transport module.\r
RPC: Registered tcp-with-tls transport module.\r
RPC: Registered tcp NFSv4.1 backchannel transport module.\r
PCI: CLS 0 bytes, default 128\r
Initialise system trusted keyrings\r
workingset: timestamp_bits=56 (anon: 51) max_order=16 bucket_order=0 (anon: 0)\r
Key type asymmetric registered\r
Asymmetric key parser 'x509' registered\r
Block layer SCSI generic (bsg) driver version 0.4 loaded (major 251)\r
io scheduler mq-deadline registered\r
io scheduler kyber registered\r
xor: measuring software checksum speed\r
   32regs_prefetch :  1095 MB/sec\r
   32regs          :  1734 MB/sec\r
   8regs_prefetch  :  1438 MB/sec\r
   8regs           :  1718 MB/sec\r
xor: using function: 32regs (1734 MB/sec)\r
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled\r
loop: module loaded\r
st: Version 20160209, fixed bufsize 32768, s/g segs 256\r
Driver for 1-wire Dallas network protocol.\r
w1_master_driver w1_bus_master1: Attaching one wire slave 0b.060504030201 crc 36\r
NET: Registered PF_INET6 protocol family\r
Segment Routing with IPv6\r
In-situ OAM (IOAM) with IPv6\r
NET: Registered PF_PACKET protocol family\r
PFKEY is deprecated and scheduled to be removed in 2027, please contact the netdev \
mailing list\r
NET: Registered PF_KEY protocol family\r
xtalk-bridge xtalk-bridge.1.auto: PCI host bridge to bus 0000:00\r
pci_bus 0000:00: root bus resource [mem 0x08200000-0x08ffffff]\r
pci_bus 0000:00: root bus resource [io  0x8200000-0x8ffffff]\r
pci_bus 0000:00: root bus resource [bus 00-ff]\r
pci 0000:00:00.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:00.0: BAR 0 [io  0x8200000-0x82000ff]\r
pci 0000:00:00.0: BAR 1 [mem 0x08200000-0x082000ff]\r
pci 0000:00:01.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:01.0: BAR 0 [io  0x8400000-0x84000ff]\r
pci 0000:00:01.0: BAR 1 [mem 0x08400000-0x084000ff]\r
pci 0000:00:02.0: [10a9:0003] type 00 class 0xff0000 conventional PCI endpoint\r
pci 0000:00:02.0: BAR 0 [mem 0x08600000-0x086fffff]\r
qla1280: QLA1040 found on PCI bus 0, dev 0\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:00.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
qla1280: QLA1040 found on PCI bus 0, dev 1\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:01.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
Loading compiled-in X.509 certificates\r
Demotion targets for Node 0: null\r
Warning: unable to open an initial console.\r
Freeing unused kernel image (initmem) memory: 424K\r
This architecture does not have kernel memory protection.\r
Run /init as init process\r
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b\r
---[ end Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b ]---\r
"""

# REAL, truncated: the same one-CPU capture's own prefix, cut right
# after its last line before init handoff ("This architecture does not
# have kernel memory protection."). Models a run that hung or was
# killed by timeout before userspace ever ran -- every line kept is
# unedited real capture text; nothing after the cut is included.
TRUNCATED_TRANSCRIPT = """\
Linux version 7.2.0-rc7 (jamesb@521d96223a7d) (mips64-linux-gnuabi64-gcc (Debian \
14.2.0-13) 14.2.0, GNU ld (GNU Binutils for Debian) 2.44) #12 SMP Wed Aug 12 03:51:50 \
UTC 2026\r
Discovered 1 cpus on 1 nodes\r
************** Topology ********************\r
    00 \r
00  10 \r
printk: legacy bootconsole [early0] enabled\r
CPU0 revision is: 00000900 (R10000)\r
FPU revision is: 00000900\r
IP27: Running on node 0.\r
Node 0 has a primary CPU.\r
Node 0 has no secondary CPU.\r
Machine is in M mode.\r
Initrd not found or empty - disabling initrd\r
REPLICATION: ON nasid 0, ktext from nasid 0, kdata from nasid 0\r
Primary instruction cache 32kB, VIPT, 2-way, linesize 64 bytes.\r
Primary data cache 32kB, 2-way, VIPT, no aliases, linesize 32 bytes\r
Unified secondary cache 1024kB 2-way, linesize 128 bytes.\r
Zone ranges:\r
  Normal   [mem 0x0000000000000000-0x000000001bffffff]\r
Movable zone start for each node\r
Early memory node ranges\r
  node   0: [mem 0x0000000000000000-0x0000000003ffffff]\r
  node   0: [mem 0x0000000008000000-0x000000000bffffff]\r
  node   0: [mem 0x0000000010000000-0x0000000013ffffff]\r
  node   0: [mem 0x0000000018000000-0x000000001bffffff]\r
Initmem setup node 0 [mem 0x0000000000000000-0x000000001bffffff]\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
On node 0, zone Normal: 16384 pages in unavailable ranges\r
percpu: Embedded 19 pages/cpu s37536 r8192 d32096 u77824\r
CPU 0 clock is 180MHz.\r
printk: log buffer data + meta data: 32768 + 114688 = 147456 bytes\r
Dentry cache hash table entries: 32768 (order: 6, 262144 bytes, linear)\r
Inode-cache hash table entries: 16384 (order: 5, 131072 bytes, linear)\r
Fallback order for Node 0: 0 \r
Built 1 zonelists, mobility grouping on.  Total pages: 65536\r
Policy zone: Normal\r
mem auto-init: stack:all(zero), heap alloc:off, heap free:off\r
SLUB: HWalign=128, Order=0-3, MinObjects=0, CPUs=1, Nodes=1\r
rcu: Hierarchical RCU implementation.\r
rcu: 	RCU event tracing is enabled.\r
rcu: 	RCU restricting CPUs from NR_CPUS=64 to nr_cpu_ids=1.\r
rcu: RCU calculated value of scheduler-enlistment delay is 100 jiffies.\r
rcu: Adjusting geometry for rcu_fanout_leaf=16, nr_cpu_ids=1\r
NR_IRQS: 256\r
rcu: srcu_init: Setting srcu_struct sizes based on contention.\r
clocksource: jiffies: mask: 0xffffffff max_cycles: 0xffffffff, max_idle_ns: \
1911260446275000 ns\r
clocksource: HUB-RT: mask: 0xfffffffffffff max_cycles: 0x127350b88, max_idle_ns: \
1763180808480 ns\r
sched_clock: 52 bits at 1250kHz, resolution 800ns, wraps every 4398046510800ns\r
Calibrating delay loop... 661.50 BogoMIPS (lpj=330752)\r
pid_max: default: 32768 minimum: 301\r
Mount-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
Mountpoint-cache hash table entries: 512 (order: 0, 4096 bytes, linear)\r
VFS: Finished mounting rootfs on nullfs\r
rcu: Hierarchical SRCU implementation.\r
rcu: 	Max phase no-delay instances is 400.\r
smp: Bringing up secondary CPUs ...\r
smp: Brought up 1 node, 1 CPU\r
Memory: 240840K/262144K available (9086K kernel code, 485K rwdata, 1972K rodata, 424K \
init, 188K bss, 19976K reserved, 0K cma-reserved)\r
posixtimers hash table entries: 512 (order: 1, 8192 bytes, linear)\r
futex hash table entries: 256 (32768 bytes on 1 NUMA nodes, total 32 KiB, linear).\r
NET: Registered PF_NETLINK/PF_ROUTE protocol family\r
thermal_sys: Registered thermal governor 'step_wise'\r
xtalk:n0/8 bridge widget\r
raid6: using intx1 recovery algorithm\r
raid6: int64x1  gen()  1333 MB/s\r
raid6: int64x2  gen()  1710 MB/s\r
raid6: int64x4  gen()  1551 MB/s\r
raid6: int64x8  gen()   695 MB/s\r
raid6: using algorithm int64x2 gen() 1710 MB/s\r
raid6: .... xor() 904 MB/s, rmw enabled\r
SCSI subsystem initialized\r
pps_core: LinuxPPS API ver. 1 registered\r
pps_core: Software ver. 5.3.6 - Copyright 2005-2007 Rodolfo Giometti <giometti@linux.it>\r
PTP clock support registered\r
clocksource: Switched to clocksource HUB-RT\r
NET: Registered PF_INET protocol family\r
IP idents hash table entries: 4096 (order: 3, 32768 bytes, linear)\r
tcp_listen_portaddr_hash hash table entries: 256 (order: 0, 4096 bytes, linear)\r
Table-perturb hash table entries: 65536 (order: 6, 262144 bytes, linear)\r
TCP established hash table entries: 2048 (order: 2, 16384 bytes, linear)\r
TCP bind hash table entries: 2048 (order: 4, 65536 bytes, linear)\r
TCP: Hash tables configured (established 2048 bind 2048)\r
UDP hash table entries: 256 (order: 2, 16384 bytes, linear)\r
NET: Registered PF_UNIX/PF_LOCAL protocol family\r
RPC: Registered named UNIX socket transport module.\r
RPC: Registered udp transport module.\r
RPC: Registered tcp transport module.\r
RPC: Registered tcp-with-tls transport module.\r
RPC: Registered tcp NFSv4.1 backchannel transport module.\r
PCI: CLS 0 bytes, default 128\r
Initialise system trusted keyrings\r
workingset: timestamp_bits=56 (anon: 51) max_order=16 bucket_order=0 (anon: 0)\r
Key type asymmetric registered\r
Asymmetric key parser 'x509' registered\r
Block layer SCSI generic (bsg) driver version 0.4 loaded (major 251)\r
io scheduler mq-deadline registered\r
io scheduler kyber registered\r
xor: measuring software checksum speed\r
   32regs_prefetch :  1095 MB/sec\r
   32regs          :  1734 MB/sec\r
   8regs_prefetch  :  1438 MB/sec\r
   8regs           :  1718 MB/sec\r
xor: using function: 32regs (1734 MB/sec)\r
Serial: 8250/16550 driver, 4 ports, IRQ sharing enabled\r
loop: module loaded\r
st: Version 20160209, fixed bufsize 32768, s/g segs 256\r
Driver for 1-wire Dallas network protocol.\r
w1_master_driver w1_bus_master1: Attaching one wire slave 0b.060504030201 crc 36\r
NET: Registered PF_INET6 protocol family\r
Segment Routing with IPv6\r
In-situ OAM (IOAM) with IPv6\r
NET: Registered PF_PACKET protocol family\r
PFKEY is deprecated and scheduled to be removed in 2027, please contact the netdev \
mailing list\r
NET: Registered PF_KEY protocol family\r
xtalk-bridge xtalk-bridge.1.auto: PCI host bridge to bus 0000:00\r
pci_bus 0000:00: root bus resource [mem 0x08200000-0x08ffffff]\r
pci_bus 0000:00: root bus resource [io  0x8200000-0x8ffffff]\r
pci_bus 0000:00: root bus resource [bus 00-ff]\r
pci 0000:00:00.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:00.0: BAR 0 [io  0x8200000-0x82000ff]\r
pci 0000:00:00.0: BAR 1 [mem 0x08200000-0x082000ff]\r
pci 0000:00:01.0: [1077:1020] type 00 class 0x010000 conventional PCI endpoint\r
pci 0000:00:01.0: BAR 0 [io  0x8400000-0x84000ff]\r
pci 0000:00:01.0: BAR 1 [mem 0x08400000-0x084000ff]\r
pci 0000:00:02.0: [10a9:0003] type 00 class 0xff0000 conventional PCI endpoint\r
pci 0000:00:02.0: BAR 0 [mem 0x08600000-0x086fffff]\r
qla1280: QLA1040 found on PCI bus 0, dev 0\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:00.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
qla1280: QLA1040 found on PCI bus 0, dev 1\r
qla1280: Wrong product ID = 0x4953,0x5020,0x2020,0x0\r
qla1280 0000:00:01.0: Direct firmware load for qlogic/1040.bin failed with error -2\r
Failed to load image "qlogic/1040.bin" err -2\r
scsi(0): initialize: pci probe failed!\r
qla1x160: Failed to initialize adapter\r
Loading compiled-in X.509 certificates\r
Demotion targets for Node 0: null\r
Warning: unable to open an initial console.\r
Freeing unused kernel image (initmem) memory: 424K\r
This architecture does not have kernel memory protection.\r
"""


class MissingAssertionsTest(unittest.TestCase):

    def test_real_one_cpu_pass_transcript_passes(self):
        self.assertEqual(
            missing_assertions(REAL_ONE_CPU_PASS_TRANSCRIPT, 1), [])

    def test_synthetic_two_cpu_pass_transcript_passes(self):
        self.assertEqual(
            missing_assertions(SYNTHETIC_TWO_CPU_PASS_TRANSCRIPT, 2), [])

    def test_fatal_signal_transcript_fails_even_though_init_ran(self):
        # The core correctness property this port's brief requires: a
        # fatal-signal kernel death must not pass as the chosen exit,
        # even though init handoff genuinely happened.
        self.assertIn('Run /init as init process',
                      SYNTHETIC_FATAL_SIGNAL_TRANSCRIPT)
        missing = missing_assertions(SYNTHETIC_FATAL_SIGNAL_TRANSCRIPT, 1)
        self.assertNotEqual(missing, [])
        self.assertIn('userspace exit status', missing)
        self.assertNotIn('init handoff', missing)

    def test_truncated_transcript_fails_missing_everything_after_the_cut(self):
        missing = missing_assertions(TRUNCATED_TRANSCRIPT, 1)
        self.assertIn('init handoff', missing)
        self.assertIn('userspace exit status', missing)

    def test_empty_transcript_fails_missing_everything(self):
        missing = missing_assertions('', 1)
        self.assertEqual(
            missing,
            ['cpu discovery', 'init handoff', 'userspace exit status'])

    def test_one_cpu_pass_does_not_satisfy_two_cpu_checks(self):
        # Cross-check: the real one-CPU transcript, judged as a two-CPU
        # run, must fail on the CPU-count-specific markers -- proves
        # those checks are not vacuously true, and that the exit-status
        # marker really is CPU-count-specific too (0x2a00 != 0x2b00).
        missing = missing_assertions(REAL_ONE_CPU_PASS_TRANSCRIPT, 2)
        self.assertIn('cpu discovery', missing)
        self.assertIn('secondary CPU execution', missing)
        self.assertIn('two-CPU scheduler bring-up', missing)
        self.assertIn('userspace exit status', missing)

    def test_two_cpu_pass_does_not_satisfy_one_cpu_exit_status(self):
        # And the reverse: the two-CPU transcript's exit code (0x2b00)
        # must not satisfy the one-CPU check (0x2a00).
        missing = missing_assertions(SYNTHETIC_TWO_CPU_PASS_TRANSCRIPT, 1)
        self.assertIn('userspace exit status', missing)

    def test_rejects_unsupported_cpu_count(self):
        with self.assertRaises(ValueError):
            missing_assertions(REAL_ONE_CPU_PASS_TRANSCRIPT, 4)

    def test_a_near_miss_exit_code_does_not_satisfy(self):
        # A value that shares every digit with the wanted one, just not
        # in the right place (0x0000002a rather than 0x00002a00), proves
        # this is a match against the full literal, not a lucky
        # substring or a digit-set comparison.
        near_miss = REAL_ONE_CPU_PASS_TRANSCRIPT.replace(
            'exitcode=0x00002a00', 'exitcode=0x0000002a')
        self.assertNotIn('exitcode=0x00002a00', near_miss)
        missing = missing_assertions(near_miss, 1)
        self.assertIn('userspace exit status', missing)


if __name__ == '__main__':
    unittest.main()
