/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * UHI semihosting uses $25 for the operation, $4-$6 for arguments, and a
 * software breakpoint with code 1. Start QEMU with semihosting enabled. Each
 * ROM prints one result line and returns the same status to the host.
 */

#include "rom.h"

struct rom_exc rom_exc;

#define UHI_EXIT  1
#define UHI_WRITE 5

/*
 * The UHI code-1 breakpoint is outside the ROM's MIPS IV assembler mode.
 */
#define UHI_TRAP ".word 0x7000007f"

static long uhi(long op, long a0, long a1, long a2)
{
    register long r_op __asm__("$25") = op;
    register long r_a0 __asm__("$4") = a0;
    register long r_a1 __asm__("$5") = a1;
    register long r_a2 __asm__("$6") = a2;
    register long ret __asm__("$2");

    __asm__ __volatile__(UHI_TRAP
                         : "=r"(ret)
                         : "r"(r_op), "r"(r_a0), "r"(r_a1), "r"(r_a2)
                         : "$3", "memory");
    return ret;
}

void rom_write(const char *buf, unsigned long len)
{
    uhi(UHI_WRITE, 2, (long)buf, (long)len);
}

void rom_puts(const char *s)
{
    const char *p = s;

    while (*p) {
        p++;
    }
    rom_write(s, (unsigned long)(p - s));
}

void rom_puthex(uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    char buf[18];
    int i = sizeof(buf);

    do {
        buf[--i] = digits[v & 0xf];
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    rom_write(&buf[i], sizeof(buf) - i);
}

void rom_exit(int status)
{
    uhi(UHI_EXIT, status, 0, 0);
    for (;;) {
        /* Do not continue if the host failed to exit the ROM. */
    }
}

void rom_pass(void)
{
    rom_puts("PASS ");
    rom_puts(rom_name);
    rom_puts("\n");
    rom_exit(0);
}

void rom_fail(int id, uint64_t expected, uint64_t actual)
{
    rom_puts("FAIL ");
    rom_puts(rom_name);
    rom_puts(" assert=");
    rom_puthex((uint64_t)id);
    rom_puts(" expected=");
    rom_puthex(expected);
    rom_puts(" actual=");
    rom_puthex(actual);
    rom_puts("\n");
    rom_exit(id);
}

/*
 * Print recorded state when no resume address was armed for the exception.
 */
void rom_report_unexpected_exception(void)
{
    rom_puts("FAIL ");
    rom_puts(rom_name);
    rom_puts(" unexpected-exception vector=");
    rom_puthex(rom_exc.vector);
    rom_puts(" cause=");
    rom_puthex(rom_exc.cause);
    rom_puts(" status=");
    rom_puthex(rom_exc.status);
    rom_puts(" epc=");
    rom_puthex(rom_exc.epc);
    rom_puts(" badvaddr=");
    rom_puthex(rom_exc.badvaddr);
    rom_puts("\n");
    rom_exit(ROM_STATUS_UNEXPECTED_EXCEPTION);
}
