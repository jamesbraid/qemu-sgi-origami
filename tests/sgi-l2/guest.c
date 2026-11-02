/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * A static 32-bit PowerPC program that does what the L2 controller's own
 * programs do to its kernel's L1 device nodes and flash: it opens the
 * control node and a slot node, issues the driver's ioctls, exchanges one
 * segment, and drives the flash behind /dev/mem with the AMD command
 * sequences. l2-shim-test.py runs it under qemu-ppc with the sgi-l2-shim
 * plugin and the L1 hub. It uses raw system calls and no C library.
 *
 * Each check prints "ok N text" or "FAIL N text"; the exit status is the
 * number of failures.
 */

#define NR_EXIT   1
#define NR_READ   3
#define NR_WRITE  4
#define NR_OPEN   5
#define NR_IOCTL  54
#define NR_MMAP   90
#define NR_POLL   167

#define O_RDWR    2
#define O_SYNC    0x101000
#define EBUSY     16
#define ENODEV    19
#define ENOTTY    25

static long sys(long n, long a, long b, long c, long d, long e, long f)
{
    register long r0 __asm__("r0") = n;
    register long r3 __asm__("r3") = a;
    register long r4 __asm__("r4") = b;
    register long r5 __asm__("r5") = c;
    register long r6 __asm__("r6") = d;
    register long r7 __asm__("r7") = e;
    register long r8 __asm__("r8") = f;
    unsigned long cr;

    __asm__ volatile("sc\n\tmfcr %1"
                     : "+r"(r3), "=&r"(cr), "+r"(r0), "+r"(r4), "+r"(r5),
                       "+r"(r6), "+r"(r7), "+r"(r8)
                     :
                     : "memory", "cr0", "r9", "r10", "r11", "r12", "ctr");
    return (cr & 0x10000000) ? -r3 : r3;
}

