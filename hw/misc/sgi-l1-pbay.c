/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The L1's end of the power-bay serial link.
 *
 * The L1 frames each transaction itself: a start byte, the body and its
 * checksum with the three framing bytes stuffed, and a terminator. It
 * drops a received frame that does not start with the start byte or is
 * not addressed to 0x0000 or to itself, and leaves the checksum to the
 * transaction layer above. After the link opens it asks the bay for its
 * address, tells the bay that address, reads the device type and the
 * capability bytes, reads the bay's records and then polls its sensors.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/log.h"
#include "migration/blocker.h"
#include "hw/misc/sgi-l1-pbay.h"

#define PB_START        0xa0
#define PB_END          0xa5
#define PB_ESCAPE       0xaa
#define PB_ESC_START    0xb0
#define PB_ESC_END      0xb5
#define PB_ESC_ESCAPE   0xba

#define PB_ADDRESS      0x4400
#define PB_HEADER       7
#define PB_REPLY_BIT    0x80

#define CMD_ADDRESS     0x02
#define CMD_SILENT      0x10
#define CMD_ASSIGN      0x12
#define CMD_TYPE        0x13
#define CMD_TUNNEL      0x20

/*
 * Retries: every request allows three transmissions with up to three reads
 * each. A read that times out goes to the next transmission, which waits
 * PB_RESEND_TICKS first; a frame that fails the reply checks costs a read,
 * and the next read is made without resending.
 */
#define PB_TRANSMISSIONS    3
#define PB_READS            3
#define PB_RESEND_TICKS     20
/*
 * The driver loads a read's countdown with 2 and decrements it every 100
 * ticks, so a read lasts 1 to 2 periods. The model takes the middle: a
 * model choice, as the phase is unknown.
 */
#define PB_READ_TICKS       150
/* Task start waits 100 * bay + 300 ticks, bay being L1 setting 2. */
#define PB_START_TICKS(bay) (100 * (bay) + 300)
#define PB_MAX_RECORDS      64
#define SCALE_MS            1000000

/* The sensor and FRU-locator records the L1 reads. */
#define REC_SENSOR_FULL     0x01
#define REC_SENSOR_COMPACT  0x02
#define REC_FRU_LOCATOR     0x11
#define ENTITY_SUPPLY       0x0a
#define ENTITY_PORT         0x15
#define ENTITY_BAY          0xb0
#define SENSOR_SUPPLY       8
#define SENSOR_PORT         9

/*
 * The brick type characters that do not run the power-bay task in the
 * packages the Origin 3000 compute brick uses, so the L1 has no bay link
 * there: 0 4 7 8 9 A E G O Q V. G's second port is a console at 38400
 * baud instead. Two older packages start the task for every letter except
 * O; the model follows the first set.
 */
static bool pb_port_closed(uint32_t letter)
{
    return letter && strchr("04789AEGOQV", letter);
}

enum {
    STEP_IDLE,
    STEP_ADDRESS,
    STEP_ASSIGN,
    STEP_TYPE,
    STEP_CAPABILITY,
    STEP_RECORD_ID,
    STEP_RECORD,
    /* The steps of a job (an operator command or a polling pass). */
    STEP_SCAN,
    STEP_FRU,
    STEP_VERSION,
    STEP_RESET,
};

enum {
    JOB_NONE,
    JOB_POLL,
    JOB_VERSION,
    JOB_DPS,
    JOB_DCPORT,
    JOB_ENV,
    JOB_RESET,
    JOB_FRU,
    JOB_SERIAL,
    JOB_INIT,
};

enum {
    WAIT_NONE,
    WAIT_START,
    WAIT_READ,
    WAIT_RESEND,
    WAIT_REREAD,
};

/*
 * Timers use the host clock: the bay is a host peer and answers in real
 * time, so the virtual clock (stopped in a qtest, and frozen while the
 * guest is halted) would never fire them. Under icount or record and replay
 * the link's timing is therefore not deterministic.
 */
static void pb_wait(SGIL1Pbay *p, unsigned kind, uint64_t ticks)
{
    p->wait = kind;
    timer_mod_ns(p->timeout, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                 ticks * p->tick_us * 1000);
}

static void pb_begin(SGIL1Pbay *p);
static void pb_poll_read(SGIL1Pbay *p, unsigned phase);
static void pb_bad_reply(SGIL1Pbay *p);

static void pb_put(uint8_t *out, unsigned *n, uint8_t b)
{
    switch (b) {
    case PB_START:
        out[(*n)++] = PB_ESCAPE;
        out[(*n)++] = PB_ESC_START;
        break;
    case PB_END:
        out[(*n)++] = PB_ESCAPE;
        out[(*n)++] = PB_ESC_END;
        break;
    case PB_ESCAPE:
        out[(*n)++] = PB_ESCAPE;
        out[(*n)++] = PB_ESC_ESCAPE;
        break;
    default:
        out[(*n)++] = b;
    }
}

static uint16_t pb_checksum(const uint8_t *body, unsigned len)
{
    unsigned sum = 0;

    for (unsigned i = 0; i < len; i++) {
        sum += body[i];
    }
    return 0xffff - (sum + len + 1);
}

static void pb_write(SGIL1Pbay *p)
{
    uint8_t wire[2 * (sizeof(p->request) + 2) + 2];
    uint16_t sum = pb_checksum(p->request, p->request_len);
    unsigned n = 0;

    wire[n++] = PB_START;
    for (unsigned i = 0; i < p->request_len; i++) {
        pb_put(wire, &n, p->request[i]);
    }
    pb_put(wire, &n, sum & 0xff);
    pb_put(wire, &n, sum >> 8);
    wire[n++] = PB_END;
    qemu_chr_fe_write_all(&p->chr, wire, n);
}

static void pb_job_end(SGIL1Pbay *p, bool ok);
static void pb_out(SGIL1Pbay *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static const char *pb_state_text(const SGIL1Pbay *p);
static void pb_job_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len);
static bool pb_job_step(const SGIL1Pbay *p);

/* An initialisation transaction gave up. */
static void pb_fail(SGIL1Pbay *p)
{
    timer_del(p->timeout);
    p->wait = WAIT_NONE;
    p->step = STEP_IDLE;
    p->status = SGI_L1_PBAY_COMM_FAILURE;
    if (p->job.kind == JOB_INIT) {
        pb_out(p, "%s\r\n", pb_state_text(p));
        pb_job_end(p, false);
    }
}

/*
 * Send a request to @dst. @netfn is the network function for a tunnelled
 * request, which carries the target byte, the function, @cmd and @data;
 * a plain request carries @data.
 */
