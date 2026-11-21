/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Madison cache-array port, an approximation of what the Altix 350 PAL
 * self-test needs.
 */

#include "qemu/osdep.h"
#include "qemu/bitops.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "system/memory.h"
#include "qemu/main-loop.h"
#include "qapi/error.h"
#include "exec/cputlb.h"
#include "cpu.h"
#include "arch/arch.h"
#include "arch/system.h"
#include "trace.h"

typedef struct IA64CacheLine {
    uint64_t data[16];
    uint64_t check[16];
    uint64_t tag;       /* addr[49:15] */
    uint64_t tag_meta;  /* valid, parity and state: tag write word 43:35 */
    bool has_data;      /* written through array 8, not only tagged */
} IA64CacheLine;

static void l3_store(CPUIA64State *env, uint64_t pa);
static void l3_flush(IA64CPU *cpu, uint64_t pa);

/* Hsiao (39,32) columns: check bit i is the parity of data32 & columns[i]. */
static const uint32_t hsiao_columns[7] = {
    0x0003c3ff, 0x12c87c0f, 0x25549c71, 0x49a42592,
    0x8e392aa4, 0xf03e1348, 0xffc3c000,
};

#define CA_CHECK_LO_SHIFT 13
#define CA_CHECK_HI_SHIFT 45
#define CA_CHECK_MASK ((0x7fULL << CA_CHECK_LO_SHIFT) | \
                       (0x7fULL << CA_CHECK_HI_SHIFT))
#define CA_TAG_BITS 35
#define CA_TAG_MASK ((1ULL << CA_TAG_BITS) - 1)
#define CA_TAG_READ_SHIFT 19    /* tag in MSR 1545 bits 53:19 */
#define CA_TAG_READ_PARITY_SHIFT 5  /* parity in MSR 1545 bits 8:5 */
#define CA_TAG_READ_REPL_SHIFT 9    /* replacement mask in bits 16:9 */
#define CA_STATE_SHIFT 5            /* state in tag_meta bits 8:5 */
#define CA_STATE_STORED 7
#define CA_MSR_TAG_READ 1545
#define CA_MSR_TAG_LOG 1509
#define CA_LOG_TAG 0x4005ULL
#define CA_LOG_SINGLE 0x100001ULL
#define CA_LOG_DOUBLE 0x080001ULL

/* The staging MSR of dword j: MSR 0x640 + 8 * (j / 4) + j % 4. */
static uint64_t stage_msr(unsigned j)
{
    return 1600 + 8 * (j >> 2) + (j & 3);
}

static uint64_t half_check(uint32_t d)
{
    uint64_t check = 0;

    for (unsigned i = 0; i < ARRAY_SIZE(hsiao_columns); i++) {
        check |= (uint64_t)(ctpop32(d & hsiao_columns[i]) & 1) << i;
    }
    return check;
}

static uint64_t line_check(uint64_t data)
{
    return (half_check(data) << CA_CHECK_LO_SHIFT) |
           (half_check(data >> 32) << CA_CHECK_HI_SHIFT);
}

/* The tag write word carries the tag bit-reversed; the read-back does not. */
static uint64_t tag_reverse(uint64_t tag)
{
    return revbit64(tag) >> (64 - CA_TAG_BITS);
}

/* Parity bit i covers a slice of the tag; the valid bit joins slice 0. */
static uint64_t tag_parity(uint64_t tag)
{
    uint64_t p = ctpop64(tag & 0xff) & 1;

    p |= (ctpop64(tag >> 8 & 0x1ff) & 1) << 1;
    p |= (ctpop64(tag >> 17 & 0x1ff) & 1) << 2;
    p |= (ctpop64(tag >> 26 & 0x1ff) & 1) << 3;
    return p;
}

/* A valid tag whose stored parity disagrees with its fields. */
static bool tag_bad(const IA64CacheLine *line)
{
    uint64_t parity = tag_parity(line->tag) ^ (line->tag_meta & 1);

    return (line->tag_meta & 1) && (line->tag_meta >> 1 & 0xf) != parity;
}

