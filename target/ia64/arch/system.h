/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Imported from syunnPC/qemu-system-ia64; see target/ia64/README.rst.
 */
/* Internal IA-64 system-register architecture API. */

#ifndef TARGET_IA64_ARCH_SYSTEM_H
#define TARGET_IA64_ARCH_SYSTEM_H

#include "cpu.h"

uint64_t ia64_system_read_pr(const CPUIA64State *env);
void ia64_system_epc(CPUIA64State *env, uint64_t fault_ip, uint64_t raw,
                     uint32_t fault_slot);
void ia64_system_write_pr(CPUIA64State *env, uint64_t value, uint64_t mask);
uint64_t ia64_system_read_ar(CPUIA64State *env, uint32_t ar_num);
void ia64_system_validate_ar_access(CPUIA64State *env, uint64_t value,
                                    uint32_t ar_num, uint32_t write,
                                    uint64_t fault_ip, uint64_t raw,
                                    uint32_t slot);
void ia64_system_write_ar(CPUIA64State *env, uint32_t ar_num, uint64_t value);
uint64_t ia64_system_read_cr(CPUIA64State *env, uint32_t cr_num);
uint64_t ia64_system_validate_cr_access(CPUIA64State *env, uint64_t value,
                                        uint32_t cr_num, uint32_t write,
                                        uint64_t fault_ip, uint64_t raw,
                                        uint32_t slot);
uint64_t ia64_system_read_cpuid(CPUIA64State *env, uint64_t index);
uint64_t ia64_system_read_dahr_indexed(CPUIA64State *env, uint64_t index);
void ia64_system_write_dahr(CPUIA64State *env, uint32_t index, uint64_t value);
void ia64_system_reset_dahr(CPUIA64State *env);
/* Inferred: MSR 1225 == 4 turns br.ia into a processor service call. */
#define IA64_MSR_BRIA_GATE        1225
#define IA64_MSR_BRIA_GATE_ON     4
/* Written by PAL, read by no image: writes are accepted and traced. */
/*
 * Estimated: after a class 4 command (0x9021...) in MSR 1546, MSR 1212 reads
 * bit 14 set until written, as PAL_B polls it. No source describes it.
 */
#define IA64_MSR_CMD_PORT        1546
#define IA64_MSR_CMD_STATUS      1212
#define IA64_MSR_CMD_CLASS4_TOP  0x9021ULL
#define IA64_MSR_CMD_DONE_BIT    (1ULL << 14)
/*
 * Healthy-processor assumption: the PAL's known-answer test (PAL_B 7.79, 5.73,
 * 2.25) expects MSR 1212 bits 13:0 = 0x317c and MSR 1226 = this constant.
 */
#define IA64_MSR_CMD_ANSWER      1226
#define IA64_MSR_CMD_ANSWER_LOW  0x317cULL
#define IA64_MSR_CMD_ANSWER_1226 0x82518167826f836cULL
/*
 * Estimated: MSR 1559 bits 26:22 are the processor-to-base clock ratio; the PAL
 * spins below 8 and takes the same path for 8 to 23.
 */
#define IA64_MSR_CLOCK_RATIO     1559
#define IA64_MSR_CLOCK_RATIO_SHIFT 22
#define IA64_MSR_CLOCK_RATIO_MASK  (0x1fULL << IA64_MSR_CLOCK_RATIO_SHIFT)
/*
 * Estimated: the PAL polls MSR 1552 bit 2 after a write (PAL_B 5.73 at
 * 0xffe89ff0); it reads set once written. The bit's meaning is unknown.
 */
#define IA64_MSR_L3_WAYS         1536
#define IA64_MSR_L3_ENABLE       1504
#define IA64_MSR_CMD1552         1552
#define IA64_MSR_CMD1552_DONE    (1ULL << 2)
#define IA64_MSR_CMD1552_START   (1ULL << 4)
/* Write value as the MSR's stored contents, as a plain MSR write would. */
void ia64_system_store_msr(CPUIA64State *env, uint64_t index, uint64_t value);
/* Cache-array port, see arch/cache-array.c. */
#define IA64_MSR_CA_ADDR         1544
#define IA64_MSR_ECC_LOG         1505
void ia64_cache_array_trigger(CPUIA64State *env);
void ia64_cache_array_restart(CPUIA64State *env);
uint64_t ia64_cache_array_load(CPUIA64State *env, uint64_t pa, uint64_t value,
                               unsigned size);
