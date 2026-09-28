/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SGI host command-port framing; device behavior belongs to the owner. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-command-port.h"
#include "trace.h"

void sgi_command_port_init(SGICommandPort *p, const SGICommandPortOps *ops,
                            void *opaque, unsigned port_id,
                            const SGICommandPortSelector *selectors,
                            unsigned num_selectors)
{
    for (unsigned i = 0; i < num_selectors; i++) {
        assert(selectors[i].argc <= SGI_COMMAND_PORT_MAX_ARGS);
    }

    memset(p, 0, sizeof(*p));
    p->ops = ops;
    p->opaque = opaque;
    p->port_id = port_id;
    p->selectors = selectors;
    p->num_selectors = num_selectors;
}

void sgi_command_port_reset(SGICommandPort *p)
{
    const SGICommandPortOps *ops = p->ops;
    void *opaque = p->opaque;
    unsigned port_id = p->port_id;
    const SGICommandPortSelector *selectors = p->selectors;
    unsigned num_selectors = p->num_selectors;

    sgi_command_port_init(p, ops, opaque, port_id, selectors, num_selectors);
}

static const SGICommandPortSelector *
find_selector(const SGICommandPort *p, uint32_t selector)
{
    for (unsigned i = 0; i < p->num_selectors; i++) {
        if (p->selectors[i].selector == selector) {
            return &p->selectors[i];
        }
    }
    return NULL;
}

static void push_word(SGICommandPort *p, uint32_t word)
{
    trace_sgi_command_port_word(p->port_id, word);

    if (!p->framing) {
        const SGICommandPortSelector *found = find_selector(p, word);

        if (!found) {
            trace_sgi_command_port_unsupported(p->port_id, word);
            if (p->ops->unsupported) {
                p->ops->unsupported(p->opaque, p->port_id, word);
            }
            return;
        }
        p->framing = true;
        p->selector = word;
        p->argc_expected = found->argc;
        p->argc_have = 0;
    } else {
        p->args[p->argc_have++] = word;
    }

    if (p->framing && p->argc_have == p->argc_expected) {
        uint32_t selector = p->selector;
        uint32_t argc = p->argc_expected;

        p->framing = false;
        p->selector = 0;
        p->argc_expected = 0;
        p->argc_have = 0;
        trace_sgi_command_port_packet(p->port_id, selector, argc);
        if (p->ops->packet) {
            p->ops->packet(p->opaque, p->port_id, selector, p->args, argc);
        }
    }
}

void sgi_command_port_write(SGICommandPort *p, uint64_t value, unsigned size)
{
    assert(size == 4 || size == 8);
    if (size == 8) {
        push_word(p, value >> 32);
        push_word(p, value & 0xffffffff);
    } else {
        push_word(p, value);
    }
}

uint32_t sgi_command_port_credit_read(SGICommandPort *p)
{
    if (p->ops->credit) {
        return p->ops->credit(p->opaque, p->port_id);
    }
    return 0;
}

void sgi_command_port_kick_write(SGICommandPort *p, uint32_t value)
{
    p->kick = value;
    if (p->ops->kick) {
        p->ops->kick(p->opaque, p->port_id, value);
    }
}

uint32_t sgi_command_port_kick_read(const SGICommandPort *p)
{
    return p->kick;
}