static void pb_request(SGIL1Pbay *p, uint16_t dst, uint8_t cmd,
                       const uint8_t *data, unsigned len, bool wait)
{
    uint8_t *r = p->request;
    unsigned n = 0;

    g_assert(PB_HEADER + len <= sizeof(p->request));
    stw_le_p(r + n, dst);
    stw_le_p(r + n + 2, p->l1_address);
    n += 4;
    r[n++] = 0;
    r[n++] = p->tag++ & 0x1f;
    r[n++] = cmd;
    if (len) {
        memcpy(r + n, data, len);
    }
    p->request_len = n + len;
    p->request_cmd = cmd;
    p->request_netfn = cmd == CMD_TUNNEL ? data[1] : 0;
    p->transmissions = 1;
    p->reads = PB_READS;
    pb_write(p);
    if (wait) {
        pb_wait(p, WAIT_READ, PB_READ_TICKS);
    }
}

static void pb_tunnel(SGIL1Pbay *p, uint8_t netfn, uint8_t cmd,
                      const uint8_t *data, unsigned len)
{
    uint8_t buf[16] = { p->target, netfn, cmd };

    g_assert(3 + len <= sizeof(buf));
    memcpy(buf + 3, data, len);
    pb_request(p, p->bay, CMD_TUNNEL, buf, 3 + len, true);
}

static void pb_log(SGIL1Pbay *p, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
static void pb_log(SGIL1Pbay *p, const char *fmt, ...)
{
    g_autofree char *text = NULL;
    va_list ap;

    va_start(ap, fmt);
    text = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    qemu_log_mask(LOG_UNIMP, "sgi-l1: pbay: the event wrapper around a log "
                  "message is not modelled\n");
    p->log(p->opaque, text);
}

/*
 * The log lines of one polling pass, from the reading of each supply and
 * port. Sensors 1 to 6 are the supplies: bit 0 set means not inserted, bits
 * 1 to 3 fail, predictive fail and AC fail. Sensors 7 to 14 are the DC
 * ports: bit 0 set means the 48 V output is off and the port is skipped,
 * bit 1 set means failed. A fault is logged when its bit
 * goes from 0 to 1 against the value the poll stored last time, and each
 * test needs its bit of the monitoring setting. Nothing is stored at start,
 * so a fault already present on the first pass counts as new. A supply read
 * with bit 0 set clears its stored value; a port read with bit 0 set is
 * skipped and keeps it. Per supply the first matching test wins (AC fail,
 * fail, predictive fail), so one fault is logged per supply per pass.
 * @ac_ok and @port_ok are the supplies whose AC fail and the ports whose
 * failure stayed in sight through the re-reads.
 *
 * Insertion and removal compare the set of present supplies with the set in
 * setting 105, which survives a restart of the link: zero logs nothing, the
 * present set is written back when it differs, and a pass that finds no
 * supply present is treated as a failed read and skips the step.
 */
static void pb_evaluate(SGIL1Pbay *p, uint8_t ac_ok, uint8_t port_ok)
{
    uint8_t env = p->env_mask;

    if (env & 1) {
        uint8_t present = 0;

        for (unsigned s = 1; s <= PB_SUPPLIES; s++) {
            if (!(p->supply[s] & 1)) {
                present |= 1 << (s - 1);
            }
        }
        if (present) {
            for (unsigned s = 1; s <= PB_SUPPLIES && p->present; s++) {
                if ((present ^ p->present) & (1 << (s - 1))) {
                    pb_log(p, "Power Bay DPS %u %s", s,
                           present & (1 << (s - 1)) ? "inserted" : "removed");
                }
            }
            p->present = present;
        }
    }

    for (unsigned s = 1; s <= PB_SUPPLIES && (env & 7); s++) {
        uint8_t now = p->supply[s];
        uint8_t rose = now & ~p->supply_last[s];

        if (now & 1) {
            p->supply_last[s] = 0;
            continue;
        }
        if ((env & 1) && (rose & 8)) {
            if (ac_ok & (1 << (s - 1))) {
                pb_log(p, "Power Bay DPS %u AC fail, lost redundancy", s);
            }
        } else if ((env & 2) && (rose & 2)) {
            pb_log(p, "Power Bay DPS %u fail, lost redundancy", s);
        } else if ((env & 4) && (rose & 4)) {
            pb_log(p, "Power Bay DPS %u predictive fail", s);
        }
        p->supply_last[s] = now;
    }
    for (unsigned n = 1; n <= PB_PORTS && (env & 8); n++) {
        uint8_t now = p->port[n];

        if (now & 1) {
            continue;
        }
        if (((now & ~p->port_last[n]) & 2) && (port_ok & (1 << (n - 1)))) {
            pb_log(p, "Power Bay DCport %u fail, lost power to brick", n);
        }
        p->port_last[n] = now;
    }
}

/*
 * The sensor behind poll position @pos (0 to 5 the supplies, 6 to 13 the
 * ports), or 0 when the records did not map it.
 */
static uint8_t pb_sensor_at(const SGIL1Pbay *p, unsigned pos)
{
    return pos < PB_SUPPLIES ? p->supply_sensor[pos + 1] :
                               p->port_sensor[pos - PB_SUPPLIES + 1];
}

/*
 * The operator's texts for each state of the link. They are paraphrased
 * from the source material: each starts "PBay " and ends in a newline.
 */
static const char *pb_state_text(const SGIL1Pbay *p)
{
    switch (p->status) {
    case SGI_L1_PBAY_NOT_INITIALISED:
        return "PBay not initialized";
    case SGI_L1_PBAY_INITIALISING:
        return "PBay busy initializing";
    case SGI_L1_PBAY_BUSY:
        return "PBay busy try again later";
    case SGI_L1_PBAY_FLASHING:
        return "PBay busy flashing";
    case SGI_L1_PBAY_COMM_FAILURE:
        return "PBay communication failure";
    default:
        return "PBay state unknown";
    }
}

/*
 * The poll task waits 6000 + 100 * bay ticks on its queue: a request
 * restarts the wait and a timeout runs the poll. pbay-poll-ms, when nonzero,
 * replaces the period for tests.
 */
static void pb_poll_arm(SGIL1Pbay *p)
{
    int64_t ns = p->poll_ms ? (int64_t)p->poll_ms * SCALE_MS :
                 (6000 + 100 * (int64_t)p->bay_number) * p->tick_us * 1000;

    timer_mod_ns(p->poll_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME) + ns);
}

