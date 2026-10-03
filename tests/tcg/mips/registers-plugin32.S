/* SPDX-License-Identifier: GPL-2.0-or-later */

.set noreorder
    .text
    .globl _start
_start:
    li      $4, 0
    li      $5, 0x1234567
    li      $6, 0x2345678
    li      $7, 0x3456789
    li      $28, 0x456789a
    li      $18, 0x56789ab
    li      $19, 0x6789abc
    li      $3, 0x789abcd
_exit:
    li      $2, 4001
    syscall
