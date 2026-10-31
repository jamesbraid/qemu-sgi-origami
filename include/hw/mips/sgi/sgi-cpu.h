/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The processor part an SGI machine's catalogue names, applied to its
 * R10000-family CPUs as -cpu properties. The part's identity -- its PRId,
 * and on the R12000 its FPU identification -- is a default that
 * "-cpu <model>,<property>=" overrides the same way on every machine. The
 * reset-mode word is the board's: it drives the word at reset, and the
 * word selects the secondary cache, so a -cpu reset-mode that disagrees
 * is refused. Board values stay machine properties.
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
 * Give the unrealized CPU @dev the board's reset-mode word @mode, refusing
 * a -cpu or -global reset-mode that disagrees.
 */
bool sgi_cpu_set_reset_mode(DeviceState *dev, uint32_t mode, Error **errp);

/*
 * Give the unrealized CPU @dev every value of @processor its model takes
 * as a property: its identity where -cpu or -global did not set it, and
 * on the R12000 its reset-mode word as sgi_cpu_set_reset_mode() does.
 */
bool sgi_cpu_apply_processor(DeviceState *dev,
                             const SGIModelProcessor *processor,
                             Error **errp);

#endif /* HW_MIPS_SGI_SGI_CPU_H */
