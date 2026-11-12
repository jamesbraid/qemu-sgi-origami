/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Sonic Solutions HDSP PCI audio card: the Intel i960 Rx I/O processor,
 * its flash, DRAM and board glue, and the four DSP56301 processors on its
 * private secondary bus, with their serial ports, DMA and timers, the link
 * to the converter box and the sample clock.
 *
 * The hardware reference is docs/system/mips/sgi/hardware/sonic-hdsp.rst.
 */

#ifndef HW_MISC_SGI_SONIC_HDSP_H
#define HW_MISC_SGI_SONIC_HDSP_H

#include "hw/pci/pci_device.h"
#include "hw/pci/pci_bridge.h"
#include "chardev/char-fe.h"
#include "hw/coproc/i960/i960.h"
#include "hw/coproc/dsp56300/dsp56300.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include "hw/misc/sgi-sonic-ada8824.h"

#define TYPE_SGI_SONIC_HDSP "sgi-sonic-hdsp"
OBJECT_DECLARE_SIMPLE_TYPE(SonicHDSPState, SGI_SONIC_HDSP)

#define TYPE_SGI_SONIC_HDSP_ATU "sgi-sonic-hdsp-atu"
OBJECT_DECLARE_SIMPLE_TYPE(SonicHDSPATUState, SGI_SONIC_HDSP_ATU)

#define TYPE_SGI_SONIC_HDSP_DSP "sgi-sonic-hdsp-dsp"
OBJECT_DECLARE_SIMPLE_TYPE(SonicHDSPDSPState, SGI_SONIC_HDSP_DSP)

#define SONIC_HDSP_NUM_DSP      4
#define SONIC_HDSP_FLASH_SIZE   (512 * 1024)
#define SONIC_HDSP_NV_WORD_DEFAULT 0x987603dfu

/* DSP56300 on-chip memory (default configuration) and external memory. */
#define SONIC_DSP_PMEM_WORDS    0x1000
#define SONIC_DSP_XMEM_WORDS    0x800
#define SONIC_DSP_YMEM_WORDS    0x800
#define SONIC_DSP_EXT_BASE      0x400000
#define SONIC_DSP_EXT_WORDS     0x20000
#define SONIC_DSP_FIFO_DEPTH    6
#define SONIC_DSP_NDMA          6
#define SONIC_DSP_NESSI         2
#define SONIC_DSP_NTIMER        3
#define SONIC_DSP_PERIPH_WORDS  128     /* X:$FFFF80 to X:$FFFFFF */
#define SONIC_HDSP_SYNC_WORDS   9       /* Y:$FFFFF7 to Y:$FFFFFF */

/* One ESSI: registers as the DSP56301 manual lists them, plus the slot. */
typedef struct SonicDspEssi {
    uint32_t cra, crb, tsma, tsmb, rsma, rsmb;
    uint32_t ssisr;
    uint32_t tx[3];
    uint32_t rx;
    uint32_t tx_written;        /* bit n: TXn written since the last slot */
    uint32_t tsr_skip;
    uint32_t sr_read;           /* SSISR read since ROE or TUE was set */
    uint32_t pcr, prr, pdr;
    uint32_t in_word;           /* chained peer's word, when in_valid */
    uint32_t in_valid;
} SonicDspEssi;

typedef struct SonicDspDma {
    uint32_t dsr, ddr, dco, dco_init, dcr;
    uint32_t blk_active;        /* a block transfer was triggered */
} SonicDspDma;

typedef struct SonicDspTimer {
    uint32_t tcsr, tlr, tcpr, tcr;
    uint32_t irq_out;           /* bit 0 compare, bit 1 overflow: raised */
} SonicDspTimer;

typedef enum SonicDspBoot {
    SONIC_DSP_BOOT_RUN = 0,     /* the program is running */
    SONIC_DSP_BOOT_COUNT,       /* the bootstrap wants a word count */
    SONIC_DSP_BOOT_ADDR,        /* then a start address */
    SONIC_DSP_BOOT_DATA,        /* then that many program words */
} SonicDspBoot;