/* Read guest RAM only: a device register must not be read for the model. */
static bool ca_read_ram(IA64CPU *cpu, uint64_t pa, void *buf, hwaddr size)
{
    hwaddr xlat, len = size;
    MemoryRegion *mr;

    {
        RCU_READ_LOCK_GUARD();
        mr = address_space_translate(CPU(cpu)->as, pa, &xlat, &len, false,
                                     MEMTXATTRS_UNSPECIFIED);
        if (!memory_region_is_ram(mr) || len < size) {
            return false;
        }
    }
    return address_space_read(CPU(cpu)->as, pa, MEMTXATTRS_UNSPECIFIED, buf,
                              size) == MEMTX_OK;
}

static void line_free(gpointer p)
{
    g_free(p);
}

#define CA_SETS 2048

static unsigned set_of(uint64_t pa)
{
    return ((pa >> 7) & 0xff) | (((pa >> 15) & 7) << 8);
}

void ia64_cache_array_reset(IA64CPU *cpu)
{
    if (cpu->ca_lines) {
        g_hash_table_remove_all(cpu->ca_lines);
    }
    if (cpu->ca_mirror) {
        memset(cpu->ca_mirror, 0, CA_SETS * sizeof(uint64_t));
    }
    memset(cpu->ca_flip, 0, sizeof(cpu->ca_flip));
    memset(cpu->ca_bad, 0, sizeof(cpu->ca_bad));
    memset(cpu->ca_repl, 0, sizeof(cpu->ca_repl));
    memset(cpu->ca_held, 0, sizeof(cpu->ca_held));
    if (cpu->ca_l3) {
        g_hash_table_remove_all(cpu->ca_l3);
    }
    memset(cpu->ca_l3_held, 0, sizeof(cpu->ca_l3_held));
    ia64_cpu_drop_l3_ways(cpu);
}

/* The PAL starts each sub-test with a write of MSR 1552 bit 4. */
void ia64_cache_array_restart(CPUIA64State *env)
{
    memset(env_archcpu(env)->ca_repl, 0, sizeof(env_archcpu(env)->ca_repl));
}

/* A line of the address's set that hits: data written, tag matching, valid. */
static IA64CacheLine *tag_hit(IA64CPU *cpu, uint64_t pa, unsigned *column)
{
    uint64_t tag = (pa >> 15) & CA_TAG_MASK;

    if (!cpu->ca_held[pa >> 7 & 0xff]) {
        return NULL;
    }
    for (unsigned col = 0; col < 8; col++) {
        uint64_t index = (pa >> 7 & 0xff) | (uint64_t)col << 8;
        IA64CacheLine *line = g_hash_table_lookup(cpu->ca_lines, &index);

        if (line && line->has_data && (line->tag_meta & 1) &&
            line->tag == tag) {
            *column = col;
            return line;
        }
    }
    return NULL;
}

/* fc drops the data of every line that hits; tags and state stay. */
void ia64_cache_array_flush(CPUIA64State *env, uint64_t pa)
{
    IA64CPU *cpu = env_archcpu(env);
    IA64CacheLine *hit;
    unsigned column;

    /* A hit needs data, so each pass finds the next matching column. */
    while (cpu->ca_lines && (hit = tag_hit(cpu, pa, &column))) {
        hit->has_data = false;
    }
    l3_flush(cpu, pa);
}

/* An access to a set holding a valid tag with wrong parity logs it. */
static void tag_parity_check(CPUIA64State *env, uint64_t pa)
{
    IA64CPU *cpu = env_archcpu(env);

    if (!cpu->ca_bad[(pa >> 7) & 0xff]) {
        return;
    }
    warn_report_once("ia64: MSR 1509 reports a cache tag parity error (an"
                     " approximation, first access of 0x%" PRIx64 ")", pa);
    trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, CA_MSR_TAG_LOG,
                                        CA_LOG_TAG, env->ip);
    ia64_system_store_msr(env, CA_MSR_TAG_LOG, CA_LOG_TAG);
}