/* The bay semaphore: a job holds it from its first request to its end. */
static void pb_job_begin(SGIL1Pbay *p, unsigned kind, uint16_t want,
                         SGIL1PbayDone done, void *opaque)
{
    p->job.kind = kind;
    p->job.want = want;
    p->job.tried = 0;
    p->job.got = 0;
    p->job.pos = 0;
    p->job.serial[0] = 0;
    p->job.done = done;
    p->job.opaque = opaque;
    p->job.out = g_string_new(NULL);
    p->status = SGI_L1_PBAY_BUSY;
}

static void pb_job_end(SGIL1Pbay *p, bool ok)
{
    SGIL1PbayDone done = p->job.done;
    void *opaque = p->job.opaque;
    g_autofree char *text = g_string_free(p->job.out, FALSE);

    p->job.out = NULL;
    p->job.kind = JOB_NONE;
    p->job.done = NULL;
    p->step = STEP_IDLE;
    if (p->status == SGI_L1_PBAY_BUSY) {
        p->status = SGI_L1_PBAY_READY;
    }
    /* A request restarts the poll task's wait. */
    pb_poll_arm(p);
    if (done) {
        done(opaque, text, ok);
    }
}

static void pb_out(SGIL1Pbay *p, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_string_append_vprintf(p->job.out, fmt, ap);
    va_end(ap);
}

/* The sensor scan: read each wanted, mapped sensor in turn. */
static void pb_scan_done(SGIL1Pbay *p);
static void pb_poll_scanned(SGIL1Pbay *p);

static void pb_scan_next(SGIL1Pbay *p)
{
    uint8_t number;

    while (p->job.pos < PB_SUPPLIES + PB_PORTS &&
           !(((p->job.want >> p->job.pos) & 1) &&
             pb_sensor_at(p, p->job.pos))) {
        p->job.pos++;
    }
    if (p->job.pos >= PB_SUPPLIES + PB_PORTS) {
        pb_scan_done(p);
        return;
    }
    number = pb_sensor_at(p, p->job.pos);
    p->job.tried |= 1 << p->job.pos;
    p->step = STEP_SCAN;
    pb_tunnel(p, 0x10, 0x2d, &number, 1);
}

static void pb_scan_start(SGIL1Pbay *p)
{
    p->job.pos = 0;
    pb_scan_next(p);
}

/*
 * The `dps` table. A supply the records did not map prints nothing; one
 * whose read failed prints an error; one with bit 0 set prints only that it
 * is not present.
 */
static void pb_dps_table(SGIL1Pbay *p)
{
    pb_out(p, "DPS  AC State  AC Range  Predictive Fail  Fail\r\n"
              "---  --------  --------  ---------------  ------\r\n");
    for (unsigned s = 1; s <= PB_SUPPLIES; s++) {
        unsigned pos = s - 1;
        uint8_t v = p->job.value[pos];

        if (!((p->job.tried >> pos) & 1)) {
            continue;
        }
        if (!((p->job.got >> pos) & 1)) {
            pb_out(p, "%u    **read error**\r\n", s);
        } else if (v & 1) {
            pb_out(p, "%u    Not_Present\r\n", s);
        } else {
            pb_out(p, "%u    %-8s  %-8s  %-15s  %s\r\n", s,
                   v & 8 ? "Low" : "Ok", v & 16 ? "110Vac" : "220Vac",
                   v & 4 ? "Failing" : "Ok", v & 2 ? "Failed" : "Ok");
        }
    }
}

/* The `dcport` table; the extra text of a row is not modelled. */
static void pb_dcport_table(SGIL1Pbay *p)
{
    pb_out(p, "DcPort     State\r\n----------------------\r\n");
    for (unsigned n = 1; n <= PB_PORTS; n++) {
        unsigned pos = PB_SUPPLIES + n - 1;
        uint8_t v = p->job.value[pos];

        if (!((p->job.tried >> pos) & 1)) {
            continue;
        }
        if (!((p->job.got >> pos) & 1)) {
            pb_out(p, "%u          **read error**\r\n", n);
        } else if (v & 1) {
            pb_out(p, "%u          48V Off\r\n", n);
        } else {
            pb_out(p, "%u          48V On, %s\r\n", n,
                   v & 2 ? "Failed" : "Ok");
        }
    }
}

static void pb_scan_done(SGIL1Pbay *p)
{
    switch (p->job.kind) {
    case JOB_POLL:
        pb_poll_scanned(p);
        break;
    case JOB_DPS:
        pb_dps_table(p);
        pb_job_end(p, true);
        break;
    case JOB_DCPORT:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: pbay dcport: the text appended to "
                      "a port's row is not modelled\n");
        pb_dcport_table(p);
        pb_job_end(p, true);
        break;
    case JOB_ENV:
        qemu_log_mask(LOG_UNIMP, "sgi-l1: pbay dcport: the text appended to "
                      "a port's row is not modelled\n");
        pb_dps_table(p);
        pb_dcport_table(p);
        pb_job_end(p, true);
        break;
    default:
        g_assert_not_reached();
    }
}

static void pb_scan_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    if (d && len >= 3) {
        p->job.value[p->job.pos] = d[2];
        p->job.got |= 1 << p->job.pos;
    }
    p->job.pos++;
    pb_scan_next(p);
}

/*
 * The FRU image of a device is read as one streamed read (pair 0x28,0x11):
 * the device byte, two address bytes of zero and a count of PB_FRU_BYTES. The
 * source material does not say how the L1 sizes or addresses its reads (the
 * latch bytes' meaning and the device's addressing are unknown), so this is
 * a model choice.
 */
#define PB_FRU_BYTES    0x80

typedef struct PbFru {
    char mfg[16], name[32], part[16], version[8], serial[24];
    bool has_date;
    uint32_t minutes;
} PbFru;

static bool pb_area_ok(const uint8_t *img, unsigned len, unsigned offset)
{
    unsigned at = offset * 8, size, sum = 0;

    if (at + 2 > len || img[at] != 1) {
        return false;
    }
    size = img[at + 1] * 8;
    if (!size || at + size > len) {
        return false;
    }
    for (unsigned i = 0; i < size; i++) {
        sum += img[at + i];
    }
    return !(sum & 0xff);
}

/* One type/length field of an area; false at its end marker or its end. */
static bool pb_field(const uint8_t *area, unsigned size, unsigned *pos,
                     char *out, size_t cap)
{
    unsigned n;

    if (*pos >= size || area[*pos] == 0xc1) {
        return false;
    }
    n = area[*pos] & 0x3f;
    if (*pos + 1 + n > size) {
        return false;
    }
    for (unsigned i = 0; i < n && i + 1 < cap; i++) {
        uint8_t c = area[*pos + 1 + i];

        out[i] = c >= 0x20 && c < 0x7f ? c : '?';
        out[i + 1] = 0;
    }
    *pos += 1 + n;
    return true;
}

