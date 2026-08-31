/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * What the Origin 200 model says this machine must be.
 *
 * Generated from docs/machines/origin200-dual.yaml and
 * docs/machines/klconfig-layout.yaml by tools/gen-klconfig --c-header.
 * Do not edit: regenerate. tools/check-model-header verifies that
 * regenerating produces exactly this file, so an edit here is caught.
 *
 * The point of generating it is that the emulator's machine and the firmware
 * it synthesises stop being two hand-written descriptions that can disagree.
 * The qtest that includes this reads both back out of a running machine and
 * checks them against the one model they came from.
 */

#ifndef TESTS_QTEST_SN0_MODEL_DUAL_H
#define TESTS_QTEST_SN0_MODEL_DUAL_H

typedef struct {
    uint64_t offset;        /* node offset in the memory space */
    unsigned width;         /* bytes, big-endian */
    uint64_t value;
    const char *field;      /* record.field, for the failure message */
} SN0ModelField;

typedef struct {
    const char *component;  /* resolved component instance identifier */
    const char *name;       /* canonical name from the component taxonomy */
    const char *qom_type;
} SN0ModelDevice;

typedef struct {
    uint64_t offset;
    unsigned width;
    uint64_t machine_value; /* what the machine is known to write instead */
    const char *field;
    const char *owner;
} SN0ModelDivergence;

typedef struct {
    unsigned nasid;
    uint64_t base;
    uint64_t size;
    uint64_t span;
    const SN0ModelField *fields;
    unsigned field_count;
} SN0ModelConfigGraph;

/* The memory size the fields below describe. */
#define SN0_MODEL_RAM_MB 128

/* Installed hardware identity and board-record namespaces. */
#define SN0_MODEL_CPU_PRID 0x926
#define SN0_MODEL_FPU_PRID 0x900
#define SN0_MODEL_HUB_NI_REVISION 0x3
#define SN0_MODEL_HUB_WIDGET_REVISION 0x1
#define SN0_MODEL_HUB_KLCONFIG_REVISION 0x3
#define SN0_MODEL_BRIDGE_REVISION 0x4
#define SN0_MODEL_IOC3_PCI_REVISION 0x1
#define SN0_MODEL_ISP1040_PCI_REVISION 0x5
#define SN0_MODEL_ISP1040_HARDWARE_REVISION 0x6
#define SN0_MODEL_CPU_CLOCK_MHZ 0xb4
#define SN0_MODEL_HUB_CLOCK_HZ 0x55d4a80
#define SN0_MODEL_L2_CACHE_MB 0x1
#define SN0_MODEL_NODE_BOARD_VERSION 0x3
#define SN0_MODEL_MIDPLANE_BOARD_VERSION 0x2
#define SN0_MODEL_BASE_IO_BOARD_VERSION 0x2

