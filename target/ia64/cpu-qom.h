/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
#ifndef TARGET_IA64_CPU_QOM_H
#define TARGET_IA64_CPU_QOM_H

#include "hw/core/cpu.h"

#define TYPE_IA64_CPU "ia64-cpu"

OBJECT_DECLARE_CPU_TYPE(IA64CPU, IA64CPUClass, IA64_CPU)

#define IA64_CPU_TYPE_SUFFIX "-" TYPE_IA64_CPU
#define IA64_CPU_TYPE_NAME(name) (name IA64_CPU_TYPE_SUFFIX)

#endif
