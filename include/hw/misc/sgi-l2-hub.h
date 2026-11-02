/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The wire between the L2 kernel stand-in plugin and the L1 hub.
 *
 * A run that executes the L2's own programs under qemu-ppc has no kernel
 * to give them the L1 USB driver's device nodes. The plugin
 * (contrib/plugins/sgi-l2-shim.c) answers the programs' system calls and
 * hands each opened node to the hub (tools/sgi/qemu-sgi-l2-hub.c), which
 * stands for the L2's USB host controller and owns the links to the L1
 * models. This is the project's own format, not a hardware wire.
 *
 * The hub listens on a sequenced-packet Unix socket, so every message keeps
 * its boundary. A client opens a connection and sends one request. The hub
 * answers with one reply. For the control node and the slot nodes the
 * connection then becomes the node itself: the program reads and writes the
 * socket directly, and a read or write is one message.
 *
 * The control node's messages are the slot map (SGI_L2_HUB_SLOTS bytes,
 * one 0 or 1 per slot), sent once on open and again after every attach or
 * detach. A slot node's messages in both directions are one router segment
 * (docs/system/mips/sgi/hardware/l1.rst, "L2 link"); the hub stamps the
 * segment's own length into its first two bytes when the program sent
 * another value, as the kernel driver does.
 */
#ifndef HW_MISC_SGI_L2_HUB_H
#define HW_MISC_SGI_L2_HUB_H

#define SGI_L2_HUB_SLOTS        40
#define SGI_L2_HUB_RECORD_LEN   9
#define SGI_L2_HUB_SEGMENT_MAX  4096

/* Names in the hub's directory. */
/* A stream socket: the L1 models' l2 chardevs. */
#define SGI_L2_HUB_L1_SOCKET    "l1.sock"
/* Sequenced packets: the plugin. */
#define SGI_L2_HUB_DEV_SOCKET   "dev.sock"

/* Request: four bytes, op, slot, argument, zero. */
#define SGI_L2_HUB_REQUEST_LEN  4
#define SGI_L2_HUB_OP_CONTROL   'C'   /* open the control node */
#define SGI_L2_HUB_OP_OPEN      'O'   /* open slot N; the socket is the node */
#define SGI_L2_HUB_OP_QUERY     'Q'   /* the device record of slot N */
/* Reset slot N; the argument is the ioctl number's low byte. */
#define SGI_L2_HUB_OP_RESET     'R'

/* Reply: one errno value (0 for success) and the slot's device record. */
#define SGI_L2_HUB_REPLY_LEN    (1 + SGI_L2_HUB_RECORD_LEN)

#endif /* HW_MISC_SGI_L2_HUB_H */