/*
 * Record a store's line once the port is in use: a hit updates the dword from
 * RAM, sets state 7 and marks its column in the set's replacement mask.
 */
void ia64_cache_array_store(CPUIA64State *env, uint64_t pa)
{
    IA64CPU *cpu = env_archcpu(env);
    IA64CacheLine *hit;
    unsigned column;
    uint8_t buf[8];

    if (!cpu->ca_mirror) {
        return;
    }
    cpu->ca_mirror[set_of(pa)] = (pa & ~127ULL) | 1;
    tag_parity_check(env, pa);
    l3_store(env, pa);
    hit = cpu->ca_lines ? tag_hit(cpu, pa, &column) : NULL;
    if (hit && ca_read_ram(cpu, pa & ~7ULL, buf, sizeof(buf))) {
        unsigned j = (pa >> 3) & 15;

        hit->data[j] = ldq_le_p(buf);
        hit->check[j] = line_check(hit->data[j]);
        hit->tag_meta = (hit->tag_meta & ~(0xfULL << CA_STATE_SHIFT)) |
                        (uint64_t)CA_STATE_STORED << CA_STATE_SHIFT;
        /* The mask starts over once every column has been stored to. */
        cpu->ca_repl[(pa >> 7) & 0xff] |= 1 << column;
        if (cpu->ca_repl[(pa >> 7) & 0xff] == 0xff) {
            cpu->ca_repl[(pa >> 7) & 0xff] = 0;
        }
    }
}

/*
 * A line the port does not hold reads as RAM of the latest line stored to its
 * set (the PAL stores, then reads back). Returns false if none.
 */
static bool line_from_ram(IA64CPU *cpu, uint64_t index, IA64CacheLine *out)
{
    uint64_t la;
    uint8_t buf[128];

    if (!cpu->ca_mirror) {
        return false;
    }
    la = cpu->ca_mirror[index & (CA_SETS - 1)];
    if (!la) {
        return false;
    }
    la &= ~1ULL;
    if (!ca_read_ram(cpu, la, buf, sizeof(buf))) {
        return false;
    }
    for (unsigned j = 0; j < 16; j++) {
        out->data[j] = ldq_le_p(buf + 8 * j);
        out->check[j] = line_check(out->data[j]);
    }
    out->tag = (la >> 15) & CA_TAG_MASK;
    return true;
}

static IA64CacheLine *line_get(IA64CPU *cpu, uint64_t index, bool create)
{
    IA64CacheLine *line;

    if (!cpu->ca_lines) {
        cpu->ca_lines = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                              g_free, line_free);
    }
    line = g_hash_table_lookup(cpu->ca_lines, &index);
    if (!line && create) {
        uint64_t *key = g_new(uint64_t, 1);

        *key = index;
        line = g_new0(IA64CacheLine, 1);
        g_hash_table_insert(cpu->ca_lines, key, line);
    }
    return line;
}

/*
 * Second array family (PAL group 11): arrays 4 (data) and 6 (tag) on the same
 * MSR port; commands and tag word are a software expectation read from the PAL.
 */
typedef struct IA64L3Line {
    uint64_t data[16];
    uint16_t check[4];
    uint32_t tag;       /* addr[49:18] */
    uint32_t extra;     /* 24 bits, MSR 1545 bits 21:10 and MSR 1547 */
    uint8_t ecc;        /* tag code, held as written */
    uint8_t state;      /* 0 invalid */
    bool has_data;
} IA64L3Line;

#define L3_MSR_TAG 1545
#define L3_MSR_TAG_HIGH 1547
#define L3_MSR_LOG 1558
#define L3_LOG_VALID (1ULL << 13)
#define L3_LOG_SOURCE (2ULL << 26)
#define L3_LOG_SINGLE (10ULL << 16)
#define L3_LOG_DOUBLE (11ULL << 16)
#define L3_CMD_FLAG_AS_IS (1ULL << 51)
#define L3_CMD_FLAG_GENERATE (1ULL << 52)

