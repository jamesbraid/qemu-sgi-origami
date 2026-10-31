.. SPDX-License-Identifier: BSD-3-Clause

MACE revision-2 untimed I2C test profile
======================================

``sgi-mace,i2c-untimed-test-profile=on`` enables the bounded I2C register
profile only under qtest. The default remains disabled. No O2 endpoint,
address map beyond the one register bank, or production wiring is selected.

The profile implements the reviewed functional contract from Origami's
``docs/datasheets/o2-mace-i2c.md``, SHA-256
``6ad9b4d5bde2ac67a577e9746df7a55645ea6764299df4773cfb7ad737597dff``.
The exact source-free clearance is SHA-256
``339bf848c67f21a2127e3a342b027c8f2ea971c5b0e5c40be457cbd439594734``.
These identify the implementation input, not physical validation.

The four native big-endian 64-bit slots are configuration at offsets 0 and
8, CONTROL at 16, and DATA at 24 within MACE's 0x330000 peripheral aperture.
Other widths, unaligned accesses and undefined offsets fail without register
or endpoint side effects. Active sampled-input reads and physical overrides
also fail. Endpoint NACK, protocol misuse and unsupported host capabilities
remain distinct outcomes. An address NACK releases ownership with captured
HOLD zero and retains the failed selection with HOLD one. Neither outcome
retries an address implicitly.

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
