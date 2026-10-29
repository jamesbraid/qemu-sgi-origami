/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Original synthetic caller inputs for strict O2 product launches. */
#ifndef SGI_O2_PRODUCT_FIXTURE_H
#define SGI_O2_PRODUCT_FIXTURE_H

#include "qemu/osdep.h"
#include "qemu/units.h"

#define O2PLUS_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "r12000-reset-mode=0x11a8603,r12000-scache-bytes=2097152," \
    "r12000-scache-block-words=16"

typedef struct O2ProductTest {
    char *dir;
    char *rom;
    char *identity;
    char *machine;
    bool plus;
    const char *serial;      /* optional caller-owned synthetic serial */
} O2ProductTest;

static inline void o2_product_prepare(O2ProductTest *t)
{
    g_autofree uint8_t *rom = g_malloc0(512 * KiB);
    uint8_t identity[128];

    t->dir = g_dir_make_tmp("o2-product-XXXXXX", NULL);
    g_assert_nonnull(t->dir);
    t->rom = g_build_filename(t->dir, "synthetic-rom", NULL);
    t->identity = g_build_filename(t->dir, "synthetic-identity", NULL);
    for (unsigned i = 0; i < sizeof(identity); i++) {
        identity[i] = (i * 37) ^ 0xa5;
    }
    rom[0] = 0x5a;
    rom[512 * KiB - 1] = 0xa5;
    g_assert_true(g_file_set_contents(t->rom, (char *)rom, 512 * KiB, NULL));
    g_assert_true(g_file_set_contents(t->identity, (char *)identity,
                                      sizeof(identity), NULL));
    t->machine = g_strdup_printf(
        "o2,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238,"
        "rom-at29c040a-protection=disabled,"
        "rom-at29c040a-lower-lock=unlocked,"
        "rom-at29c040a-upper-lock=unlocked,"
        "identity-test-serial=%s,identity-eprom-file=%s,"
        "identity-status=0123456789abcd,password-clear-input=0",
        t->rom, t->serial ? t->serial : "1032547698ba", t->identity);
}

static inline void o2_product_select_plus(O2ProductTest *t)
{
    char *machine = g_strdup_printf("%s,variant=o2plus", t->machine);

    g_free(t->machine);
    t->machine = machine;
    t->plus = true;
}

static inline void o2_product_cleanup(O2ProductTest *t)
{
    g_assert_cmpint(g_unlink(t->rom), ==, 0);
    g_assert_cmpint(g_unlink(t->identity), ==, 0);
    g_assert_cmpint(g_rmdir(t->dir), ==, 0);
    g_free(t->machine);
    g_free(t->identity);
    g_free(t->rom);
    g_free(t->dir);
}

#endif /* SGI_O2_PRODUCT_FIXTURE_H */