/* Check bit k is the parity of the four dwords ANDed with row k. */
static const uint64_t l3_code[10][4] = {
    { 0xff2492787d88db6dULL, 0x8009249001249249ULL,
      0xf90db6db6c924924ULL, 0x9249249601240049ULL },
    { 0xff4924b8be896db6ULL, 0x8012492002492492ULL,
      0xfa16db6db5249249ULL, 0x2492492a02480092ULL },
    { 0xff924938df09b6dbULL, 0x0024924004924924ULL,
      0xfc1b6db6da492492ULL, 0x4924924c04900124ULL },
    { 0x24ff81df03b40003ULL, 0xb6c7e002481c0e07ULL,
      0x472070381c7e3f1fULL, 0x81c0e070c01c9200ULL },
    { 0x49ff8e2f03d4001cULL, 0x5b78e00490e07038ULL,
      0x874381c0e38fc7e3ULL, 0x8e07038140e12400ULL },
    { 0x92fff03703e400e0ULL, 0x6dbf0009270381c0ULL,
      0x079c0e0703f1f8fcULL, 0x70381c0187024800ULL },
    { 0x0303ffc3ec720700ULL, 0x01c003f1f80001ffULL,
      0x08e00007fc0000ffULL, 0x803ffff018000e07ULL },
    { 0x1c1c7fc5f4723800ULL, 0x0e001c7e3803fe00ULL,
      0x10e00ff80001ff00ULL, 0x7fc01ff028007038ULL },
    { 0xe0e07fc6f873c000ULL, 0x70001f8fc7fc0000ULL,
      0x20fff00003fe0000ULL, 0x7fffe000300381c0ULL },
    { 0x0000000000000000ULL, 0x0000000000000000ULL,
      0xffffffffffffffffULL, 0xffffffffffffffffULL },
};

static unsigned l3_granule_check(const uint64_t *d)
{
    unsigned check = 0;

    for (unsigned k = 0; k < ARRAY_SIZE(l3_code); k++) {
        uint64_t x = (d[0] & l3_code[k][0]) ^ (d[1] & l3_code[k][1]) ^
                     (d[2] & l3_code[k][2]) ^ (d[3] & l3_code[k][3]);

        check |= (ctpop64(x) & 1) << k;
    }
    return check;
}

static IA64L3Line *l3_get(IA64CPU *cpu, uint64_t key, bool create)
{
    IA64L3Line *line;

    if (!cpu->ca_l3) {
        cpu->ca_l3 = g_hash_table_new_full(g_int64_hash, g_int64_equal,
                                           g_free, g_free);
    }
    line = g_hash_table_lookup(cpu->ca_l3, &key);
    if (!line && create) {
        uint64_t *k = g_new(uint64_t, 1);

        *k = key;
        line = g_new0(IA64L3Line, 1);
        g_hash_table_insert(cpu->ca_l3, k, line);
    }
    return line;
}

/* The line of a set that holds the address: data, valid state, same tag. */
static IA64L3Line *l3_hit(IA64CPU *cpu, uint64_t pa)
{
    uint32_t tag = pa >> 18;
    uint64_t set = pa >> 7 & 0x7ff;

    if (!cpu->ca_l3_held[set]) {
        return NULL;
    }
    for (unsigned way = 0; way < 32; way++) {
        uint64_t key = set | (uint64_t)way << 11;
        IA64L3Line *line = g_hash_table_lookup(cpu->ca_l3, &key);

        if (line && line->has_data && line->state && line->tag == tag) {
            return line;
        }
    }
    return NULL;
}

