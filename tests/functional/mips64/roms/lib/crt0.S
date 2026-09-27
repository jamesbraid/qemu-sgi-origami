/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Startup for the bare-metal test ROMs.
 *
 * The processor arrives here from the reset vector with the Status register
 * holding only the boot-exception-vector and error-level bits, all general
 * registers undefined and no memory initialised. This code establishes the
 * one state every test starts from, then calls the test.
 *
 * Error level is cleared because while it is set the user segment is neither
 * translated nor cached, which would hide every TLB behaviour the suite
 * exists to check. The boot-exception-vector bit is kept set so exceptions
 * reach the entry points this ROM carries; the tests that care about the
 * other setting clear the bit themselves and use the copies this code places
 * in low memory.
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

	/*
	 * Place the second copy of the exception entry points at 0x80000000,
	 * where the processor looks when the boot-exception-vector bit is
	 * clear. Writing them through the cached compatibility segment is safe
	 * only because no cache is enabled yet; a ROM that turned the caches on
	 * would have to write them back before jumping to them.
	 */
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

	/*
	 * Stack, then the test itself.
	 *
		 * Every processor arrives here on a multiprocessor machine and
		 * would overwrite the others' frames. Two of the four separate
	 * themselves: this data lives at a node-local address, so the
	 * processors of each node already have their own copy of it. What is
	 * left is the two processors WITHIN a node, so each takes a slice
	 * chosen by its own number.
	 *
	 * The number comes from the Hub, because the register that carries it
	 * on later processors cannot be read here -- mfc0 from EBase raises
	 * Reserved Instruction on this one, measured. The Hub answers each
	 * reader with its own number, and the address below is the reader's
	 * OWN Hub whichever node it sits on.
	 *
	 * Inert on a single-processor run: the number is zero and the stack is
	 * exactly where it was.
	 */
	lui	$29, %hi(_stack_top)
	daddiu	$29, $29, %lo(_stack_top)
	/*
	 * Only the R10000 family is asked, because the address below carries
	 * an uncached-space attribute in two bits the R4000 reads as physical
	 * address. They place the access far beyond its physical range, so on
	 * it the load takes an address error before any test begins. The
	 * models without the attribute run this suite one processor at a
	 * time, and their number is zero without asking.
	 */
	mfc0	$9, $15				/* PRId */
	srl	$9, $9, 8
	andi	$9, $9, 0xff			/* implementation number */
	li	$10, 0x09			/* the R10000 family's */
	bne	$9, $10, 7f
	 move	$9, $0				/* others: processor zero */
	/*
	 * The Hub is only reachable through the 64-bit unmapped space, which
	 * needs 64-bit kernel addressing on -- without it this load takes an
	 * address error rather than reading anything. Turn it on for the read
	 * and put the register back exactly as it was, so every test still
	 * begins from the state it began from before.	 *
	 * The instructions between enabling it and the load happen to cover the
	 * coprocessor hazard, because loading a 64-bit constant expands to
	 * several. That is incidental rather than chosen: the emulator has no
	 * hazard, and on silicon this would be resting on an expansion length
	 * nobody picked. Shortening it wants the separation kept deliberately.
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