void ia64_cache_array_store(CPUIA64State *env, uint64_t pa);
void ia64_cache_array_reset(IA64CPU *cpu);
void ia64_cpu_drop_l3_ways(IA64CPU *cpu);
void ia64_cache_array_msr1504(CPUIA64State *env, uint64_t old,
                              uint64_t value);
/* Estimated training registers, see ia64_msr1730_latch() in system.c. */
#define IA64_MSR_TRAIN_CTL       1728
#define IA64_MSR_TRAIN_STATUS    1730
#define IA64_MSR_TRAIN_STROBE_A  (1ULL << 48)
#define IA64_MSR_TRAIN_STROBE_B  (1ULL << 49)
#define IA64_MSR_WRITE_ONLY_FIRST 1632
#define IA64_MSR_WRITE_ONLY_LAST  1635
/* The value a read would return, without the warning, trace or poll count. */
uint64_t ia64_system_peek_msr(CPUIA64State *env, uint64_t index);
uint64_t ia64_system_read_msr(CPUIA64State *env, uint64_t index);
void ia64_system_write_msr(CPUIA64State *env, uint64_t index, uint64_t value);
uint64_t ia64_system_read_dbr(CPUIA64State *env, uint32_t index);
void ia64_system_write_dbr(CPUIA64State *env, uint32_t index, uint64_t value);
uint64_t ia64_system_read_ibr(CPUIA64State *env, uint32_t index);
void ia64_system_write_ibr(CPUIA64State *env, uint32_t index, uint64_t value);
uint64_t ia64_system_read_pmc(CPUIA64State *env, uint32_t index);
void ia64_system_write_pmc(CPUIA64State *env, uint32_t index, uint64_t value);
uint64_t ia64_system_read_pmc_indexed(CPUIA64State *env, uint64_t index);
void ia64_system_write_pmc_indexed(CPUIA64State *env, uint64_t index,
                                   uint64_t value);
uint64_t ia64_system_read_pmd(CPUIA64State *env, uint32_t index);
uint64_t ia64_system_read_pmd_checked(CPUIA64State *env, uint64_t index,
                                      uint64_t fault_ip, uint64_t raw,
                                      uint32_t slot);
void ia64_system_write_pmd(CPUIA64State *env, uint32_t index, uint64_t value);
uint64_t ia64_system_read_pmd_indexed(CPUIA64State *env, uint64_t index);
void ia64_system_write_pmd_indexed(CPUIA64State *env, uint64_t index,
                                   uint64_t value);
void ia64_system_st_spill_unat(CPUIA64State *env, uint32_t reg, uint64_t addr);
void ia64_system_clear_psr_fault_suppression(CPUIA64State *env);
void ia64_system_set_psr_bn(CPUIA64State *env, uint32_t bank1);
void ia64_system_ssm(CPUIA64State *env, uint64_t imm);
void ia64_system_rsm(CPUIA64State *env, uint64_t imm);
void ia64_system_mov_psr_write(CPUIA64State *env, uint64_t value,
                               uint32_t unused);
uint64_t ia64_system_mov_rrgr_read(CPUIA64State *env, uint64_t rr_addr);
uint64_t ia64_system_validate_rr_value(CPUIA64State *env, uint64_t value,
                                       uint64_t fault_ip, uint64_t raw,
                                       uint32_t slot);
void ia64_system_mov_grrr_write(CPUIA64State *env, uint64_t rr_addr,
                                uint64_t value);
uint64_t ia64_system_mov_pkrgr_read(CPUIA64State *env, uint32_t pkr_num);
uint64_t ia64_system_mov_pkrgr_indexed_read(CPUIA64State *env,
                                            uint64_t pkr_num);
void ia64_system_mov_grpkr_write(CPUIA64State *env, uint32_t pkr_num,
                                 uint64_t value);
void ia64_system_mov_grpkr_indexed_write(CPUIA64State *env, uint64_t pkr_num,
                                         uint64_t value);

#endif /* TARGET_IA64_ARCH_SYSTEM_H */