static void l3_trigger(CPUIA64State *env, uint64_t cmd, unsigned type,
                       unsigned array)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t key = ia64_system_peek_msr(env, IA64_MSR_CA_ADDR) & 0xffff;
    IA64L3Line *line = l3_get(cpu, key, type == 3);
    IA64L3Line zero = {};

    if (!line) {
        line = &zero;
    }
    if (array == 4 && type == 3) {
        line->has_data = true;
        cpu->ca_l3_held[key & 0x7ff] = 1;
        for (unsigned j = 0; j < 16; j++) {
            line->data[j] = ia64_system_peek_msr(env, stage_msr(j));
        }
        for (unsigned g = 0; g < 4; g++) {
            line->check[g] = cmd & L3_CMD_FLAG_GENERATE ?
                l3_granule_check(line->data + 4 * g) :
                ia64_system_peek_msr(env, 1604 + 8 * g) & 0x3ff;
        }
    } else if (array == 4) {
        for (unsigned j = 0; j < 16; j++) {
            ia64_system_store_msr(env, stage_msr(j), line->data[j]);
        }
        /* Bit 51 of a read is unknown: the PAL reads the codes either way. */
        for (unsigned g = 0; g < 4; g++) {
            ia64_system_store_msr(env, 1604 + 8 * g, line->check[g]);
        }
    } else if (array == 6 && type == 3) {
        uint64_t w = ia64_system_peek_msr(env, L3_MSR_TAG);

        line->tag = w >> 22;
        line->extra = (w >> 10 & 0xfff) |
                      (ia64_system_peek_msr(env, L3_MSR_TAG_HIGH) & 0xfff)
                          << 12;
        line->ecc = w >> 3 & 0x7f;
        line->state = w & 7;
    } else if (array == 6) {
        ia64_system_store_msr(env, L3_MSR_TAG,
                              (uint64_t)line->tag << 22 |
                              (uint64_t)(line->extra & 0xfff) << 10 |
                              line->ecc << 3 | line->state);
        ia64_system_store_msr(env, L3_MSR_TAG_HIGH, line->extra >> 12);
    }
}

/* A store to a line copies the dword from RAM and regenerates the code. */
static void l3_store(CPUIA64State *env, uint64_t pa)
{
    IA64CPU *cpu = env_archcpu(env);
    IA64L3Line *line = cpu->ca_l3 ? l3_hit(cpu, pa) : NULL;
    uint8_t buf[8];

    if (line && ca_read_ram(cpu, pa & ~7ULL, buf, sizeof(buf))) {
        unsigned g = pa >> 5 & 3;

        line->data[pa >> 3 & 15] = ldq_le_p(buf);
        line->check[g] = l3_granule_check(line->data + 4 * g);
    }
}

/* fc drops every matching second-level line's data too; tag and state stay. */
static void l3_flush(IA64CPU *cpu, uint64_t pa)
{
    IA64L3Line *line;

    while (cpu->ca_l3 && (line = l3_hit(cpu, pa))) {
        line->has_data = false;
    }
}

/*
 * A hit returns the line's data; a granule whose stored code differs from its
 * data's logs MSR 1558 (one bit correctable, two not).
 */
static uint64_t l3_load(CPUIA64State *env, uint64_t pa, uint64_t value,
                        unsigned size)
{
    IA64CPU *cpu = env_archcpu(env);
    IA64L3Line *line = cpu->ca_l3 ? l3_hit(cpu, pa) : NULL;
    unsigned g = pa >> 5 & 3;
    unsigned syndrome;

    if (!line) {
        return value;
    }
    if (size == 8) {
        value = line->data[pa >> 3 & 15];
    }
    syndrome = line->check[g] ^ l3_granule_check(line->data + 4 * g);
    if (syndrome) {
        uint64_t log = L3_LOG_VALID | L3_LOG_SOURCE |
                       (ctpop32(syndrome) == 1 ? L3_LOG_SINGLE : L3_LOG_DOUBLE);

        warn_report_once("ia64: MSR 1558 reports a second-level cache ECC"
                         " error from a modelled line (an approximation,"
                         " first load of 0x%" PRIx64 ")", pa);
        trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, L3_MSR_LOG,
                                            log, env->ip);
        ia64_system_store_msr(env, L3_MSR_LOG, log);
    }
    return value;
}

/* Private L3 ways (PAL procedure 520). */
#define L3_CMD_RANGE 5
#define L3_TAG_SHIFT 18

