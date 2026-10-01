/*
 * Test R10000 performance counters and secondary cache.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Origin 200 and Origin 2000 use the only CPU built here; firmware is off so
 * each guest fragment owns the reset vector and low RAM.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "libqtest.h"
#include "sgi-migrate.h"
#include "sgi-test.h"

#define IMAGE_SIZE          0x1000
#define MAIN_LOOP           0x480
#define CACHE_START         0x900
#define CACHE_LOOP          0xa00
#define CACHE_READ          0xa40
#define CACHE_DONE          0xb00

#define STATE_PHYS          0x00100000
#define STATE_KSEG1_HI      0xa010
#define READY_OFF           0x00
#define COUNT_OFF           0x08
#define CACHE_READY_OFF     0x120
#define CACHE_COMMAND_OFF   0x128
#define CACHE_DONE_OFF      0x130
#define CACHE_TAGLO_OFF     0x138
#define CACHE_TAGHI_OFF     0x140
#define CACHE_TAG_STRIDE    0x10
#define CACHE_DATA_LO_OFF   0x168
#define CACHE_DATA_HI_OFF   0x170
#define CACHE_ECC_OFF       0x178
#define CACHE_OTHER_LO_OFF  0x180
#define CACHE_OTHER_HI_OFF  0x188
#define CACHE_OTHER_ECC_OFF 0x190
#define CAPACITY_READY_OFF  0x220
#define CAPACITY_COMMAND_OFF 0x228
#define CAPACITY_DONE_OFF   0x230
#define CAPACITY_CONFIG_OFF 0x240
#define CAPACITY_START      0x900
#define CAPACITY_LOOP       0xb00
#define CAPACITY_READ       0xb40
#define CAPACITY_HALT       0xf00
#define CAPACITY_TAG_OFF    0x300
#define CAPACITY_TAG_STRIDE 0x10
#define CAPACITY_DATA_OFF   0x380
#define CAPACITY_DATA_STRIDE 0x30
#define CAPACITY_INVALID_TAG_OFF 0x500
#define READY_MAGIC         0x1400
#define CACHE_READY_MAGIC   0x14ca
#define CACHE_DONE_MAGIC    0x14cb
#define INITIAL_COUNT       0x12340000u
#define CACHE_TAGLO         0x1234c5aau
#define CACHE_TAGHI         0x0000000du
#define CACHE_DATA_LO       0x2468ace0u
#define CACHE_DATA_HI       0x13579bdfu
#define CACHE_ECC           0x2aau
#define CACHE_OTHER_LO      0x10293847u
#define CACHE_OTHER_HI      0x5a6b7c8du
#define CACHE_OTHER_ECC     0x155u
#define CACHE_SENTINEL      0xffffffffu
#define CACHE_ECC_SENTINEL  0x3ffu
#define POLL_TRIES          20000
#define POLL_INTERVAL_US    1000
#define ANALYZE_SCRIPT      "scripts/analyze-migration.py"
#define STREAM_ADVANCE      (1u << 26)

#define OPC_SPECIAL         (0x00u << 26)
#define OPC_BEQ             (0x04u << 26)
#define OPC_BNE             (0x05u << 26)
#define OPC_ADDIU           (0x09u << 26)
#define OPC_ORI             (0x0du << 26)
#define OPC_CACHE           (0x2fu << 26)
#define OPC_AND             (0x24u | OPC_SPECIAL)
#define OPC_LUI             (0x0fu << 26)
#define OPC_CP0             (0x10u << 26)
#define OPC_LD              (0x37u << 26)
#define OPC_SD              (0x3fu << 26)
#define OPC_MFC0            (OPC_CP0 | (0x00u << 21))
#define OPC_MTC0            (OPC_CP0 | (0x04u << 21))
#define OPC_SYNC            (0x0fu | OPC_SPECIAL)

#define R_ZERO              0
#define R_T0                8
#define R_T1                9
#define R_T2                10
#define R_T3                11

#define CP0_STATUS          12
#define CP0_CONFIG0         16
#define CP0_PERFORMANCE     25
#define CP0_ERRCTL          26
#define CP0_TAGLO           28
#define CP0_TAGHI           29
#define PERF_KERNEL         (1u << 1)

typedef struct {
    const char *machine;
    const char *cpu;
    unsigned int control_sel;
    unsigned int counter_sel;
} TestConfig;

typedef struct {
    uint8_t *code;
    unsigned int pos;
} GuestImage;

typedef struct {
    uint16_t address_hi;
    int16_t address_off;
    uint32_t taglo;
    uint32_t taghi;
} CacheTagProbe;

typedef struct {
    uint16_t address_hi;
    int16_t address_off;
    uint32_t taglo;
    uint32_t taghi;
    uint32_t ecc;
    bool valid;
} CacheDataProbe;

typedef struct {
    const char *name;
    uint32_t bytes;
    uint32_t reset_mode;
} CacheCapacity;

static const TestConfig configurations[] = {
    { "origin200", "R10000", 0, 1 },
    { "malta", "R16000,big-endian=on,r16000-experimental=on,"
      "r16000-reset-mode=0x3a005,r16000-scache-bytes=4194304", 0, 1 },
};

static const CacheTagProbe r10000_cache_probe = {
    .address_hi = 0xa000,
    .address_off = 0,
    .taglo = CACHE_TAGLO,
    .taghi = CACHE_TAGHI,
};

static const CacheCapacity cache_capacities[] = {
    { "4m", 4 * 1024 * 1024, 0x3a005 },
    { "8m", 8 * 1024 * 1024, 0x4a005 },
    { "16m", 16 * 1024 * 1024, 0x5a005 },
};

static void emit(GuestImage *img, uint32_t insn)
{
    g_assert_cmpuint(img->pos + 4, <=, IMAGE_SIZE);
    stl_be_p(img->code + img->pos, insn);
    img->pos += 4;
}

static void emit_branch(GuestImage *img, unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, OPC_BEQ | (off & 0xffff));
}

static void emit_bne(GuestImage *img, int rs, int rt, unsigned int target)
{
    int32_t off = ((int32_t)target - (int32_t)(img->pos + 4)) / 4;

    emit(img, OPC_BNE | (rs << 21) | (rt << 16) | (off & 0xffff));
}

static void emit_mfc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MFC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_mtc0(GuestImage *img, int rt, int rd, int sel)
{
    emit(img, OPC_MTC0 | (rt << 16) | (rd << 11) | sel);
}

static void emit_load_u32(GuestImage *img, int rt, uint32_t value)
{
    emit(img, OPC_LUI | (rt << 16) | (value >> 16));
    emit(img, OPC_ORI | (rt << 21) | (rt << 16) | (value & 0xffff));
}

static void emit_cache(GuestImage *img, int operation, int base, uint16_t off)
{
    emit(img, OPC_CACHE | (base << 21) | (operation << 16) | off);
}

static void cache_address(uint32_t paddr, uint16_t *hi, int16_t *off)
{
    uint32_t vaddr = 0xa0000000 | paddr;

    *hi = (vaddr + 0x8000) >> 16;
    *off = (int16_t)vaddr;
}

static void cache_capacity_probes(uint32_t capacity, CacheTagProbe tags[4],
                                  CacheDataProbe data[6])
{
    uint32_t first_extended_paddr = capacity / 4;
    uint32_t previous_capacity_alias = first_extended_paddr - 128;
    uint32_t high_set_paddr = capacity / 2 - 128;
    uint32_t prior_quad_paddr = capacity / 2 - 32;
    uint32_t last_quad_paddr = capacity / 2 - 16;

    cache_address(previous_capacity_alias, &tags[0].address_hi,
                  &tags[0].address_off);
    cache_address(first_extended_paddr, &tags[1].address_hi,
                  &tags[1].address_off);
    cache_address(high_set_paddr, &tags[2].address_hi,
                  &tags[2].address_off);
    cache_address(high_set_paddr + 1, &tags[3].address_hi,
                  &tags[3].address_off);
    tags[0].taglo = 0x1111c400;
    tags[0].taghi = 0x1;
    tags[1].taglo = 0x2222c400;
    tags[1].taghi = 0x2;
    tags[2].taglo = 0x3333c400;
    tags[2].taghi = 0x3;
    tags[3].taglo = 0x4444c400;
    tags[3].taghi = 0x80000004;

    cache_address(last_quad_paddr, &data[0].address_hi,
                  &data[0].address_off);
    cache_address(last_quad_paddr + 9, &data[1].address_hi,
                  &data[1].address_off);
    cache_address(prior_quad_paddr + 8, &data[2].address_hi,
                  &data[2].address_off);
    cache_address(prior_quad_paddr + 1, &data[3].address_hi,
                  &data[3].address_off);
    cache_address(first_extended_paddr, &data[4].address_hi,
                  &data[4].address_off);
    cache_address(first_extended_paddr + 9, &data[5].address_hi,
                  &data[5].address_off);
    for (unsigned int i = 0; i < 6; i++) {
        data[i].valid = true;
        data[i].taglo = 0x51000000 | (i + 1);
        data[i].taghi = 0xa5000000 | (i + 1);
        data[i].ecc = 0x120 + i;
    }
}

static void emit_cache_value(GuestImage *img, int reg, int16_t off,
                             uint64_t result_off)
{
    emit_mfc0(img, R_T0, CP0_TAGLO, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | result_off);
    emit_mfc0(img, R_T0, CP0_TAGHI, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | (result_off + 8));
    emit_mfc0(img, R_T0, CP0_ERRCTL, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | (result_off + 16));
    emit_cache(img, 0x1b, reg, off ^ 8);
    emit_mfc0(img, R_T0, CP0_TAGLO, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | (result_off + 24));
    emit_mfc0(img, R_T0, CP0_TAGHI, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | (result_off + 32));
    emit_mfc0(img, R_T0, CP0_ERRCTL, 0);
    emit(img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | (result_off + 40));
}

static void write_capacity_guest_image(int fd, uint32_t capacity)
{
    CacheTagProbe tags[4];
    CacheDataProbe data[6] = { };
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    cache_capacity_probes(capacity, tags, data);
    emit_branch(&img, CACHE_START);
    emit(&img, 0);
    img.pos = CAPACITY_START;
    emit(&img, OPC_LUI | (R_T2 << 16) | STATE_KSEG1_HI);
    emit_mfc0(&img, R_T0, CP0_CONFIG0, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CAPACITY_CONFIG_OFF);

    for (unsigned int i = 0; i < ARRAY_SIZE(tags); i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | tags[i].address_hi);
        emit_load_u32(&img, R_T0, tags[i].taglo);
        emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
        emit_load_u32(&img, R_T0, tags[i].taghi);
        emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
        emit_cache(&img, 0x0b, R_T1, tags[i].address_off);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(data); i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | data[i].address_hi);
        emit_load_u32(&img, R_T0, data[i].taglo);
        emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
        emit_load_u32(&img, R_T0, data[i].taghi);
        emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
        emit_load_u32(&img, R_T0, data[i].ecc);
        emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
        emit_cache(&img, 0x1f, R_T1, data[i].address_off);
    }
    emit_load_u32(&img, R_T0, CACHE_READY_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CAPACITY_READY_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CAPACITY_LOOP;
    emit(&img, OPC_LD | (R_T2 << 21) | (R_T0 << 16) |
              CAPACITY_COMMAND_OFF);
    emit_bne(&img, R_T0, R_ZERO, CAPACITY_READ);
    emit(&img, 0);
    emit_branch(&img, CAPACITY_LOOP);
    emit(&img, 0);

    img.pos = CAPACITY_READ;
    for (unsigned int i = 0; i < ARRAY_SIZE(tags); i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | tags[i].address_hi);
        emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
        emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
        emit_cache(&img, 0x07, R_T1, tags[i].address_off);
        emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                  (CAPACITY_TAG_OFF + i * CAPACITY_TAG_STRIDE));
        emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                  (CAPACITY_TAG_OFF + i * CAPACITY_TAG_STRIDE + 8));
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(data); i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | data[i].address_hi);
        emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
        emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
        emit_mtc0(&img, R_ZERO, CP0_ERRCTL, 0);
        emit_cache(&img, 0x1b, R_T1, data[i].address_off);
        emit_cache_value(&img, R_T1, data[i].address_off,
                         CAPACITY_DATA_OFF + i * CAPACITY_DATA_STRIDE);
    }

    emit(&img, OPC_LUI | (R_T1 << 16) | tags[2].address_hi);
    emit_cache(&img, 0x03, R_T1, tags[2].address_off);
    for (unsigned int i = 0; i < ARRAY_SIZE(tags); i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | tags[i].address_hi);
        emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
        emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
        emit_cache(&img, 0x07, R_T1, tags[i].address_off);
        emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                  (CAPACITY_INVALID_TAG_OFF + i * CACHE_TAG_STRIDE));
    }
    emit_load_u32(&img, R_T0, CACHE_DONE_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CAPACITY_DONE_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CAPACITY_HALT;
    emit_branch(&img, CAPACITY_HALT);
    emit(&img, 0);
    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static char *r16000_cpu_args(const CacheCapacity *capacity, uint32_t mode)
{
    return g_strdup_printf("R16000,big-endian=on,r16000-experimental=on,"
                           "r16000-reset-mode=0x%x,r16000-scache-bytes=%u",
                           mode, capacity->bytes);
}

static char *cpu_qom_path(QTestState *qts)
{
    QDict *cpus = qtest_qmp(qts,
        "{'execute':'query-cpus-fast'}");
    QObject *result = qdict_get(cpus, "return");
    QList *cpu_list;
    QDict *cpu;
    g_autofree char *path = NULL;

    g_assert_null(qdict_get(cpus, "error"));
    g_assert_nonnull(result);
    cpu_list = qobject_to(QList, result);
    cpu = qobject_to(QDict, qlist_peek(cpu_list));
    path = g_strdup(qdict_get_str(cpu, "qom-path"));
    qobject_unref(cpus);
    return g_steal_pointer(&path);
}

static uint64_t cpu_uint_property(QTestState *qts, const char *property)
{
    g_autofree char *path = cpu_qom_path(qts);
    QDict *value = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    QObject *result_obj = qdict_get(value, "return");
    uint64_t result;

    g_assert_null(qdict_get(value, "error"));
    g_assert_nonnull(result_obj);
    result = qdict_get_int(value, "return");
    qobject_unref(value);
    return result;
}

static bool cpu_bool_property(QTestState *qts, const char *property)
{
    g_autofree char *path = cpu_qom_path(qts);
    QDict *value = qtest_qmp(qts,
        "{'execute':'qom-get','arguments':{'path':%s,'property':%s}}",
        path, property);
    QObject *result_obj = qdict_get(value, "return");
    bool result;

    g_assert_null(qdict_get(value, "error"));
    g_assert_nonnull(result_obj);
    result = qdict_get_bool(value, "return");
    qobject_unref(value);
    return result;
}

static void assert_r16000_properties(QTestState *qts,
                                     const CacheCapacity *capacity,
                                     uint32_t mode)
{
    g_assert_true(cpu_bool_property(qts, "r16000-experimental"));
    g_assert_cmpuint(cpu_uint_property(qts, "r16000-reset-mode"), ==, mode);
    g_assert_cmpuint(cpu_uint_property(qts, "r16000-scache-bytes"), ==,
                     capacity->bytes);
}

static void write_guest_image(int fd, const TestConfig *config)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;

    /* Branch clear of the low words so seeded board data cannot matter. */
    emit_branch(&img, CODE_START);
    emit(&img, 0);

    img.pos = CODE_START;
    emit_mfc0(&img, R_T0, CP0_STATUS, 0);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T1 << 16) | 0xfffb);
    emit(&img, OPC_AND | (R_T0 << 21) | (R_T1 << 16) | (R_T0 << 11));
    emit_mtc0(&img, R_T0, CP0_STATUS, 0);
    emit(&img, 0);

    emit(&img, OPC_LUI | (R_T0 << 16) | (INITIAL_COUNT >> 16));
    emit(&img, OPC_ORI | (R_T0 << 21) | (R_T0 << 16) |
               (INITIAL_COUNT & 0xffff));
    emit_mtc0(&img, R_T0, CP0_PERFORMANCE, config->counter_sel);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T0 << 16) | PERF_KERNEL);
    emit_mtc0(&img, R_T0, CP0_PERFORMANCE, config->control_sel);
    emit(&img, OPC_LUI | (R_T2 << 16) | STATE_KSEG1_HI);
    emit(&img, OPC_ADDIU | (R_ZERO << 21) | (R_T0 << 16) | READY_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | READY_OFF);

    img.pos = MAIN_LOOP;
    emit_mfc0(&img, R_T0, CP0_PERFORMANCE, config->counter_sel);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | COUNT_OFF);
    emit(&img, OPC_SYNC);
    emit_branch(&img, MAIN_LOOP);
    emit(&img, 0);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static void write_cache_guest_image(int fd, const CacheTagProbe *probes,
                                    size_t nprobes)
{
    uint8_t image[IMAGE_SIZE] = { };
    GuestImage img = { .code = image, .pos = 0 };
    ssize_t written;
    size_t i;

    g_assert_cmpuint(nprobes, >, 0);

    /* Branch clear of the low words so seeded board data cannot matter. */
    emit_branch(&img, CACHE_START);
    emit(&img, 0);

    img.pos = CACHE_START;
    emit(&img, OPC_LUI | (R_T2 << 16) | STATE_KSEG1_HI);
    for (i = 0; i < nprobes; i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | probes[i].address_hi);
        emit_load_u32(&img, R_T0, probes[i].taglo);
        emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
        emit_load_u32(&img, R_T0, probes[i].taghi);
        emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
        emit_cache(&img, 0x0b, R_T1, probes[i].address_off);
    }
    /* Fill the quadword's other half first; the store below pads over it. */
    emit_load_u32(&img, R_T0, CACHE_OTHER_LO);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_load_u32(&img, R_T0, CACHE_OTHER_HI);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_OTHER_ECC);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1f, R_T1, 0);
    emit_load_u32(&img, R_T0, CACHE_DATA_LO);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_load_u32(&img, R_T0, CACHE_DATA_HI);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_ECC);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1f, R_T1, 8);
    emit_load_u32(&img, R_T0, CACHE_READY_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_READY_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CACHE_LOOP;
    emit(&img, OPC_LD | (R_T2 << 21) | (R_T0 << 16) |
               CACHE_COMMAND_OFF);
    emit_bne(&img, R_T0, R_ZERO, CACHE_READ);
    emit(&img, 0);
    emit_branch(&img, CACHE_LOOP);
    emit(&img, 0);

    img.pos = CACHE_READ;
    for (i = 0; i < nprobes; i++) {
        emit(&img, OPC_LUI | (R_T1 << 16) | probes[i].address_hi);
        emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
        emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
        emit_cache(&img, 0x07, R_T1, probes[i].address_off);
        emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                   (CACHE_TAGLO_OFF + i * CACHE_TAG_STRIDE));
        emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
        emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) |
                   (CACHE_TAGHI_OFF + i * CACHE_TAG_STRIDE));
    }
    emit_mtc0(&img, R_ZERO, CP0_TAGLO, 0);
    emit_mtc0(&img, R_ZERO, CP0_TAGHI, 0);
    emit_mtc0(&img, R_ZERO, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1b, R_T1, 8);
    emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DATA_LO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DATA_HI_OFF);
    emit_mfc0(&img, R_T0, CP0_ERRCTL, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_ECC_OFF);
    emit_load_u32(&img, R_T0, CACHE_SENTINEL);
    emit_mtc0(&img, R_T0, CP0_TAGLO, 0);
    emit_mtc0(&img, R_T0, CP0_TAGHI, 0);
    emit_load_u32(&img, R_T0, CACHE_ECC_SENTINEL);
    emit_mtc0(&img, R_T0, CP0_ERRCTL, 0);
    emit_cache(&img, 0x1b, R_T1, 0);
    emit_mfc0(&img, R_T0, CP0_TAGLO, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_LO_OFF);
    emit_mfc0(&img, R_T0, CP0_TAGHI, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_HI_OFF);
    emit_mfc0(&img, R_T0, CP0_ERRCTL, 0);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_OTHER_ECC_OFF);
    emit_load_u32(&img, R_T0, CACHE_DONE_MAGIC);
    emit(&img, OPC_SD | (R_T2 << 21) | (R_T0 << 16) | CACHE_DONE_OFF);
    emit(&img, OPC_SYNC);

    img.pos = CACHE_DONE;
    emit_branch(&img, CACHE_DONE);
    emit(&img, 0);

    written = write(fd, image, sizeof(image));
    g_assert_cmpint(written, ==, sizeof(image));
}

