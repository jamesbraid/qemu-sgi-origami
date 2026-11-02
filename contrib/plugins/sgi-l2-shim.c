/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * sgi-l2-shim: stand in for the L2 controller's Linux kernel when the
 * L2's own programs run under qemu-ppc.
 *
 * Those programs expect three things of their kernel that a host does not
 * give them: the L1 USB driver's device nodes with their ioctls, the flash
 * behind /dev/mem, and nothing else the host lacks. This plugin filters
 * the guest's system calls:
 *
 *  - open of the L1 control node and the slot nodes is answered with a
 *    sequenced-packet socket connected to the L1 hub (qemu-sgi-l2-hub), so
 *    read, write and poll then run natively with one message per transfer;
 *  - the driver's ioctls on those descriptors are answered here, with the
 *    answers the hub gives for a device record, and a reset forwarded;
 *  - /dev/mem is an ordinary file in the unpacked root. The plugin learns
 *    where the guest maps it and gives the mapped window the command
 *    behaviour of the AMD-style flash behind it: auto-select, sector and
 *    chip erase, program, reset. A store into the window is decoded after
 *    it happens and the plugin restores or fills the bytes the command
 *    changed through guest memory writes.
 *
 * It changes no QEMU file. Without hub=DIR it only logs, which is how the
 * system calls a program needs are collected.
 *
 * Options: hub=DIR (the hub's directory), log=FILE (default stderr),
 * flash-log=on (log every store into the window), trace=on (log every
 * system call that names a descriptor or path in /dev).
 *
 * Limits: a sequenced-packet socket truncates a read into a short buffer
 * instead of failing, returns EAGAIN rather than 0 on an empty
 * non-blocking read, and a second write never waits for the first; the
 * programs read with a buffer larger than a segment and only when poll says
 * data is waiting, so none of these paths is expected to run. Flash
 * operations complete at once. The window's shadow is per process, so two
 * programs that both program flash see each other's results in the shared
 * mapping but restore command words from their own copy.
 */
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <glib.h>

#include <qemu-plugin.h>
#include "../../include/hw/misc/sgi-l2-hub.h"

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

/* PowerPC Linux system call numbers. */
#define NR_EXECVE   11
#define NR_OPEN     5
#define NR_CLOSE    6
#define NR_DUP      41
#define NR_IOCTL    54
#define NR_DUP2     63
#define NR_MMAP     90
#define NR_MMAP2    192
#define NR_OPENAT   286

/* PowerPC Linux flag and error values. */
#define PPC_O_NONBLOCK  0x800
#define PPC_O_CLOEXEC   0x80000
#define PPC_AT_FDCWD    (-100)

/* The driver's ioctls, PowerPC encoding. */
#define IOCTL_REVISION      0x40045306u
#define IOCTL_CS_RECORD     0xc0095307u
#define IOCTL_DEV_RECORD    0x40095304u
#define IOCTL_RESET_RX      0x20005301u
#define IOCTL_RESET_TX      0x20005302u
#define IOCTL_RESET_DEVICE  0x20005303u
#define IOCTL_RESET_BOTH    0x20005305u
#define DRIVER_REVISION     2

#define NODE_CS    "/dev/sgil1_cs"
#define NODE_SLOT  "/dev/sgil1_"
#define NODE_MEM   "/dev/mem"

#define MAX_FDS    1024
#define MAX_VCPUS  256

/*
 * The guest maps /dev/mem at the flash's physical address, which does not
 * fit the signed file offset the emulator hands the host (32-bit PowerPC
 * passes the offset in bytes, and 0xfc000000 reads as negative). The
 * plugin gives the guest a /dev/mem file that holds the 36 MiB from
 * MEM_PHYS_BASE up, and rewrites such an offset to a position in it. The
 * first 4 MiB are a bus with nothing on it; the flash is at MEM_FLASH_FILE.
 */
#define MEM_PHYS_BASE       0xfc000000u
#define MEM_PHYS_END        0xfe400000u
#define MEM_FLASH_PHYS      0xfe000000u
#define MEM_FLASH_FILE      (MEM_FLASH_PHYS - MEM_PHYS_BASE)

/* The flash window. */
#define FLASH_SIZE          0x400000u
#define FLASH_CMD_MASK      0x1fffu
#define FLASH_CMD_ADDR1     0x1554u
#define FLASH_CMD_ADDR2     0x0aa8u
#define FLASH_BOTTOM_SECTOR 0x20000u
#define FLASH_TOP_START     0x3e0000u
#define FLASH_TOP_SECTOR    0x4000u
#define FLASH_MANUFACTURER  0x00010001u
#define FLASH_DEVICE        0x22282228u

typedef enum { FD_NONE, FD_CS, FD_SLOT, FD_MEM } FdKind;

typedef struct {
    FdKind kind;
    int slot;
    uint8_t record[SGI_L2_HUB_RECORD_LEN];
} FdEntry;

/* What a syscall entry saw, kept for its return callback. */
typedef struct {
    int64_t num;
    FdKind open_kind;       /* what an open will make of a result */
    bool is_open_mem;
    char path[96];
    int src_fd;             /* a dup source, or the mmap2 descriptor */
    uint64_t length;
    uint64_t pgoff;
    unsigned int ioctl_req;
} Pending;

typedef enum {
    FL_IDLE, FL_C1, FL_C2, FL_PROGRAM, FL_ERASE1, FL_ERASE2, FL_ERASE3,
} FlashState;

static GMutex lock;
static FdEntry fds[MAX_FDS];
static Pending pend[MAX_VCPUS];
static struct {
    struct qemu_plugin_register *r8;
    struct qemu_plugin_register *pc;
} regs[MAX_VCPUS];
static FILE *logf;
static char *hub_dir;
static bool log_flash;
static bool trace_dev;

/* The mapped window, if the guest has mapped /dev/mem. */
static struct {
    bool mapped;
    uint64_t base;      /* guest address of the first byte */
    uint8_t *shadow;    /* FLASH_SIZE bytes: what the array holds */
    FlashState state;
    bool autoselect;
} win;

static void plog(const char *fmt, ...) G_GNUC_PRINTF(1, 2);

static void plog(const char *fmt, ...)
{
    va_list ap;
    g_autofree char *line = NULL;

    va_start(ap, fmt);
    line = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    fprintf(logf, "l2-shim[%d]: %s\n", (int)getpid(), line);
    fflush(logf);
}

static bool read_guest(uint64_t addr, void *out, size_t len)
{
    GByteArray *buf = g_byte_array_new();
    bool ok = qemu_plugin_read_memory_vaddr(addr, buf, len);

    if (ok) {
        memcpy(out, buf->data, len);
    }
    g_byte_array_free(buf, true);
    return ok;
}

/*
 * The protection the host gives the page at the same address, read from the
 * process's own mappings; -1 if no mapping covers it. The emulator runs a
 * guest at its own address unless told otherwise, so the host page at a
 * guest address is the guest page.
 */
static int host_page_prot(uint64_t addr)
{
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    int prot = -1;

    while (maps && fgets(line, sizeof(line), maps)) {
        unsigned long lo, hi;
        char perms[8];

        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) == 3 &&
            addr >= lo && addr < hi) {
            prot = (perms[0] == 'r' ? PROT_READ : 0) |
                   (perms[1] == 'w' ? PROT_WRITE : 0) |
                   (perms[2] == 'x' ? PROT_EXEC : 0);
            break;
        }
    }
    if (maps) {
        fclose(maps);
    }
    return prot;
}