void ia64_cpu_drop_l3_ways(IA64CPU *cpu)
{
    bool locked = bql_locked();

    if (!locked) {
        bql_lock();
    }
    memory_region_transaction_begin();
    for (unsigned way = 0; way < IA64_L3_WAYS; way++) {
        memory_region_set_enabled(&cpu->l3_way[way], false);
    }
    memory_region_transaction_commit();
    tlb_flush(CPU(cpu));
    if (!locked) {
        bql_unlock();
    }
}

/*
 * The PAL ends the private ways by clearing the MSR 1504 bits it set when it
 * enabled them; its range writes with state 0 are an invalidate sweep.
 */
#define L3_ENABLE_BITS (BIT_ULL(16) | BIT_ULL(24) | BIT_ULL(32) | \
                        BIT_ULL(33) | BIT_ULL(40) | BIT_ULL(41))

void ia64_cache_array_msr1504(CPUIA64State *env, uint64_t old, uint64_t value)
{
    if ((old & L3_ENABLE_BITS) && !(value & L3_ENABLE_BITS)) {
        ia64_cpu_drop_l3_ways(env_archcpu(env));
    }
}

static void l3_way_range(CPUIA64State *env, uint64_t cmd)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t first = ia64_system_peek_msr(env, IA64_MSR_CA_ADDR) & 0xffff;
    uint64_t word = ia64_system_peek_msr(env, L3_MSR_TAG);
    unsigned way = first >> 11;
    unsigned state = word & 7;
    uint64_t base = (word >> 22) << L3_TAG_SHIFT;
    bool locked = bql_locked();

    /* Only a whole way is modelled. */
    if (way >= IA64_L3_WAYS || (first & 0x7ff) ||
        (cmd & 0xffff) != (first | 0x7ff) || state != 3) {
        return;
    }
    if (!locked) {
        bql_lock();
    }
    memory_region_transaction_begin();
    memset(memory_region_get_ram_ptr(&cpu->l3_way[way]), 0, IA64_L3_WAY_SIZE);
    memory_region_set_address(&cpu->l3_way[way], base);
    memory_region_set_enabled(&cpu->l3_way[way], true);
    memory_region_transaction_commit();
    warn_report_once("ia64: an L3 way became private CPU memory (an"
                     " approximation, first at 0x%" PRIx64 ")", base);
    /* The listener flushes later; flush now so the next access sees the way. */
    tlb_flush(CPU(cpu));
    trace_ia64_l3_way(CPU(cpu)->cpu_index, way, base, state);
    if (!locked) {
        bql_unlock();
    }
}

/*
 * The reserved M word 0x108000000 runs the command last written to MSR 1546 on
 * the line MSR 1544 names, moving 16 dwords to or from MSRs 1600 to 1627.
 */
