/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "sn1-topology.h"

unsigned sn1_cpu_slot(const SN1MachineModel *model, unsigned logical)
{
    unsigned mask = model->cpu_population_mask ?
        model->cpu_population_mask : ((1U << model->cpu_count) - 1);

    for (unsigned physical = 0; physical < 4; physical++) {
        if (mask & (1U << physical)) {
            if (logical == 0) {
                return physical;
            }
            logical--;
        }
    }
    g_assert_not_reached();
}