/*
 * Write guest memory. A page that holds translated code is write-protected
 * on the host so that a store to it is noticed, and the plugin API then
 * refuses the write even though the guest may write the page. The program's
 * data often shares a page with its code, so lift the protection for the
 * write and put it back; nothing here writes code.
 */
static bool write_guest(uint64_t addr, const void *data, size_t len)
{
    GByteArray *buf = g_byte_array_new();
    /* A plugin does not link the emulator, so not qemu_real_host_page_size. */
    long page_size = sysconf(_SC_PAGE_SIZE);
    bool ok;

    g_byte_array_append(buf, data, len);
    ok = qemu_plugin_write_memory_vaddr(addr, buf);
    if (!ok && (addr & (page_size - 1)) + len <= page_size) {
        void *page = (void *)(uintptr_t)(addr & ~(uint64_t)(page_size - 1));
        int prot = host_page_prot(addr);

        if (prot > 0 && !(prot & PROT_WRITE) &&
            !mprotect(page, page_size, prot | PROT_WRITE)) {
            ok = qemu_plugin_write_memory_vaddr(addr, buf);
            mprotect(page, page_size, prot);
        }
    }
    g_byte_array_free(buf, true);
    return ok;
}

/*
 * A string from guest memory, read up to each page boundary so that a
 * string near the end of a mapping does not fail the whole read.
 */
