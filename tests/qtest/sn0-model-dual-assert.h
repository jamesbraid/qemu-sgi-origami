/*
 * Origin 200 dual-profile model assertions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H
#define TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H

void sn0_model_assert_object_graph(QTestState *qts, const char *machine_type,
                                   const SN0ModelDevice *expected_devices,
                                   size_t expected_count);
void sn0_model_test_dual_config_graph(const void *data);
void sn0_model_test_dual_object_graph(const void *data);

#endif /* TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H */