/* A DSP's host-port function on the secondary bus. */
struct SonicHDSPDSPState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/
    SonicHDSPState *hub;
    uint32_t index;
    MemoryRegion bar0;

    DSP56300State core;
    uint32_t pmem[SONIC_DSP_PMEM_WORDS];
    uint32_t xmem[SONIC_DSP_XMEM_WORDS];
    uint32_t ymem[SONIC_DSP_YMEM_WORDS];
    uint32_t ext_p[SONIC_DSP_EXT_WORDS];
    uint32_t ext_y[SONIC_DSP_EXT_WORDS];

    /* HI32 host side, as the page lists it. */
    uint32_t hctr;
    uint32_t hcvr;
    uint32_t tx_fifo[SONIC_DSP_FIFO_DEPTH];     /* host to DSP */
    uint32_t tx_count;
    uint32_t rx_fifo[SONIC_DSP_FIFO_DEPTH];     /* DSP to host */
    uint32_t rx_count;
    bool in_reset;
    bool pci_mode;      /* HI32 in PCI target mode: answers config cycles */

    /* DSP-side peripherals (sgi-sonic-hdsp-dsp.c). */
    uint32_t ipr_c, ipr_p, dctr;
    uint32_t dor[4];
    uint32_t dtd;
    uint32_t boot;              /* a SonicDspBoot */
    uint32_t boot_count, boot_next, boot_start;
    uint32_t hc_raised;
    uint32_t dma_busy;          /* inside the DMA loop; not migrated */
    uint32_t cyc_rem;           /* core cycles not yet a timer count */
    SonicDspDma dma[SONIC_DSP_NDMA];
    SonicDspEssi essi[SONIC_DSP_NESSI];
    SonicDspTimer timer[SONIC_DSP_NTIMER];
    uint32_t periph[SONIC_DSP_PERIPH_WORDS];    /* stored-only registers */
};

/* Function 1: the primary ATU and its messaging unit. */
struct SonicHDSPATUState {
    /*< private >*/
    PCIDevice parent_obj;
    /*< public >*/
    SonicHDSPState *hub;
    MemoryRegion bar0;
};

typedef struct SonicHDSPDMA {
    uint32_t ccr, csr, dar, ndar, padr, puadr, ladr, bcr, dcr;
} SonicHDSPDMA;

typedef struct SonicHDSPTimer {
    uint32_t trr;
    uint32_t tcr;           /* count as of start_ns */
    uint32_t tmr;
    int64_t start_ns;
} SonicHDSPTimer;

/* Function 0: the bridge unit, and the owner of everything on the board. */
struct SonicHDSPState {
    /*< private >*/
    PCIBridge parent_obj;
    /*< public >*/
    SonicHDSPATUState *atu;
    SonicHDSPDSPState *dsp[SONIC_HDSP_NUM_DSP];

    /* Properties. */
    char *flash_image;
    bool flash_writable;
    bool flash_persist;
    uint64_t dram_size;
    CharFrontend iop_console;
    uint32_t nv_word;
    uint32_t iop_clock;
    uint32_t dsp_clock;
    uint32_t sample_rate;
    bool essi1_chain;
    SonicAda8824State *box;

    /* DSP scheduling: one timer runs the four cores and the sample clock. */
    QEMUTimer *dsp_timer;
    int64_t dsp_last_ns;
    int64_t frame_epoch_ns;
    uint64_t frame_count;
    uint64_t dsp_ns_rem;
    int64_t mig_dsp_age, mig_frame_epoch_age;
    uint32_t sync[SONIC_HDSP_SYNC_WORDS];       /* Y:$FFFFF7 and up */

    /* The processor. */
    I960State cpu;
    QEMUTimer *iop_timer;
    int64_t iop_last_ns;
    MemoryRegion dram;
    uint8_t *dram_ptr;
    uint8_t flash[SONIC_HDSP_FLASH_SIZE];
    int flash_fd;

    /* AT29C040A programming model. */
    bool fl_loading;
    bool fl_committing;
    bool fl_protect;
    bool fl_arm_protect;
    bool fl_armed;
    uint8_t fl_seq;
    uint32_t fl_sector;
    uint32_t fl_last;
    uint32_t fl_count;
    uint8_t fl_data[256];
    uint8_t fl_loaded[32];
    int64_t fl_deadline_ns;
    int64_t fl_busy_until_ns;
    bool fl_toggle;