static bool read_guest_string(uint64_t addr, char *out, size_t size)
{
    size_t n = 0;

    while (n + 1 < size) {
        size_t chunk = 0x1000 - ((addr + n) & 0xfff);

        if (chunk > size - 1 - n) {
            chunk = size - 1 - n;
        }
        if (!read_guest(addr + n, out + n, chunk)) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            if (!out[n + i]) {
                return true;
            }
        }
        n += chunk;
    }
    out[size - 1] = 0;
    return true;
}

static int hub_connect(const uint8_t *request, uint8_t *reply)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    g_autofree char *path = NULL;
    struct timeval tv = { .tv_sec = 5 };
    int fd;
    int err = 0;
    ssize_t n;

    path = g_build_filename(hub_dir, SGI_L2_HUB_DEV_SOCKET, NULL);
    if (strlen(path) >= sizeof(addr.sun_path)) {
        return -ENAMETOOLONG;
    }
    strcpy(addr.sun_path, path);
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (fd < 0) {
        return -errno;
    }
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    /* errno means something only after a call that failed. */
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        err = errno;
    } else {
        n = send(fd, request, SGI_L2_HUB_REQUEST_LEN, MSG_NOSIGNAL);
        if (n != SGI_L2_HUB_REQUEST_LEN) {
            err = n < 0 ? errno : ECONNRESET;
        }
    }
    if (!err) {
        n = recv(fd, reply, SGI_L2_HUB_REPLY_LEN, 0);
        if (n != SGI_L2_HUB_REPLY_LEN) {
            err = n < 0 ? errno : ECONNRESET;
        }
    }
    if (err) {
        close(fd);
        return -err;
    }
    if (reply[0]) {
        close(fd);
        return -reply[0];
    }
    return fd;
}

static void track(int fd, FdKind kind, int slot, const uint8_t *record)
{
    if (fd < 0 || fd >= MAX_FDS) {
        return;
    }
    g_mutex_lock(&lock);
    fds[fd].kind = kind;
    fds[fd].slot = slot;
    if (record) {
        memcpy(fds[fd].record, record, SGI_L2_HUB_RECORD_LEN);
    }
    g_mutex_unlock(&lock);
}

static FdEntry tracked(int fd)
{
    FdEntry e = { FD_NONE };

    if (fd >= 0 && fd < MAX_FDS) {
        g_mutex_lock(&lock);
        e = fds[fd];
        g_mutex_unlock(&lock);
    }
    return e;
}

