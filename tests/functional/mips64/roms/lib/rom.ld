/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Layout for the bare-metal test ROMs.
 *
 * Code and read-only data are linked for the reset vector: physical
 * 0x1fc00000, seen through the uncached unmapped compatibility segment at
 * virtual 0xffffffffbfc00000. The processor starts fetching there after
 * reset, so a ROM image needs no loader beyond being placed at that
 * physical address.
 *
 * Writable data is linked for physical 0x00100000, also seen uncached at
 * virtual 0xffffffffa0100000. Uncached is deliberate: a ROM runs before any
 * cache is initialised, so nothing here relies on a cache flush being
 * correct. The initial contents of writable data travel in the ROM image and
 * the startup code copies them into RAM, so a test can hold constants the
 * compiler is not allowed to fold away.
 *
 * Both bases are values a 32-bit immediate can sign-extend to, which is what
 * lets every address in a ROM be formed with one lui and one daddiu and lets
 * the exception stubs run without touching the assembler temporary register.
 *
 * The exception entry points sit at the addresses the processor jumps to when
 * the boot-exception-vector bit of the Status register is set: 0xbfc00200 for
 * a TLB refill, 0xbfc00280 for an extended-addressing TLB refill, 0xbfc00300
 * for a cache error and 0xbfc00380 for everything else. The four stubs in the
 * trampoline block are the same entry points for when that bit is clear, at
 * 0x80000000 and the same offsets; the startup code copies the block there.
 */

OUTPUT_ARCH(mips)
OUTPUT_FORMAT(elf64-tradbigmips)
ENTRY(_start)

ROM_BASE   = 0xffffffffbfc00000;
DATA_BASE  = 0xffffffffa0100000;
STACK_SIZE = 0x00010000;

SECTIONS
{
    . = ROM_BASE;
    .start           : { *(.start) }

    .vec.tlb    ROM_BASE + 0x200 : { *(.vec.tlb) }
    .vec.xtlb   ROM_BASE + 0x280 : { *(.vec.xtlb) }
    .vec.cache  ROM_BASE + 0x300 : { *(.vec.cache) }
    .vec.gen    ROM_BASE + 0x380 : { *(.vec.gen) }

    /*
     * One 0x80-byte slot per stub so the copied block reproduces the
     * architectural vector spacing at 0x80000000.
     */
    .tramp      ROM_BASE + 0x400 : {
        _tramp_start = .;
        *(.tramp.tlb)   . = _tramp_start + 0x080;
        *(.tramp.xtlb)  . = _tramp_start + 0x100;
        *(.tramp.cache) . = _tramp_start + 0x180;
        *(.tramp.gen)   . = _tramp_start + 0x200;
        _tramp_end = .;
    }

    .text            : { *(.text) *(.text.*) }
    .rodata          : { *(.rodata) *(.rodata.*) }
    . = ALIGN(16);
    _rom_end = .;

    /*
     * Writable data lives in RAM but its initial contents are part of the ROM
     * image, placed right after the read-only data so the image stays one
     * contiguous run of bytes. The startup code copies it across.
     */
    . = DATA_BASE;
    .data : AT(_rom_end) {
        _data_start = .;
        *(.data) *(.data.*) *(.sdata) *(.sdata.*)
        . = ALIGN(8);
        _data_end = .;
    }
    _data_load = LOADADDR(.data);

    .bss  (NOLOAD)   : {
        . = ALIGN(8);
        _bss_start = .;
        *(.bss) *(.bss.*) *(.sbss) *(.sbss.*) *(COMMON)
        . = ALIGN(8);
        _bss_end = .;
    }
    _stack_top = ALIGN(_bss_end + STACK_SIZE, 16);

    /DISCARD/        : {
        *(.comment) *(.note*) *(.pdr) *(.reginfo)
        *(.MIPS.abiflags) *(.MIPS.options) *(.gnu.attributes)
    }
}

ASSERT(_tramp_end - _tramp_start == 0x200,
       "the trampoline block must be exactly the four architectural vector slots")
