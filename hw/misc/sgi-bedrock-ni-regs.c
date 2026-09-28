/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock NI register state; no link protocol is implemented. */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-ni-regs.h"

/* Port errors' defined fields end at bit 52; sideband's at bit 19. */
#define NI_PORT_ERRORS_MASK   ((1ULL << 53) - 1)
#define NI_PORT_SIDEBAND_MASK ((1ULL << 20) - 1)

static uint64_t control_mask(uint64_t offset)
{
    switch (offset) {
    case 0x10:
        return 1;
    case 0x18:
        return 7;
    case 0x20:
        return 0x1ff;
    case 0x30:
        return 0x7efffffffULL;
    case 0x38:
        return 0xffff;
    default:
        return 0;
    }
}

void sgi_bedrock_ni_regs_reset(SGIBedrockNIRegs *s)
{
    uint64_t sideband = s->sideband;

    /* Hardware defaults are unknown; zero is deterministic emulator state. */
    memset(s, 0, sizeof(*s));
    s->sideband = sideband;
}

/* Bits 52:24 raise the interrupt; the three byte counters below do not. */
bool sgi_bedrock_ni_regs_error_level(const SGIBedrockNIRegs *s)
{
    return (s->errors & 0x1fffffff000000ULL) != 0;
}

bool sgi_bedrock_ni_regs_read(SGIBedrockNIRegs *s, uint64_t offset,
                             uint64_t *value)
{
    if (offset & 7) {
        return false;
    }
    if (control_mask(offset)) {
        *value = s->control[offset / 8];
        return true;
    }
    if (offset >= 0x1000 && offset < 0x1400) {
        *value = s->local_route[(offset - 0x1000) / 8];
        return true;
    }
    switch (offset) {
    case 0:
        /* No remote power, and the reserved M-mode bit is clear. Link-up
         * is the owning node's to add from its cable. */
        *value = 0;
        break;
    case 0x100:
        *value = s->errors;
        break;
    case 0x108:
    case 0x110:
        *value = s->header[(offset - 0x108) / 8];
        break;
    case 0x118:
        *value = s->sideband;
        break;
    case 0x120:
        *value = s->errors;
        s->errors = 0;
        break;
    case 0x2000:
        *value = s->global_route;
        break;
    default:
        /* Channel-test field positions are not in the cleared contract. */
        return false;
    }
    return true;
}

SGIBedrockNIWriteResult sgi_bedrock_ni_regs_write(SGIBedrockNIRegs *s,
                                                uint64_t offset,
                                                uint64_t value)
{
    uint64_t mask;

    if (offset & 7) {
        return SGI_BEDROCK_NI_UNIMPLEMENTED;
    }
    mask = control_mask(offset);
    if (mask) {
        s->control[offset / 8] = value & mask;
        return SGI_BEDROCK_NI_WRITTEN;
    }
    if (offset >= 0x1000 && offset < 0x1400) {
        s->local_route[(offset - 0x1000) / 8] = value & 0x1f;
        return SGI_BEDROCK_NI_WRITTEN;
    }
    switch (offset) {
    case 0x08:
        /* Reset enable gates incoming link resets, not this local request. */
        return value & 4 ? SGI_BEDROCK_NI_LOCAL_RESET : SGI_BEDROCK_NI_WRITTEN;
    case 0x2000:
        s->global_route = value & 0x1f;
        return SGI_BEDROCK_NI_WRITTEN;
    case 0x100:
        /*
         * The stock PROM zeroes port errors and sideband at power-on. What a
         * write does on hardware is unknown; storing it
         * is the model's choice, so those zero writes clear.
        */
        s->errors = value & NI_PORT_ERRORS_MASK;
        return SGI_BEDROCK_NI_WRITTEN;
    case 0x118:
        s->sideband = value & NI_PORT_SIDEBAND_MASK;
        return SGI_BEDROCK_NI_WRITTEN;
    case 0:
    case 0x108:
    case 0x110:
    case 0x120:
        return SGI_BEDROCK_NI_READ_ONLY;
    default:
        return SGI_BEDROCK_NI_UNIMPLEMENTED;
    }
}
