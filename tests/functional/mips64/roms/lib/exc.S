/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Exception handling for the bare-metal test ROMs.
 *
 * There are eight entry points: the four architectural ones for a TLB refill,
 * an extended-addressing TLB refill, a cache error and everything else, in
 * each of the two places the processor looks for them depending on the
 * boot-exception-vector bit of the Status register. Each entry point records
 * which one it was and funnels into one handler.
 *
 * The handler touches only the two registers the architecture reserves for
 * kernel scratch use, so a test's registers come back from an exception
 * exactly as they went in and a test can compare values across a fault. It
 * writes what it saw into a record in uncached memory and then either fills
 * the TLB and retries the access, or returns to an address the test armed
 * beforehand, or — if the test armed nothing — reports an unexpected
 * exception and stops.
 *
 * The armed address is consumed when it is used. A second unexpected fault
 * therefore reports rather than looping, which is what turns a runaway test
 * into a legible failure instead of a timeout.
 */

#include "exc-offsets.h"

	.set	noreorder
	.set	noat

/*
 * An entry point: remember which one ran, then jump to the handler. The
 * absolute jump matters for the copies in low memory, because a region-based
 * jump from there could not reach the handler in ROM.
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
	 * Filling the TLB and retrying only makes sense for the two refill
	 * entry points; an invalid or modified entry arrives at the general
	 * one, and a test that wants to see that must not have it repaired
	 * underneath it.
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
	 * The faulting virtual page number is already in EntryHi, put there
	 * when the exception was taken, so the entry the test staged completes
	 * the translation. A random write is what a refill handler does.
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
	 * A test that wants to see what taking the exception did — as opposed
	 * to what returning from it does — arms this flag along with the
	 * resume address, and the handler goes back by an ordinary jump. ERET
	 * is defined to end a load-linked sequence on its own, so a jump
	 * return is the only way a test can put a store-conditional after a
	 * fault and observe what the entry alone did to the link. What ERET
	 * would have done to the exception level is done by hand, with the
	 * set-then-flip pair because the two scratch registers leave no room
	 * for a mask; the staged Status word is not consulted on this path.
	 * The resume address was just parked in the exception program
	 * counter, so it is read back from there for the jump.
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
	 * A test that faulted from user mode cannot be resumed at a kernel
	 * address without also putting the processor back in kernel mode, so it
	 * may stage the Status register to install on the way out. Like the
	 * resume address, it is consumed when it is used.
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
