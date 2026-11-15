/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "libqtest.h"

#define FIXTURE_PATH "/machine/peripheral/irq-test"

static QTestState *start_fixture(void)
{
    return qtest_init("-machine none -device qtest-irq-fixture,id=irq-test");
}

static void pulse(QTestState *qts, const char *name, int line)
{
    qtest_set_irq_in(qts, FIXTURE_PATH, name, line, 1);
    qtest_set_irq_in(qts, FIXTURE_PATH, name, line, 0);
}

static void test_inputs(void)
{
    QTestState *qts = start_fixture();

    qtest_irq_intercept_in(qts, FIXTURE_PATH);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 511), ==, 0);
    for (int i = 0; i < 3; i++) {
        pulse(qts, "wide", 511);
    }
    g_assert_false(qtest_get_irq(qts, 511));
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 511), ==, 3);

    pulse(qts, "narrow", 1);
    g_assert_false(qtest_get_irq(qts, 1));
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 511), ==, 3);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 4096), ==, 0);
    g_assert_false(qtest_get_irq(qts, 4096));
    qtest_quit(qts);
}

static void test_wide_outputs(void)
{
    QTestState *qts = start_fixture();

    qtest_irq_intercept_out_named(qts, FIXTURE_PATH, "wide-out");
    pulse(qts, "wide", 511);
    g_assert_false(qtest_get_irq(qts, 511));
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 511), ==, 1);
    pulse(qts, "narrow", 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, 0);
    qtest_quit(qts);
}

static void test_narrow_outputs(void)
{
    QTestState *qts = start_fixture();

    qtest_irq_intercept_out_named(qts, FIXTURE_PATH, "narrow-out");
    pulse(qts, "narrow", 1);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 1), ==, 1);
    pulse(qts, "wide", 511);
    g_assert_cmpuint(qtest_get_irq_raise_count(qts, 511), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/qtest/irq/inputs", test_inputs);
    qtest_add_func("/qtest/irq/wide-outputs", test_wide_outputs);
    qtest_add_func("/qtest/irq/narrow-outputs", test_narrow_outputs);
    return g_test_run();
}