/* Handle an open of path. Returns true when the plugin supplies the result. */
static bool serve_open(const char *path, uint64_t flags, int64_t *ret)
{
    uint8_t req[SGI_L2_HUB_REQUEST_LEN] = { 0 };
    uint8_t reply[SGI_L2_HUB_REPLY_LEN];
    FdKind kind;
    int slot = 0;
    int fd;

    if (!hub_dir) {
        return false;
    }
    if (!strcmp(path, NODE_CS)) {
        kind = FD_CS;
        req[0] = SGI_L2_HUB_OP_CONTROL;
    } else if (!strncmp(path, NODE_SLOT, strlen(NODE_SLOT)) &&
               g_ascii_isdigit(path[strlen(NODE_SLOT)])) {
        char *end;
        uint64_t number;

        /* A number too large for the type saturates, so range-check it wide. */
        number = g_ascii_strtoull(path + strlen(NODE_SLOT), &end, 10);
        if (*end || number >= SGI_L2_HUB_SLOTS) {
            *ret = -ENODEV;
            plog("open %s: no such slot", path);
            return true;
        }
        slot = number;
        kind = FD_SLOT;
        req[0] = SGI_L2_HUB_OP_OPEN;
        req[1] = slot;
    } else {
        return false;
    }

    fd = hub_connect(req, reply);
    if (fd < 0) {
        plog("open %s: hub: %s", path, strerror(-fd));
        *ret = (kind == FD_CS && fd == -ECONNREFUSED) ? -ENOENT : fd;
        return true;
    }
    if (flags & PPC_O_NONBLOCK) {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    if (flags & PPC_O_CLOEXEC) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    track(fd, kind, slot, reply + 1);
    plog("open %s -> fd %d", path, fd);
    *ret = fd;
    return true;
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

static bool serve_ioctl(unsigned int vcpu, int fd, uint32_t req,
                        uint64_t argp, int64_t *ret)
{
    FdEntry e = tracked(fd);
    uint8_t rec[SGI_L2_HUB_RECORD_LEN];

    if (e.kind != FD_CS && e.kind != FD_SLOT) {
        return false;
    }
    *ret = 0;
    switch (req) {
    case IOCTL_REVISION:
        if (e.kind == FD_CS) {
            uint8_t v[4];

            put_be32(v, DRIVER_REVISION);
            *ret = write_guest(argp, v, sizeof(v)) ? 0 : -EFAULT;
            return true;
        }
        break;
    case IOCTL_CS_RECORD:
        if (e.kind == FD_CS) {
            uint8_t request[SGI_L2_HUB_REQUEST_LEN] = {
                SGI_L2_HUB_OP_QUERY, 0, 0, 0 };
            uint8_t reply[SGI_L2_HUB_REPLY_LEN];
            int q;

            if (!read_guest(argp, rec, sizeof(rec))) {
                *ret = -EFAULT;
                return true;
            }
            if (rec[2] >= SGI_L2_HUB_SLOTS) {
                *ret = -EINVAL;
                return true;
            }
            request[1] = rec[2];
            q = hub_connect(request, reply);
            if (q < 0) {
                *ret = -EINVAL;
                return true;
            }
            close(q);
            *ret = write_guest(argp, reply + 1, SGI_L2_HUB_RECORD_LEN) ?
                   0 : -EFAULT;
            return true;
        }
        break;
    case IOCTL_DEV_RECORD:
        if (e.kind == FD_SLOT) {
            *ret = write_guest(argp, e.record, sizeof(e.record)) ?
                   0 : -EFAULT;
            return true;
        }
        break;
    case IOCTL_RESET_RX:
    case IOCTL_RESET_TX:
    case IOCTL_RESET_DEVICE:
    case IOCTL_RESET_BOTH:
        if (e.kind == FD_SLOT) {
            uint8_t request[SGI_L2_HUB_REQUEST_LEN] = {
                SGI_L2_HUB_OP_RESET, e.slot, req & 0xff, 0 };
            uint8_t reply[SGI_L2_HUB_REPLY_LEN];
            int q = hub_connect(request, reply);

            if (q >= 0) {
                close(q);
            }
            return true;
        }
        break;
    default:
        break;
    }
    plog("ioctl 0x%08x on fd %d (%s): ENOTTY", req, fd,
         e.kind == FD_CS ? "control node" : "slot node");
    *ret = -ENOTTY;
    return true;
}

/* ---- flash ---- */

static bool flash_in_window(uint64_t vaddr, size_t size)
{
    return win.mapped && vaddr >= win.base &&
           vaddr + size <= win.base + FLASH_SIZE;
}

static void flash_write_array(uint32_t off, size_t len)
{
    write_guest(win.base + off, win.shadow + off, len);
}

static void flash_present_ids(void)
{
    uint8_t ids[8];

    put_be32(ids, FLASH_MANUFACTURER);
    put_be32(ids + 4, FLASH_DEVICE);
    write_guest(win.base, ids, sizeof(ids));
}

static void flash_leave_autoselect(void)
{
    if (win.autoselect) {
        win.autoselect = false;
        flash_write_array(0, 8);
    }
}

static void flash_sector(uint32_t off, uint32_t *start, uint32_t *len)
{
    if (off < FLASH_TOP_START) {
        *start = off & ~(FLASH_BOTTOM_SECTOR - 1);
        *len = FLASH_BOTTOM_SECTOR;
    } else {
        *start = FLASH_TOP_START +
                 ((off - FLASH_TOP_START) & ~(FLASH_TOP_SECTOR - 1));
        *len = FLASH_TOP_SECTOR;
    }
}

/* A command cycle wrote cmd to guest memory at off; put the array back. */
static void flash_restore_word(uint32_t off, size_t size)
{
    off &= ~3u;
    if (off + 4 <= FLASH_SIZE) {
        flash_write_array(off, 4);
    }
    (void)size;
}

static void flash_store(uint32_t off, size_t size, uint32_t value)
{
    /* Both 16-bit chips see the command; their halves match. */
    uint32_t cmd = (size == 4 ? value & 0xffff : value) & 0xff;
    uint32_t cycle = off & FLASH_CMD_MASK;
    bool c1 = cycle == FLASH_CMD_ADDR1;
    bool c2 = cycle == FLASH_CMD_ADDR2;

    if (log_flash) {
        plog("flash store off 0x%06x size %zu value 0x%x (state %d%s)", off,
             size, value, win.state, win.autoselect ? ", autoselect" : "");
    }
    switch (win.state) {
    case FL_IDLE:
        if (cmd == 0xf0) {
            flash_restore_word(off, size);
            flash_leave_autoselect();
        } else if (c1 && cmd == 0xaa) {
            win.state = FL_C1;
            flash_restore_word(off, size);
        } else {
            /* A plain store to the array: flash ignores it. */
            plog("flash: store outside a command at 0x%06x ignored", off);
            flash_restore_word(off, size);
        }
        break;
    case FL_C1:
        flash_restore_word(off, size);
        win.state = (c2 && cmd == 0x55) ? FL_C2 : FL_IDLE;
        if (win.state == FL_IDLE) {
            plog("flash: command cycle 2 expected, got 0x%x at 0x%06x", cmd,
                 off);
        }
        break;
    case FL_C2:
        flash_restore_word(off, size);
        win.state = FL_IDLE;
        if (c1 && cmd == 0x90) {
            win.autoselect = true;
            flash_present_ids();
        } else if (c1 && cmd == 0xa0) {
            win.state = FL_PROGRAM;
        } else if (c1 && cmd == 0x80) {
            win.state = FL_ERASE1;
        } else if (cmd == 0xf0) {
            flash_leave_autoselect();
        } else {
            plog("flash: unknown command 0x%x at 0x%06x", cmd, off);
        }
        break;
    case FL_PROGRAM:
        {
            uint32_t base = off & ~(size - 1);
            uint8_t cur[4], keep[4];
            size_t n = size > 4 ? 4 : size;

            /* The store wrote value; programming can only clear bits. */
            memcpy(keep, win.shadow + base, n);
            for (size_t i = 0; i < n; i++) {
                keep[i] &= (value >> (8 * (n - 1 - i))) & 0xff;
                cur[i] = keep[i];
            }
            memcpy(win.shadow + base, cur, n);
            flash_write_array(base, n);
            win.state = FL_IDLE;
        }
        break;
    case FL_ERASE1:
        flash_restore_word(off, size);
        win.state = (c1 && cmd == 0xaa) ? FL_ERASE2 : FL_IDLE;
        break;
    case FL_ERASE2:
        flash_restore_word(off, size);
        win.state = (c2 && cmd == 0x55) ? FL_ERASE3 : FL_IDLE;
        break;
    case FL_ERASE3:
        win.state = FL_IDLE;
        if (c1 && cmd == 0x10) {
            memset(win.shadow, 0xff, FLASH_SIZE);
            flash_write_array(0, FLASH_SIZE);
            plog("flash: chip erase");
        } else if (cmd == 0x30) {
            uint32_t start, len;

            flash_sector(off, &start, &len);
            memset(win.shadow + start, 0xff, len);
            flash_write_array(start, len);
            plog("flash: sector erase 0x%06x +0x%x", start, len);
        } else {
            plog("flash: unknown erase command 0x%x at 0x%06x", cmd, off);
            flash_restore_word(off, size);
        }
        break;
    }
}

static void vcpu_mem(unsigned int vcpu_index, qemu_plugin_meminfo_t info,
                     uint64_t vaddr, void *userdata)
{
    qemu_plugin_mem_value v;
    size_t size;
    uint32_t value;

    /*
     * The window is set up by a system call on another thread. The common
     * case, no window, is answered without the lock; the rest of the window's
     * state is read under it.
     */
    if (!__atomic_load_n(&win.mapped, __ATOMIC_ACQUIRE)) {
        return;
    }
    size = 1u << qemu_plugin_mem_size_shift(info);
    if (size > 4) {
        return;
    }
    v = qemu_plugin_mem_get_value(info);
    switch (v.type) {
    case QEMU_PLUGIN_MEM_VALUE_U8:
        value = v.data.u8;
        break;
    case QEMU_PLUGIN_MEM_VALUE_U16:
        value = v.data.u16;
        break;
    default:
        value = v.data.u32;
        break;
    }
    g_mutex_lock(&lock);
    if (flash_in_window(vaddr, size)) {
        flash_store(vaddr - win.base, size, value);
    }
    g_mutex_unlock(&lock);
}

static void vcpu_tb_trans(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);

        qemu_plugin_register_vcpu_mem_cb(insn, vcpu_mem,
                                         QEMU_PLUGIN_CB_NO_REGS,
                                         QEMU_PLUGIN_MEM_W, NULL);
    }
}