static uint64_t state_word(QTestState *qts, uint64_t off)
{
    return qtest_readq(qts, STATE_PHYS + off);
}

static void assert_cache_tags(QTestState *qts, const CacheTagProbe *probes,
                              size_t nprobes)
{
    size_t i;

    for (i = 0; i < nprobes; i++) {
        g_assert_cmphex(state_word(qts, CACHE_TAGLO_OFF +
                                   i * CACHE_TAG_STRIDE), ==,
                        probes[i].taglo);
        g_assert_cmphex(state_word(qts, CACHE_TAGHI_OFF +
                                   i * CACHE_TAG_STRIDE), ==,
                        probes[i].taghi);
    }
}

static uint64_t wait_for_change(QTestState *qts, uint64_t old)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint64_t value = state_word(qts, COUNT_OFF);

        if (value != old) {
            return value;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static uint32_t wait_for_advance(QTestState *qts, uint32_t initial,
                                 uint32_t advance)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        uint32_t value = state_word(qts, COUNT_OFF);

        if (value - initial >= advance) {
            return value;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static void wait_for_ready(QTestState *qts)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, READY_OFF) == READY_MAGIC) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_not_reached();
}

static void wait_for_state(QTestState *qts, uint64_t off, uint64_t expected)
{
    unsigned int tries;

    for (tries = 0; tries < POLL_TRIES; tries++) {
        if (state_word(qts, off) == expected) {
            return;
        }
        g_usleep(POLL_INTERVAL_US);
    }
    g_assert_cmphex(state_word(qts, off), ==, expected);
}