/* Every field the configuration graph must carry. */
static const SN0ModelField sn0_model_graph[] = {
    { 0x00004000, 8, 0xbeedbabeULL, "config_header.signature" },
    { 0x00004008, 4, 0x0ULL, "config_header.version" },
    { 0x0000400c, 4, 0x48ULL, "config_header.arena_offset" },
    { 0x00004010, 4, 0x18ULL, "config_header.console_offset" },
    { 0x00004014, 4, 0x4100ULL, "config_header.first_board_offset" },
    { 0x00004018, 8, 0x9200000008620178ULL, "console.uart_base" },
    { 0x00004020, 8, 0x9200000008022000ULL, "console.config_base" },
    { 0x00004028, 8, 0x9200000008600000ULL, "console.memory_base" },
    { 0x00004030, 2, 0x2580ULL, "console.baud" },
    { 0x00004034, 4, 0x0ULL, "console.type" },
    { 0x00004038, 2, 0x0ULL, "console.nasid" },
    { 0x0000403a, 1, 0x8ULL, "console.widget" },
    { 0x0000403b, 1, 0x2ULL, "console.pci_devices" },
    { 0x00004100, 4, 0x41c0ULL, "board.next" },
    { 0x00004104, 1, 0x1ULL, "board.record_kind" },
    { 0x00004105, 1, 0x11ULL, "board.board_type" },
    { 0x00004106, 1, 0x3ULL, "board.record_version" },
    { 0x00004117, 1, 0x4ULL, "board.component_count" },
    { 0x00004120, 2, 0x0ULL, "board.nasid" },
    { 0x00004124, 4, 0x4400ULL, "board[node].components[0]" },
    { 0x00004400, 1, 0x1ULL, "component.component_type" },
    { 0x00004401, 1, 0x1ULL, "component.record_version" },
    { 0x00004403, 1, 0xffULL, "component.revision" },
    { 0x00004402, 1, 0x1ULL, "component.flags" },
    { 0x0000441c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004418, 1, 0x0ULL, "component.physical_id" },
    { 0x00004420, 1, 0x0ULL, "component.widget" },
    { 0x00004422, 2, 0x0ULL, "component.nasid" },
    { 0x00004428, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004438, 2, 0x926ULL, "processor.identification" },
    { 0x0000443a, 2, 0x900ULL, "processor.fp_identification" },
    { 0x0000443c, 2, 0xb4ULL, "processor.clock_mhz" },
    { 0x0000443e, 2, 0x1ULL, "processor.cache_mb" },
    { 0x00004440, 2, 0x5aULL, "processor.cache_clock_mhz" },
    { 0x00004128, 4, 0x4480ULL, "board[node].components[1]" },
    { 0x00004480, 1, 0x1ULL, "component.component_type" },
    { 0x00004481, 1, 0x1ULL, "component.record_version" },
    { 0x00004483, 1, 0xffULL, "component.revision" },
    { 0x00004482, 1, 0x1ULL, "component.flags" },
    { 0x0000449c, 4, 0x1ULL, "component.virtual_id" },
    { 0x00004498, 1, 0x1ULL, "component.physical_id" },
    { 0x000044a0, 1, 0x0ULL, "component.widget" },
    { 0x000044a2, 2, 0x0ULL, "component.nasid" },
    { 0x000044a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000044b8, 2, 0x926ULL, "processor.identification" },
    { 0x000044ba, 2, 0x900ULL, "processor.fp_identification" },
    { 0x000044bc, 2, 0xb4ULL, "processor.clock_mhz" },
    { 0x000044be, 2, 0x1ULL, "processor.cache_mb" },
    { 0x000044c0, 2, 0x5aULL, "processor.cache_clock_mhz" },
    { 0x0000412c, 4, 0x4500ULL, "board[node].components[2]" },
    { 0x00004500, 1, 0x2ULL, "component.component_type" },
    { 0x00004501, 1, 0x1ULL, "component.record_version" },
    { 0x00004503, 1, 0x3ULL, "component.revision" },
    { 0x00004502, 1, 0x1ULL, "component.flags" },
    { 0x0000451c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004518, 1, 0x0ULL, "component.physical_id" },
    { 0x00004520, 1, 0x0ULL, "component.widget" },
    { 0x00004522, 2, 0x0ULL, "component.nasid" },
    { 0x00004528, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004558, 8, 0x55d4a80ULL, "hub.clock_hz" },
    { 0x0000453c, 8, 0xffff000000000000ULL, "hub.port" },
    { 0x00004130, 4, 0x4580ULL, "board[node].components[3]" },
    { 0x00004580, 1, 0x3ULL, "component.component_type" },
    { 0x00004581, 1, 0x1ULL, "component.record_version" },
    { 0x00004583, 1, 0xffULL, "component.revision" },
    { 0x00004582, 1, 0x1ULL, "component.flags" },
    { 0x0000459c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004598, 1, 0x0ULL, "component.physical_id" },
    { 0x000045a0, 1, 0x0ULL, "component.widget" },
    { 0x000045a2, 2, 0x0ULL, "component.nasid" },
    { 0x000045a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000045bc, 2, 0x40ULL, "memory_bank.bank_sizes[0]" },
    { 0x000045be, 2, 0x0ULL, "memory_bank.bank_sizes[1]" },
    { 0x000045c0, 2, 0x0ULL, "memory_bank.bank_sizes[2]" },
    { 0x000045c2, 2, 0x0ULL, "memory_bank.bank_sizes[3]" },
    { 0x000045c4, 2, 0x0ULL, "memory_bank.bank_sizes[4]" },
    { 0x000045c6, 2, 0x0ULL, "memory_bank.bank_sizes[5]" },
    { 0x000045c8, 2, 0x0ULL, "memory_bank.bank_sizes[6]" },
    { 0x000045ca, 2, 0x0ULL, "memory_bank.bank_sizes[7]" },
    { 0x000045ba, 2, 0x0ULL, "memory_bank.bank_mapping" },
    { 0x000045b8, 2, 0x40ULL, "memory_bank.total_mb" },
    { 0x000041c0, 4, 0x4280ULL, "board.next" },
    { 0x000041c4, 1, 0x1ULL, "board.record_kind" },
    { 0x000041c5, 1, 0x41ULL, "board.board_type" },
    { 0x000041c6, 1, 0x2ULL, "board.record_version" },
    { 0x000041d7, 1, 0x0ULL, "board.component_count" },
    { 0x000041e0, 2, 0x0ULL, "board.nasid" },
    { 0x000041e4, 4, 0x4600ULL, "board[midplane].components[0]" },
    { 0x00004600, 1, 0x1aULL, "component.component_type" },
    { 0x00004601, 1, 0x1ULL, "component.record_version" },
    { 0x00004603, 1, 0xffULL, "component.revision" },
    { 0x00004602, 1, 0x1ULL, "component.flags" },
    { 0x0000461c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004618, 1, 0x0ULL, "component.physical_id" },
    { 0x00004620, 1, 0x0ULL, "component.widget" },
    { 0x00004622, 2, 0x0ULL, "component.nasid" },
    { 0x00004628, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004280, 4, 0x0ULL, "board.next" },
    { 0x00004284, 1, 0x1ULL, "board.record_kind" },
    { 0x00004285, 1, 0x21ULL, "board.board_type" },
    { 0x00004286, 1, 0x2ULL, "board.record_version" },
    { 0x00004297, 1, 0x4ULL, "board.component_count" },
    { 0x000042a0, 2, 0x0ULL, "board.nasid" },
    { 0x000042a4, 4, 0x4680ULL, "board[base-io].components[0]" },
    { 0x00004680, 1, 0x5ULL, "component.component_type" },
    { 0x00004681, 1, 0x1ULL, "component.record_version" },
    { 0x00004683, 1, 0x4ULL, "component.revision" },
    { 0x00004682, 1, 0x1ULL, "component.flags" },
    { 0x0000469c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004698, 1, 0x8ULL, "component.physical_id" },
    { 0x000046a0, 1, 0x8ULL, "component.widget" },
    { 0x000046a2, 2, 0x0ULL, "component.nasid" },
    { 0x000046a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042a8, 4, 0x4700ULL, "board[base-io].components[1]" },
    { 0x00004700, 1, 0xbULL, "component.component_type" },
    { 0x00004701, 1, 0x1ULL, "component.record_version" },
    { 0x00004703, 1, 0x5ULL, "component.revision" },
    { 0x00004702, 1, 0x1ULL, "component.flags" },
    { 0x0000471c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004718, 1, 0x0ULL, "component.physical_id" },
    { 0x00004720, 1, 0x8ULL, "component.widget" },
    { 0x00004722, 2, 0x0ULL, "component.nasid" },
    { 0x00004728, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042ac, 4, 0x4780ULL, "board[base-io].components[2]" },
    { 0x00004780, 1, 0xbULL, "component.component_type" },
    { 0x00004781, 1, 0x1ULL, "component.record_version" },
    { 0x00004783, 1, 0x5ULL, "component.revision" },
    { 0x00004782, 1, 0x1ULL, "component.flags" },
    { 0x0000479c, 4, 0x1ULL, "component.virtual_id" },
    { 0x00004798, 1, 0x1ULL, "component.physical_id" },
    { 0x000047a0, 1, 0x8ULL, "component.widget" },
    { 0x000047a2, 2, 0x0ULL, "component.nasid" },
    { 0x000047a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042b0, 4, 0x4800ULL, "board[base-io].components[3]" },
    { 0x00004800, 1, 0x6ULL, "component.component_type" },
    { 0x00004801, 1, 0x1ULL, "component.record_version" },
    { 0x00004803, 1, 0x1ULL, "component.revision" },
    { 0x00004802, 1, 0x1ULL, "component.flags" },
    { 0x0000481c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004818, 1, 0x2ULL, "component.physical_id" },
    { 0x00004820, 1, 0x8ULL, "component.widget" },
    { 0x00004822, 2, 0x0ULL, "component.nasid" },
    { 0x00004828, 8, 0x0ULL, "component.firmware_back" },
};