/*
 * Decode an IPMI-shaped FRU image (public FRU storage definition): an 8-byte
 * common header with a zero sum, then areas at 8-byte offsets, each ending in
 * a zero-sum checksum. The source material names the areas and the board
 * date but not which area each column comes from; the model takes the
 * manufacturer, product name, part number, version and serial from the
 * product area and the date from the board area, a model choice. False on a
 * checksum error.
 */
static bool pb_fru_decode(const uint8_t *img, unsigned len, PbFru *f)
{
    unsigned sum = 0;

    memset(f, 0, sizeof(*f));
    if (len < 8 || img[0] != 1) {
        return false;
    }
    for (unsigned i = 0; i < 8; i++) {
        sum += img[i];
    }
    if (sum & 0xff) {
        return false;
    }
    if (img[3]) {
        if (!pb_area_ok(img, len, img[3])) {
            return false;
        }
        f->has_date = true;
        f->minutes = img[img[3] * 8 + 3] | img[img[3] * 8 + 4] << 8 |
                     img[img[3] * 8 + 5] << 16;
    }
    if (img[4]) {
        const uint8_t *area = img + img[4] * 8;
        unsigned size = area[1] * 8, pos = 3;

        if (!pb_area_ok(img, len, img[4])) {
            return false;
        }
        if (pb_field(area, size, &pos, f->mfg, sizeof(f->mfg)) &&
            pb_field(area, size, &pos, f->name, sizeof(f->name)) &&
            pb_field(area, size, &pos, f->part, sizeof(f->part)) &&
            pb_field(area, size, &pos, f->version, sizeof(f->version))) {
            pb_field(area, size, &pos, f->serial, sizeof(f->serial));
        }
    }
    return true;
}

static void pb_fru_row(SGIL1Pbay *p, unsigned slot, const uint8_t *img,
                       unsigned len)
{
    char first[16];
    PbFru f;

    if (slot == 0) {
        pstrcpy(first, sizeof(first), "Bay");
    } else if (slot == PB_FRU_SLOTS - 1) {
        pstrcpy(first, sizeof(first), "Card");
    } else {
        snprintf(first, sizeof(first), "%u", slot);
    }
    if (!img || !pb_fru_decode(img, len, &f)) {
        pb_out(p, "%-5s %s\r\n", first,
               img ? "**FRU checksum error**" : "**read error**");
        return;
    }
    pb_out(p, "%-5s %-6.5s %-14.13s %-12.11s %-5.4s %-14.13s ", first, f.mfg,
           f.name, f.part, f.version, f.serial);
    if (f.has_date) {
        /* Minutes from 1996-01-01 UTC. */
        g_autoptr(GDateTime) t = g_date_time_new_from_unix_utc(
            820454400 + (int64_t)f.minutes * 60);

        pb_out(p, "%02d/%02d/%04d", g_date_time_get_month(t),
               g_date_time_get_day_of_month(t), g_date_time_get_year(t));
    } else {
        pb_out(p, "-");
    }
    pb_out(p, "\r\n");
}

/* Read the next wanted slot's FRU device, or finish. */
static void pb_fru_next(SGIL1Pbay *p)
{
    while (p->job.pos < PB_FRU_SLOTS &&
           !(((p->job.want >> p->job.pos) & 1) &&
             p->fru_device[p->job.pos])) {
        p->job.pos++;
    }
    if (p->job.pos >= PB_FRU_SLOTS) {
        if (p->job.kind == JOB_SERIAL) {
            g_free(p->serial);
            p->serial = g_strdup(p->job.serial);
            pb_out(p, "%s%s", p->serial, p->serial[0] ? "\r\n" : "");
        }
        pb_job_end(p, true);
        return;
    }
    {
        const uint8_t a[] = { p->fru_device[p->job.pos], 0, 0, PB_FRU_BYTES };

        p->step = STEP_FRU;
        pb_tunnel(p, 0x28, 0x11, a, sizeof(a));
    }
}

static void pb_fru_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    bool ok = d && len >= 1 + PB_FRU_BYTES && d[0] == PB_FRU_BYTES;

    if (p->job.kind == JOB_SERIAL) {
        PbFru f;

        p->job.serial[0] = 0;
        if (ok && pb_fru_decode(d + 1, PB_FRU_BYTES, &f)) {
            pstrcpy(p->job.serial, sizeof(p->job.serial), f.serial);
        }
    } else {
        pb_fru_row(p, p->job.pos, ok ? d + 1 : NULL, PB_FRU_BYTES);
    }
    p->job.pos++;
    pb_fru_next(p);
}

/*
 * The other jobs are one request: `version` (link command 0xc0) and `reset`
 * (pair 0x18,0x03).
 */
static void pb_version_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    if (!d || len < 6) {
        /* No text is known for no reply: a model choice. */
        pb_out(p, "PBay version unavailable\r\n");
        pb_job_end(p, false);
        return;
    }
    {
        /* Built time: a Unix time, UTC (inferred). */
        g_autoptr(GDateTime) t = g_date_time_new_from_unix_utc(
            lduw_le_p(d + 2) | (int64_t)lduw_le_p(d + 4) << 16);

        pb_out(p, "PBay FW Version %02d.%02d, Built %02d/%02d/%04d "
                  "%02d:%02d:%02d\r\n", d[1], d[0],
               g_date_time_get_month(t), g_date_time_get_day_of_month(t),
               g_date_time_get_year(t), g_date_time_get_hour(t),
               g_date_time_get_minute(t), g_date_time_get_second(t));
    }
    pb_job_end(p, true);
}

static void pb_reset_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    pb_out(p, d ? "Ok\r\n" : "Timeout\r\n");
    pb_job_end(p, d != NULL);
}

/*
 * A reply, or d == NULL when the transaction gave up, for the step of a
 * job.
 */
static void pb_job_reply(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    switch (p->step) {
    case STEP_SCAN:
        pb_scan_reply(p, d, len);
        break;
    case STEP_FRU:
        pb_fru_reply(p, d, len);
        break;
    case STEP_VERSION:
        pb_version_reply(p, d, len);
        break;
    case STEP_RESET:
        pb_reset_reply(p, d, len);
        break;
    default:
        g_assert_not_reached();
    }
}

static bool pb_job_step(const SGIL1Pbay *p)
{
    return p->step >= STEP_SCAN;
}

