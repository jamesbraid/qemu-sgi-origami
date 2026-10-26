/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI host command-port framing; device behavior belongs to the owner. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-command-port.h"
#include "trace.h"

/*
 * A selector table's lookup, built once per table and kept for the life of
 * the process: slot[s] is one more than the index of the entry for selector
 * s, 0 for none. Every word a port takes between packets is looked up, and
 * an owner's table can run to a few hundred entries, so a linear search
 * cost more than the framing it served. A table with a selector at or past
 * SGI_COMMAND_PORT_INDEX_LIMIT is not indexed and is still searched.
 */
#define SGI_COMMAND_PORT_INDEX_LIMIT 0x10000

struct SGICommandPortIndex {
    const SGICommandPortSelector *selectors;
    unsigned num_selectors;
    bool indexed;
    uint32_t limit;
    uint16_t slot[];
};

static GMutex sgi_command_port_indexes_lock;
static GHashTable *sgi_command_port_indexes;

static SGICommandPortIndex *
sgi_command_port_index_new(const SGICommandPortSelector *selectors,
                           unsigned num_selectors)
{
    SGICommandPortIndex *index;
    bool indexed = num_selectors < UINT16_MAX;
    uint32_t limit = 0;

    for (unsigned i = 0; i < num_selectors; i++) {
        if (selectors[i].selector >= SGI_COMMAND_PORT_INDEX_LIMIT) {
            indexed = false;
        } else {
            limit = MAX(limit, selectors[i].selector + 1);
        }
    }
    if (!indexed) {
        limit = 0;
    }
    index = g_malloc0(sizeof(*index) + limit * sizeof(index->slot[0]));
    index->selectors = selectors;
    index->num_selectors = num_selectors;
    index->indexed = indexed;
    index->limit = limit;
    /* Backwards, so a repeated selector keeps its first entry. */
    for (unsigned i = indexed ? num_selectors : 0; i-- > 0;) {
        index->slot[selectors[i].selector] = i + 1;
    }
    return index;
}

/*
 * The lookup for a table, shared by every port framing on it. Two ports
 * may give the same table different lengths; each length gets its own.
 */
static const SGICommandPortIndex *
sgi_command_port_index(const SGICommandPortSelector *selectors,
                       unsigned num_selectors)
{
    SGICommandPortIndex *index = NULL;
    GPtrArray *lengths;

    g_mutex_lock(&sgi_command_port_indexes_lock);
    if (!sgi_command_port_indexes) {
        sgi_command_port_indexes = g_hash_table_new(NULL, NULL);
    }
    lengths = g_hash_table_lookup(sgi_command_port_indexes, selectors);
    if (!lengths) {
        lengths = g_ptr_array_new();
        g_hash_table_insert(sgi_command_port_indexes, (gpointer)selectors,
                            lengths);
    }
    for (unsigned i = 0; i < lengths->len && !index; i++) {
        SGICommandPortIndex *candidate = g_ptr_array_index(lengths, i);

        if (candidate->num_selectors == num_selectors) {
            index = candidate;
        }
    }
    if (!index) {
        index = sgi_command_port_index_new(selectors, num_selectors);
        g_ptr_array_add(lengths, index);
    }
    g_mutex_unlock(&sgi_command_port_indexes_lock);
    return index;
}

const SGICommandPortSelector *
sgi_command_port_selector(const SGICommandPort *p, uint32_t selector)
{
    const SGICommandPortIndex *index = p->index;

    if (!index->indexed) {
        for (unsigned i = 0; i < p->num_selectors; i++) {
            if (p->selectors[i].selector == selector) {
                return &p->selectors[i];
            }
        }
        return NULL;
    }
    if (selector >= index->limit || !index->slot[selector]) {
        return NULL;
    }
    return &p->selectors[index->slot[selector] - 1];
}

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
    p->index = sgi_command_port_index(selectors, num_selectors);
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

static void push_word(SGICommandPort *p, uint32_t word)
{
    trace_sgi_command_port_word(p->port_id, word);

    if (!p->framing) {
        const SGICommandPortSelector *found =
            sgi_command_port_selector(p, word);

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