    /* ATU registers that are not in the host-visible configuration space. */
    uint32_t atu_x[(0xb8 - 0x40) / 4];

    /* Messaging unit. */
    uint32_t imr[2], omr[2];
    uint32_t idr, iisr, iimr, odr, oisr, oimr;
    uint32_t mucr, qbar;
    uint32_t qptr[8];               /* IFH IFT IPH IPT OFH OFT OPH OPT */
    uint32_t iar, arsr, awr;
    bool nmi_latched;

    SonicHDSPDMA dma[3];
    uint32_t mc[(0x40) / 4];        /* memory controller 0x1500-0x153f */
    uint32_t lba[2];
    uint32_t reg_1610;
    SonicHDSPTimer timer[2];
    uint32_t i2c[5];                /* ICR ISR ISAR IDBR ICCR */
    uint32_t lmcon[16];
    uint32_t core_mmr[64];          /* bus configuration and breakpoints */

    /* Board glue and byte-wide registers. */
    uint8_t glue_reset;
    uint8_t glue_cmd;
    uint8_t glue_intreq;
    uint8_t glue_enable[2];
    uint8_t glue_080000;
    uint8_t glue_038000;
    uint8_t uart_regs[16];
    uint8_t board_reg;
    uint8_t uart_rx[16];
    uint8_t uart_rx_n;

    /* The I2C EEPROM (24C08 class, 1 KiB) and its transfer state. */
    uint8_t eeprom[1024];
    uint8_t i2c_state;
    uint16_t i2c_ptr;

    /* Host-visible state. */
    uint64_t config_retries;
    /* Virtual-time stamps as ages, for a destination whose clock restarts. */
    int64_t mig_timer_age[2];
    int64_t mig_deadline_left;
    int64_t mig_busy_left;
    bool iop_access;                /* a config cycle from the i960 itself */
    bool irq_level;
};

/* sgi-sonic-hdsp-iop.c */
void sonic_hdsp_iop_realize(SonicHDSPState *h, Error **errp);
void sonic_hdsp_iop_reset(SonicHDSPState *h);
void sonic_hdsp_iop_unrealize(SonicHDSPState *h);
void sonic_hdsp_iop_kick(SonicHDSPState *h);
bool sonic_hdsp_iop_state_valid(SonicHDSPState *h);
void sonic_hdsp_iop_pre_save(SonicHDSPState *h);
void sonic_hdsp_iop_post_load(SonicHDSPState *h);
uint32_t sonic_hdsp_local_read(SonicHDSPState *h, uint32_t addr,
                               unsigned size);
void sonic_hdsp_local_write(SonicHDSPState *h, uint32_t addr, uint32_t val,
                            unsigned size);
uint32_t sonic_hdsp_mu_host_read(SonicHDSPState *h, hwaddr off, unsigned size);
void sonic_hdsp_mu_host_write(SonicHDSPState *h, hwaddr off, uint32_t val,
                              unsigned size);
void sonic_hdsp_update_irq(SonicHDSPState *h);
bool sonic_hdsp_dsp_visible(SonicHDSPState *h, unsigned slot);

/* sgi-sonic-hdsp-dsp.c */
bool sonic_hdsp_dsp_realize(SonicHDSPState *h, Error **errp);
void sonic_hdsp_dsp_unrealize(SonicHDSPState *h);
void sonic_hdsp_dsp_board_reset(SonicHDSPState *h);
void sonic_hdsp_dsp_pre_save(SonicHDSPState *h);
void sonic_hdsp_dsp_post_load(SonicHDSPState *h);
void sonic_hdsp_dsp_reset_line(SonicHDSPState *h, unsigned n, bool held);

/* sgi-sonic-hdsp.c */
bool sonic_hdsp_retry(SonicHDSPState *h);
uint32_t sonic_hdsp_secondary_config_read(SonicHDSPState *h, uint32_t addr);
void sonic_hdsp_secondary_config_write(SonicHDSPState *h, uint32_t addr,
                                       uint32_t val);

#endif /* HW_MISC_SGI_SONIC_HDSP_H */
