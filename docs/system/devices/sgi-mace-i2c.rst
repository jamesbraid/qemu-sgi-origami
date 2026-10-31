.. SPDX-License-Identifier: BSD-3-Clause

MACE I2C test profile
=====================

``sgi-mace,i2c-untimed-test-profile=on`` turns on a register model of the
SGI O2 MACE I/O ASIC's I2C master, under QTest only. No machine uses it.
The SGI MACE page describes the registers, the byte-at-once transfer
model, its limits and its tests: :ref:`sgi-mace-i2c`.