static unsigned int slen(const char *s)
{
    unsigned int n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

static void out(const char *s)
{
    sys(NR_WRITE, 1, (long)s, slen(s), 0, 0, 0);
}

static void out_num(long v)
{
    char buf[16];
    int i = 15;
    unsigned long u = v < 0 ? -v : v;

    buf[i] = 0;
    do {
        buf[--i] = '0' + u % 10;
        u /= 10;
    } while (u);
    if (v < 0) {
        buf[--i] = '-';
    }
    out(buf + i);
}

static int failures;
static int checks;

static void check(int cond, const char *text)
{
    checks++;
    out(cond ? "ok " : "FAIL ");
    out_num(checks);
    out(" ");
    out(text);
    out("\n");
    if (!cond) {
        failures++;
    }
}

static void check_eq(long got, long want, const char *text)
{
    check(got == want, text);
    if (got != want) {
        out("  got ");
        out_num(got);
        out(" want ");
        out_num(want);
        out("\n");
    }
}

static int same(const unsigned char *a, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

/* volatile: the plugin edits the window behind the compiler. */
static volatile unsigned int *flash;

static void w32(unsigned int off, unsigned int v)
{
    flash[off / 4] = v;
}

static unsigned int r32(unsigned int off)
{
    return flash[off / 4];
}

static void cmd(unsigned int third)
{
    w32(0x1554, 0x00aa00aa);
    w32(0x0aa8, 0x00550055);
    w32(0x1554, third);
}

static void program(unsigned int off, unsigned int v)
{
    cmd(0x00a000a0);
    w32(off, v);
}

static void erase_sector(unsigned int off)
{
    cmd(0x00800080);
    w32(0x1554, 0x00aa00aa);
    w32(0x0aa8, 0x00550055);
    w32(off, 0x00300030);
}

/*
 * A global, not a local: the program's data shares a page with its code,
 * which the emulator write-protects, and the driver's answer must still
 * reach it.
 */
static unsigned char revision[4] = { 9, 9, 9, 9 };

static void test_nodes(void)
{
    unsigned char map[40];
    unsigned char rec[9] = { 0 };
    unsigned char rec2[9] = { 0 };
    unsigned char seg[1040];
    struct { int fd; short events; short revents; } p;
    long cs, n;
    long dev;

    cs = sys(NR_OPEN, (long)"/dev/sgil1_cs", 0, 0, 0, 0, 0);
    check(cs >= 0, "open the control node");
    check_eq(sys(NR_IOCTL, cs, 0x40045306, (long)revision, 0, 0, 0), 0,
             "revision ioctl succeeds");
    check(revision[0] == 0 && revision[3] == 2, "driver revision is 2");
    n = sys(NR_READ, cs, (long)map, sizeof(map), 0, 0, 0);
    check_eq(n, 40, "the slot map is 40 bytes");
    check(map[0] == 1 && map[1] == 0, "slot 0 is connected and slot 1 is not");

    rec[2] = 0;
    check_eq(sys(NR_IOCTL, cs, 0xc0095307, (long)rec, 0, 0, 0), 0,
             "device record by slot");
    rec2[2] = 7;
    check_eq(sys(NR_IOCTL, cs, 0xc0095307, (long)rec2, 0, 0, 0), -22,
             "device record of an empty slot is EINVAL");
    rec2[2] = 40;
    check_eq(sys(NR_IOCTL, cs, 0xc0095307, (long)rec2, 0, 0, 0), -22,
             "device record above slot 39 is EINVAL");

    check_eq(sys(NR_OPEN, (long)"/dev/sgil1_3", O_RDWR, 0, 0, 0, 0), -ENODEV,
             "an empty slot node is ENODEV");
    dev = sys(NR_OPEN, (long)"/dev/sgil1_0", O_RDWR, 0, 0, 0, 0);
    check(dev >= 0, "open slot 0");
    check_eq(sys(NR_OPEN, (long)"/dev/sgil1_0", O_RDWR, 0, 0, 0, 0), -EBUSY,
             "a slot node already open is EBUSY");
    check_eq(sys(NR_IOCTL, dev, 0x40095304, (long)rec2, 0, 0, 0), 0,
             "device record of the node");
    check(same(rec, rec2, 9) && rec[0] == 1, "both records agree");
    check_eq(sys(NR_IOCTL, dev, 0x20005305, 0, 0, 0, 0), 0, "reset ioctl");
    check_eq(sys(NR_IOCTL, dev, 0x20005399, 0, 0, 0, 0), -ENOTTY,
             "an unknown ioctl is ENOTTY");
    check_eq(sys(NR_IOCTL, cs, 0x40095304, (long)rec2, 0, 0, 0), -ENOTTY,
             "a slot ioctl on the control node is ENOTTY");

    p.fd = dev;
    p.events = 1;
    p.revents = 0;
    check_eq(sys(NR_POLL, (long)&p, 1, 3000, 0, 0, 0), 1,
             "poll reports the segment");
    n = sys(NR_READ, dev, (long)seg, sizeof(seg), 0, 0, 0);
    check_eq(n, 28, "one read is one segment");
    check(seg[0] == 0 && seg[1] == 28, "the segment starts with its length");
    /* The driver stamps the transfer length over the first two bytes. */
    seg[0] = 0xde;
    seg[1] = 0xad;
    seg[6] = 1;
    check_eq(sys(NR_WRITE, dev, (long)seg, 28, 0, 0, 0), 28,
             "one write is one segment");
    check_eq(sys(NR_WRITE, dev, (long)seg, 20, 0, 0, 0), 20,
             "a shorter segment");
}

static void test_flash(void)
{
    long fd = sys(NR_OPEN, (long)"/dev/mem", O_RDWR | O_SYNC, 0, 0, 0, 0);
    long base;

    check(fd >= 0, "open /dev/mem");
    base = sys(NR_MMAP, 0, 0x400000, 3, 1, fd, (long)0xfc000000);
    check(base > 0 && base < 0xfffff000, "map the empty bus at 0xfc000000");
    base = sys(NR_MMAP, 0, 0x400000, 3, 1, fd, (long)0xfe000000);
    check(base > 0 && base < 0xfffff000, "map the flash at 0xfe000000");
    /* volatile: see the declaration. */
    flash = (volatile unsigned int *)base;

    check_eq(r32(0), -1, "erased flash reads ones");
    cmd(0x00900090);
    check_eq(r32(0), 0x00010001, "auto-select manufacturer word");
    check_eq(r32(4), 0x22282228, "auto-select device word");
    w32(0, 0x00f000f0);
    check_eq(r32(0), -1, "reset returns to the array");
    check_eq(r32(4), -1, "reset returns the second word too");
    check_eq(r32(0x1554), -1, "command words are not left behind");
    check_eq(r32(0x0aa8), -1, "the second command word either");

    program(0x100, 0x12345678);
    check_eq(r32(0x100), 0x12345678, "program a word");
    program(0x100, 0x0ff0ff0f);
    check_eq(r32(0x100), 0x02305608, "programming only clears bits");
    program(0x3ec000, 0x11111111);
    program(0x3f0000, 0x22222222);
    program(0x3f3ffc, 0x33333333);
    program(0x3f4000, 0x44444444);
    erase_sector(0x3f0000);
    check_eq(r32(0x3f0000), -1, "a top sector erases to ones");
    check_eq(r32(0x3f3ffc), -1, "to its last word");
    check_eq(r32(0x3ec000), 0x11111111, "the sector below is untouched");
    check_eq(r32(0x3f4000), 0x44444444, "the sector above is untouched");

    program(0x3ffff4, 0x55555555);
    program(0x5fffc, 0x66666666);
    program(0x60000, 0x77777777);
    program(0x1ffffc, 0x88888888);
    erase_sector(0x40000);
    check_eq(r32(0x5fffc), -1, "a bottom sector is 128 KiB");
    check_eq(r32(0x60000), 0x77777777, "the next sector is untouched");
    check_eq(r32(0x1ffffc), 0x88888888, "a far sector is untouched");
    check_eq(r32(0x100), 0x02305608, "programmed data survives an erase");
}

void _start(void)
{
    test_nodes();
    test_flash();
    out("done ");
    out_num(failures);
    out(" failures\n");
    sys(NR_EXIT, failures, 0, 0, 0, 0, 0);
    for (;;) {
    }
}
