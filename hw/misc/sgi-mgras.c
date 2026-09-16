/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bounded functional SI Viz endpoint for the Origin 2000 graphics path. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-mgras.h"
#include "migration/vmstate.h"
#include "trace.h"

#define SI_PENDING_FLAGS 0x00080000
#define SI_PENDING_CONTEXT 0x10000000
#define SI_PENDING_RETRACE 0x00000001
#define SI_PENDING_ERROR 0x00000002

static void si_update_irq(SGIMgrasState *s)
{
    uint32_t active = s->pending & s->irq_mask;
    qemu_set_irq(s->irq[0], !!(active & SI_PENDING_FLAGS));
    qemu_set_irq(s->irq[1], !!(active & SI_PENDING_CONTEXT));
    qemu_set_irq(s->irq[2], !!(active & SI_PENDING_RETRACE));
    qemu_set_irq(s->irq[3], !!(active & SI_PENDING_ERROR));
}

static void si_error(SGIMgrasState *s, hwaddr addr, unsigned size,
                     const char *why)
{
    s->error = 1;
    s->error_addr = addr;
    s->error_width = size;
    s->protocol = 0xff;
    s->pending |= SI_PENDING_ERROR;
    si_update_irq(s);
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mgras: %s at 0x%" HWADDR_PRIx
                  " width %u\n", why, addr, size);
}

static void si_retrace(void *opaque)
{
    SGIMgrasState *s = opaque;
    if (s->display_enabled) {
        s->retraces++;
        s->pending |= SI_PENDING_RETRACE;
        si_update_irq(s);
    }
    timer_mod(s->retrace_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              NANOSECONDS_PER_SECOND / 60);
}

static void si_complete_context(SGIMgrasState *s)
{
    s->ready = true;
    s->protocol = 3;
    s->pending |= SI_PENDING_CONTEXT;
    si_update_irq(s);
}

static void si_context_write(SGIMgrasState *s, uint32_t value)
{
    if (!s->entry_selected || s->protocol == 0xff) {
        si_error(s, 0x120400, 4, "context write in invalid state");
        return;
    }
    if (s->protocol == 1) {
        if (!s->first_ack) {
            si_error(s, 0x120400, 4, "context write before first acknowledgement");
            return;
        }
        if (s->context_words == 63) {
            if (!s->second_ack || value != 0x0000f400) {
                si_error(s, 0x120400, 4, "context token missing");
                return;
            }
            si_complete_context(s);
            return;
        }
        s->context[s->context_words++] = value;
        if (s->context_words == 63) {
            s->pending |= SI_PENDING_CONTEXT;
            si_update_irq(s);
        }
        return;
    }
    si_error(s, 0x120400, 4, "context write after completion");
}

static void si_draw_fill(SGIMgrasState *s)
{
    uint32_t xy0 = s->command[0x147], xy1 = s->command[0x148];
    uint32_t color = s->command[0x140] & 0x00ffffff;
    int x0 = (xy0 >> 16) & 0xffff, y0 = xy0 & 0xffff;
    int x1 = (xy1 >> 16) & 0xffff, y1 = xy1 & 0xffff;
    if (x0 > x1 || y0 > y1 || x0 >= SGI_MGRAS_WIDTH || y0 >= SGI_MGRAS_HEIGHT) {
        return;
    }
    x1 = MIN(x1, SGI_MGRAS_WIDTH - 1);
    y1 = MIN(y1, SGI_MGRAS_HEIGHT - 1);
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            s->front[y * SGI_MGRAS_WIDTH + x] = color;
        }
    }
}

static void si_command(SGIMgrasState *s, uint64_t word)
{
    uint32_t selector = (word >> 34) & 0xffffff;
    uint32_t value = word;
    if (selector < 0x60001 || ((selector - 0x60001) & 0xf)) {
        si_error(s, 0x120400, 8, "unknown command selector");
        return;
    }
    uint32_t reg = (selector - 0x60001) >> 4;
    if (reg >= ARRAY_SIZE(s->command)) {
        si_error(s, 0x120400, 8, "command register out of range");
        return;
    }
    if (s->fifo_count == ARRAY_SIZE(s->fifo) / 2) {
        si_error(s, 0x120400, 8, "command FIFO full");
        return;
    }
    s->command[reg] = value;
    s->command_valid[reg] = 1;
    if (reg == 0x47) {
        if (!s->command_valid[0x147] || !s->command_valid[0x148] ||
            !s->command_valid[0x140]) {
            si_error(s, 0x120400, 8, "incomplete fill command");
            return;
        }
        si_draw_fill(s);
        s->fifo_count = 0;
    }
}