/*
 * The poll task's pass. It does nothing unless monitoring is enabled
 * (setting 85 nonzero), the status is ready and the bay semaphore is free;
 * the wait then starts again. A pass reads the supplies (when any of the
 * setting's bits 0 to 2 is set), re-reads them up to three times while an AC
 * fail is in sight, reads the ports (bit 3), re-reads them after 100 ticks
 * while a failure is in sight, then logs.
 */
enum {
    PHASE_SUPPLIES,
    PHASE_AC_REREAD,
    PHASE_PORTS,
    PHASE_PORT_REREAD,
};

#define PB_AC_REREADS       3
#define PB_PORT_REREADS     2
#define PB_PORT_REREAD_TICKS 100

static void pb_poll_read(SGIL1Pbay *p, unsigned phase)
{
    p->job.phase = phase;
    p->job.want = phase == PHASE_SUPPLIES || phase == PHASE_AC_REREAD ?
                  0x3f : 0x3fc0;
    p->job.tried = 0;
    p->job.got = 0;
    pb_scan_start(p);
}

static void pb_poll_finish(SGIL1Pbay *p)
{
    pb_evaluate(p, p->job.ac_cand, p->job.port_cand);
    p->passes++;
    pb_job_end(p, true);
}

static void pb_poll_ports(SGIL1Pbay *p)
{
    if (p->env_mask & 8) {
        pb_poll_read(p, PHASE_PORTS);
    } else {
        pb_poll_finish(p);
    }
}

/* Keep the readings of this scan; a sensor that did not answer is absent. */
static void pb_poll_take(SGIL1Pbay *p)
{
    for (unsigned s = 1; s <= PB_SUPPLIES; s++) {
        if ((p->job.tried >> (s - 1)) & 1) {
            p->supply[s] = ((p->job.got >> (s - 1)) & 1) ?
                           p->job.value[s - 1] : 1;
        }
    }
    for (unsigned n = 1; n <= PB_PORTS; n++) {
        unsigned pos = PB_SUPPLIES + n - 1;

        if ((p->job.tried >> pos) & 1) {
            p->port[n] = ((p->job.got >> pos) & 1) ? p->job.value[pos] : 1;
        }
    }
}

static void pb_poll_scanned(SGIL1Pbay *p)
{
    pb_poll_take(p);
    switch (p->job.phase) {
    case PHASE_SUPPLIES:
    case PHASE_AC_REREAD:
        if (p->job.phase == PHASE_SUPPLIES) {
            p->job.ac_cand = 0;
            p->job.reread = 0;
            for (unsigned s = 1; s <= PB_SUPPLIES && (p->env_mask & 1); s++) {
                if (!(p->supply[s] & 1) && (p->supply[s] & 8) &&
                    !(p->supply_last[s] & 8)) {
                    p->job.ac_cand |= 1 << (s - 1);
                }
            }
        } else {
            /* A supply stays a candidate while its AC fail is still there. */
            for (unsigned s = 1; s <= PB_SUPPLIES; s++) {
                if ((p->supply[s] & 9) != 8) {
                    p->job.ac_cand &= ~(1 << (s - 1));
                }
            }
            p->job.reread++;
        }
        if (p->job.ac_cand && p->job.reread < PB_AC_REREADS) {
            pb_poll_read(p, PHASE_AC_REREAD);
        } else {
            pb_poll_ports(p);
        }
        break;
    case PHASE_PORTS:
    case PHASE_PORT_REREAD:
        if (p->job.phase == PHASE_PORTS) {
            p->job.port_cand = 0;
            p->job.reread = 0;
            for (unsigned n = 1; n <= PB_PORTS; n++) {
                if ((p->port[n] & 3) == 2 && !(p->port_last[n] & 2)) {
                    p->job.port_cand |= 1 << (n - 1);
                }
            }
        } else {
            for (unsigned n = 1; n <= PB_PORTS; n++) {
                if ((p->port[n] & 3) != 2) {
                    p->job.port_cand &= ~(1 << (n - 1));
                }
            }
            p->job.reread++;
        }
        if (p->job.port_cand && p->job.reread < PB_PORT_REREADS) {
            p->job.phase = PHASE_PORT_REREAD;
            pb_wait(p, WAIT_REREAD, PB_PORT_REREAD_TICKS);
        } else {
            pb_poll_finish(p);
        }
        break;
    }
}

static void pb_poll_start(void *opaque)
{
    SGIL1Pbay *p = opaque;

    if (!p->env_mask || p->status != SGI_L1_PBAY_READY ||
        p->step != STEP_IDLE) {
        pb_poll_arm(p);
        return;
    }
    pb_job_begin(p, JOB_POLL, 0, NULL, NULL);
    p->job.ac_cand = 0;
    p->job.port_cand = 0;
    /* What is not read stays absent. */
    for (unsigned s = 1; s <= PB_SUPPLIES; s++) {
        p->supply[s] = 1;
    }
    memset(p->port, 1, sizeof(p->port));
    if (p->env_mask & 7) {
        pb_poll_read(p, PHASE_SUPPLIES);
    } else {
        pb_poll_ports(p);
    }
}

/*
 * Take what a record tells the L1: the sensors behind the supplies and DC
 * ports (a sensor record of entity 0x0a and type 8, or of entity 0x15 and
 * type 9, whose sensor numbers run on from its own; the records do not
 * say how many, so the model takes six and eight), and the FRU device of
 * each slot (a FRU locator of entity 0x0a instances 1 to 6, 0xb0 for slot 0
 * and 0x15 for the seventh slot). @r is the record after its id.
 */
static void pb_record(SGIL1Pbay *p, const uint8_t *r, unsigned len)
{
    if (len < 12) {
        return;
    }
    switch (r[1]) {
    case REC_SENSOR_FULL:
    case REC_SENSOR_COMPACT:
        if (r[6] == ENTITY_SUPPLY && r[10] == SENSOR_SUPPLY) {
            for (unsigned i = 1; i <= PB_SUPPLIES; i++) {
                p->supply_sensor[i] = r[5] + i - 1;
            }
        } else if (r[6] == ENTITY_PORT && r[10] == SENSOR_PORT) {
            for (unsigned i = 1; i <= PB_PORTS; i++) {
                p->port_sensor[i] = r[5] + i - 1;
            }
        }
        break;
    case REC_FRU_LOCATOR:
        if (r[10] == ENTITY_SUPPLY && r[11] >= 1 && r[11] <= PB_SUPPLIES) {
            p->fru_device[r[11]] = r[4];
        } else if (r[10] == ENTITY_BAY) {
            p->fru_device[0] = r[4];
        } else if (r[10] == ENTITY_PORT) {
            p->fru_device[PB_FRU_SLOTS - 1] = r[4];
        }
        break;
    }
}