/* Every field the configuration graph must carry. */
static const SN0ModelField sn0_model_graph_1[] = {
    { 0x00004000, 8, 0xbeedbabeULL, "config_header.signature" },
    { 0x00004008, 4, 0x0ULL, "config_header.version" },
    { 0x0000400c, 4, 0x48ULL, "config_header.arena_offset" },
    { 0x00004010, 4, 0x18ULL, "config_header.console_offset" },
    { 0x00004014, 4, 0x4100ULL, "config_header.first_board_offset" },
    { 0x00004018, 8, 0x9200000008620178ULL, "console.uart_base" },
    { 0x00004020, 8, 0x9200000008022000ULL, "console.config_base" },
    { 0x00004028, 8, 0x9200000008600000ULL, "console.memory_base" },
    { 0x00004030, 2, 0x2580ULL, "console.baud" },
    { 0x00004034, 4, 0x0ULL, "console.type" },
    { 0x00004038, 2, 0x1ULL, "console.nasid" },
    { 0x0000403a, 1, 0x8ULL, "console.widget" },
    { 0x0000403b, 1, 0x2ULL, "console.pci_devices" },
    { 0x00004100, 4, 0x41c0ULL, "board.next" },
    { 0x00004104, 1, 0x1ULL, "board.record_kind" },
    { 0x00004105, 1, 0x11ULL, "board.board_type" },
    { 0x00004106, 1, 0x3ULL, "board.record_version" },
    { 0x00004117, 1, 0x4ULL, "board.component_count" },
    { 0x00004120, 2, 0x1ULL, "board.nasid" },
    { 0x00004124, 4, 0x4400ULL, "board[node].components[0]" },
    { 0x00004400, 1, 0x1ULL, "component.component_type" },
    { 0x00004401, 1, 0x1ULL, "component.record_version" },
    { 0x00004403, 1, 0xffULL, "component.revision" },
    { 0x00004402, 1, 0x1ULL, "component.flags" },
    { 0x0000441c, 4, 0x2ULL, "component.virtual_id" },
    { 0x00004418, 1, 0x0ULL, "component.physical_id" },
    { 0x00004420, 1, 0x0ULL, "component.widget" },
    { 0x00004422, 2, 0x1ULL, "component.nasid" },
    { 0x00004428, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004438, 2, 0x926ULL, "processor.identification" },
    { 0x0000443a, 2, 0x900ULL, "processor.fp_identification" },
    { 0x0000443c, 2, 0xb4ULL, "processor.clock_mhz" },
    { 0x0000443e, 2, 0x1ULL, "processor.cache_mb" },
    { 0x00004440, 2, 0x5aULL, "processor.cache_clock_mhz" },
    { 0x00004128, 4, 0x4480ULL, "board[node].components[1]" },
    { 0x00004480, 1, 0x1ULL, "component.component_type" },
    { 0x00004481, 1, 0x1ULL, "component.record_version" },
    { 0x00004483, 1, 0xffULL, "component.revision" },
    { 0x00004482, 1, 0x1ULL, "component.flags" },
    { 0x0000449c, 4, 0x3ULL, "component.virtual_id" },
    { 0x00004498, 1, 0x1ULL, "component.physical_id" },
    { 0x000044a0, 1, 0x0ULL, "component.widget" },
    { 0x000044a2, 2, 0x1ULL, "component.nasid" },
    { 0x000044a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000044b8, 2, 0x926ULL, "processor.identification" },
    { 0x000044ba, 2, 0x900ULL, "processor.fp_identification" },
    { 0x000044bc, 2, 0xb4ULL, "processor.clock_mhz" },
    { 0x000044be, 2, 0x1ULL, "processor.cache_mb" },
    { 0x000044c0, 2, 0x5aULL, "processor.cache_clock_mhz" },
    { 0x0000412c, 4, 0x4500ULL, "board[node].components[2]" },
    { 0x00004500, 1, 0x2ULL, "component.component_type" },
    { 0x00004501, 1, 0x1ULL, "component.record_version" },
    { 0x00004503, 1, 0x3ULL, "component.revision" },
    { 0x00004502, 1, 0x1ULL, "component.flags" },
    { 0x0000451c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004518, 1, 0x1ULL, "component.physical_id" },
    { 0x00004520, 1, 0x0ULL, "component.widget" },
    { 0x00004522, 2, 0x1ULL, "component.nasid" },
    { 0x00004528, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004558, 8, 0x55d4a80ULL, "hub.clock_hz" },
    { 0x0000453c, 8, 0xffff000000000000ULL, "hub.port" },
    { 0x00004130, 4, 0x4580ULL, "board[node].components[3]" },
    { 0x00004580, 1, 0x3ULL, "component.component_type" },
    { 0x00004581, 1, 0x1ULL, "component.record_version" },
    { 0x00004583, 1, 0xffULL, "component.revision" },
    { 0x00004582, 1, 0x1ULL, "component.flags" },
    { 0x0000459c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004598, 1, 0x0ULL, "component.physical_id" },
    { 0x000045a0, 1, 0x0ULL, "component.widget" },
    { 0x000045a2, 2, 0x1ULL, "component.nasid" },
    { 0x000045a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000045bc, 2, 0x40ULL, "memory_bank.bank_sizes[0]" },
    { 0x000045be, 2, 0x0ULL, "memory_bank.bank_sizes[1]" },
    { 0x000045c0, 2, 0x0ULL, "memory_bank.bank_sizes[2]" },
    { 0x000045c2, 2, 0x0ULL, "memory_bank.bank_sizes[3]" },
    { 0x000045c4, 2, 0x0ULL, "memory_bank.bank_sizes[4]" },
    { 0x000045c6, 2, 0x0ULL, "memory_bank.bank_sizes[5]" },
    { 0x000045c8, 2, 0x0ULL, "memory_bank.bank_sizes[6]" },
    { 0x000045ca, 2, 0x0ULL, "memory_bank.bank_sizes[7]" },
    { 0x000045ba, 2, 0x0ULL, "memory_bank.bank_mapping" },
    { 0x000045b8, 2, 0x40ULL, "memory_bank.total_mb" },
    { 0x000041c0, 4, 0x4280ULL, "board.next" },
    { 0x000041c4, 1, 0x1ULL, "board.record_kind" },
    { 0x000041c5, 1, 0x41ULL, "board.board_type" },
    { 0x000041c6, 1, 0x2ULL, "board.record_version" },
    { 0x000041d7, 1, 0x0ULL, "board.component_count" },
    { 0x000041e0, 2, 0x1ULL, "board.nasid" },
    { 0x000041e4, 4, 0x4600ULL, "board[midplane].components[0]" },
    { 0x00004600, 1, 0x1aULL, "component.component_type" },
    { 0x00004601, 1, 0x1ULL, "component.record_version" },
    { 0x00004603, 1, 0xffULL, "component.revision" },
    { 0x00004602, 1, 0x1ULL, "component.flags" },
    { 0x0000461c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004618, 1, 0x0ULL, "component.physical_id" },
    { 0x00004620, 1, 0x0ULL, "component.widget" },
    { 0x00004622, 2, 0x1ULL, "component.nasid" },
    { 0x00004628, 8, 0x0ULL, "component.firmware_back" },
    { 0x00004280, 4, 0x0ULL, "board.next" },
    { 0x00004284, 1, 0x1ULL, "board.record_kind" },
    { 0x00004285, 1, 0x21ULL, "board.board_type" },
    { 0x00004286, 1, 0x2ULL, "board.record_version" },
    { 0x00004297, 1, 0x4ULL, "board.component_count" },
    { 0x000042a0, 2, 0x1ULL, "board.nasid" },
    { 0x000042a4, 4, 0x4680ULL, "board[base-io].components[0]" },
    { 0x00004680, 1, 0x5ULL, "component.component_type" },
    { 0x00004681, 1, 0x1ULL, "component.record_version" },
    { 0x00004683, 1, 0x4ULL, "component.revision" },
    { 0x00004682, 1, 0x1ULL, "component.flags" },
    { 0x0000469c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004698, 1, 0x8ULL, "component.physical_id" },
    { 0x000046a0, 1, 0x8ULL, "component.widget" },
    { 0x000046a2, 2, 0x1ULL, "component.nasid" },
    { 0x000046a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042a8, 4, 0x4700ULL, "board[base-io].components[1]" },
    { 0x00004700, 1, 0xbULL, "component.component_type" },
    { 0x00004701, 1, 0x1ULL, "component.record_version" },
    { 0x00004703, 1, 0x5ULL, "component.revision" },
    { 0x00004702, 1, 0x1ULL, "component.flags" },
    { 0x0000471c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004718, 1, 0x0ULL, "component.physical_id" },
    { 0x00004720, 1, 0x8ULL, "component.widget" },
    { 0x00004722, 2, 0x1ULL, "component.nasid" },
    { 0x00004728, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042ac, 4, 0x4780ULL, "board[base-io].components[2]" },
    { 0x00004780, 1, 0xbULL, "component.component_type" },
    { 0x00004781, 1, 0x1ULL, "component.record_version" },
    { 0x00004783, 1, 0x5ULL, "component.revision" },
    { 0x00004782, 1, 0x1ULL, "component.flags" },
    { 0x0000479c, 4, 0x1ULL, "component.virtual_id" },
    { 0x00004798, 1, 0x1ULL, "component.physical_id" },
    { 0x000047a0, 1, 0x8ULL, "component.widget" },
    { 0x000047a2, 2, 0x1ULL, "component.nasid" },
    { 0x000047a8, 8, 0x0ULL, "component.firmware_back" },
    { 0x000042b0, 4, 0x4800ULL, "board[base-io].components[3]" },
    { 0x00004800, 1, 0x6ULL, "component.component_type" },
    { 0x00004801, 1, 0x1ULL, "component.record_version" },
    { 0x00004803, 1, 0x1ULL, "component.revision" },
    { 0x00004802, 1, 0x1ULL, "component.flags" },
    { 0x0000481c, 4, 0x0ULL, "component.virtual_id" },
    { 0x00004818, 1, 0x2ULL, "component.physical_id" },
    { 0x00004820, 1, 0x8ULL, "component.widget" },
    { 0x00004822, 2, 0x1ULL, "component.nasid" },
    { 0x00004828, 8, 0x0ULL, "component.firmware_back" },
};