void ia64_cache_array_trigger(CPUIA64State *env)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t cmd = ia64_system_peek_msr(env, 1546);

    env->ca_active = 1;
    ia64_store_watch_update(env);
    /* Bits 15:13 are dropped: the PAL writes them clear and reads them set. */
    uint64_t index = ia64_system_peek_msr(env, IA64_MSR_CA_ADDR) &
                     (CA_SETS - 1);
    unsigned type = (cmd >> 58) & 7;
    unsigned array = (cmd >> 53) & 0x1f;
    IA64CacheLine *line;
    IA64CacheLine ram = {};

    if (!cpu->ca_mirror) {
        cpu->ca_mirror = g_new0(uint64_t, CA_SETS);
    }
    if ((cmd >> 63) && type == L3_CMD_RANGE && array == 6) {
        l3_way_range(env, cmd);
        return;
    }
    if (!(cmd >> 63) || (type != 2 && type != 3)) {
        return;
    }
    if (array == 4 || array == 6) {
        l3_trigger(env, cmd, type, array);
        return;
    }
    if (array < 8 || array > 10) {
        return;
    }
    if (type == 3 && array == 9) {
        /* Check-bit injection: staged words are XORed into later data writes. */
        for (unsigned w = 0; w < 4; w++) {
            cpu->ca_flip[w] = ia64_system_peek_msr(env, stage_msr(w)) &
                              CA_CHECK_MASK;
        }
        return;
    }
    line = line_get(cpu, index, type == 3);
    if ((!line || (!line->has_data && array != 10 && type == 2)) &&
        line_from_ram(cpu, index, &ram)) {
        if (line) {
            ram.tag = line->tag;
            ram.tag_meta = line->tag_meta;
        }
        line = &ram;
    }
    if (!line) {
        return;
    }
    if (type == 3 && array == 8) {
        line->has_data = true;
        cpu->ca_held[index & 0xff] = 1;
        for (unsigned j = 0; j < 16; j++) {
            line->data[j] = ia64_system_peek_msr(env, stage_msr(j));
            line->check[j] = line_check(line->data[j]) ^ cpu->ca_flip[j & 3];
        }
    } else if (type == 3 && array == 10) {
        uint64_t w = ia64_system_peek_msr(env, stage_msr(0));
        unsigned set = index & 0xff;

        cpu->ca_bad[set] -= tag_bad(line);
        line->tag = tag_reverse(w & CA_TAG_MASK);
        line->tag_meta = w >> CA_TAG_BITS & 0x1ff;
        cpu->ca_bad[set] += tag_bad(line);
    } else if (type == 2 && array == 10) {
        /* The valid bit is held but not read back: nothing places it. */
        ia64_system_store_msr(env, CA_MSR_TAG_READ,
                              line->tag << CA_TAG_READ_SHIFT |
                              (uint64_t)cpu->ca_repl[index & 0xff]
                                  << CA_TAG_READ_REPL_SHIFT |
                              (line->tag_meta >> 1 & 0xf)
                                  << CA_TAG_READ_PARITY_SHIFT |
                              (line->tag_meta >> CA_STATE_SHIFT & 0xf));
    } else if (type == 2) {
        for (unsigned j = 0; j < 16; j++) {
            ia64_system_store_msr(env, stage_msr(j),
                                  array == 8 ? line->data[j] : line->check[j]);
        }
    }
}

/*
 * A hit checks the dword's check bits (odd count corrected, even double-bit)
 * and reports in MSR 1505; a doubleword hit on a valid tag returns line data.
 */
uint64_t ia64_cache_array_load(CPUIA64State *env, uint64_t pa, uint64_t value,
                               unsigned size)
{
    IA64CPU *cpu = env_archcpu(env);
    uint64_t index = set_of(pa);
    unsigned column;
    IA64CacheLine *hit = tag_hit(cpu, pa, &column);
    IA64CacheLine *line = hit ? hit : cpu->ca_held[index & 0xff] ?
                          g_hash_table_lookup(cpu->ca_lines, &index) : NULL;
    unsigned j = (pa >> 3) & 15;
    uint64_t syndrome;
    uint64_t log;

    tag_parity_check(env, pa);
    if (!hit) {
        value = l3_load(env, pa, value, size);
    }
    if (!line || !line->has_data) {
        return value;
    }
    if (hit && size == 8) {
        warn_report_once("ia64: a load returned modelled cache-array data (an"
                         " approximation, first load of 0x%" PRIx64 ")", pa);
        value = hit->data[j];
    }
    syndrome = (line_check(line->data[j]) ^ line->check[j]) & CA_CHECK_MASK;
    if (!syndrome) {
        return value;
    }
    log = ctpop64(syndrome) & 1 ? CA_LOG_SINGLE : CA_LOG_DOUBLE;
    warn_report_once("ia64: MSR 1505 reports a cache-array error from the check"
                     " bits of a modelled line (an approximation, first load of"
                     " 0x%" PRIx64 "); the arrays are not modelled beyond that",
                     pa);
    trace_ia64_msr_estimated_completion(CPU(cpu)->cpu_index, IA64_MSR_ECC_LOG,
                                        log, env->ip);
    ia64_system_store_msr(env, IA64_MSR_ECC_LOG, log);
    return value;
}
