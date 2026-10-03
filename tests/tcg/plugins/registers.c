/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Copyright (C) 2026, Florian Hofhammer <florian.hofhammer@epfl.ch>
 */
#include "glib.h"
#include <inttypes.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdint.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

static bool mips_target;
static size_t mips_reg_width;
static bool mips_big_endian;
static struct qemu_plugin_register *mips_regs[9];
static bool mips_values_verified;

static const char *const mips_reg_names[] = {
    "r4", "r5", "r6", "r7", "r28", "r18", "r19", "r2", "r3",
};

static uint64_t mips_reg_values[9];

static bool mips_target_info(const char *name)
{
    if (g_str_equal(name, "mips") || g_str_equal(name, "mipsel")) {
        mips_reg_width = 4;
    } else if (g_str_equal(name, "mips64") ||
               g_str_equal(name, "mips64el") ||
               g_str_equal(name, "mipsn32") ||
               g_str_equal(name, "mipsn32el")) {
        mips_reg_width = 8;
    } else {
        return false;
    }
    mips_big_endian = g_str_equal(name, "mips") ||
                      g_str_equal(name, "mips64") ||
                      g_str_equal(name, "mipsn32");
    return true;
}

static void mips_check_values(unsigned int vcpu_index, void *userdata)
{
    g_autoptr(GByteArray) buf = g_byte_array_new();

    for (size_t i = 0; i < G_N_ELEMENTS(mips_regs); i++) {
        uint64_t value = 0;
        bool success = qemu_plugin_read_register(mips_regs[i], buf);

        g_assert(success);
        g_assert_cmpuint(buf->len, ==, mips_reg_width);
        for (size_t j = 0; j < buf->len; j++) {
            size_t byte = mips_big_endian ? j : buf->len - j - 1;
            value = (value << 8) | buf->data[byte];
        }
        g_assert_cmphex(value, ==, mips_reg_values[i]);
        g_byte_array_set_size(buf, 0);
    }
    mips_values_verified = true;
}

static void mips_check_syscall(struct qemu_plugin_tb *tb, void *userdata)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        char *disas = qemu_plugin_insn_disas(insn);

        if (g_str_has_prefix(disas, "syscall")) {
            qemu_plugin_register_vcpu_insn_exec_cb(
                insn, mips_check_values, QEMU_PLUGIN_CB_R_REGS, NULL);
        }
        g_free(disas);
    }
}

static void mips_check_exit(void *userdata)
{
    g_assert(mips_values_verified);
}

/*
 * This plugin tests whether we can read and write registers via the plugin
 * API. We try to just read/write a single register, as some architectures have
 * registers that cannot be written to, which would fail the test.
 * See: https://lists.gnu.org/archive/html/qemu-devel/2026-02/msg07025.html
 */
static void vcpu_init_cb(unsigned int vcpu_index, void *userdata)
{
    g_autoptr(GArray) regs = qemu_plugin_get_registers();
    g_assert(regs != NULL);
    g_autoptr(GByteArray) buf = g_byte_array_sized_new(0);
    qemu_plugin_reg_descriptor *reg_desc = NULL;
    bool success = false;

    if (mips_target) {
        g_assert_cmpuint(regs->len, ==, 73);
        for (size_t i = 0; i < G_N_ELEMENTS(mips_reg_names); i++) {
            bool found = false;

            for (size_t j = 0; j < regs->len; j++) {
                reg_desc = &g_array_index(regs, qemu_plugin_reg_descriptor, j);
                if (g_strcmp0(reg_desc->name, mips_reg_names[i]) == 0) {
                    g_byte_array_set_size(buf, 0);
                    g_assert(qemu_plugin_read_register(reg_desc->handle, buf));
                    g_assert_cmpuint(buf->len, ==, mips_reg_width);
                    mips_regs[i] = reg_desc->handle;
                    found = true;
                    break;
                }
            }
            g_assert(found);
        }
    }

    /* Make sure we can read and write a register not marked as readonly */
    for (size_t i = 0; i < regs->len; i++) {
        reg_desc = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (!reg_desc->is_readonly) {
            g_byte_array_set_size(buf, 0);
            success = qemu_plugin_read_register(reg_desc->handle, buf);
            g_assert(success);
            g_assert(buf->len > 0);
            success = qemu_plugin_write_register(reg_desc->handle, buf);
            g_assert(success);
            break;
        } else {
            reg_desc = NULL;
        }
    }
    g_assert(regs->len == 0 || reg_desc != NULL);

    /*
     * Check whether we can still read a read-only register. On each
     * architecture, at least the PC should be read-only because it's only
     * supposed to be modified via the qemu_plugin_set_pc() function.
     */
    for (size_t i = 0; i < regs->len; i++) {
        reg_desc = &g_array_index(regs, qemu_plugin_reg_descriptor, i);
        if (reg_desc->is_readonly) {
            g_byte_array_set_size(buf, 0);
            success = qemu_plugin_read_register(reg_desc->handle, buf);
            g_assert(success);
            g_assert(buf->len > 0);
            break;
        } else {
            reg_desc = NULL;
        }
    }
    g_assert(regs->len == 0 || reg_desc != NULL);
    /*
     * Note: we currently do not test whether the read-only register can be
     * written to, because doing so would throw an assert in the plugin API.
     */
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,
                                           const qemu_info_t *info,
                                           int argc, char **argv)
{
    mips_target = argc == 1 &&
                  g_strcmp0(argv[0], "mips-registers=on") == 0;
    if (mips_target) {
        if (!mips_target_info(info->target_name)) {
            return -1;
        }
        mips_reg_values[0] = 0;
        mips_reg_values[1] = 0x1234567;
        mips_reg_values[2] = 0x2345678;
        mips_reg_values[3] = 0x3456789;
        mips_reg_values[4] = 0x456789a;
        mips_reg_values[5] = 0x56789ab;
        mips_reg_values[6] = 0x6789abc;
        mips_reg_values[7] = g_str_equal(info->target_name, "mipsn32") ||
                             g_str_equal(info->target_name, "mipsn32el") ?
                             6058 : mips_reg_width == 8 ? 5058 : 4001;
        mips_reg_values[8] = 0x789abcd;
        qemu_plugin_register_vcpu_tb_trans_cb(id, mips_check_syscall, NULL);
        qemu_plugin_register_atexit_cb(id, mips_check_exit, NULL);
    }
    qemu_plugin_register_vcpu_init_cb(id, vcpu_init_cb, NULL);
    return 0;
}