/* Device types and counts selected by this profile. */
static const SN0ModelDevice sn0_model_devices[] = {
    {
        "node0-cpu1", "R10000",
        "R10000-mips64-cpu",
    },
    {
        "node1-cpu1", "R10000",
        "R10000-mips64-cpu",
    },
    {
        "node0-cpu0", "R10000",
        "R10000-mips64-cpu",
    },
    {
        "node0-hub", "Hub",
        "sgi-hub",
    },
    {
        "node0-bridge", "Bridge",
        "sgi-bridge",
    },
    {
        "node0-ioc3", "IOC3",
        "sgi-ioc3",
    },
    {
        "node0-ioc3-eth", "IOC3 ethernet",
        "sgi-ioc3-eth",
    },
    {
        "node0-enet-phy", "DP83840A",
        "dp83840",
    },
    {
        "node0-isp1040", "QLogic ISP1040",
        "qlogic-isp1040",
    },
    {
        "node0-isp1040-1", "QLogic ISP1040",
        "qlogic-isp1040",
    },
    {
        "node0-boot-flash", "boot flash",
        "cfi.pflash02",
    },
    {
        "node0-rtc", "MK48T35-class Timekeeper",
        "m48t35",
    },
    {
        "node0-pcf8584", "PCF8584",
        "pcf8584",
    },
    {
        "node0-sgi-elsc", "ELSC",
        "sgi-elsc",
    },
    {
        "node1-cpu0", "R10000",
        "R10000-mips64-cpu",
    },
    {
        "node1-hub", "Hub",
        "sgi-hub",
    },
    {
        "node1-bridge", "Bridge",
        "sgi-bridge",
    },
    {
        "node1-ioc3", "IOC3",
        "sgi-ioc3",
    },
    {
        "node1-ioc3-eth", "IOC3 ethernet",
        "sgi-ioc3-eth",
    },
    {
        "node1-enet-phy", "DP83840A",
        "dp83840",
    },
    {
        "node1-isp1040", "QLogic ISP1040",
        "qlogic-isp1040",
    },
    {
        "node1-isp1040-1", "QLogic ISP1040",
        "qlogic-isp1040",
    },
    {
        "node1-boot-flash", "boot flash",
        "cfi.pflash02",
    },
    {
        "node1-rtc", "MK48T35-class Timekeeper",
        "m48t35",
    },
    {
        "node1-pcf8584", "PCF8584",
        "pcf8584",
    },
    {
        "node1-sgi-elsc", "ELSC",
        "sgi-elsc",
    },
};