static void test_active_counter(const void *opaque)
{
    const TestConfig *config = opaque;
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    uint32_t before, after, delta;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-perf-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_guest_image(fd, config);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-migration-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);

    source_args = g_strdup_printf(
        "-M %s -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown", config->machine,
        config->cpu, image);
    dest_args = g_strdup_printf(
        "-M %s -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown -incoming defer",
        config->machine, config->cpu, image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_ready(from);
    before = wait_for_change(from, INITIAL_COUNT);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    before = state_word(from, COUNT_OFF);

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 60);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    after = wait_for_change(to, before);
    delta = after - before;
    g_test_message("%s on %s before=%" PRIu32 " after=%" PRIu32
                   " delta=%" PRIu32, config->cpu, config->machine,
                   before, after, delta);
    g_assert_cmpuint(delta, >, 0);
    g_assert_cmpuint(delta, <, 1u << 30);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_stream_representation(const void *opaque)
{
    const TestConfig *config = opaque;
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *stream = NULL;
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *json = NULL;
    GError *error = NULL;
    QTestState *from;
    const char *python = g_getenv("PYTHON");
    int fd, state_fd, pid, wstatus;

    if (!python) {
        g_test_skip("PYTHON variable not set");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-perf-XXXXXX", &image, &error);
    g_assert_no_error(error);
    write_guest_image(fd, config);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-stream-XXXXXX", &error);
    g_assert_no_error(error);
    stream = g_build_filename(directory, "migration", NULL);
    state = g_build_filename(directory, "state.json", NULL);
    uri = g_strdup_printf("exec:cat > %s", stream);
    source_args = g_strdup_printf(
        "-M origin200 -m 64M -cpu %s -bios %s "
        "-accel tcg -no-reboot -no-shutdown", config->cpu, image);
    from = qtest_init(source_args);
    wait_for_ready(from);
    /* Let the counter advance far enough that the stream carries a value. */
    wait_for_advance(from, INITIAL_COUNT, STREAM_ADVANCE);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 60);
    qtest_quit(from);

    state_fd = open(state, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    g_assert_cmpint(state_fd, >=, 0);
    pid = fork();
    g_assert_cmpint(pid, >=, 0);
    if (!pid) {
        dup2(state_fd, STDOUT_FILENO);
        close(state_fd);
        execl(python, python, ANALYZE_SCRIPT, "-f", stream,
              "-d", "state", NULL);
        _exit(127);
    }
    close(state_fd);
    g_assert_cmpint(waitpid(pid, &wstatus, 0), ==, pid);
    g_assert_true(WIFEXITED(wstatus));
    g_assert_cmpint(WEXITSTATUS(wstatus), ==, 0);
    g_assert_true(g_file_get_contents(state, &json, NULL, &error));
    g_assert_no_error(error);

    g_assert_null(strstr(json, "\"cpu/r14k-performance\""));
    g_assert_null(strstr(json, "\"cpu/r14k-scache\""));

    unlink(image);
    unlink(stream);
    unlink(state);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r10000_scache_migration(void)
{
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-scache-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_cache_guest_image(fd, &r10000_cache_probe, 1);
    close(fd);
    directory = g_dir_make_tmp("sn0-r10k-scache-migration-XXXXXX",
                               &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M origin2000 -m 256M -smp 8 "
        "-cpu R10000 -bios %s -accel tcg,thread=single -nodefaults "
        "-no-reboot -no-shutdown", image);
    dest_args = g_strdup_printf(
        "-M origin2000 -m 256M -smp 8 "
        "-cpu R10000 -bios %s -accel tcg,thread=single -nodefaults "
        "-no-reboot -no-shutdown -incoming defer", image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_state(from, CACHE_READY_OFF, CACHE_READY_MAGIC);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 60);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_writeq(to, STATE_PHYS + CACHE_COMMAND_OFF, 1);
    wait_for_state(to, CACHE_DONE_OFF, CACHE_DONE_MAGIC);

    assert_cache_tags(to, &r10000_cache_probe, 1);
    g_assert_cmphex(state_word(to, CACHE_DATA_LO_OFF), ==, CACHE_DATA_LO);
    g_assert_cmphex(state_word(to, CACHE_DATA_HI_OFF), ==, CACHE_DATA_HI);
    g_assert_cmphex(state_word(to, CACHE_ECC_OFF), ==, CACHE_ECC);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r16000_scache_capacity_migration(gconstpointer opaque)
{
    const CacheCapacity *capacity = opaque;
    CacheTagProbe tags[4];
    CacheDataProbe data[6] = { };
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *cpu_args = r16000_cpu_args(capacity,
                                                 capacity->reset_mode);
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    cache_capacity_probes(capacity->bytes, tags, data);
    fd = g_file_open_tmp("sn0-r16000-capacity-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_capacity_guest_image(fd, capacity->bytes);
    close(fd);
    directory = g_dir_make_tmp("sn0-r16000-capacity-migration-XXXXXX",
                               &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown",
        cpu_args, image);
    dest_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown "
        "-incoming defer", cpu_args, image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_state(from, CAPACITY_READY_OFF, CACHE_READY_MAGIC);

    uint64_t config0 = state_word(from, CAPACITY_CONFIG_OFF);
    g_assert_cmpuint((config0 >> 16) & 7, ==,
                     (capacity->reset_mode >> 16) & 7);
    g_assert_cmpuint((config0 >> 13) & 1, ==, 1);
    assert_r16000_properties(from, capacity, capacity->reset_mode);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");

    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);
    sgi_wait_migration_status(from, "completed", 60);
    assert_r16000_properties(to, capacity, capacity->reset_mode);
    qtest_qmp_assert_success(to, "{'execute':'cont'}");
    qtest_writeq(to, STATE_PHYS + CAPACITY_COMMAND_OFF, 1);
    wait_for_state(to, CAPACITY_DONE_OFF, CACHE_DONE_MAGIC);

    for (unsigned int i = 0; i < ARRAY_SIZE(tags); i++) {
        uint64_t result = CAPACITY_TAG_OFF + i * CAPACITY_TAG_STRIDE;
        uint32_t expected_hi = tags[i].taghi & 0x8000000f;

        if (i == 2 || i == 3) {
            expected_hi |= 0x80000000;
        }
        g_assert_cmphex(state_word(to, result), ==, tags[i].taglo);
        g_assert_cmphex(state_word(to, result + 8), ==,
                        (uint64_t)(int64_t)(int32_t)expected_hi);
    }
    for (unsigned int i = 0; i < ARRAY_SIZE(data); i++) {
        uint64_t result = CAPACITY_DATA_OFF + i * CAPACITY_DATA_STRIDE;

        g_assert_cmphex(state_word(to, result), ==, data[i].taglo);
        g_assert_cmphex(state_word(to, result + 8), ==,
                        (uint64_t)(int64_t)(int32_t)data[i].taghi);
        g_assert_cmphex(state_word(to, result + 16), ==, data[i].ecc);
        g_assert_cmphex(state_word(to, result + 24), ==, 0);
        g_assert_cmphex(state_word(to, result + 32), ==, 0);
        g_assert_cmphex(state_word(to, result + 40), ==, data[i].ecc);
    }

    g_assert_cmphex(state_word(to, CAPACITY_INVALID_TAG_OFF), ==,
                    tags[0].taglo);
    g_assert_cmphex(state_word(to, CAPACITY_INVALID_TAG_OFF +
                               CAPACITY_TAG_STRIDE), ==,
                    tags[1].taglo);
    {
        uint32_t invalid_paddr = capacity->bytes / 2 - 128;
        uint32_t invalid_vaddr = 0xa0000000 | invalid_paddr;
        uint32_t invalid_taglo =
            (((invalid_paddr >> 18) & 0x3ffff) << 14) |
            (((invalid_vaddr >> 12) & 3) << 7);

        /* Index WriteBack Invalidate rewrites the tag from this address. */
        g_assert_cmphex(state_word(to, CAPACITY_INVALID_TAG_OFF +
                                   2 * CAPACITY_TAG_STRIDE), ==,
                        invalid_taglo);
    }
    g_assert_cmphex(state_word(to, CAPACITY_INVALID_TAG_OFF +
                               3 * CAPACITY_TAG_STRIDE), ==,
                    tags[3].taglo);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void assert_r16000_migration_rejected(const CacheCapacity *source,
                                             uint32_t source_mode,
                                             const CacheCapacity *dest,
                                             uint32_t dest_mode)
{
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *source_cpu_args = r16000_cpu_args(source, source_mode);
    g_autofree char *dest_cpu_args = r16000_cpu_args(dest, dest_mode);
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;
    int64_t deadline;
    bool failed = false;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r16000-mismatch-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_capacity_guest_image(fd, source->bytes);
    close(fd);
    directory = g_dir_make_tmp("sn0-r16000-mismatch-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown",
        source_cpu_args, image);
    dest_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown "
        "-incoming defer", dest_cpu_args, image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_state(from, CAPACITY_READY_OFF, CACHE_READY_MAGIC);
    assert_r16000_properties(from, source, source_mode);
    assert_r16000_properties(to, dest, dest_mode);
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        QDict *ret = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(ret, "status");

        failed = g_str_equal(status, "failed");
        qobject_unref(ret);
        if (failed) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(failed);
    {
        QDict *ret = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *error_desc = qdict_get_str(ret, "error-desc");

        g_assert_nonnull(strstr(error_desc, "device 'cpu'"));
        g_assert_nonnull(strstr(error_desc, "r16000-profile"));
        qobject_unref(ret);
    }
    assert_r16000_properties(to, dest, dest_mode);

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r16000_scache_mode_mismatch(void)
{
    const CacheCapacity *capacity = &cache_capacities[0];

    assert_r16000_migration_rejected(capacity, capacity->reset_mode,
                                     capacity, 0x3a003);
}

static void test_r16000_scache_capacity_mismatch(void)
{
    assert_r16000_migration_rejected(&cache_capacities[0],
                                     cache_capacities[0].reset_mode,
                                     &cache_capacities[1],
                                     cache_capacities[1].reset_mode);
}

static void test_r16000_scache_capacity_mismatch_reverse(void)
{
    assert_r16000_migration_rejected(&cache_capacities[1],
                                     cache_capacities[1].reset_mode,
                                     &cache_capacities[0],
                                     cache_capacities[0].reset_mode);
}

static void assert_r16000_cpu_model_migration_rejected(
    const char *source_model, const char *dest_model,
    bool r16000_source, bool r16000_dest, const char *expected_error)
{
    const CacheCapacity *capacity = &cache_capacities[0];
    g_autofree char *image = NULL;
    g_autofree char *directory = NULL;
    g_autofree char *socket = NULL;
    g_autofree char *uri = NULL;
    g_autofree char *r16000_args = r16000_cpu_args(capacity,
                                                    capacity->reset_mode);
    const char *source_cpu = r16000_source ? r16000_args : source_model;
    const char *dest_cpu = r16000_dest ? r16000_args : dest_model;
    g_autofree char *source_args = NULL;
    g_autofree char *dest_args = NULL;
    GError *error = NULL;
    QTestState *from, *to;
    int fd;
    int64_t deadline;
    bool failed = false;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r16000-model-mismatch-XXXXXX", &image,
                         &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_capacity_guest_image(fd, capacity->bytes);
    close(fd);
    directory = g_dir_make_tmp("sn0-r16000-model-mismatch-XXXXXX", &error);
    g_assert_no_error(error);
    socket = g_build_filename(directory, "migration.sock", NULL);
    uri = g_strdup_printf("unix:%s", socket);
    source_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown",
        source_cpu, image);
    dest_args = g_strdup_printf(
        "-M malta -m 64M -cpu %s -bios %s "
        "-accel tcg,thread=single -nodefaults -no-reboot -no-shutdown "
        "-incoming defer", dest_cpu, image);
    from = qtest_init(source_args);
    to = qtest_init(dest_args);
    wait_for_state(from, CAPACITY_READY_OFF, CACHE_READY_MAGIC);
    if (r16000_source) {
        assert_r16000_properties(from, capacity, capacity->reset_mode);
    }
    if (r16000_dest) {
        assert_r16000_properties(to, capacity, capacity->reset_mode);
    }
    qtest_qmp_assert_success(from, "{'execute':'stop'}");
    qtest_qmp_assert_success(to,
        "{'execute':'migrate-incoming', 'arguments':{'uri':%s,"
        "'exit-on-error':false}}", uri);
    qtest_qmp_assert_success(from,
        "{'execute':'migrate', 'arguments':{'uri':%s}}", uri);

    deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        QDict *ret = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *status = qdict_get_str(ret, "status");

        failed = g_str_equal(status, "failed");
        qobject_unref(ret);
        if (failed) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(failed);
    {
        QDict *ret = qtest_qmp_assert_success_ref(to,
            "{'execute':'query-migrate'}");
        const char *error_desc = qdict_get_str(ret, "error-desc");

        g_assert_nonnull(error_desc);
        g_assert_nonnull(strstr(error_desc, expected_error));
        qobject_unref(ret);
    }
    if (r16000_dest) {
        assert_r16000_properties(to, capacity, capacity->reset_mode);
    }

    qtest_quit(from);
    qtest_quit(to);
    unlink(image);
    unlink(socket);
    g_assert_cmpint(rmdir(directory), ==, 0);
}

static void test_r16000_to_r14000_migration_rejected(void)
{
    assert_r16000_cpu_model_migration_rejected(
        NULL, "R14000", true, false, "cpu/r16000-profile");
}

static void test_r14000_to_r16000_migration_rejected(void)
{
    assert_r16000_cpu_model_migration_rejected(
        "R14000", NULL, false, true, "device 'cpu'");
}

static void test_r16000_to_r4000_migration_rejected(void)
{
    assert_r16000_cpu_model_migration_rejected(
        NULL, "R4000", true, false, "cpu/r16000-profile");
}

/*
 * Index Store Data (S) writes the whole quadword: the doubleword the address
 * names comes from TagHi and TagLo, the other is padded with zeroes, and the
 * check bits from ECC[9:0] cover the result. Fill the other half first, store
 * the addressed half, and read the other half back: it must be zero and carry
 * the second store's check bits, not the first's data. The read goes through
 * all-ones sentinels so a load that did nothing cannot pass as a zero pad.
 */
static void test_r10000_scache_data_padding(void)
{
    g_autofree char *image = NULL;
    g_autofree char *args = NULL;
    GError *error = NULL;
    QTestState *qts;
    int fd;

    if (!qtest_has_accel("tcg")) {
        g_test_skip("TCG is unavailable");
        return;
    }

    fd = g_file_open_tmp("sn0-r10k-scache-XXXXXX", &image, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    write_cache_guest_image(fd, &r10000_cache_probe, 1);
    close(fd);
    args = g_strdup_printf(
        "-M origin200 -m 64M -cpu R10000 -bios %s "
        "-accel tcg -nodefaults -no-reboot -no-shutdown", image);
    qts = qtest_init(args);
    wait_for_state(qts, CACHE_READY_OFF, CACHE_READY_MAGIC);
    qtest_writeq(qts, STATE_PHYS + CACHE_COMMAND_OFF, 1);
    wait_for_state(qts, CACHE_DONE_OFF, CACHE_DONE_MAGIC);

    g_assert_cmphex(state_word(qts, CACHE_DATA_LO_OFF), ==, CACHE_DATA_LO);
    g_assert_cmphex(state_word(qts, CACHE_DATA_HI_OFF), ==, CACHE_DATA_HI);
    g_assert_cmphex(state_word(qts, CACHE_ECC_OFF), ==, CACHE_ECC);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_LO_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_HI_OFF), ==, 0);
    g_assert_cmphex(state_word(qts, CACHE_OTHER_ECC_OFF), ==, CACHE_ECC);

    qtest_quit(qts);
    unlink(image);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_data_func("sn0/r10k-perf-migration/r10000",
                        &configurations[0], test_active_counter);
    qtest_add_data_func("sn0/r10k-perf-migration/r16000",
                        &configurations[1], test_active_counter);
    qtest_add_data_func("sn0/r10k-perf-stream/r10000",
                        &configurations[0], test_stream_representation);
    qtest_add_func("sn0/r10000-scache-migration/stock",
                   test_r10000_scache_migration);
    qtest_add_func("sn0/r10000-scache-data/padding",
                   test_r10000_scache_data_padding);
    for (unsigned int i = 0; i < ARRAY_SIZE(cache_capacities); i++) {
        g_autofree char *name = g_strdup_printf(
            "sn0/r16000-scache-capacity/%s", cache_capacities[i].name);

        qtest_add_data_func(name, &cache_capacities[i],
                            test_r16000_scache_capacity_migration);
    }
    qtest_add_func("sn0/r16000-scache-capacity/mode-mismatch",
                   test_r16000_scache_mode_mismatch);
    qtest_add_func("sn0/r16000-scache-capacity/size-mismatch",
                   test_r16000_scache_capacity_mismatch);
    qtest_add_func("sn0/r16000-scache-capacity/size-mismatch-reverse",
                   test_r16000_scache_capacity_mismatch_reverse);
    qtest_add_func("sn0/r16000-cpu-migration/r16000-to-r14000",
                   test_r16000_to_r14000_migration_rejected);
    qtest_add_func("sn0/r16000-cpu-migration/r14000-to-r16000",
                   test_r14000_to_r16000_migration_rejected);
    qtest_add_func("sn0/r16000-cpu-migration/r16000-to-r4000",
                   test_r16000_to_r4000_migration_rejected);
    return g_test_run();
}
