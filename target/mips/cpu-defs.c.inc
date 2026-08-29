/*
 *  MIPS emulation for qemu: CPU initialisation routines.
 *
 *  Copyright (c) 2004-2005 Jocelyn Mayer
 *  Copyright (c) 2007 Herve Poussineau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

/* CPU / CPU family specific config register values. */

/* Have config1, uncached coherency */
#define MIPS_CONFIG0                                              \
  ((1U << CP0C0_M) | (0x2 << CP0C0_K0))

/* Have config2, no coprocessor2 attached, no MDMX support attached,
   no performance counters, watch registers present,
   no code compression, EJTAG present, no FPU */
#define MIPS_CONFIG1                                              \
((1U << CP0C1_M) |                                                \
 (0 << CP0C1_C2) | (0 << CP0C1_MD) | (0 << CP0C1_PC) |            \
 (1 << CP0C1_WR) | (0 << CP0C1_CA) | (1 << CP0C1_EP) |            \
 (0 << CP0C1_FP))

/* Have config3, no tertiary/secondary caches implemented */
#define MIPS_CONFIG2                                              \
((1U << CP0C2_M))

/* No config4, no DSP ASE, no large physaddr (PABITS),
   no external interrupt controller, no vectored interrupts,
   no 1kb pages, no SmartMIPS ASE, no trace logic */
#define MIPS_CONFIG3                                              \
((0 << CP0C3_M) | (0 << CP0C3_DSPP) | (0 << CP0C3_LPA) |          \
 (0 << CP0C3_VEIC) | (0 << CP0C3_VInt) | (0 << CP0C3_SP) |        \
 (0 << CP0C3_SM) | (0 << CP0C3_TL))

#define MIPS_CONFIG4                                              \
((0 << CP0C4_M))

#define MIPS_CONFIG5                                              \
((0 << CP0C5_M))

