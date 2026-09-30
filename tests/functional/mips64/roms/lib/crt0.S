/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Reset leaves memory and general registers undefined. Clear ERL for TLB and
 * cache tests, but keep BEV so exceptions use the ROM vectors. Tests that clear
 * BEV use the vector copies placed at 0x80000000 below.
 */

	.set	noreorder

	.section .start, "ax"
	.globl	_start
	.ent	_start
_start:
	/* Kernel mode, interrupts off, 32-bit addressing, ROM vectors. */
	li	$8, (1 << 22)			/* Status.BEV */
	mtc0	$8, $12				/* Status */
	mtc0	$0, $13				/* Cause */
	mtc0	$0, $9				/* Count */
	li	$8, -1
	/* Compare: no timer interrupt pending. */
	mtc0	$8, $11
	/* Wired: no reserved TLB entries. */
	mtc0	$0, $6
	mtc0	$0, $4				/* Context */
	mtc0	$0, $5				/* PageMask: 4 Kbyte pages */
	/* EntryHi: virtual page 0, address space 0. */
	dmtc0	$0, $10

	/* Copy the initial contents of writable data out of the ROM image. */
	lui	$8, %hi(_data_load)
	daddiu	$8, $8, %lo(_data_load)
	lui	$9, %hi(_data_start)
	daddiu	$9, $9, %lo(_data_start)
	lui	$10, %hi(_data_end)
	daddiu	$10, $10, %lo(_data_end)
5:	sltu	$11, $9, $10
	beqz	$11, 6f
	 nop
	ld	$12, 0($8)
	sd	$12, 0($9)
	daddiu	$8, $8, 8
	daddiu	$9, $9, 8
	b	5b
	 nop
6:

	/* Zero the remaining writable data. */
	lui	$8, %hi(_bss_start)
	daddiu	$8, $8, %lo(_bss_start)
	lui	$9, %hi(_bss_end)
	daddiu	$9, $9, %lo(_bss_end)
1:	sltu	$10, $8, $9
	beqz	$10, 2f
	 nop
	sd	$0, 0($8)
	daddiu	$8, $8, 8
	b	1b
	 nop
2:

	/* BEV-clear vectors go at 0x80000000 before caches are enabled. */
	lui	$8, %hi(_tramp_start)
	daddiu	$8, $8, %lo(_tramp_start)
	lui	$9, %hi(_tramp_end)
	daddiu	$9, $9, %lo(_tramp_end)
	lui	$10, 0x8000			/* 0xffffffff80000000 */
3:	sltu	$11, $8, $9
	beqz	$11, 4f
	 nop
	ld	$12, 0($8)
	sd	$12, 0($10)
	daddiu	$8, $8, 8
	daddiu	$10, $10, 8
	b	3b
	 nop
4:

	/* Node-local stacks need a separate 4 KiB slice per processor. Read the
	 * local Hub's processor number; EBase is not readable on R10000. A
	 * single-processor run uses offset zero.
	 */
	lui	$29, %hi(_stack_top)
	daddiu	$29, $29, %lo(_stack_top)
	/* Only R10000 reads the Hub CPU number; other models run single-CPU. */
	mfc0	$9, $15				/* PRId */
	srl	$9, $9, 8
	andi	$9, $9, 0xff			/* implementation number */
	li	$10, 0x09			/* the R10000 family's */
	bne	$9, $10, 7f
	 move	$9, $0				/* others: processor zero */
	/*
	 * Temporarily enable KX for the Hub read, then restore Status. Keep CP0
	 * hazard spacing explicit; do not rely on dli expansion for the delay.
	 */
	mfc0	$10, $12			/* Status */
	ori	$11, $10, (1 << 7)		/* Status.KX */
	mtc0	$11, $12
	dli	$8, 0x9200000001000020		/* own Hub, processor number */
	ld	$9, 0($8)
	mtc0	$10, $12			/* Status back as it was */
7:	andi	$9, $9, 1			/* which of the node's two */
	sll	$9, $9, 12			/* 4 KiB apart */
	dsubu	$29, $29, $9
	move	$28, $0
	move	$30, $0
	lui	$8, %hi(rom_main)
	daddiu	$8, $8, %lo(rom_main)
	jalr	$8
	 nop

	/* Returning from the test means every assertion in it held. */
	lui	$8, %hi(rom_pass)
	daddiu	$8, $8, %lo(rom_pass)
	jr	$8
	 nop
	.end	_start
