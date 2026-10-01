/* SPDX-License-Identifier: BSD-3-Clause */
/* Origin 300 L1 controller and node-side UART state. */
#ifndef HW_MISC_SGI_L1_H
#define HW_MISC_SGI_L1_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"

#define TYPE_SGI_L1 "sgi-l1"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1State, SGI_L1)
#define TYPE_SGI_L1_UART "sgi-l1-uart"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1UARTState, SGI_L1_UART)
#define SGI_L1_SIZE 0x40
/* 256 payload bytes plus the two check bytes: the firmware's receive cap. */
#define SGI_L1_FRAME_SIZE 258
#define SGI_L1_QUEUE_SIZE 4096
#define SGI_L1_LOG_ENTRIES 128
#define SGI_L1_LOG_TEXT 48

typedef struct SGIL1ProductProfile SGIL1ProductProfile;
typedef struct SGIL1QueryRouter SGIL1QueryRouter;

typedef struct SGIL1QueryOwner {
    SGIL1State *controller;
    uint64_t identity;
} SGIL1QueryOwner;

typedef struct SGIL1QueryAssociation {
    uint32_t from;
    uint32_t to;
    bool reachable;
} SGIL1QueryAssociation;

typedef struct SGIL1QueryAttachment {
    uint32_t issuer;
    uint32_t owner;
} SGIL1QueryAttachment;

struct SGIL1QueryRouter {
    SGIL1QueryOwner *owners;
    unsigned owner_count;
    SGIL1QueryAssociation *associations;
    unsigned association_count;
    SGIL1QueryAttachment *attachments;
    unsigned attachment_count;
    bool active;
};

/* The synchronous return path is private to this request. */
typedef struct SGIL1QueryRequest {
    SGIL1State *issuer;
    SGIL1State *recipient;
    uint32_t destination;
    const uint8_t *payload;
    unsigned length;
    uint8_t channel;
} SGIL1QueryRequest;

void sgi_l1_query_router_init(SGIL1QueryRouter *router,
                             SGIL1QueryOwner *owners, unsigned owner_count,
                             SGIL1QueryAssociation *associations,
                             unsigned association_count, Error **errp);
bool sgi_l1_query_router_set_attached(SGIL1QueryRouter *router,
                                     SGIL1QueryAttachment *attachments,
                                     unsigned count, Error **errp);
void sgi_l1_query_router_destroy(SGIL1QueryRouter *router);
void sgi_l1_query_route(SGIL1QueryRouter *router,
                       const SGIL1QueryRequest *request);

/* Explicit functional experiment; no physical controller bus is modeled. */
typedef enum SGIL1BICStartup {
    SGI_L1_BIC_STARTUP_NONE,
    SGI_L1_BIC_STARTUP_EXPERIMENT,
} SGIL1BICStartup;

typedef bool (*SGIL1BICOperation)(Object *receiver, unsigned position,
                                uint8_t bus, uint8_t address,
                                uint8_t command, uint8_t value);

typedef struct SGIL1BICEndpoint {
    Object *receiver;
    uint64_t identity;
} SGIL1BICEndpoint;

bool sgi_l1_bind_bic_startup(SGIL1State *s, SGIL1BICStartup selection,
                           const SGIL1BICEndpoint endpoints[2],
                           SGIL1BICOperation operation, Error **errp);

typedef struct SGIL1EEPROM {
    uint32_t length;
    uint8_t *data;
    bool invalid;
} SGIL1EEPROM;

typedef struct SGIL1SelectedEEPROM {
    bool present;
    uint8_t kind;
    uint32_t capacity;
    uint8_t *image;
    uint32_t start[5];
    uint32_t length[5];
    uint8_t status[5];
} SGIL1SelectedEEPROM;

/* Qtest-only inputs for the selected image's software status builder. */
typedef struct SGIL1USBTestState {
    bool set;
    uint8_t config;
    uint8_t ep0;
    uint8_t out1;
    uint8_t in2;
} SGIL1USBTestState;

/* The controller: what the L1 keeps on its own side of the link. */
struct SGIL1State {
    DeviceState parent_obj;
    SGIL1UARTState *uart;
    /*
     * The other chassis's controller across the RS-422 pair in a two-chassis
     * Origin 300's NUMAlink cable, or NULL. The link carries no wire format:
     * a forwarded request is an in-process call. It works only while both
     * controllers' network mode is 422.
     */
    struct SGIL1State *peer;
    bool network_422;
    SGIL1QueryRouter *query_router;
    uint32_t bic_startup;
    SGIL1BICEndpoint bic_endpoints[2];
    SGIL1BICOperation bic_operation;
    bool bic_operation_active;
    bool bic_startup_needed;
    uint64_t query_owner_identity;
    /* While answering a peer's forwarded request, where its replies go. */
    struct SGIL1State *reply_to;
    CharFrontend chr;
    qemu_irq node_power_out;
    qemu_irq node_reset_out;
    qemu_irq soft_reset_out;
    qemu_irq nmi_out;
    QEMUBH *power_up_bh;
    int32_t revision_major;
    int32_t revision_minor;
    int32_t revision_patch;
    bool auto_power;
    bool node_power;
    bool power_up_pending;
    bool console_mode;
    bool escaped;
    bool warned_off;
    bool promreq_debug;
    bool power_down_pending;
    uint32_t debug_switches;
    uint32_t partition;
    uint8_t two_bit_setting;
    uint8_t last_reason[2];
    char *system_serial;
    char *fuel_mac_eeprom;
    uint8_t fuel_mac[6];
    bool fuel_mac_valid;
    char *brick_serial;
    char *brick_type;
    uint32_t brick_type_code;
    const SGIL1ProductProfile *profile;
    uint32_t product_identity;
    uint32_t processors;
    GString *capture;
    /* Running a command the cabled peer relayed with ctc; not migrated. */
    bool serving_peer;
    uint8_t command[128];
    uint32_t command_len;
    char **chassis_files;
    char **board_files;
    char **spd_files;
    char **board_record_files;
    char **spd_record_files;
    uint32_t chassis_count;
    uint32_t board_count;
    uint32_t spd_count;
    uint32_t board_record_count;
    uint32_t spd_record_count;
    SGIL1EEPROM records[48];
    SGIL1SelectedEEPROM selected[6];
    uint32_t rack;
    uint32_t bay;
    uint32_t pending_rack;
    uint32_t pending_bay;
    uint32_t node_position;
    uint8_t port_field;
    uint8_t event_subchannel;
    bool events_enabled;
    int64_t clock_offset;
    int64_t log_time[SGI_L1_LOG_ENTRIES];
    uint8_t log_text[SGI_L1_LOG_ENTRIES][SGI_L1_LOG_TEXT];
    uint32_t log_head;
    uint32_t log_len;
    uint8_t frame[SGI_L1_FRAME_SIZE];
    uint32_t frame_len;
    bool in_frame;
    bool escape;
    bool overflow;
    uint8_t display[2][128];
    SGIL1USBTestState usb_test_state;
    char usb_test_result[16];
    char usb_test_descriptor_result[74];
};

bool sgi_l1_board_mac(DeviceState *dev, uint8_t mac[6]);

/* The node-side junk-bus UART window through which the Bedrock reaches it. */
struct SGIL1UARTState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    SGIL1State *controller;
    bool thre_pending;
    uint8_t lcr;
    uint8_t ier;
    uint8_t mcr;
    uint8_t scratch;
    uint8_t divisor[2];
    uint8_t queue[SGI_L1_QUEUE_SIZE];
    uint32_t queue_head;
    uint32_t queue_len;
};

#endif
