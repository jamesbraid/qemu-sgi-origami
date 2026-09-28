/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared SGI graphics command-port framing, without device behavior. */
#ifndef HW_MISC_SGI_COMMAND_PORT_H
#define HW_MISC_SGI_COMMAND_PORT_H

/*
 * The host pushes 32-bit words: selector, then a fixed number of arguments.
 * Separate registers report credit/occupancy and kick the received word
 * count. InfiniteReality uses distinct kernel, textport and userspace
 * instances.
 */
/*
 * This plain non-QOM value has no rendering, graphics engine, MemoryRegion,
 * reset phase or identity. Owners place registers, wire reset/migration and
 * handle packets, as in sgi-bedrock-ni-regs.{c,h}. At init they supply
 * selector argument counts. An unknown selector between packets calls
 * unsupported once and is discarded; the next word starts fresh, preserving
 * later packet alignment.
 */

/* Largest argument count any owner's selector table may declare. */
#define SGI_COMMAND_PORT_MAX_ARGS 32

typedef struct SGICommandPortSelector {
    uint32_t selector;
    uint32_t argc;
} SGICommandPortSelector;

typedef struct SGICommandPortOps {
    /*
     * A complete packet: selector plus its argc argument words. Called
     * synchronously from sgi_command_port_write(); must not call back into
     * that port.
     */
    void (*packet)(void *opaque, unsigned port_id, uint32_t selector,
                    const uint32_t *args, unsigned argc);
    /*
     * A word seen while the port was between packets that does not match any
     * entry in the owner's selector table. A real device's callback must log
     * the unrecognized selector with qemu_log_mask(LOG_GUEST_ERROR, ...) in
     * addition to any trace event, so it stays visible without enabling
     * tracing.
     */
    void (*unsupported)(void *opaque, unsigned port_id, uint32_t word);
    /* The kick/word-count register was written; value is what was written. */
    void (*kick)(void *opaque, unsigned port_id, uint32_t value);
    /*
     * The credit/occupancy register was read. The return value is what is
     * reported to the guest; a NULL pointer here reports zero. This is the
     * port's one hook for an owner's approximation policy, such as
     * InfiniteReality's fixed zero that satisfies both the driver's and the
     * firmware's opposite polarities for "proceed."
     */
    uint32_t (*credit)(void *opaque, unsigned port_id);
} SGICommandPortOps;

typedef struct SGICommandPort {
    /*
     * Set once by the owner at sgi_command_port_init() and unchanged for the
     * object's lifetime. Not migrated: identical-build migration re-runs the
     * owner's realize path, which calls sgi_command_port_init() again with
     * the same arguments before any migrated state is loaded.
     */
    const SGICommandPortOps *ops;
    void *opaque;
    unsigned port_id;
    const SGICommandPortSelector *selectors;
    unsigned num_selectors;

    /* Migrated state below; the owner's VMStateDescription lists these. */
    uint32_t kick;
    bool framing;
    uint32_t selector;
    uint32_t argc_expected;
    uint32_t argc_have;
    uint32_t args[SGI_COMMAND_PORT_MAX_ARGS];
} SGICommandPort;

void sgi_command_port_init(SGICommandPort *p, const SGICommandPortOps *ops,
                            void *opaque, unsigned port_id,
                            const SGICommandPortSelector *selectors,
                            unsigned num_selectors);

void sgi_command_port_reset(SGICommandPort *p);

/*
 * The push port: a 32 or 64-bit store at the port's own address. A 64-bit
 * store pushes two words, high half first, in keeping with this project's
 * big-endian mips64 targets; no cited source documents a board actually doing
 * this, so treat it as this object's own width support rather than an
 * observed register width.
 */
void sgi_command_port_write(SGICommandPort *p, uint64_t value, unsigned size);

uint32_t sgi_command_port_credit_read(SGICommandPort *p);
void sgi_command_port_kick_write(SGICommandPort *p, uint32_t value);
uint32_t sgi_command_port_kick_read(const SGICommandPort *p);

#endif /* HW_MISC_SGI_COMMAND_PORT_H */
