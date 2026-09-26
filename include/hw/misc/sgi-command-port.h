/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * A reusable host command-port model.
 *
 * Several SGI graphics boards take commands from their host CPU through
 * the same shape of interface: the host pushes one 32-bit word at a time
 * into a port register, the first word of a run is a selector and the
 * following words are its fixed-count arguments, a separate register
 * reports queue credit or occupancy, and a separate kick register tells
 * the board how many words it just received. This object is that shape,
 * with no rendering, no graphics-engine and nothing board-specific in it.
 * InfiniteReality's kernel, textport and userspace command ports are its
 * first owners, each as a distinct instance of this object.
 *
 * This is a plain, non-QOM value: no MemoryRegion, no reset phase and no
 * identity of its own. The owning device places its registers, wires
 * reset and migration, and turns delivered packets into behaviour.
 * sgi-bedrock-ni-regs.{c,h} is the precedent for this split.
 *
 * The owner supplies a selector-to-argument-count table at
 * sgi_command_port_init() time. A word offered while the port is
 * between packets and not found in that table is never held pending a
 * guess at its length: it is reported once through
 * SGICommandPortOps.unsupported and the next word is read fresh, so an
 * unrecognised selector cannot desynchronise later, correctly-framed
 * traffic.
 */
#ifndef HW_MISC_SGI_COMMAND_PORT_H
#define HW_MISC_SGI_COMMAND_PORT_H

/* Largest argument count any owner's selector table may declare. */
#define SGI_COMMAND_PORT_MAX_ARGS 32

typedef struct SGICommandPortSelector {
    uint32_t selector;
    uint32_t argc;
} SGICommandPortSelector;

typedef struct SGICommandPortOps {
    /*
     * A complete packet: selector plus its argc argument words. Called
     * synchronously from sgi_command_port_write(); must not call back
     * into that port.
     */
    void (*packet)(void *opaque, unsigned port_id, uint32_t selector,
                    const uint32_t *args, unsigned argc);
    /*
     * A word seen while the port was between packets that does not
     * match any entry in the owner's selector table. A real device's
     * callback must log the unrecognized selector with
     * qemu_log_mask(LOG_GUEST_ERROR, ...) in addition to any trace
     * event, so it stays visible without enabling tracing.
     */
    void (*unsupported)(void *opaque, unsigned port_id, uint32_t word);
    /* The kick/word-count register was written; value is what was written. */
    void (*kick)(void *opaque, unsigned port_id, uint32_t value);
    /*
     * The credit/occupancy register was read. The return value is what
     * is reported to the guest; a NULL pointer here reports zero. This
     * is the port's one hook for an owner's approximation policy, such
     * as InfiniteReality's fixed zero that satisfies both the driver's
     * and the firmware's opposite polarities for "proceed."
     */
    uint32_t (*credit)(void *opaque, unsigned port_id);
} SGICommandPortOps;

typedef struct SGICommandPort {
    /*
     * Set once by the owner at sgi_command_port_init() and unchanged
     * for the object's lifetime. Not migrated: identical-build
     * migration re-runs the owner's realize path, which calls
     * sgi_command_port_init() again with the same arguments before any
     * migrated state is loaded.
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
 * The push port: a 32 or 64-bit store at the port's own address. A
 * 64-bit store pushes two words, high half first, in keeping with this
 * project's big-endian mips64 targets; no cited source documents a
 * board actually doing this, so treat it as this object's own width
 * support rather than an observed register width.
 */
void sgi_command_port_write(SGICommandPort *p, uint64_t value, unsigned size);

uint32_t sgi_command_port_credit_read(SGICommandPort *p);
void sgi_command_port_kick_write(SGICommandPort *p, uint32_t value);
uint32_t sgi_command_port_kick_read(const SGICommandPort *p);

#endif /* HW_MISC_SGI_COMMAND_PORT_H */