/* @d is the tunnelled reply's data, from its first byte after the codes. */
static void pb_step_done(SGIL1Pbay *p, const uint8_t *d, unsigned len)
{
    timer_del(p->timeout);
    p->wait = WAIT_NONE;
    if (pb_job_step(p)) {
        pb_job_reply(p, d, len);
        return;
    }
    switch (p->step) {
    case STEP_ADDRESS:
        p->bay = len >= 2 ? lduw_le_p(d) : 0;
        if (!p->bay) {
            /* No bay answered: the L1 falls back to the usual address. */
            p->bay = PB_ADDRESS;
            pb_fail(p);
            return;
        }
        p->step = STEP_ASSIGN;
        {
            uint8_t a[2];

            stw_le_p(a, p->bay);
            pb_request(p, p->bay, CMD_ASSIGN, a, 2, true);
        }
        break;
    case STEP_ASSIGN:
        p->step = STEP_TYPE;
        pb_request(p, p->bay, CMD_TYPE, NULL, 0, true);
        break;
    case STEP_TYPE:
        p->target = len >= 1 ? d[0] : 0x22;
        p->step = STEP_CAPABILITY;
        pb_tunnel(p, 0x00, 0x00, NULL, 0);
        break;
    case STEP_CAPABILITY:
        if (len < 4) {
            pb_fail(p);
            return;
        }
        memcpy(p->capability, d + 1, 3);
        /*
         * Byte 1, when nonzero, is the fallback FRU device of slot 0 until a
         * locator for entity 0xb0 replaces it. No record has been read yet.
         */
        if (p->capability[0]) {
            p->fru_device[0] = p->capability[0];
        }
        if (p->capability[1]) {
            p->record_id = 0;
            p->records = 0;
            p->step = STEP_RECORD_ID;
            {
                const uint8_t a[] = { 0, 0, 0, 0, 0, 5 };

                pb_tunnel(p, 0x28, 0x23, a, sizeof(a));
            }
            break;
        }
        goto ready;
    case STEP_RECORD_ID:
        if (len < 4) {
            pb_fail(p);
            return;
        }
        /* The first reply bytes are the next id; the record's own follow. */
        p->record_id = lduw_le_p(d + 2);
        p->step = STEP_RECORD;
        {
            uint8_t a[] = { 0, 0, p->record_id & 0xff, p->record_id >> 8,
                            0, 0xff };

            pb_tunnel(p, 0x28, 0x23, a, sizeof(a));
        }
        break;
    case STEP_RECORD: {
        uint16_t next;

        if (len < 2) {
            pb_fail(p);
            return;
        }
        next = lduw_le_p(d);
        if (len > 4) {
            pb_record(p, d + 4, len - 4);
        }
        p->records++;
        if (next == 0xffff || p->records >= PB_MAX_RECORDS) {
            goto ready;
        }
        {
            uint8_t a[] = { 0, 0, next & 0xff, next >> 8, 0, 5 };

            p->step = STEP_RECORD_ID;
            pb_tunnel(p, 0x28, 0x23, a, sizeof(a));
        }
        break;
    }
    default:
        break;
    }
    return;

ready:
    p->step = STEP_IDLE;
    p->status = SGI_L1_PBAY_READY;
    if (p->job.kind == JOB_INIT) {
        pb_out(p, "PBay ready\r\n");
        pb_job_end(p, true);
    } else {
        pb_poll_arm(p);
    }
}

/* No reply came in time: the next transmission, or give up. */
static void pb_read_expired(SGIL1Pbay *p)
{
    if (p->transmissions >= PB_TRANSMISSIONS) {
        if (pb_job_step(p)) {
            pb_job_reply(p, NULL, 0);
            return;
        }
        if (p->step == STEP_ADDRESS) {
            p->bay = PB_ADDRESS;
        }
        pb_fail(p);
        return;
    }
    pb_wait(p, WAIT_RESEND, PB_RESEND_TICKS);
}

/* A frame that failed the reply checks costs a read; no resend yet. */
static void pb_bad_reply(SGIL1Pbay *p)
{
    if (--p->reads == 0) {
        timer_del(p->timeout);
        pb_read_expired(p);
        return;
    }
    pb_wait(p, WAIT_READ, PB_READ_TICKS);
}

/*
 * A received frame, unstuffed and addressed to this L1. The transaction
 * layer checks the checksum, the control byte and tag, the command and the
 * completion codes, and for a tunnelled reply the inner message.
 */
static void pb_frame(SGIL1Pbay *p)
{
    uint8_t *f = p->rx;
    unsigned body, data_at;
    uint8_t want_tag = p->request[5] & 0x1f;

    if (p->step == STEP_IDLE || p->wait != WAIT_READ) {
        return;
    }
    if (p->rx_len < PB_HEADER + 1 + 2) {
        pb_bad_reply(p);
        return;
    }
    body = p->rx_len - 2;
    if (lduw_le_p(f + body) != pb_checksum(f, body) ||
        !(f[5] & PB_REPLY_BIT) || (f[5] & 0x1f) != want_tag ||
        f[6] != p->request_cmd || f[7] != 0) {
        pb_bad_reply(p);
        return;
    }
    data_at = 8;
    if (p->request_cmd == CMD_TUNNEL) {
        uint8_t reply_nf, want_nf = p->request_netfn >> 2;

        if (body < 12 || f[11] != 0 || f[10] != p->request[PB_HEADER + 2]) {
            pb_bad_reply(p);
            return;
        }
        reply_nf = f[9];
        if (!(reply_nf & 4) || ((reply_nf >> 2) & ~1) != (want_nf & ~1)) {
            pb_bad_reply(p);
            return;
        }
        data_at = 12;
    }
    pb_step_done(p, f + data_at, body - data_at);
}

