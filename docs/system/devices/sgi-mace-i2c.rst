.. SPDX-License-Identifier: BSD-3-Clause

MACE revision-2 untimed I2C test profile
======================================

``sgi-mace,i2c-untimed-test-profile=on`` enables the bounded I2C register
profile only under qtest. The default remains disabled. No O2 endpoint,
address map beyond the one register bank, or production wiring is selected.

The profile implements the reviewed functional contract from Origami's
``docs/datasheets/o2-mace-i2c.md``, SHA-256
``6f980a5c1209fdb276dc84e3f1e29b5f982b1b54650d430b289d1e63b3509417``.
The exact source-free clearance is SHA-256
``bfc92d0a323acca0dd8d58f19d7bd1dbfb2210d7ce6f9c7c70311d4b2abc1bd4``.
These identify the implementation input, not physical validation.

The four native big-endian 64-bit slots are configuration at offsets 0 and
8, CONTROL at 16, and DATA at 24 within MACE's 0x330000 peripheral aperture.
Other widths, unaligned accesses and undefined offsets fail without register
or endpoint side effects. Active sampled-input reads and physical overrides
also fail. Endpoint NACK, protocol misuse and unsupported host capabilities
remain distinct outcomes.

DATA synchronously transfers one actual endpoint byte. A receive returns
that same byte without prefetch. Held receives retain a logical ACK decision
until continuation, final receive CONTROL, force idle or reset. Termination
sends exactly one NACK before STOP. Transmit CONTROL updates preserve the
addressed transfer, so held DATA remains payload. Configuration bit 0 uses
the reviewed local reset-request edge approximation.

The private ``i2c-wire`` bus admits at most 32 cold-plug endpoints and one
resolved master. An endpoint must explicitly opt into the synchronous
untimed profile and include I2C slave migration state. Its nonzero profile
key identifies immutable endpoint configuration. Migration compares the
ordered endpoint types, addresses and keys, and restores held ownership,
endpoint data and a pending receive ACK without replaying callbacks.
Incompatible source migration fails before changing the accepted prefix.

The synthetic endpoint in ``tests/qtest/fixtures/sgi-mace-i2c.c`` exists only
in test builds. Its unequal receive bytes and event counters distinguish
same-byte receive, finalization and callback order. It is not a fitted O2
camera, decoder or other peripheral.

Full MACE I2C completion and ordinary O2 admission remain unfinished.
Repeated START needs an independently established MACE request mechanism.
Physical reset and override behavior, active sampled inputs, stretching,
arbitration/loss, standard/fast wire timing, physical access/reply phases,
full hardware error clearing, multiplicity/aliases and fitted board topology
also remain required. The register tests establish the bounded functional
contract only. No guest or physical-system acceptance follows from them.
