/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock network-interface register state. */
#ifndef HW_MISC_SGI_BEDROCK_NI_REGS_H
#define HW_MISC_SGI_BEDROCK_NI_REGS_H

#define SGI_BEDROCK_NI_SIZE 0x2008

typedef struct SGIBedrockNIRegs {
    uint64_t control[8];
    uint64_t errors;
    uint64_t header[2];
    uint64_t sideband;
    uint8_t local_route[128];
    uint8_t global_route;
} SGIBedrockNIRegs;

typedef enum SGIBedrockNIWriteResult {
    SGI_BEDROCK_NI_UNIMPLEMENTED,
    SGI_BEDROCK_NI_WRITTEN,
    SGI_BEDROCK_NI_READ_ONLY,
    SGI_BEDROCK_NI_LOCAL_RESET,
} SGIBedrockNIWriteResult;

void sgi_bedrock_ni_regs_reset(SGIBedrockNIRegs *s);
bool sgi_bedrock_ni_regs_read(SGIBedrockNIRegs *s, uint64_t offset,
                             uint64_t *value);
SGIBedrockNIWriteResult sgi_bedrock_ni_regs_write(SGIBedrockNIRegs *s,
                                                uint64_t offset,
                                                uint64_t value);
bool sgi_bedrock_ni_regs_error_level(const SGIBedrockNIRegs *s);
#endif