static void pb_rx_byte(SGIL1Pbay *p, uint8_t b)
{
    if (!p->rx_in_frame) {
        /*
         * Outside a frame a stray escape byte sets the escape state, which
         * swallows the next start byte and everything until a terminator.
         */
        if (b == PB_END) {
            p->rx_escape = false;
        } else if (b == PB_ESCAPE) {
            p->rx_escape = true;
        } else if (b == PB_START && !p->rx_escape) {
            p->rx_in_frame = true;
            p->rx_len = 0;
            p->rx_escape = false;
            p->rx_drop = false;
        }
        return;
    }
    if (b == PB_END) {
        /* A terminator ends an escape too, as it does outside a frame. */
        p->rx_in_frame = false;
        p->rx_escape = false;
        if (!p->rx_drop && p->rx_len) {
            pb_frame(p);
        }
        return;
    }
    if (p->rx_drop || b == PB_START) {
        /* A bare start byte inside a frame is dropped, not a restart. */
        return;
    }
    if (p->rx_escape) {
        p->rx_escape = false;
        switch (b) {
        case PB_ESC_START:
            b = PB_START;
            break;
        case PB_ESC_END:
            b = PB_END;
            break;
        case PB_ESC_ESCAPE:
            b = PB_ESCAPE;
            break;
        default:
            return;
        }
    } else if (b == PB_ESCAPE) {
        p->rx_escape = true;
        return;
    }
    if (p->rx_len == sizeof(p->rx)) {
        p->rx_drop = true;
        return;
    }
    p->rx[p->rx_len++] = b;
    if (p->rx_len == 2) {
        uint16_t dst = lduw_le_p(p->rx);

        /* The destination must be 0x0000 or this L1's own address. */
        if (dst && dst != p->l1_address) {
            p->rx_drop = true;
        }
    }
}

static int pb_can_receive(void *opaque)
{
    return 256;
}

static void pb_receive(void *opaque, const uint8_t *buf, int size)
{
    SGIL1Pbay *p = opaque;

    for (int i = 0; i < size; i++) {
        pb_rx_byte(p, buf[i]);
    }
}

static void pb_timeout(void *opaque)
{
    SGIL1Pbay *p = opaque;
    unsigned kind = p->wait;

    p->wait = WAIT_NONE;
    switch (kind) {
    case WAIT_START:
        pb_begin(p);
        break;
    case WAIT_READ:
        if (p->step != STEP_IDLE) {
            pb_read_expired(p);
        }
        break;
    case WAIT_REREAD:
        pb_poll_read(p, PHASE_PORT_REREAD);
        break;
    case WAIT_RESEND:
        if (p->step != STEP_IDLE) {
            p->transmissions++;
            p->reads = PB_READS;
            pb_write(p);
            pb_wait(p, WAIT_READ, PB_READ_TICKS);
        }
        break;
    }
}

/* The sequence: a silent request, the address, then the rest. */
static void pb_begin(SGIL1Pbay *p)
{
    memset(p->supply_sensor, 0, sizeof(p->supply_sensor));
    memset(p->port_sensor, 0, sizeof(p->port_sensor));
    memset(p->fru_device, 0, sizeof(p->fru_device));
    pb_request(p, 0, CMD_SILENT, NULL, 0, false);
    p->step = STEP_ADDRESS;
    pb_request(p, 0, CMD_ADDRESS, NULL, 0, true);
}

/*
 * Task start: status 1, the port opens, and the task waits
 * 100 * bay + 300 ticks before its first request.
 */
static void pb_start(void *opaque)
{
    SGIL1Pbay *p = opaque;

    p->status = SGI_L1_PBAY_INITIALISING;
    p->rx_in_frame = false;
    p->rx_escape = false;
    pb_wait(p, WAIT_START, PB_START_TICKS(p->bay_number));
}

bool sgi_l1_pbay_active(const SGIL1Pbay *p)
{
    return p->start_bh != NULL;
}

/*
 * The brick types whose L1 takes the pbay command. Y and & fold to X and N in
 * the L1; both forms are in the string.
 */
#define PB_COMMAND_BRICKS   "C356P\x25#=IXYRN&!"    /* 0x25 is a percent sign */

static bool pb_digits(const char *text, unsigned limit, uint16_t *mask)
{
    *mask = 0;
    for (; *text; text++) {
        if (*text < '1' || *text > '0' + limit) {
            return false;
        }
        *mask |= 1 << (*text - '1');
    }
    return *mask != 0;
}

static void pb_command_done(SGIL1PbayDone done, void *opaque,
                            const char *text, bool ok)
{
    done(opaque, text, ok);
}