static uint64_t si_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIMgrasState *s = opaque;
    trace_sgi_mgras_mmio(addr, 0, size, false);
    if (size != 4 && size != 8) {
        si_error(s, addr, size, "unsupported read width");
        return 0;
    }
    switch (addr) {
    case 0x000004:
        return (s->revision << 28) | (s->part << 12) |
               (s->manufacturer << 1) | 1;
    case 0x008000: return 0;
    case 0x011020: return s->pending;
    case 0x011040: return s->irq_mask;
    case 0x020000: return s->error ? 0x80000000 : 1;
    case 0x020008: return s->fifo_count;
    case 0x020100: return 31 << 4;
    case 0x020200: return 0;
    case 0x02c578: return 0x100;
    case 0x071208: return 0x08;
    case 0x071c88: return s->cmap_index;
    case 0x071f00: return 1;
    case 0x072038: return s->cursor_control;
    case 0x130500: return s->entry;
    case 0x070d18: return s->cmap[s->cmap_index & 255];
    case 0x120400: return s->ready ? 0xf400 : 0;
    case 0x58230:
    case 0x5822c:
        return s->ge_addr < s->ge_words ? s->ge[s->ge_addr] : 0;
    default:
        if (addr >= 0x050000 && addr < 0x050000 + sizeof(s->hq) && size == 4) {
            return s->hq[(addr - 0x050000) >> 2] & 0x00ffffff;
        }
        si_error(s, addr, size, "unsupported read");
        return 0;
    }
}

static void si_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    SGIMgrasState *s = opaque;
    trace_sgi_mgras_mmio(addr, value, size, true);
    if ((size != 4 && size != 8) || (addr & (size - 1))) {
        si_error(s, addr, size, "unsupported or unaligned write");
        return;
    }
    switch (addr) {
    case 0x011028:
        s->pending &= ~(uint32_t)value;
        if (value & SI_PENDING_FLAGS) s->first_ack = true;
        if (value & SI_PENDING_CONTEXT) s->second_ack = true;
        si_update_irq(s); return;
    case 0x011040: s->irq_mask = value; si_update_irq(s); return;
    case 0x011048: s->error = 0; s->pending &= ~SI_PENDING_ERROR; si_update_irq(s); return;
    case 0x130500:
        if (size != 4 || value != 0x4fc || !s->prefix) {
            si_error(s, addr, size, "invalid HQ entry"); return;
        }
        s->entry = value; s->entry_selected = true; s->protocol = 1;
        s->first_ack = s->second_ack = false;
        s->pending |= SI_PENDING_FLAGS; si_update_irq(s); return;
    case 0x120400:
        if (size == 8 && value == 0x11000 && !s->entry_selected) {
            s->prefix = value;
        } else if (size == 4) {
            si_context_write(s, value);
        } else if (size == 8 && s->ready) {
            si_command(s, value);
        } else {
            si_error(s, addr, size, "unsupported context width");
        }
        return;
    case 0x070c30: s->cmap_index = value & 255; return;
    case 0x070d18: s->cmap[s->cmap_index & 255] = value; return;
    case 0x071f00: return;
    case 0x072038: s->cursor_control = value; s->cursor_enabled = !!(value & 0x1d000000); return;
    case 0x072000: s->vc3[(value >> 2) & 63] = value; s->display_enabled = true; return;
    default: break;
    }
    if (addr >= 0x050000 && addr < 0x050000 + sizeof(s->hq) && size == 4) {
        s->hq[(addr - 0x050000) >> 2] = value;
        s->hq_words = MAX(s->hq_words, ((addr - 0x050000) >> 2) + 1);
        return;
    }
    if (addr == 0x120400 && size == 8) { si_command(s, value); return; }
    if (addr == 0x58044 || addr == 0x5804c) { s->ge_addr = value % (59392 * 3); return; }
    if (addr == 0x58040 || addr == 0x58048) {
        if (s->ge_addr < 59392 * 3) {
            s->ge[s->ge_addr++] = value;
            s->ge_words = MAX(s->ge_words, s->ge_addr);
        }
        else si_error(s, addr, size, "GE upload overflow");
        return;
    }
    si_error(s, addr, size, "unsupported write");
}

