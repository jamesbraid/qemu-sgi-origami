/*
 * Origin 200 dual-profile model assertions.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H
#define TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H

typedef struct {
    unsigned int hub_ni_revision;
    unsigned int hub_widget_revision;
    unsigned int bridge_revision;
    unsigned int ioc3_pci_revision;
    unsigned int isp1040_pci_revision;
} SN0ModelDeviceIdentity;

#define SN0_MODEL_DEVICE_IDENTITY_INIT { \
    .hub_ni_revision = SN0_MODEL_HUB_NI_REVISION, \
    .hub_widget_revision = SN0_MODEL_HUB_WIDGET_REVISION, \
    .bridge_revision = SN0_MODEL_BRIDGE_REVISION, \
    .ioc3_pci_revision = SN0_MODEL_IOC3_PCI_REVISION, \
    .isp1040_pci_revision = SN0_MODEL_ISP1040_PCI_REVISION, \
}

void sn0_model_assert_device_identity(QTestState *qts, unsigned int node,
                                      const SN0ModelDeviceIdentity *expected);
bool sn0_model_reconstruct_graph(const SN0ModelConfigGraph *graph,
                                 uint8_t *image, size_t image_size);
bool sn0_model_apply_graph_divergences(
    const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences,
    const uint8_t *got, uint8_t *expected);
void sn0_model_assert_graph_fields(
    QTestState *qts, const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences);
void sn0_model_assert_graph_image(
    QTestState *qts, const SN0ModelConfigGraph *graph,
    const SN0ModelDivergence *divergences);
void sn0_model_assert_object_graph(QTestState *qts, const char *machine_type,
                                   const SN0ModelDevice *expected_devices,
                                   size_t expected_count);
void sn0_model_test_dual_config_graph(const void *data);
void sn0_model_test_dual_device_identity(const void *data);
void sn0_model_test_dual_object_graph(const void *data);
void sn0_model_test_dual_divergence_masking(const void *data);

#endif /* TESTS_QTEST_SN0_MODEL_DUAL_ASSERT_H */