int sgi_l1_pbay_command(SGIL1Pbay *p, int argc, char **argv, bool can_wait,
                        SGIL1PbayDone done, void *opaque)
{
    const char *sub = argc > 1 ? argv[1] : NULL;
    uint16_t want = 0;
    unsigned kind = JOB_NONE;
    bool bare = argc == 1, init = false, env_set = false;
    bool env_on = false;
    g_autofree char *text = NULL;

    if (!p->letter || !strchr(PB_COMMAND_BRICKS, p->letter)) {
        return SGI_L1_PBAY_REFUSED;
    }
    if (bare) {
        /* Parsed below with the state check. */
    } else if (!strcmp(sub, "version") && argc == 2) {
        kind = JOB_VERSION;
    } else if (!strcmp(sub, "dps") && argc <= 3) {
        kind = JOB_DPS;
        want = 0x3f;
        if (argc == 3 && !pb_digits(argv[2], PB_SUPPLIES, &want)) {
            return SGI_L1_PBAY_BAD_ARGS;
        }
    } else if (!strcmp(sub, "dcport") && argc <= 3) {
        uint16_t ports = 0xff;

        kind = JOB_DCPORT;
        if (argc == 3 && !pb_digits(argv[2], PB_PORTS, &ports)) {
            return SGI_L1_PBAY_BAD_ARGS;
        }
        want = ports << PB_SUPPLIES;
    } else if (!strcmp(sub, "fru") && argc <= 3) {
        int slot;

        kind = JOB_FRU;
        want = 0x7f;
        if (argc == 3) {
            /* A negative argument selects the seventh device. */
            if (qemu_strtoi(argv[2], NULL, 10, &slot) < 0 || slot > 6) {
                return SGI_L1_PBAY_BAD_ARGS;
            }
            want = 1 << (slot < 0 ? PB_FRU_SLOTS - 1 : slot);
        }
    } else if (!strcmp(sub, "reset") && argc == 2) {
        kind = JOB_RESET;
    } else if (!strcmp(sub, "env") && argc <= 3) {
        if (argc == 3) {
            if (strcmp(argv[2], "on") && strcmp(argv[2], "off")) {
                return SGI_L1_PBAY_BAD_ARGS;
            }
            env_set = true;
            env_on = !strcmp(argv[2], "on");
        } else {
            kind = JOB_ENV;
            want = 0x3fff;
        }
    } else if (!strcmp(sub, "init") && argc == 2) {
        init = true;
    } else if (!strcmp(sub, "serial") && argc == 2) {
        kind = JOB_SERIAL;
        want = 1;
    } else {
        return SGI_L1_PBAY_BAD_ARGS;
    }

    if (init) {
        if (p->status != SGI_L1_PBAY_COMM_FAILURE) {
            pb_command_done(done, opaque, "PBay already initialized\r\n",
                            true);
            return SGI_L1_PBAY_DONE;
        }
        if (!can_wait) {
            goto no_wait;
        }
        /*
         * The start-up runs again from the initialisation order. Whether
         * the task's start delay applies again is not stated: the model
         * skips it, a model choice.
         */
        pb_job_begin(p, JOB_INIT, 0, done, opaque);
        p->status = SGI_L1_PBAY_INITIALISING;
        g_free(p->serial);
        p->serial = NULL;
        pb_begin(p);
        return SGI_L1_PBAY_PENDING;
    }
    if (p->status != SGI_L1_PBAY_READY || p->step != STEP_IDLE) {
        /* A poll in progress holds the semaphore. */
        text = g_strdup_printf("%s\r\n", p->status == SGI_L1_PBAY_READY ?
                               "PBay busy try again later" :
                               pb_state_text(p));
        pb_command_done(done, opaque, text, false);
        return SGI_L1_PBAY_DONE;
    }
    if (env_set) {
        p->env_mask = env_on ? 0x0f : 0;
        pb_command_done(done, opaque, "", true);
        return SGI_L1_PBAY_DONE;
    }
    if (bare) {
        qemu_log_mask(LOG_UNIMP, "sgi-l1: pbay: the per-device current "
                      "figures are not modelled; their source is unknown\n");
        pb_command_done(done, opaque, "", true);
        return SGI_L1_PBAY_DONE;
    }
    if (kind == JOB_SERIAL && p->serial) {
        text = g_strdup_printf("%s%s", p->serial, p->serial[0] ? "\r\n" : "");
        pb_command_done(done, opaque, text, true);
        return SGI_L1_PBAY_DONE;
    }
    if (!can_wait) {
        goto no_wait;
    }

    pb_job_begin(p, kind, want, done, opaque);
    switch (kind) {
    case JOB_VERSION:
        p->step = STEP_VERSION;
        pb_request(p, p->bay, 0xc0, NULL, 0, true);
        break;
    case JOB_RESET:
        p->step = STEP_RESET;
        pb_tunnel(p, 0x18, 0x03, NULL, 0);
        break;
    case JOB_ENV:
        pb_out(p, "Power Bay env monitoring %s\r\n\r\n",
               p->env_mask ? "enabled" : "disabled");
        pb_scan_start(p);
        break;
    case JOB_DPS:
    case JOB_DCPORT:
        pb_scan_start(p);
        break;
    case JOB_FRU:
        pb_out(p, "Dps  Mfg   Product Name  Part Number Rev  Serial        "
                  "Mfg Date\r\n"
                  "---- ----- ------------- ----------- ---- ------------- "
                  "--------\r\n");
        /* fall through */
    default:
        p->job.pos = 0;
        pb_fru_next(p);
        break;
    }
    return SGI_L1_PBAY_PENDING;

no_wait:
    qemu_log_mask(LOG_UNIMP, "sgi-l1: pbay: a command that talks to the bay "
                  "cannot run through a relayed command\n");
    pb_command_done(done, opaque,
                    "ERROR: pbay is not available here\r\n", false);
    return SGI_L1_PBAY_DONE;
}

void sgi_l1_pbay_reset(SGIL1Pbay *p)
{
    if (!sgi_l1_pbay_active(p)) {
        return;
    }
    if (p->job.kind != JOB_NONE) {
        /* The link goes down under a command that was waiting for it. */
        p->status = SGI_L1_PBAY_NOT_INITIALISED;
        pb_out(p, "%s\r\n", pb_state_text(p));
        pb_job_end(p, false);
    }
    timer_del(p->timeout);
    timer_del(p->poll_timer);
    p->wait = WAIT_NONE;
    p->step = STEP_IDLE;
    p->status = SGI_L1_PBAY_NOT_INITIALISED;
    p->records = 0;
    /* Setting 105 (p->present) and setting 85 are the L1's: they stay. */
    memset(p->supply_last, 0, sizeof(p->supply_last));
    memset(p->port_last, 0, sizeof(p->port_last));
    qemu_bh_schedule(p->start_bh);
}

void sgi_l1_pbay_init(Object *obj, SGIL1Pbay *p)
{
    object_property_add_uint8_ptr(obj, "pbay-status", &p->status,
                                  OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "pbay-records", &p->records,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint32_ptr(obj, "pbay-passes", &p->passes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint8_ptr(obj, "pbay-fru-slot0", &p->fru_device[0],
                                  OBJ_PROP_FLAG_READ);
}

bool sgi_l1_pbay_realize(SGIL1Pbay *p, Object *owner, uint32_t letter,
                         SGIL1PbayLog log, void *opaque, Error **errp)
{
    if (!qemu_chr_fe_backend_connected(&p->chr)) {
        return true;
    }
    if (pb_port_closed(letter)) {
        if (letter == 'G') {
            error_setg(errp, "sgi-l1: the power-bay UART is the 38400 baud "
                       "console on brick type '%c'", letter);
        } else {
            error_setg(errp, "sgi-l1: no power-bay task runs on brick "
                       "type '%c'", letter);
        }
        return false;
    }
    if (!p->tick_us) {
        error_setg(errp, "sgi-l1: pbay-tick-us must be at least 1");
        return false;
    }
    /* The bay's state is outside this machine, so it is not saved. */
    error_setg(&p->blocker,
               "sgi-l1: migration is not supported with a power-bay link");
    if (migrate_add_blocker(&p->blocker, errp) < 0) {
        return false;
    }
    p->log = log;
    p->opaque = opaque;
    p->letter = letter;
    p->timeout = timer_new_ns(QEMU_CLOCK_REALTIME, pb_timeout, p);
    p->poll_timer = timer_new_ms(QEMU_CLOCK_REALTIME, pb_poll_start, p);
    p->start_bh = qemu_bh_new(pb_start, p);
    qemu_chr_fe_set_handlers(&p->chr, pb_can_receive, pb_receive, NULL,
                             NULL, p, NULL, true);
    qemu_bh_schedule(p->start_bh);
    return true;
}

void sgi_l1_pbay_finalize(SGIL1Pbay *p)
{
    if (p->blocker) {
        migrate_del_blocker(&p->blocker);
    }
    g_free(p->serial);
    p->serial = NULL;
    if (p->job.out) {
        g_string_free(p->job.out, TRUE);
        p->job.out = NULL;
    }
    if (p->start_bh) {
        qemu_bh_delete(p->start_bh);
        p->start_bh = NULL;
        timer_free(p->timeout);
        timer_free(p->poll_timer);
    }
}
