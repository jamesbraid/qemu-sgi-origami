.. SPDX-License-Identifier: GPL-2.0-or-later

R10000 processor data-cache transactions
=======================================

The optional ``r10k-data-cache`` CPU property supports ordinary data loads
and stores using CCA 3 and CCA 5 with an explicitly attached external agent.
The Octane machine does not enable this property. This interface specifies
no HEART behavior, board DMA path, duplicate tags or board reset modes.

The hardware reference is the public *MIPS R10000 Microprocessor User's
Manual*, version 2.0, 007-2490-001, printed pages 48--58, 113, 127--130 and
141--148. The processor contract in Origami at ``b4e4f7a87f`` and its
independent evidence review clear this data-cache scope. This implementation
still requires independent code review. It establishes no R12000 or R14000
behavior and adds no functional ECC contract.

Requests and storage
--------------------

CCA is retained from TLB EntryLo C, Config K0 and xkphys. The R10000 kseg0
translation takes CCA from current Config K0 rather than the generic MIPS
segment-control value initialized at reset. Changing K0 flushes translated
pages, so an existing TCG TLB entry cannot retain the former algorithm.
These changes select the R10000 model by name and do not change the other
R10000-family models.

A primary miss satisfied by the secondary cache creates no external read.
A secondary miss creates a noncoherent read for CCA 3, a shared read for a
CCA 5 load, or an exclusive read for a CCA 5 store. A store to Shared creates
an upgrade, including the exceptional CCA 3 Shared case. An agent-free CCA 3
experiment retains its earlier backing-memory path. CCA 5 requires an agent
and has no backing-memory fallback.

The existing diagnostic arrays hold resident data and tags. Shared,
CleanExclusive and DirtyExclusive have distinct states. Primary changes
are collected before outgoing data is copied. Both 64-byte and 128-byte
secondary blocks are selectable through ``r10k-scache-line-size``. This
property supplies synthetic geometry, not an Octane board default.

A request retains its identity, original address, aligned block, size,
selected tag and way, CCA, cause and former state. A secondary victim is
invalidated before the new read issues. Received data remains separate
from resident storage until ACK. Read ACK requires data. Upgrade ACK can
omit data only while the Shared copy remains valid. ERR on a cached block
read or upgrade raises Bus Error for loads and stores. Posted uncached
store behavior is unchanged. NACK restarts the instruction's cache lookup.

Transport and synchronization
-----------------------------

``include/hw/mips/r10k-coherence.h`` declares the agent operations. Every
operation requires the BQL. The System-request MemoryRegion uses an
8-byte big-endian read with the physical address below bit 48 and the
request kind starting at bit 48. It preserves the maintained branch's
request values 0--4 and its index-width field starting at bit 52. New
transaction notifications occupy subsequent request values. Requester
identity travels in ``MemTxAttrs.requester_id`` and CCA in ``mips_cca``.
The notification read is neither data delivery nor an ACK.

The adapter obtains a request snapshot, admits it for issue, and supplies
data and completion separately. All snapshots and data delivery copy the
whole block. No adapter borrows a pointer into a cache entry or transaction
record. The implementation has one processor request, one independent
outgoing write buffer and two admitted external requests. It rejects issue
while an external request or outgoing write conflicts with that block.
External state replies follow admission order. Completion is also FIFO in
this implementation.

Ordinary cache MMIO is protected by the BQL. CACHE and SYNC helpers acquire
it explicitly. A delayed transaction halts the CPU and restores the
instruction for replay. QEMU's CPU exit cleanup releases the BQL. Response
delivery clears the halt and kicks that CPU. The CPU does not wait while
holding the lock required by response delivery. Maskable interrupts wait
until the pending processor transaction has been consumed.

An outgoing block write owns a full byte copy before its resident entry
can be reused. Starting transfer excludes a conflicting external request
until the adapter consumes or copies all bytes. That consumption event
releases storage and is not a processor ACK or a globally-performed signal.
Before transfer, an intervention converts the write to independently owned
DirtyExclusive response data. An invalidate deletes it without data.

An ordinary dirty shared intervention returns Shared data, and the adapter
must write it to memory. An exclusive intervention returns DirtyExclusive
data and invalidates the CPU copy. Invalidate returns no data and logs the
abnormal loss of an exclusive copy. Every resident primary data subset is
updated. SYNC waits for the separate globally-performed indication and
outstanding external/write transfers. It does not flush dirty resident data.

Synthetic test device
---------------------

``r10k-test-agent`` exists only with ``CONFIG_TEST_DEVICES`` and MIPS. It
attaches explicitly to CPU 0, owns a small backing RAM region, and exposes
host QOM controls for issuing, replying, intervening and consuming copied
data. It never attaches to an Octane DMA device. Its controls are not guest
registers. It does not automatically manufacture successful coherence
responses from RAM accesses.

``tests/functional/mips64/test_r10000_coherence.py`` uses original instruction
fixtures to check the request matrix, delayed ACK, response states, NACK,
Bus Error, Shared upgrades, ordinary interventions, conflicts before issue,
an invalidated outstanding upgrade, FIFO completion, dirty subsets,
synonyms, both line sizes, outgoing storage after cache-entry reuse, SYNC
and explicit migration rejection. Each fixture initializes the cache tags
it uses. Deterministic empty cold-reset arrays remain a QEMU approximation.

Limits
------

Agent-free cache migration records the configured secondary line size in
version 2 of the functional-data-cache subsection. A destination with a
different line size rejects it before execution resumes. Version 1 streams
are rejected because they carry no geometry. Matching 64-byte and 128-byte
migration and both mismatch directions have dedicated tests.

Migration with an attached agent is rejected, even when quiescent. Pending
CPU transactions also reject migration. Supporting migration requires a
contract for both processor and adapter state. The synthetic agent is
marked unmigratable.

Instruction-fetch coherence, LL/SC coherence, unaligned instruction replay,
speculation, prefetch, eliminate notifications, uncached-store buffering,
cycle timing and interrupted-reset recovery are outside this acceptance.
No board mode inputs are inferred from the processor manual. The existing
branch's diagnostic and ECC code remains separate from this clearance.
Software requiring any excluded behavior must not enable this experiment.
