/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Eight vectors funnel into one handler, which records exception state without
 * clobbering test registers. It can refill the TLB, resume at an armed address,
 * or report an unexpected exception. Resume state is consumed to prevent loops.
 */

#include "exc-offsets.h"

	.set	noreorder
	.set	noat

/*
 * Low-memory vectors need an absolute jump to reach the ROM handler.
 */
	.macro	rom_vector id
	lui	$26, %hi(rom_exc_entry)
	daddiu	$26, $26, %lo(rom_exc_entry)
	li	$27, \id
	jr	$26
	 nop
	.endm

	.section .vec.tlb, "ax"
	rom_vector (ROM_VEC_BOOTSTRAP | ROM_VEC_TLB_REFILL)

	.section .vec.xtlb, "ax"
	rom_vector (ROM_VEC_BOOTSTRAP | ROM_VEC_XTLB_REFILL)

	.section .vec.cache, "ax"
	rom_vector (ROM_VEC_BOOTSTRAP | ROM_VEC_CACHE_ERROR)

	.section .vec.gen, "ax"
	rom_vector (ROM_VEC_BOOTSTRAP | ROM_VEC_GENERAL)

	.section .tramp.tlb, "ax"
	rom_vector ROM_VEC_TLB_REFILL

	.section .tramp.xtlb, "ax"
	rom_vector ROM_VEC_XTLB_REFILL

	.section .tramp.cache, "ax"
	rom_vector ROM_VEC_CACHE_ERROR

	.section .tramp.gen, "ax"
	rom_vector ROM_VEC_GENERAL

	.text
	.globl	rom_exc_entry
	.ent	rom_exc_entry
rom_exc_entry:
	lui	$26, %hi(rom_exc)
	daddiu	$26, $26, %lo(rom_exc)
	sd	$27, EXC_VECTOR($26)

	ld	$27, EXC_COUNT($26)
	daddiu	$27, $27, 1
	sd	$27, EXC_COUNT($26)

	mfc0	$27, $13			/* Cause */
	sd	$27, EXC_CAUSE($26)
	mfc0	$27, $12			/* Status */
	sd	$27, EXC_STATUS($26)
	dmfc0	$27, $14			/* EPC */
	sd	$27, EXC_EPC($26)
	dmfc0	$27, $8				/* BadVAddr */
	sd	$27, EXC_BADVADDR($26)
	dmfc0	$27, $4				/* Context */
	sd	$27, EXC_CONTEXT($26)
	dmfc0	$27, $20			/* XContext */
	sd	$27, EXC_XCONTEXT($26)
	dmfc0	$27, $10			/* EntryHi */
	sd	$27, EXC_ENTRYHI($26)
	dmfc0	$27, $30			/* ErrorEPC */
	sd	$27, EXC_ERROREPC($26)

/*
 * Refill only on refill vectors; invalid or modified entries use the general.
 */
	ld	$27, EXC_VECTOR($26)
	andi	$27, $27, 3
	sltiu	$27, $27, ROM_VEC_CACHE_ERROR
	beqz	$27, rom_exc_resume
	 nop
	ld	$27, EXC_REFILL($26)
	beqz	$27, rom_exc_resume
	 nop

/*
 * EntryHi holds the faulting VPN; install the staged entry and retry.
 */
	ld	$27, EXC_ENTRYLO0($26)
	dmtc0	$27, $2				/* EntryLo0 */
	ld	$27, EXC_ENTRYLO1($26)
	dmtc0	$27, $3				/* EntryLo1 */
	ld	$27, EXC_PAGEMASK($26)
	mtc0	$27, $5				/* PageMask */
	tlbwr
	eret

rom_exc_resume:
	ld	$27, EXC_RESUME($26)
	beqz	$27, rom_exc_unexpected
	 nop
	sd	$0, EXC_RESUME($26)
	dmtc0	$27, $14			/* EPC */

	/*
	 * Jump return preserves LL/SC state: ERET clears the link. Clear EXL
	 * manually, then resume at EPC; staged Status is ignored on this path.
	 */
	ld	$27, EXC_JUMP($26)
	beqz	$27, 1f
	 nop
	sd	$0, EXC_JUMP($26)
	mfc0	$27, $12			/* Status */
	ori	$27, $27, 2
	xori	$27, $27, 2			/* exception level off */
	mtc0	$27, $12
	dmfc0	$27, $14			/* EPC */
	jr	$27
	 nop

	/*
	 * A user-mode test stages Status before resuming in kernel mode.
	 */
1:	ld	$27, EXC_STATUS_SET($26)
	beqz	$27, 2f
	 nop
	sd	$0, EXC_STATUS_SET($26)
	mtc0	$27, $12			/* Status */
2:	eret

rom_exc_unexpected:
	lui	$26, %hi(rom_report_unexpected_exception)
	daddiu	$26, $26, %lo(rom_report_unexpected_exception)
	jr	$26
	 nop
	.end	rom_exc_entry