/* ---- system calls ---- */

static bool path_wanted(const char *path)
{
    return !strncmp(path, "/dev/", 5);
}

/*
 * Make the guest repeat its mmap with the offset argument replaced. The
 * argument is in r8; the vCPU runs the call again from the "sc"
 * instruction, whose address is the next instruction's minus 4. The caller
 * saved registers are free for the system call to clobber, so the guest
 * cannot tell.
 */
static bool restart_with_offset(unsigned int vcpu_index, uint64_t value)
{
    GByteArray *buf = g_byte_array_new();
    uint8_t v[4];
    uint32_t pc;

    if (!regs[vcpu_index % MAX_VCPUS].r8 || !regs[vcpu_index % MAX_VCPUS].pc) {
        goto out;
    }
    put_be32(v, (uint32_t)value);
    g_byte_array_append(buf, v, sizeof(v));
    if (!qemu_plugin_write_register(regs[vcpu_index % MAX_VCPUS].r8, buf)) {
        goto out;
    }
    g_byte_array_set_size(buf, 0);
    if (!qemu_plugin_read_register(regs[vcpu_index % MAX_VCPUS].pc, buf) ||
        buf->len != 4) {
        goto out;
    }
    pc = ((uint32_t)buf->data[0] << 24) | (buf->data[1] << 16) |
         (buf->data[2] << 8) | buf->data[3];
    g_byte_array_free(buf, true);
    qemu_plugin_set_pc(pc - 4);
    return true;
out:
    g_byte_array_free(buf, true);
    return false;
}

