/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_TIMER_H
#define HW_MIPS_SGI_MACE_TIMER_H

static inline uint64_t sgi_mace_compare_due_tick(uint64_t now_tick,
                                                 uint32_t compare)
{
    uint32_t delta = compare - (uint32_t)now_tick;

    return delta ? now_tick + delta : UINT64_MAX;
}

static inline bool sgi_mace_compare_reached(uint64_t due_tick,
                                            uint64_t now_tick)
{
    return due_tick != UINT64_MAX && now_tick >= due_tick;
}

#endif