#define SN0_MODEL_GRAPH_BASE 0x4000
#define SN0_MODEL_GRAPH_SPAN 0x830
#define SN0_MODEL_GRAPH_SIZE 0xc000

#define SN0_MODEL_GRAPH_1_BASE 0x4000
#define SN0_MODEL_GRAPH_1_SPAN 0x830
#define SN0_MODEL_GRAPH_1_SIZE 0xc000

#define SN0_MODEL_CONFIG_GRAPH_COUNT 2
static const SN0ModelConfigGraph sn0_model_config_graphs[] = {
    {
        0,
        SN0_MODEL_GRAPH_BASE,
        SN0_MODEL_GRAPH_SIZE,
        SN0_MODEL_GRAPH_SPAN,
        sn0_model_graph,
        sizeof(sn0_model_graph) / sizeof(sn0_model_graph[0]),
    },
    {
        1,
        SN0_MODEL_GRAPH_1_BASE,
        SN0_MODEL_GRAPH_1_SIZE,
        SN0_MODEL_GRAPH_1_SPAN,
        sn0_model_graph_1,
        sizeof(sn0_model_graph_1) / sizeof(sn0_model_graph_1[0]),
    },
};

/*
 * Where the machine is known to write something the model does not.
 * Sentinel-terminated: an empty list is the goal, and must still build.
 */
static const SN0ModelDivergence sn0_model_divergences[] = {
    { 0, 0, 0, NULL, NULL },
};

#define SN0_MODEL_MACHINE "origin200"
#define SN0_MODEL_MACHINE_TYPE "origin200-machine"

#endif /* TESTS_QTEST_SN0_MODEL_DUAL_H */