/*****************************************************************************/
/* MIPS CPU definitions */
const mips_def_t mips_defs[] =
{
    {
        .name = "4Kc",
        .CP0_PRid = 0x00018000,
        .CP0_Config0 = MIPS_CONFIG0 | (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (0 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1278FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R1,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "4Km",
        .CP0_PRid = 0x00018300,
        /* Config1 implemented, fixed mapping MMU,
           no virtual icache, uncached coherency. */
        .CP0_Config0 = MIPS_CONFIG0 | (MMU_TYPE_FMT << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1258FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R1 | ASE_MIPS16,
        .mmu_type = MMU_TYPE_FMT,
    },
    {
        .name = "4KEcR1",
        .CP0_PRid = 0x00018400,
        .CP0_Config0 = MIPS_CONFIG0 | (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (0 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1278FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R1,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "XBurstR1",
        .CP0_PRid = 0x1ed0024f,
        .CP0_Config0 = MIPS_CONFIG0 | (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (0 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1278FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R1 | ASE_MXU,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "4KEmR1",
        .CP0_PRid = 0x00018500,
        .CP0_Config0 = MIPS_CONFIG0 | (MMU_TYPE_FMT << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1258FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R1 | ASE_MIPS16,
        .mmu_type = MMU_TYPE_FMT,
    },
    {
        .name = "4KEc",
        .CP0_PRid = 0x00019000,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                    (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (0 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (0 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1278FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "4KEm",
        .CP0_PRid = 0x00019100,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                       (MMU_TYPE_FMT << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1258FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16,
        .mmu_type = MMU_TYPE_FMT,
    },
    {
        .name = "24Kc",
        .CP0_PRid = 0x00019300,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (0 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        /* No DSP implemented. */
        .CP0_Status_rw_bitmask = 0x1278FF1F,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "24KEc",
        .CP0_PRid = 0x00019600,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_DSPP) | (0 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        /* we have a DSP, but no FPU */
        .CP0_Status_rw_bitmask = 0x1378FF1F,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16 | ASE_DSP,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "24Kf",
        .CP0_PRid = 0x00019300,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                    (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (0 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        /* No DSP implemented. */
        .CP0_Status_rw_bitmask = 0x3678FF1F,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) | (0x93 << FCR0_PRID),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "34Kf",
        .CP0_PRid = 0x00019500,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (63 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_VInt) | (1 << CP0C3_MT) |
                       (1 << CP0C3_DSPP),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3778FF1F,
        .CP0_TCStatus_rw_bitmask = (0 << CP0TCSt_TCU3) | (0 << CP0TCSt_TCU2) |
                    (1 << CP0TCSt_TCU1) | (1 << CP0TCSt_TCU0) |
                    (0 << CP0TCSt_TMX) | (1 << CP0TCSt_DT) |
                    (1 << CP0TCSt_DA) | (1 << CP0TCSt_A) |
                    (0x3 << CP0TCSt_TKSU) | (1 << CP0TCSt_IXMT) |
                    (0xff << CP0TCSt_TASID),
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) | (0x95 << FCR0_PRID),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .CP0_SRSCtl = (0xf << CP0SRSCtl_HSS),
        .CP0_SRSConf0_rw_bitmask = 0x3fffffff,
        .CP0_SRSConf0 = (1U << CP0SRSC0_M) | (0x3fe << CP0SRSC0_SRS3) |
                    (0x3fe << CP0SRSC0_SRS2) | (0x3fe << CP0SRSC0_SRS1),
        .CP0_SRSConf1_rw_bitmask = 0x3fffffff,
        .CP0_SRSConf1 = (1U << CP0SRSC1_M) | (0x3fe << CP0SRSC1_SRS6) |
                    (0x3fe << CP0SRSC1_SRS5) | (0x3fe << CP0SRSC1_SRS4),
        .CP0_SRSConf2_rw_bitmask = 0x3fffffff,
        .CP0_SRSConf2 = (1U << CP0SRSC2_M) | (0x3fe << CP0SRSC2_SRS9) |
                    (0x3fe << CP0SRSC2_SRS8) | (0x3fe << CP0SRSC2_SRS7),
        .CP0_SRSConf3_rw_bitmask = 0x3fffffff,
        .CP0_SRSConf3 = (1U << CP0SRSC3_M) | (0x3fe << CP0SRSC3_SRS12) |
                    (0x3fe << CP0SRSC3_SRS11) | (0x3fe << CP0SRSC3_SRS10),
        .CP0_SRSConf4_rw_bitmask = 0x3fffffff,
        .CP0_SRSConf4 = (0x3fe << CP0SRSC4_SRS15) |
                    (0x3fe << CP0SRSC4_SRS14) | (0x3fe << CP0SRSC4_SRS13),
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16 | ASE_DSP,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "74Kf",
        .CP0_PRid = 0x00019700,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                    (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_DSP2P) | (1 << CP0C3_DSPP) |
                       (1 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3778FF1F,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) | (0x93 << FCR0_PRID),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MIPS16 | ASE_DSP | ASE_DSP_R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "XBurstR2",
        .CP0_PRid = 0x2ed1024f,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                    (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_CA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_DSP2P) | (1 << CP0C3_DSPP) |
                       (1 << CP0C3_VInt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3778FF1F,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) | (0x93 << FCR0_PRID),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MXU,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "M14K",
        .CP0_PRid = 0x00019b00,
        /* Config1 implemented, fixed mapping MMU,
           no virtual icache, uncached coherency. */
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_KU) | (0x2 << CP0C0_K23) |
                       (0x1 << CP0C0_AR) | (MMU_TYPE_FMT << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1,
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (0x2 << CP0C3_ISA) | (1 << CP0C3_VInt) |
                       (1 << CP0C3_M),
        .CP0_Config4 = MIPS_CONFIG4 | (1 << CP0C4_M),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_NFExists),
        .CP0_Config7 = 1 << CP0C7_WII,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1258FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MICROMIPS,
        .mmu_type = MMU_TYPE_FMT,
    },
    {
        .name = "M14Kc",
        /* This is the TLB-based MMU core.  */
        .CP0_PRid = 0x00019c00,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (15 << CP0C1_MMU) |
                       (0 << CP0C1_IS) | (3 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (0 << CP0C1_DS) | (3 << CP0C1_DL) | (1 << CP0C1_DA),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (0x2 << CP0C3_ISA) | (0 << CP0C3_VInt) |
                       (1 << CP0C3_M),
        .CP0_Config4 = MIPS_CONFIG4 | (1 << CP0C4_M),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_NFExists),
        .CP0_Config7 = 1 << CP0C7_WII,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x1278FF17,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R2 | ASE_MICROMIPS,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /* FIXME:
         * Config3: VZ, CTXTC, CDMM, TL
         * Config4: MMUExtDef
         * Config5: MRP
         * */
        .name = "P5600",
        .CP0_PRid = 0x0001A800,
        .CP0_Config0 = MIPS_CONFIG0 | (1 << CP0C0_MM) | (1 << CP0C0_AR) |
                    (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (0x3F << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_FP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) |
                       (1 << CP0C3_CMGCR) | (1 << CP0C3_MSAP) |
                       (1 << CP0C3_BP) | (1 << CP0C3_BI) | (1 << CP0C3_SC) |
                       (1 << CP0C3_PW) | (1 << CP0C3_ULRI) | (1 << CP0C3_RXI) |
                       (1 << CP0C3_LPA) | (1 << CP0C3_VInt),
        .CP0_Config4 = MIPS_CONFIG4 | (1U << CP0C4_M) | (2 << CP0C4_IE) |
                       (0x1c << CP0C4_KScrExist),
        .CP0_Config4_rw_bitmask = 0,
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_EVA) | (1 << CP0C5_MVH) |
                       (1 << CP0C5_LLB) | (1 << CP0C5_MRP),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_K) | (1 << CP0C5_CV) |
                                  (1 << CP0C5_MSAEn) | (1 << CP0C5_UFE) |
                                  (1 << CP0C5_FRE) | (1 << CP0C5_UFR),
        .CP0_Config7 = 1 << CP0C7_WII,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3C68FF1F,
        .CP0_PageGrain_rw_bitmask = (1U << CP0PG_RIE) | (1 << CP0PG_XIE) |
                    (1 << CP0PG_ELPA) | (1 << CP0PG_IEC),
        .CP0_EBaseWG_rw_bitmask = (1 << CP0EBase_WG),
        .CP1_fcr0 = (1 << FCR0_FREP) | (1 << FCR0_UFRP) | (1 << FCR0_HAS2008) |
                    (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) | (0x03 << FCR0_PRID),
        .CP1_fcr31 = (1 << FCR31_ABS2008) | (1 << FCR31_NAN2008),
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 32,
        .PABITS = 40,
        .insn_flags = CPU_MIPS32R5,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /* A generic CPU supporting MIPS32 Release 6 ISA.
           FIXME: Support IEEE 754-2008 FP.
                  Eventually this should be replaced by a real CPU model. */
        .name = "mips32r6-generic",
        .CP0_PRid = 0x00010000,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AR) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (31 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (0 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_MSAP) |
                       (1 << CP0C3_BP) | (1 << CP0C3_BI) |
                       (2 << CP0C3_ISA) | (1 << CP0C3_ULRI) |
                       (1 << CP0C3_RXI) | (1U << CP0C3_M),
        .CP0_Config4 = MIPS_CONFIG4 | (0xfc << CP0C4_KScrExist) |
                       (3 << CP0C4_IE) | (1U << CP0C4_M),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_XNP) | (1 << CP0C5_LLB),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_MSAEn) | (1 << CP0C5_UFE) |
                                  (1 << CP0C5_FRE) | (1 << CP0C5_SBRI),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3058FF1F,
        .CP0_PageGrain = (1 << CP0PG_IEC) | (1 << CP0PG_XIE) |
                         (1U << CP0PG_RIE),
        .CP0_PageGrain_rw_bitmask = 0,
        .CP1_fcr0 = (1 << FCR0_FREP) | (1 << FCR0_HAS2008) | (1 << FCR0_F64) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x00 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = (1 << FCR31_ABS2008) | (1 << FCR31_NAN2008),
        .CP1_fcr31_rw_bitmask = 0x0103FFFF,
        .MSAIR = 0x03 << MSAIR_ProcID,
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R6 | ASE_MICROMIPS,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "I7200",
        .CP0_PRid = 0x00010000,
        .CP0_Config0 = MIPS_CONFIG0 | (1 << CP0C0_MM) | (0x2 << CP0C0_AR) |
                        (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = (1U << CP0C1_M) | (15 << CP0C1_MMU) | (2 << CP0C1_IS) |
                       (4 << CP0C1_IL) | (3 << CP0C1_IA) | (2 << CP0C1_DS) |
                       (4 << CP0C1_DL) | (3 << CP0C1_DA) | (1 << CP0C1_PC) |
                       (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) | (1 << CP0C3_CMGCR) |
                       (1 << CP0C3_BI) | (1 << CP0C3_SC) | (3 << CP0C3_MMAR) |
                       (1 << CP0C3_ISA_ON_EXC) | (1 << CP0C3_ISA) |
                       (1 << CP0C3_ULRI) | (1 << CP0C3_RXI) |
                       (1 << CP0C3_DSP2P) | (1 << CP0C3_DSPP) |
                       (1 << CP0C3_CTXTC) | (1 << CP0C3_VInt) |
                       (1 << CP0C3_CDMM) | (1 << CP0C3_MT) | (1 << CP0C3_TL),
        .CP0_Config4 = MIPS_CONFIG4 | (0xfc << CP0C4_KScrExist) |
                       (2 << CP0C4_IE) | (1U << CP0C4_M),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_MVH) | (1 << CP0C5_LLB),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_SBRI) | (1 << CP0C5_FRE) |
                                  (1 << CP0C5_UFE),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3158FF1F,
        .CP0_PageGrain = (1 << CP0PG_IEC) | (1 << CP0PG_XIE) |
                         (1U << CP0PG_RIE),
        .CP0_PageGrain_rw_bitmask = 0,
        .CP1_fcr0 = (1 << FCR0_FREP) | (1 << FCR0_HAS2008) | (1 << FCR0_F64) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x02 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = (1 << FCR31_ABS2008) | (1 << FCR31_NAN2008),
        .SEGBITS = 32,
        .PABITS = 32,
        .insn_flags = CPU_MIPS32R6 | ISA_NANOMIPS32 |
                      ASE_DSP | ASE_DSP_R2 | ASE_DSP_R3,
        .mmu_type = MMU_TYPE_R4000,
    },
#if defined(TARGET_MIPS64)
    {
        .name = "R4000",
        .CP0_PRid = 0x00000400,
        /* No L2 cache, icache size 8k, dcache size 8k, uncached coherency. */
        .CP0_Config0 = (2 << CP0C0_Impl) | (1 << CP0C0_IC) | (1 << CP0C0_DC) |
                       (2 << CP0C0_K0),
        /* Note: Config1 is only used internally, the R4000 has only Config0. */
        .CP0_Config1 = (1 << CP0C1_FP) | (47 << CP0C1_MMU),
        .CP0_LLAddr_rw_bitmask = 0xFFFFFFFF,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 16,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3678FFFF,
        /* The R4000 has a full 64bit FPU but doesn't use the fcr0 bits. */
        .CP1_fcr0 = (0x5 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0x0183FFFF,
        .SEGBITS = 40,
        .PABITS = 36,
        .insn_flags = CPU_MIPS3,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /*
         * The processor every SGI Origin shipped with. Config0 uses the
         * R10000's own layout, whose bits 24..0 come from the reset mode bits
         * the board drives. On the Origin 200 machine the board does drive
         * them: its ratified mode word (SN0_PROCESSOR_MODE_BITS, hw/mips/
         * sn0.h) is latched over bits 24..0 at every reset, so of this
         * default only the hardwired cache size fields above them reach a
         * guest there. The mode-bit values below are a fallback for a bare
         * R10000 on some other machine, and they are not the Origin 200's:
         * the board's word differs in the kseg0 algorithm, the secondary
         * cache clock divisor and the outstanding-request limit.
         * Bit 31 of the layout is the top bit of the primary instruction
         * cache size field and reads zero, so it does not claim a second
         * Config register the way a MIPS32 processor's bit 31 would.
         *
         * Virtual addresses are 44 bits and physical addresses 40, which is
         * wider than the R4000 and is what gives the XContext register its own
         * field positions.
         *
         * LLAddr is a plain 32-bit scratch register here: unlike the R4000 the
         * R10000 does not record a load-linked address in it, and keeps it only
         * for compatibility.
         */
        .name = "R10000",
        .CP0_PRid = 0x00000926,
        .CP0_Config0 = (3 << CP0C0_R10K_IC) | (3 << CP0C0_R10K_DC) |
                       (3 << CP0C0_R10K_SC) | (1 << CP0C0_R10K_SS) |
                       (1 << CP0C0_R10K_SB) | (3 << CP0C0_R10K_EC) |
                       (3 << CP0C0_K0),
        /* Config1 is only used internally; the R10000 has only Config0. */
        .CP0_Config1 = (1 << CP0C1_FP) | (63 << CP0C1_MMU),
        /*
         * The Config layout (manual 14.14) hardwires the primary cache size
         * fields in bits 31:26 and reserves 25:22, so a write preserves
         * everything above the kseg0 algorithm in bits 2:0 -- the one field
         * the manual carries forward from the R4400. The generic preserve
         * mask was cut for the MIPS32 layout and zeroes bits 30:25, which
         * destroys both cache size fields on any read-modify-write.
         */
        .CP0_Config0_preserve_bitmask = 0xFFFFFFF8,
        /*
         * PageMask's mask field is bits 24:13 - seven sizes, 4 Kbyte to
         * 16 Mbyte (manual 14.5) - and the fields either side are reserved,
         * written as zeroes and returning zeroes. Dropping them before the
         * width check stops a reserved bit reading back or widening a page.
         */
        .CP0_PageMask_rw_bitmask = 0x01FFE000,
        /*
         * FrameMask is sixteen bits, masking physical address bits 39:24 out
         * of a TLB entry as it is written. The rest of the register is
         * ignored on write and reads as zero.
         */
        .CP0_Framemask_rw_bitmask = 0xFFFF,
        /*
         * EntryLo's uncached attribute occupies the top two bits, above
         * everything the physical address mask names, and has to survive a
         * write.
         */
        .CP0_EntryLo_extra_rw_bitmask = 3ULL << CP0EnLo_UC,
        /*
         * xkseg ends a full 2^32-byte band below the compatibility spaces
         * rather than at the generic bound: the manual gives the segment as
         * 0xC000000000000000 through 0xC0000FFEFFFFFFFF in the 64-bit kernel
         * mode figure, the virtual address space table and the TLB refill
         * vector table alike, with everything above that an address error.
         */
        .xkseg_limit = 0xC0000FFEFFFFFFFFULL,
        .CP0_LLAddr_rw_bitmask = 0xFFFFFFFF,
        .CP0_LLAddr_shift = 0,
        /*
         * Every bit of TagLo is writable and reads back what was written.
         * The R10000 manual states this twice about this register, in the
         * body of its TagLo and TagHi section and again in that section's
         * own footnote: the register is a 32-bit read/write register, and a
         * value written by MTC0 can be retrieved by MFC0 unless a CACHE
         * instruction has modified the content in between. The stated reason
         * is compatibility with an operating system that used the register
         * as scratch, which is why it holds bits no cache tag field claims.
         *
         * The generic mask drops bits 0, 3, 8 and 9, and on this processor
         * all four are defined for a primary instruction cache tag: tag
         * parity, the least-recently-used bit, and two bits of the physical
         * address the tag carries. The vendor firmware's cache self-test
         * writes a tag, stores it, loads it back and compares, so dropping
         * those bits made that test fail against this model.
         */
        .CP0_TagLo_rw_bitmask = 0xFFFFFFFF,
        /*
         * Coprocessor 0 register 26 is this processor's ECC register, and it
         * is ten bits wide: bit 9 is the parity bit for a secondary cache
         * quadword, and bits 8:0 carry that quadword's nine ECC bits, with a
         * primary data cache's four parity bits and a primary instruction
         * cache's single parity bit occupying the low end of the same nine.
         * Bits 31:10 are reserved, and the manual says they must be written
         * as zeroes and return zeroes when read. The R10000 manual calls it a
         * 10-bit read/write register in that section's opening sentence.
         *
         * Not the same answer as TagLo above, which is why the manual was
         * read rather than the previous fix repeated. TagLo is promised to
         * round-trip all thirty-two bits; this register is promised ten.
         *
         * Naming any writable bits here also selects plain register behaviour
         * for register 26, because the field semantics QEMU otherwise applies
         * to it belong to a different processor's register of the same number
         * and would discard nine of these ten bits.
         */
        .CP0_ECC_rw_bitmask = 0x3FF,
        .SYNCI_Step = 32,
        .CCRes = 2,
        /*
         * Four bits more than the R4000 mask this was derived from. Bit 31
         * is XX rather than a fourth coprocessor's usable bit, bit 30 is
         * CU2 — a functional usability bit for a coprocessor that does not
         * exist, selecting which exception its instructions take (User's
         * Manual version 2.0, sections 14.11 and 17.7) — bit 16 is the
         * cache error disable, which software asserts to stop parity and ECC
         * errors raising exceptions, and bit 18 is the cache-hit indicator
         * the secondary Hit CacheOps set and clear — hardware status, but
         * status the manual makes writable twice over: "This bit is readable
         * and writable by software" in the CACHE instruction's section, "can
         * also be modified by a MTC0 instruction" in the CACHE chapter. The
         * two bits above the diagnostic status field stay read-only because
         * they are reserved, and the fields that report what the hardware
         * last did stay read-only because they are status rather than
         * control.
         */
        .CP0_Status_rw_bitmask = 0xF67DFFFF,
        /* A full 64-bit FPU that does not use the fcr0 feature bits. */
        .CP1_fcr0 = (0x9 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        /*
         * The MIPS IV compare model: eight condition bits, bit 23 and bits
         * 31:25, all written by a control-register move — the manual's FSR
         * chapter leaves only bits 22:18 unimplemented, ignored on write
         * and zero on read. The single-condition-bit mask this entry was
         * derived from dropped the upper seven, which silently corrupted
         * any context restore carrying a compare result there.
         */
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 44,
        .PABITS = 40,
        .insn_flags = CPU_MIPS4 | INSN_R10K,
        /*
         * Every behaviour the rest of the target selects by name. This is the
         * only place this processor's identity is mapped onto them, so a
         * later member of the family names its own set here and nothing else
         * has to be revisited.
         */
        .cfg = {
            .uncached_attribute = true,
            .erl_restricts_xuseg = true,
            .watch_is_physical = true,
            .tlb_has_no_ehinv = true,
            .warm_reset_reports_sr = true,
            .cache_error_is_error_level = true,
            .exception_breaks_ll_link = true,
            .lladdr_is_scratch = true,
            .fp_traps_denormal_result = true,
            .fp_cvt_l_limited_to_51_bits = true,
            .stores_are_posted = true,
            .has_cache_diagnostics = true,
            .has_branch_prediction_table = true,
            .has_performance_counters = true,
            .compare_match_is_continuous = true,
            .mtc0_entrylo_moves_64_bits = true,
        },
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "VR5432",
        .CP0_PRid = 0x00005400,
        /* No L2 cache, icache size 8k, dcache size 8k, uncached coherency. */
        .CP0_Config0 = (2 << CP0C0_Impl) | (1 << CP0C0_IC) | (1 << CP0C0_DC) |
                       (2 << CP0C0_K0),
        .CP0_Config1 = (1 << CP0C1_FP) | (47 << CP0C1_MMU),
        .CP0_LLAddr_rw_bitmask = 0xFFFFFFFFL,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 16,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x3678FFFF,
        /* The VR5432 has a full 64bit FPU but doesn't use the fcr0 bits. */
        .CP1_fcr0 = (0x54 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 40,
        .PABITS = 32,
        .insn_flags = CPU_MIPS4 | INSN_VR54XX,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "5Kc",
        .CP0_PRid = 0x00018100,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (31 << CP0C1_MMU) |
                       (1 << CP0C1_IS) | (4 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (1 << CP0C1_DS) | (4 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x12F8FFFF,
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R1,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "5Kf",
        .CP0_PRid = 0x00018100,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (31 << CP0C1_MMU) |
                       (1 << CP0C1_IS) | (4 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (1 << CP0C1_DS) | (4 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x36F8FFFF,
        /* The 5Kf has F64 / L / W but doesn't use the fcr0 bits. */
        .CP1_fcr0 = (1 << FCR0_D) | (1 << FCR0_S) |
                    (0x81 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R1,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "20Kc",
        /* We emulate a later version of the 20Kc, earlier ones had a broken
           WAIT instruction. */
        .CP0_PRid = 0x000182a0,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AT) |
                    (MMU_TYPE_R4000 << CP0C0_MT) | (1 << CP0C0_VI),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (47 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 1,
        .CP0_Status_rw_bitmask = 0x36FBFFFF,
        /* The 20Kc has F64 / L / W but doesn't use the fcr0 bits. */
        .CP1_fcr0 = (1 << FCR0_3D) | (1 << FCR0_PS) |
                    (1 << FCR0_D) | (1 << FCR0_S) |
                    (0x82 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 40,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R1,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /* A generic CPU providing MIPS64 Release 2 features.
           FIXME: Eventually this should be replaced by a real CPU model. */
        .name = "MIPS64R2-generic",
        .CP0_PRid = 0x00010000,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (63 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_LPA),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x36FBFFFF,
        .CP0_EBaseWG_rw_bitmask = (1 << CP0EBase_WG),
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_3D) | (1 << FCR0_PS) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x00 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "5KEc",
        .CP0_PRid = 0x00018900,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (31 << CP0C1_MMU) |
                       (1 << CP0C1_IS) | (4 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (1 << CP0C1_DS) | (4 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x12F8FFFF,
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "5KEf",
        .CP0_PRid = 0x00018900,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (31 << CP0C1_MMU) |
                       (1 << CP0C1_IS) | (4 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (1 << CP0C1_DS) | (4 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3,
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x36F8FFFF,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_L) | (1 << FCR0_W) |
                    (1 << FCR0_D) | (1 << FCR0_S) |
                    (0x89 << FCR0_PRID) | (0x0 << FCR0_REV),
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "I6400",
        .CP0_PRid = 0x1A900,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (15 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (5 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (5 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (0 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) |
                       (1 << CP0C3_CMGCR) | (1 << CP0C3_MSAP) |
                       (1 << CP0C3_BP) | (1 << CP0C3_BI) | (1 << CP0C3_ULRI) |
                       (1 << CP0C3_RXI) | (1 << CP0C3_LPA) | (1 << CP0C3_VInt),
        .CP0_Config4 = MIPS_CONFIG4 | (1U << CP0C4_M) | (3 << CP0C4_IE) |
                       (1 << CP0C4_AE) | (0xfc << CP0C4_KScrExist),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_CRCP) | (1 << CP0C5_XNP) |
                       (1 << CP0C5_VP) | (1 << CP0C5_LLB) | (1 << CP0C5_MRP) |
                       (3 << CP0C5_GI),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_MSAEn) | (1 << CP0C5_SBRI) |
                                  (1 << CP0C5_FRE) | (1 << CP0C5_UFE),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x30D8FFFF,
        .CP0_PageGrain = (1 << CP0PG_IEC) | (1 << CP0PG_XIE) |
                         (1U << CP0PG_RIE),
        .CP0_PageGrain_rw_bitmask = (1 << CP0PG_ELPA),
        .CP0_EBaseWG_rw_bitmask = (1 << CP0EBase_WG),
        .CP1_fcr0 = (1 << FCR0_FREP) | (1 << FCR0_HAS2008) | (1 << FCR0_F64) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x03 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = (1 << FCR31_ABS2008) | (1 << FCR31_NAN2008),
        .CP1_fcr31_rw_bitmask = 0x0103FFFF,
        .MSAIR = 0x03 << MSAIR_ProcID,
        .SEGBITS = 48,
        .PABITS = 48,
        .insn_flags = CPU_MIPS64R6,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "I6500",
        .CP0_PRid = 0x1B000,
        .CP0_Config0 = MIPS_CONFIG0 | (0x2 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (15 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (5 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (5 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (0 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) |
                       (1 << CP0C3_CMGCR) | (1 << CP0C3_MSAP) |
                       (1 << CP0C3_BP) | (1 << CP0C3_BI) | (1 << CP0C3_ULRI) |
                       (1 << CP0C3_RXI) | (1 << CP0C3_LPA) | (1 << CP0C3_VInt),
        .CP0_Config4 = MIPS_CONFIG4 | (1U << CP0C4_M) | (3 << CP0C4_IE) |
                       (1 << CP0C4_AE) | (0xfc << CP0C4_KScrExist),
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_CRCP) | (1 << CP0C5_XNP) |
                       (1 << CP0C5_VP) | (1 << CP0C5_LLB) | (1 << CP0C5_MRP) |
                       (3 << CP0C5_GI),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_MSAEn) | (1 << CP0C5_SBRI) |
                                  (1 << CP0C5_FRE) | (1 << CP0C5_UFE),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 64,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x30D8FFFF,
        .CP0_PageGrain = (1 << CP0PG_IEC) | (1 << CP0PG_XIE) |
                         (1U << CP0PG_RIE),
        .CP0_PageGrain_rw_bitmask = (1 << CP0PG_ELPA),
        .CP0_EBaseWG_rw_bitmask = (1 << CP0EBase_WG),
        .CP1_fcr0 = (1 << FCR0_FREP) | (1 << FCR0_HAS2008) | (1 << FCR0_F64) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x03 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = (1 << FCR31_ABS2008) | (1 << FCR31_NAN2008),
        .CP1_fcr31_rw_bitmask = 0x0103FFFF,
        .MSAIR = 0x03 << MSAIR_ProcID,
        .SEGBITS = 48,
        .PABITS = 48,
        .insn_flags = CPU_MIPS64R6,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "Loongson-2E",
        .CP0_PRid = 0x6302,
        /* 64KB I-cache and d-cache. 4 way with 32 bit cache line size.  */
        .CP0_Config0 = (3 << CP0C0_Impl) | (4 << CP0C0_IC) | (4 << CP0C0_DC) |
                       (1 << CP0C0_IB) | (1 << CP0C0_DB) | (0x2 << CP0C0_K0),
        /* Note: Config1 is only used internally,
           Loongson-2E has only Config0.  */
        .CP0_Config1 = (1 << CP0C1_FP) | (47 << CP0C1_MMU),
        .SYNCI_Step = 16,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x35D0FFFF,
        .CP1_fcr0 = (0x5 << FCR0_PRID) | (0x1 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 40,
        .PABITS = 40,
        .insn_flags = CPU_MIPS3 | INSN_LOONGSON2E,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "Loongson-2F",
        .CP0_PRid = 0x6303,
        /* 64KB I-cache and d-cache. 4 way with 32 bit cache line size.  */
        .CP0_Config0 = (3 << CP0C0_Impl) | (4 << CP0C0_IC) | (4 << CP0C0_DC) |
                       (1 << CP0C0_IB) | (1 << CP0C0_DB) | (0x2 << CP0C0_K0),
        /* Note: Config1 is only used internally,
           Loongson-2F has only Config0.  */
        .CP0_Config1 = (1 << CP0C1_FP) | (47 << CP0C1_MMU),
        .SYNCI_Step = 16,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0xF5D0FF1F,   /* Bits 7:5 not writable.  */
        .CP1_fcr0 = (0x5 << FCR0_PRID) | (0x1 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 40,
        .PABITS = 40,
        .insn_flags = CPU_MIPS3 | INSN_LOONGSON2F | ASE_LMMI,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "Loongson-3A1000", /* Loongson-3A R1, GS464-based */
        .CP0_PRid = 0x6305,
        /* 64KB I-cache and d-cache. 4 way with 32 bit cache line size.  */
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (63 << CP0C1_MMU) |
                       (3 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (3 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2 | (7 << CP0C2_SS) | (4 << CP0C2_SL) |
                       (3 << CP0C2_SA),
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_LPA),
        .CP0_LLAddr_rw_bitmask = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x74D8FFFF,
        .CP0_PageGrain = (1 << CP0PG_ELPA),
        .CP0_PageGrain_rw_bitmask = (1 << CP0PG_ELPA),
        .CP1_fcr0 = (0x5 << FCR0_PRID) | (0x1 << FCR0_REV) | (0x1 << FCR0_F64) |
                    (0x1 << FCR0_PS) | (0x1 << FCR0_L) | (0x1 << FCR0_W) |
                    (0x1 << FCR0_D) | (0x1 << FCR0_S),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 48,
        .PABITS = 48,
        .insn_flags = CPU_MIPS64R2 | INSN_LOONGSON3A |
                      ASE_LMMI | ASE_LEXT,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        .name = "Loongson-3A4000", /* Loongson-3A R4, GS464V-based */
        .CP0_PRid = 0x14C000,
        /* 64KB I-cache and d-cache. 4 way with 32 bit cache line size.  */
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (63 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (5 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (5 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2 | (5 << CP0C2_SS) | (5 << CP0C2_SL) |
                       (15 << CP0C2_SA),
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) | (1 << CP0C3_MSAP) |
                       (1 << CP0C3_BP) | (1 << CP0C3_BI) | (1 << CP0C3_ULRI) |
                       (1 << CP0C3_RXI) | (1 << CP0C3_LPA) | (1 << CP0C3_VInt),
        .CP0_Config4 = MIPS_CONFIG4 | (1U << CP0C4_M) | (2 << CP0C4_IE) |
                       (1 << CP0C4_AE) | (0x1c << CP0C4_KScrExist),
        .CP0_Config4_rw_bitmask = 0,
        .CP0_Config5 = MIPS_CONFIG5 | (1 << CP0C5_CRCP) | (1 << CP0C5_NFExists),
        .CP0_Config5_rw_bitmask = (1 << CP0C5_K) | (1 << CP0C5_CV) |
                                  (1 << CP0C5_MSAEn) | (1 << CP0C5_UFE) |
                                  (1 << CP0C5_FRE) | (1 << CP0C5_SBRI),
        .CP0_Config6 = (1 << CP0C6_VCLRU) | (1 << CP0C6_DCLRU) |
                       (1 << CP0C6_SFBEN) | (1 << CP0C6_VLTINT) |
                       (1 << CP0C6_INSTPREF) | (1 << CP0C6_DATAPREF),
        .CP0_Config6_rw_bitmask = (1 << CP0C6_BPPASS) | (0x3f << CP0C6_KPOS) |
                       (1 << CP0C6_KE) | (1 << CP0C6_VTLBONLY) |
                       (1 << CP0C6_LASX) | (1 << CP0C6_SSEN) |
                       (1 << CP0C6_DISDRTIME) | (1 << CP0C6_PIXNUEN) |
                       (1 << CP0C6_SCRAND) | (1 << CP0C6_LLEXCEN) |
                       (1 << CP0C6_DISVC) | (1 << CP0C6_VCLRU) |
                       (1 << CP0C6_DCLRU) | (1 << CP0C6_PIXUEN) |
                       (1 << CP0C6_DISBLKLYEN) | (1 << CP0C6_UMEMUALEN) |
                       (1 << CP0C6_SFBEN) | (1 << CP0C6_FLTINT) |
                       (1 << CP0C6_VLTINT) | (1 << CP0C6_DISBTB) |
                       (3 << CP0C6_STPREFCTL) | (1 << CP0C6_INSTPREF) |
                       (1 << CP0C6_DATAPREF),
        .CP0_Config7 = 0,
        .CP0_Config7_rw_bitmask = (1 << CP0C7_NAPCGEN) | (1 << CP0C7_UNIMUEN) |
                                  (1 << CP0C7_VFPUCGEN),
        .CP0_LLAddr_rw_bitmask = 1,
        .SYNCI_Step = 16,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x7DDBFFFF,
        .CP0_PageGrain = (1 << CP0PG_ELPA),
        .CP0_PageGrain_rw_bitmask = (1U << CP0PG_RIE) | (1 << CP0PG_XIE) |
                    (1 << CP0PG_ELPA) | (1 << CP0PG_IEC),
        .CP1_fcr0 = (0x5 << FCR0_PRID) | (0x1 << FCR0_REV) | (0x1 << FCR0_F64) |
                    (0x1 << FCR0_PS) | (0x1 << FCR0_L) | (0x1 << FCR0_W) |
                    (0x1 << FCR0_D) | (0x1 << FCR0_S),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .MSAIR = (0x01 << MSAIR_ProcID) | (0x40 << MSAIR_Rev),
        .lcsr_cpucfg1 = (1 << CPUCFG1_FP) | (2 << CPUCFG1_FPREV) |
                    (1 << CPUCFG1_MSA1) | (1 << CPUCFG1_LSLDR0) |
                    (1 << CPUCFG1_LSPERF) | (1 << CPUCFG1_LSPERFX) |
                    (1 << CPUCFG1_LSSYNCI) | (1 << CPUCFG1_LLEXC) |
                    (1 << CPUCFG1_SCRAND) | (1 << CPUCFG1_MUALP) |
                    (1 << CPUCFG1_KMUALEN) | (1 << CPUCFG1_ITLBT) |
                    (1 << CPUCFG1_SFBP) | (1 << CPUCFG1_CDMAP),
        .lcsr_cpucfg2 = (1 << CPUCFG2_LEXT1) | (1 << CPUCFG2_LCSRP) |
                    (1 << CPUCFG2_LDISBLIKELY),
        .SEGBITS = 48,
        .PABITS = 48,
        .insn_flags = CPU_MIPS64R2 | INSN_LOONGSON3A |
                      ASE_LMMI | ASE_LEXT,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /* A generic CPU providing MIPS64 DSP R2 ASE features.
           FIXME: Eventually this should be replaced by a real CPU model. */
        .name = "mips64dspr2",
        .CP0_PRid = 0x00010000,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) | (63 << CP0C1_MMU) |
                       (2 << CP0C1_IS) | (4 << CP0C1_IL) | (3 << CP0C1_IA) |
                       (2 << CP0C1_DS) | (4 << CP0C1_DL) | (3 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1U << CP0C3_M) | (1 << CP0C3_DSP2P) |
                       (1 << CP0C3_DSPP) | (1 << CP0C3_LPA),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 0,
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x37FBFFFF,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_3D) | (1 << FCR0_PS) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x00 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 42,
        .PABITS = 36,
        .insn_flags = CPU_MIPS64R2 | ASE_DSP | ASE_DSP_R2,
        .mmu_type = MMU_TYPE_R4000,
    },
    {
        /*
         * Octeon 68xx with MIPS64 Cavium Octeon features.
         */
        .name = "Octeon68XX",
        .CP0_PRid = 0x000D9100,
        .CP0_Config0 = MIPS_CONFIG0 | (0x1 << CP0C0_AR) | (0x2 << CP0C0_AT) |
                       (MMU_TYPE_R4000 << CP0C0_MT),
        .CP0_Config1 = MIPS_CONFIG1 | (1 << CP0C1_FP) |
                       (0x3F << CP0C1_MMU) |
                       (1 << CP0C1_IS) | (4 << CP0C1_IL) | (1 << CP0C1_IA) |
                       (1 << CP0C1_DS) | (4 << CP0C1_DL) | (1 << CP0C1_DA) |
                       (1 << CP0C1_PC) | (1 << CP0C1_WR) | (1 << CP0C1_EP),
        .CP0_Config2 = MIPS_CONFIG2,
        .CP0_Config3 = MIPS_CONFIG3 | (1 << CP0C3_LPA),
        .CP0_Config4 = MIPS_CONFIG4 | (1U << CP0C4_M) |
                       (0x3c << CP0C4_KScrExist) | (1U << CP0C4_MMUExtDef) |
                       (3U << CP0C4_MMUSizeExt),
        .CP0_LLAddr_rw_bitmask = 0,
        .CP0_LLAddr_shift = 4,
        .CP0_PageGrain = (1 << CP0PG_ELPA),
        .SYNCI_Step = 32,
        .CCRes = 2,
        .CP0_Status_rw_bitmask = 0x36F8FFFF,
        .CP1_fcr0 = (1 << FCR0_F64) | (1 << FCR0_3D) | (1 << FCR0_PS) |
                    (1 << FCR0_L) | (1 << FCR0_W) | (1 << FCR0_D) |
                    (1 << FCR0_S) | (0x00 << FCR0_PRID) | (0x0 << FCR0_REV),
        .CP1_fcr31 = 0,
        .CP1_fcr31_rw_bitmask = 0xFF83FFFF,
        .SEGBITS = 42,
        .PABITS = 49,
        .insn_flags = CPU_MIPS64R2 | INSN_OCTEON,
        .mmu_type = MMU_TYPE_R4000,
    },

#endif
};
const int mips_defs_number = ARRAY_SIZE(mips_defs);

static void fpu_init (CPUMIPSState *env, const mips_def_t *def)
{
    int i;

    for (i = 0; i < MIPS_FPU_MAX; i++)
        env->fpus[i].fcr0 = def->CP1_fcr0;

    memcpy(&env->active_fpu, &env->fpus[0], sizeof(env->active_fpu));
}

static void mvp_init(CPUMIPSState *env)
{
    MIPSCPU *cpu = env_archcpu(env);

    cpu->mvp = g_malloc0(sizeof(CPUMIPSMVPContext));

    if (!ase_mt_available(env)) {
        return;
    }

    /* MVPConf1 implemented, TLB shareable, no gating storage support,
       programmable cache partitioning implemented, number of allocatable
       and shareable TLB entries, MVP has allocatable TCs, 2 VPEs
       implemented, 5 TCs implemented. */
    cpu->mvp->CP0_MVPConf0 = (1U << CP0MVPC0_M) | (1 << CP0MVPC0_TLBS) |
                             (0 << CP0MVPC0_GS) | (1 << CP0MVPC0_PCP) |
// TODO: actually do 2 VPEs.
//                             (1 << CP0MVPC0_TCA) | (0x1 << CP0MVPC0_PVPE) |
//                             (0x04 << CP0MVPC0_PTC);
                             (1 << CP0MVPC0_TCA) | (0x0 << CP0MVPC0_PVPE) |
                             (0x00 << CP0MVPC0_PTC);
#if !defined(CONFIG_USER_ONLY)
    /* Usermode has no TLB support */
    cpu->mvp->CP0_MVPConf0 |= (env->tlb->nb_tlb << CP0MVPC0_PTLBE);
#endif

    /* Allocatable CP1 have media extensions, allocatable CP1 have FP support,
       no UDI implemented, no CP2 implemented, 1 CP1 implemented. */
    cpu->mvp->CP0_MVPConf1 = (1U << CP0MVPC1_CIM) | (1 << CP0MVPC1_CIF) |
                             (0x0 << CP0MVPC1_PCX) | (0x0 << CP0MVPC1_PCP2) |
                             (0x1 << CP0MVPC1_PCP1);
}
