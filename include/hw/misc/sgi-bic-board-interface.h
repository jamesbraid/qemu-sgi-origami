/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_SGI_BIC_BOARD_INTERFACE_H
#define HW_SGI_BIC_BOARD_INTERFACE_H

#include "hw/core/qdev.h"
#include "exec/memattrs.h"

#define TYPE_SGI_BIC_BOARD_INTERFACE "sgi-bic-board-interface"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBICBoardInterfaceState, SGI_BIC_BOARD_INTERFACE)

/* Offsets in a caller-selected remote Hub-special address space. */
MemTxResult sgi_bic_board_read(SGIBICBoardInterfaceState *s, hwaddr offset,
                              unsigned size, uint64_t *value);
MemTxResult sgi_bic_board_write(SGIBICBoardInterfaceState *s, hwaddr offset,
                               unsigned size, uint64_t value);
/* Position zero is fitted. Completion failure preserves value and validity. */
MemTxResult sgi_bic_board_controller_byte(SGIBICBoardInterfaceState *s,
                                   unsigned position, bool high,
                                   uint8_t value, bool completed);
#endif
