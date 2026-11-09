/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * Original synthetic caller inputs for strict O2 product launches. The
 * identity chip is the machine's own, built from its identity properties.
 */
#ifndef SGI_O2_PRODUCT_FIXTURE_H
#define SGI_O2_PRODUCT_FIXTURE_H

#include "qemu/osdep.h"
#include "qemu/units.h"

#define O2PLUS_CPU "R12000,r12000-prid=0xe35,r12000-fpu-id=0x900," \
    "reset-mode=0x11a8603"

typedef struct O2ProductTest {
    char *dir;
    char *rom;
    char *machine;
    bool plus;
} O2ProductTest;

static inline void o2_product_prepare(O2ProductTest *t)
{
    g_autofree uint8_t *rom = g_malloc0(512 * KiB);

    t->dir = g_dir_make_tmp("o2-product-XXXXXX", NULL);
    g_assert_nonnull(t->dir);
    t->rom = g_build_filename(t->dir, "synthetic-rom", NULL);
    rom[0] = 0x5a;
    rom[512 * KiB - 1] = 0xa5;
    g_assert_true(g_file_set_contents(t->rom, (char *)rom, 512 * KiB, NULL));
    t->machine = g_strdup_printf(
        "o2,rom-image=%s,rom-size=524288,rom-offset=0,rom-pad-byte=238,"
        "rom-at29c040a-protection=disabled,"
        "rom-at29c040a-lower-lock=unlocked,"
        "rom-at29c040a-upper-lock=unlocked,"
        "password-clear-input=0", t->rom);
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
    g_assert_cmpint(g_rmdir(t->dir), ==, 0);
    g_free(t->machine);
    g_free(t->rom);
    g_free(t->dir);
}

#endif /* SGI_O2_PRODUCT_FIXTURE_H */
