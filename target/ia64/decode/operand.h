/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 *
 * Typed operands produced by the IA-64 instruction decoder.
 */

#ifndef TARGET_IA64_DECODE_OPERAND_H
#define TARGET_IA64_DECODE_OPERAND_H

#include <stddef.h>

#include "target/ia64/decoder.h"
#include "target/ia64/decode/opcode.h"

typedef enum Ia64PredicateUpdate {
    IA64_PRED_UPDATE_NORMAL,
    IA64_PRED_UPDATE_AND,
    IA64_PRED_UPDATE_OR,
    IA64_PRED_UPDATE_OR_ANDCM,
} Ia64PredicateUpdate;

/*
 * The opcode tables distinguish architecturally reserved encodings from
 * encodings which fault only when their qualifying predicate is true.
 * Keep that distinction in the decoded instruction instead of collapsing
 * both cases into an unqualified IA64_OP_ILLEGAL.
 */
typedef enum Ia64EncodingClass {
    IA64_ENCODING_DEFINED,
    IA64_ENCODING_IGNORED,
    IA64_ENCODING_RESERVED,
    IA64_ENCODING_RESERVED_IF_QP,
    IA64_ENCODING_RESERVED_IF_QP_B,
    IA64_ENCODING_CONSTANT_ZERO_VIOLATION,
} Ia64EncodingClass;

/*
 * All views below describe the decoder's canonical operand slots.  Keeping
 * the raw decoder spelling in its own view makes it impossible for an
 * instruction generator to accidentally depend on encoding-oriented r1/r2
 * names.  Family generators consume only their corresponding typed view.
 */
typedef struct IA64DecodedOperands {
    uint8_t r1;
    uint8_t r2;
    uint8_t r3;
    uint8_t p1;
    uint8_t p2;
    uint8_t b1;
    uint8_t b2;
    uint8_t sf;
    uint8_t fp_precision;
    int64_t imm;
} IA64DecodedOperands;

typedef struct IA64CommonOperands {
    uint8_t destination;
    uint8_t source1;
    uint8_t source2;
    uint8_t auxiliary1;
    uint8_t auxiliary2;
    uint8_t branch1;
    uint8_t branch2;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64CommonOperands;

typedef struct IA64IntegerOperands {
    uint8_t destination;
    uint8_t source1;
    uint8_t source2;
    uint8_t predicate1;
    uint8_t predicate2;
    uint8_t branch1;
    uint8_t branch2;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64IntegerOperands;

typedef struct IA64MemoryOperands {
    uint8_t destination;
    uint8_t source;
    uint8_t base;
    uint8_t auxiliary1;
    uint8_t auxiliary2;
    uint8_t branch1;
    uint8_t branch2;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64MemoryOperands;

typedef struct IA64BranchOperands {
    uint8_t general_destination;
    uint8_t general_source1;
    uint8_t general_source2;
    uint8_t predicate1;
    uint8_t predicate2;
    uint8_t link;
    uint8_t target;
    uint8_t status_field;
    uint8_t precision;
    int64_t displacement;
} IA64BranchOperands;

typedef struct IA64FloatingOperands {
    uint8_t destination;
    uint8_t source1;
    uint8_t source2;
    uint8_t auxiliary1;
    uint8_t auxiliary2;
    uint8_t branch1;
    uint8_t branch2;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64FloatingOperands;

typedef struct IA64SimdOperands {
    uint8_t destination;
    uint8_t source1;
    uint8_t source2;
    uint8_t predicate1;
    uint8_t predicate2;
    uint8_t branch1;
    uint8_t branch2;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64SimdOperands;

typedef struct IA64SystemOperands {
    uint8_t destination;
    uint8_t source;
    uint8_t register_index;
    uint8_t auxiliary1;
    uint8_t auxiliary2;
    uint8_t branch_destination;
    uint8_t branch_source;
    uint8_t status_field;
    uint8_t precision;
    int64_t immediate;
} IA64SystemOperands;

typedef union IA64Operands {
    /* Only decode/decode.c may write this encoding-oriented view. */
    IA64DecodedOperands decoder;
    /* Common translation policy may inspect canonical operand roles. */
    IA64CommonOperands common;
    IA64IntegerOperands integer;
    IA64MemoryOperands memory;
    IA64BranchOperands branch;
    IA64FloatingOperands floating;
    IA64SimdOperands simd;
    IA64SystemOperands system;
} IA64Operands;

#define IA64_OPERAND_LAYOUT_ASSERT(type, member)                         \
    _Static_assert(offsetof(type, member) == 16, #type " layout");      \
    _Static_assert(sizeof(type) == sizeof(IA64DecodedOperands),          \
                   #type " size")

IA64_OPERAND_LAYOUT_ASSERT(IA64CommonOperands, immediate);
IA64_OPERAND_LAYOUT_ASSERT(IA64IntegerOperands, immediate);
IA64_OPERAND_LAYOUT_ASSERT(IA64MemoryOperands, immediate);
IA64_OPERAND_LAYOUT_ASSERT(IA64BranchOperands, displacement);
IA64_OPERAND_LAYOUT_ASSERT(IA64FloatingOperands, immediate);
IA64_OPERAND_LAYOUT_ASSERT(IA64SimdOperands, immediate);
IA64_OPERAND_LAYOUT_ASSERT(IA64SystemOperands, immediate);

#undef IA64_OPERAND_LAYOUT_ASSERT

_Static_assert(offsetof(IA64DecodedOperands, imm) == 16,
               "decoded operand layout");
_Static_assert(offsetof(IA64FloatingOperands, precision) == 8,
               "floating precision operand offset");

typedef struct Ia64Instruction {
    /* Translator-owned context; the decoder treats this as opaque. */
    void *ctx;
    Ia64Opcode opcode;
    IA64SlotUnit unit;
    uint64_t raw;
    uint64_t address;
    uint8_t slot;
    uint8_t qp;
    IA64Operands operands;
    Ia64EncodingClass encoding_class;
    Ia64PredicateUpdate pred_update;
    bool hint_m_reg_increment;
    bool reg_base_update;
    bool imm_base_update;
    bool mem_acquire;
    bool mem_release;
    bool compare_unc;
    bool clear_p2_before_predicate;
    bool check_fp;
    bool fp_load_speculative;
    bool fp_load_advanced;
    bool fp_load_check;
    bool fp_load_check_clear;
    bool probe_fault;
    bool probe_imm;
    bool placement_illegal;
    bool reserved_field;
    bool valid;
} Ia64Instruction;

#endif /* TARGET_IA64_DECODE_OPERAND_H */