static const MemoryRegionOps si_ops = {
    .read = si_read, .write = si_write, .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 8 },
};

static void si_reset(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    memset(s->front, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->front));
    memset(s->back, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT * sizeof(*s->back));
    memset(s->wid, 0, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    memset(s->hq, 0, sizeof(s->hq)); memset(s->ge, 0, sizeof(uint32_t) * 59392 * 3);
    memset(s->context, 0, sizeof(s->context)); memset(s->cursor, 0, sizeof(s->cursor));
    memset(s->command_valid, 0, sizeof(s->command_valid));
    s->pending = s->irq_mask = s->error = s->context_words = 0;
    s->entry = s->protocol = s->prefix = 0; s->entry_selected = s->ready = false;
    s->display_enabled = s->cursor_enabled = false; s->fifo_count = 0;
    s->xmap_mode = 0x7a4; s->cmap_index = 0; s->retraces = 0;
    si_update_irq(s);
}

static void si_realize(DeviceState *dev, Error **errp)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    s->front = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->back = g_new0(uint32_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->front_count = s->back_count = SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT;
    s->wid = g_new0(uint8_t, SGI_MGRAS_WIDTH * SGI_MGRAS_HEIGHT);
    s->ge = g_new0(uint32_t, 59392 * 3);
    memory_region_init_io(&s->regs, OBJECT(dev), &si_ops, s, "sgi-mgras.regs", SGI_MGRAS_MMIO_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->regs);
    qdev_init_gpio_out(DEVICE(dev), s->irq, 4);
    s->retrace_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, si_retrace, s);
    si_reset(dev);
}

static void si_unrealize(DeviceState *dev)
{
    SGIMgrasState *s = SGI_MGRAS(dev);
    timer_del(s->retrace_timer); timer_free(s->retrace_timer);
    g_free(s->front); g_free(s->back); g_free(s->wid); g_free(s->ge);
}

static const VMStateDescription vmstate_sgi_mgras = {
    .name = TYPE_SGI_MGRAS, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(front_count, SGIMgrasState),
        VMSTATE_VARRAY_UINT32(front, SGIMgrasState, front_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32(back_count, SGIMgrasState),
        VMSTATE_VARRAY_UINT32(back, SGIMgrasState, back_count, 0,
                              vmstate_info_uint32, uint32_t),
        VMSTATE_UINT32_ARRAY(hq, SGIMgrasState, 5120),
        VMSTATE_UINT32_ARRAY(context, SGIMgrasState, 63),
        VMSTATE_UINT32(context_words, SGIMgrasState), VMSTATE_UINT32(entry, SGIMgrasState),
        VMSTATE_UINT32(protocol, SGIMgrasState), VMSTATE_UINT64(prefix, SGIMgrasState),
        VMSTATE_BOOL(entry_selected, SGIMgrasState), VMSTATE_BOOL(ready, SGIMgrasState),
        VMSTATE_UINT32(pending, SGIMgrasState), VMSTATE_UINT32(irq_mask, SGIMgrasState),
        VMSTATE_UINT32(error, SGIMgrasState), VMSTATE_UINT32(error_addr, SGIMgrasState),
        VMSTATE_UINT32(error_width, SGIMgrasState), VMSTATE_UINT32(cmap_index, SGIMgrasState),
        VMSTATE_UINT32_ARRAY(cmap, SGIMgrasState, 256), VMSTATE_UINT64(retraces, SGIMgrasState),
        VMSTATE_UINT32(revision, SGIMgrasState), VMSTATE_UINT32(part, SGIMgrasState),
        VMSTATE_UINT32(manufacturer, SGIMgrasState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property si_properties[] = {
    DEFINE_PROP_UINT32("revision", SGIMgrasState, revision, 0),
    DEFINE_PROP_UINT32("part", SGIMgrasState, part, 0xc003),
    DEFINE_PROP_UINT32("manufacturer", SGIMgrasState, manufacturer, 0x02aa),
};

static void si_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = si_realize; dc->unrealize = si_unrealize; device_class_set_legacy_reset(dc, si_reset);
    dc->vmsd = &vmstate_sgi_mgras; dc->desc = "SGI MGRAS SI functional endpoint";
    device_class_set_props(dc, si_properties);
}

static const TypeInfo si_types[] = {{ .name = TYPE_SGI_MGRAS, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIMgrasState), .class_init = si_class_init }};
DEFINE_TYPES(si_types)
