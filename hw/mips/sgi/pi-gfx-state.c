/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Graphics flow control in the Hub and Bedrock processor interfaces: the
 * migrated state and the node interface. See the header for the contract.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "hw/mips/sgi/pi-gfx.h"

/* A stream's values are kept to the fields a write would keep. */
static int sgi_pi_gfx_post_load(void *opaque, int version_id)
{
    SGIPIGfx *g = opaque;

    for (unsigned int n = 0; n < SGI_PI_GFX_REGS; n++) {
        sgi_pi_gfx_write(g, n, sgi_pi_gfx_read(g, n));
    }
    return 0;
}

static bool sgi_pi_gfx_starved_needed(void *opaque)
{
    const SGIPIGfx *g = opaque;

    for (unsigned int i = 0; i < SGI_PI_GFX_SLOTS; i++) {
        if (g->starved[i]) {
            return true;
        }
    }
    return false;
}

/* Only while a slot's graphics interrupt waits to be re-armed. */
static const VMStateDescription vmstate_sgi_pi_gfx_starved = {
    .name = "sgi-pi-gfx/starved",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_pi_gfx_starved_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL_ARRAY(starved, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_pi_gfx_stopped_needed(void *opaque)
{
    const SGIPIGfx *g = opaque;

    for (unsigned int i = 0; i < SGI_PI_GFX_SLOTS; i++) {
        if (g->stopped[i]) {
            return true;
        }
    }
    return false;
}

/* Only while a Bedrock slot's next counted store must wait. */
static const VMStateDescription vmstate_sgi_pi_gfx_stopped = {
    .name = "sgi-pi-gfx/stopped",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_pi_gfx_stopped_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL_ARRAY(stopped, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_END_OF_LIST()
    },
};

static bool sgi_pi_gfx_int_running_needed(void *opaque)
{
    const SGIPIGfx *g = opaque;

    for (unsigned int i = 0; i < SGI_PI_GFX_SLOTS; i++) {
        if (g->int_running[i]) {
            return true;
        }
    }
    return false;
}

/* Only while a Bedrock slot's interrupt counter runs. */
static const VMStateDescription vmstate_sgi_pi_gfx_int_running = {
    .name = "sgi-pi-gfx/int-running",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = sgi_pi_gfx_int_running_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL_ARRAY(int_running, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_UINT64_ARRAY(int_epoch, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_END_OF_LIST()
    },
};

/* The stall log flag is host-only and starts clear on the destination. */
const VMStateDescription vmstate_sgi_pi_gfx = {
    .name = "sgi-pi-gfx",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = sgi_pi_gfx_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64_ARRAY(page, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_UINT32_ARRAY(credit, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_UINT32_ARRAY(bias, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_UINT32_ARRAY(int_count, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_UINT32_ARRAY(int_compare, SGIPIGfx, SGI_PI_GFX_SLOTS),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_pi_gfx_starved,
        &vmstate_sgi_pi_gfx_stopped,
        &vmstate_sgi_pi_gfx_int_running,
        NULL
    },
};

bool sgi_pi_gfx_store_counted(void)
{
    static bool warned;

    if (!current_cpu) {
        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_GUEST_ERROR, "sgi-pi-gfx: a head asked whether "
                          "a store was counted with no processor storing; "
                          "its credit will not be returned\n");
        }
        return false;
    }
    return sgi_pi_gfx_take_mark();
}

void sgi_pi_gfx_return_credits(Object *node, unsigned int widget,
                               uint32_t credits)
{
    SGIPIGfxClass *pc = SGI_PI_GFX_GET_CLASS(node);

    if (!pc->return_credits) {
        qemu_log_mask(LOG_UNIMP, "sgi-pi-gfx: %s returns no graphics "
                      "credits\n", object_get_typename(node));
        return;
    }
    pc->return_credits(node, widget, credits);
}

static const TypeInfo sgi_pi_gfx_types[] = {
    {
        .name = TYPE_SGI_PI_GFX,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(SGIPIGfxClass),
    },
};

DEFINE_TYPES(sgi_pi_gfx_types)