static void vcpu_init(unsigned int vcpu_index, void *userdata)
{
    GArray *list = qemu_plugin_get_registers();

    for (guint i = 0; i < list->len; i++) {
        qemu_plugin_reg_descriptor *d =
            &g_array_index(list, qemu_plugin_reg_descriptor, i);

        if (!strcmp(d->name, "r8")) {
            regs[vcpu_index % MAX_VCPUS].r8 = d->handle;
        } else if (!strcmp(d->name, "pc") || !strcmp(d->name, "nip")) {
            regs[vcpu_index % MAX_VCPUS].pc = d->handle;
        }
    }
    g_array_free(list, true);
}

static bool vcpu_syscall_filter(unsigned int vcpu_index, int64_t num,
                                uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6,
                                uint64_t a7, uint64_t a8, int64_t *sysret,
                                void *userdata)
{
    Pending *p = &pend[vcpu_index % MAX_VCPUS];
    char path[96] = "";

    memset(p, 0, sizeof(*p));
    p->num = num;
    p->src_fd = -1;
    switch (num) {
    case NR_OPEN:
    case NR_OPENAT:
        {
            uint64_t path_addr = num == NR_OPEN ? a1 : a2;
            uint64_t flags = num == NR_OPEN ? a2 : a3;

            if (!read_guest_string(path_addr, path, sizeof(path))) {
                return false;
            }
            if (!path_wanted(path)) {
                return false;
            }
            if (num == NR_OPENAT && (int32_t)a1 != PPC_AT_FDCWD &&
                path[0] != '/') {
                return false;
            }
            if (trace_dev || !hub_dir) {
                plog("open(%s, 0x%x)", path, (unsigned int)flags);
            }
            if (serve_open(path, flags, sysret)) {
                return true;
            }
            g_strlcpy(p->path, path, sizeof(p->path));
            p->is_open_mem = !strcmp(path, NODE_MEM);
        }
        return false;
    case NR_CLOSE:
        {
            int fd = (int)a1;

            if (tracked(fd).kind != FD_NONE) {
                track(fd, FD_NONE, 0, NULL);
            }
        }
        return false;
    case NR_DUP:
    case NR_DUP2:
        p->src_fd = (int)a1;
        return false;
    case NR_IOCTL:
        {
            int fd = (int)a1;
            uint32_t req = (uint32_t)a2;

            if (serve_ioctl(vcpu_index, fd, req, (uint32_t)a3, sysret)) {
                return true;
            }
            p->src_fd = fd;
            p->ioctl_req = req;
        }
        return false;
    case NR_MMAP:
    case NR_MMAP2:
        {
            int fd = (int)a5;
            uint64_t bytes = num == NR_MMAP ? (uint32_t)a6
                                            : (uint64_t)(uint32_t)a6 << 12;

            if (fd < 0 || tracked(fd).kind != FD_MEM) {
                return false;
            }
            if (bytes >= MEM_PHYS_BASE && bytes < MEM_PHYS_END) {
                uint64_t file = bytes - MEM_PHYS_BASE;

                plog("mmap(len 0x%x, /dev/mem fd %d, physical 0x%" PRIx64
                     "): restart with file offset 0x%" PRIx64, (uint32_t)a2,
                     fd, bytes, file);
                if (restart_with_offset(vcpu_index, num == NR_MMAP ? file :
                                        file >> 12)) {
                    /* not reached: the vCPU loop restarts the call */
                    return false;
                }
                *sysret = -EINVAL;
                return true;
            }
            p->src_fd = fd;
            p->length = (uint32_t)a2;
            p->pgoff = bytes;
            plog("mmap(len 0x%" PRIx64 ", prot 0x%x, flags 0x%x, /dev/mem "
                 "fd %d, file offset 0x%" PRIx64 ")", p->length,
                 (unsigned int)a3, (unsigned int)a4, fd, bytes);
        }
        return false;
    case NR_EXECVE:
        if (read_guest_string(a1, path, sizeof(path))) {
            plog("execve(%s)", path);
        }
        return false;
    default:
        return false;
    }
}

