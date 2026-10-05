/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The processor part an SGI machine's catalogue names, applied to its
 * R10000-family CPUs as -cpu properties. A value that describes the part
 * -- its PRId, and on the R12000 its FPU identification, reset-mode word
 * and the cache geometry that word selects -- is a CPU property, so
 * "-cpu <model>,<property>=" overrides it the same way on every machine.
 * Board values stay machine properties.
 */

#ifndef HW_MIPS_SGI_SGI_CPU_H
#define HW_MIPS_SGI_SGI_CPU_H

#include "hw/mips/sgi/sgi-machines.h"

/*
 * Give the unrealized CPU @dev the PRId @prid through its r10000-prid or
 * r14000-prid property, unless -cpu or -global already set it. A CPU
 * model with neither property is left alone.
 */
bool sgi_cpu_set_prid(DeviceState *dev, uint32_t prid, Error **errp);

/*
 * Give the unrealized CPU @dev every value of @processor its model takes
 * as a property, each only where -cpu or -global did not set it.
 */
bool sgi_cpu_apply_processor(DeviceState *dev,
                             const SGIModelProcessor *processor,
                             Error **errp);

#endif /* HW_MIPS_SGI_SGI_CPU_H */