static void vcpu_syscall_ret(unsigned int vcpu_index, int64_t num,
                             int64_t ret, void *userdata)
{
    Pending *p = &pend[vcpu_index % MAX_VCPUS];

    if (p->num != num) {
        return;
    }
    p->num = -1;
    switch (num) {
    case NR_OPEN:
    case NR_OPENAT:
        if (p->path[0]) {
            if (trace_dev || !hub_dir) {
                plog("  -> %" PRId64 "%s%s", ret, ret < 0 ? " " : "",
                     ret < 0 ? strerror((int)-ret) : "");
            }
            if (p->is_open_mem && ret >= 0) {
                track((int)ret, FD_MEM, 0, NULL);
            }
        }
        break;
    case NR_DUP:
    case NR_DUP2:
        if (ret >= 0 && p->src_fd >= 0) {
            FdEntry e = tracked(p->src_fd);

            if (e.kind != FD_NONE) {
                track((int)ret, e.kind, e.slot, e.record);
            }
        }
        break;
    case NR_IOCTL:
        if (p->src_fd >= 0 && (trace_dev || !hub_dir) &&
            tracked(p->src_fd).kind != FD_NONE) {
            plog("ioctl(fd %d, 0x%08x) -> %" PRId64, p->src_fd,
                 p->ioctl_req, ret);
        }
        break;
    case NR_MMAP:
    case NR_MMAP2:
        if (p->src_fd >= 0 && ret > 0 && ret < 0xfffff000) {
            uint64_t base = (uint32_t)ret;

            plog("mmap -> 0x%" PRIx64, base);
            if (p->length >= FLASH_SIZE && p->pgoff == MEM_FLASH_FILE) {
                g_mutex_lock(&lock);
                g_free(win.shadow);
                win.shadow = g_malloc(FLASH_SIZE);
                if (read_guest(base, win.shadow, FLASH_SIZE)) {
                    win.base = base;
                    __atomic_store_n(&win.mapped, true, __ATOMIC_RELEASE);
                    win.state = FL_IDLE;
                    win.autoselect = false;
                    plog("flash window at 0x%" PRIx64, base);
                } else {
                    plog("flash window unreadable");
                }
                g_mutex_unlock(&lock);
            }
        } else if (p->src_fd >= 0) {
            plog("mmap failed: %" PRId64 " %s", ret, strerror((int)-ret));
        }
        break;
    default:
        break;
    }
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    const char *log_path = NULL;

    if (info->system_emulation) {
        fprintf(stderr, "sgi-l2-shim: only useful for user emulation\n");
        return -1;
    }
    for (int i = 0; i < argc; i++) {
        g_auto(GStrv) tokens = g_strsplit(argv[i], "=", 2);

        if (!tokens[1]) {
            fprintf(stderr, "sgi-l2-shim: option %s needs a value\n",
                    argv[i]);
            return -1;
        } else if (!strcmp(tokens[0], "hub")) {
            hub_dir = g_strdup(tokens[1]);
        } else if (!strcmp(tokens[0], "log")) {
            log_path = tokens[1];
            log_path = g_strdup(log_path);
        } else if (!strcmp(tokens[0], "flash-log")) {
            log_flash = !strcmp(tokens[1], "on");
        } else if (!strcmp(tokens[0], "trace")) {
            trace_dev = !strcmp(tokens[1], "on");
        } else {
            fprintf(stderr, "sgi-l2-shim: unknown option %s\n", argv[i]);
            return -1;
        }
    }
    logf = stderr;
    if (log_path) {
        logf = fopen(log_path, "a");
        if (!logf) {
            fprintf(stderr, "sgi-l2-shim: %s: %s\n", log_path,
                    strerror(errno));
            return -1;
        }
    }
    qemu_plugin_register_vcpu_syscall_filter_cb(id, vcpu_syscall_filter,
                                                NULL);
    qemu_plugin_register_vcpu_syscall_ret_cb(id, vcpu_syscall_ret, NULL);
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init, NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id, vcpu_tb_trans, NULL);
    return 0;
}
