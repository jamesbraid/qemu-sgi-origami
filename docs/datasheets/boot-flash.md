# Datasheet: Origin 200 and Origin 2000 boot flash

References rendered as citation identities name material outside this document
set. Those targets are not included or cleared for redistribution. Review and
runtime statements retain their recorded dates and scope.

## Status

Reviewed 2026-08-31. This records the existing public-evidence and emulator
contract. It clears no new physical-hardware claim.

The optional Origin 2000 backing interface was independently reviewed on
2026-09-09. Clearance covers the emulator storage interface, with no new
physical flash-part or timing claim.

## Implementation files

`hw/mips/sn0.c`, `hw/mips/sn0-origin2000.c`, `hw/block/pflash_cfi02.c`, and
`include/hw/block/flash.h`

**Sources used:**

- Linux mainline `arch/mips/include/asm/sn/sn0/addrs.h`, definitions for the
  mapped and unmapped PROM bases and the flash header and data windows.
- [The reviewed Origin 200 address map](address-map.md#the-boot-flash-region),
  which records the one-megabyte region, its two guest-visible views, and its
attachment to the Hub memory and directory path.
- QEMU `include/hw/block/flash.h` and `hw/block/pflash_cfi02.c`, which define
  `cfi.pflash02` and its AMD/Fujitsu command-set implementation.

## Role and address contract

The boot flash stores the firmware image. The processor reaches the same
one-megabyte region through the reset-vector view at node memory offset
`0x1fc0_0000` and the Hub special view at offset `0x1000_0000`. The machine
model owns those mappings and the Hub-side bus adaptation. `cfi.pflash02` owns
the flash command state.

The address-map datasheet remains authoritative for the two views and their
selection. This datasheet does not change an address, a mapping, or the Hub
memory and directory attachment.

## Emulator contract

The selected QEMU device is the `cfi.pflash02` QOM type implemented in
`hw/block/pflash_cfi02.c`. The machine configures one byte-wide mapping with
sixteen 64 KiB sectors, for a total of 1 MiB. It uses the AMD/Fujitsu command
set and supplies the command addresses expected by the Hub's bus adapter.

The machine also supplies autoselect identifiers `0x01` and `0xd5`. Those
values configure QEMU's command responses. They do not identify the physical
flash fitted to an IP29 System Board, and this model makes no physical-part
claim from them.

Origin 200 initializes the device without a backing drive and seeds the
firmware image after realization. The pflash device owns program, erase,
autoselect, query, reset, and migration state. The Hub wrapper owns the two
guest-visible views and converts Hub cycles to byte-wide device accesses.

## Origin 2000 backing interface

The four-node Origin 2000 model (citation: `docs/machines/origin2000.md`) gives each
physical node its own 1 MiB flash. Optional QEMU pflash drive indices 0 through
3 select physical node slots 1 through 4, independently of descriptor order.
Supply all four drives or none. Each backend must expose a 1 MiB capacity
through QEMU's block interface. That interface rounds raw file lengths up to
512-byte sectors. The launcher requires an exact 1 MiB file length.
QEMU rejects `-bios` when the drives are present, and loads each flash from
its backend without reseeding it. Without drives, the existing `-bios`
loader remains in use.

Program and erase writes use the existing `cfi.pflash02` block backend.
Reset clears command state and retains the programmed bytes. After QEMU
closes, the raw backend files contain the node images and can be copied or
reopened. The launcher backing option (citation: `docs/origami-maintainer-guide.md#keep-writable-origin-2000-node-proms`)
creates independent working files while preserving the supplied input image.

Identical-build migration uses the existing pflash RAM and command-state
migration. The supported handoff closes the source before the destination opens
the same backing files. Synchronizing unrelated destination backend files is
outside this contract. This interface changes storage persistence only and
makes no new claim about a physical flash part or command timing.

## Out of scope

- The physical flash manufacturer and part fitted to IP29 hardware.
- The fixed hardware decode that selects flash instead of node memory at the
  overlapping node offset. The address-map datasheet keeps that question open.
- Hub timing and ready-bit registers, which belong to the Hub memory and
  directory contract.

## qtest checklist

- Instantiate `cfi.pflash02` with the selected geometry and x8 access width.
- Read the same initial bytes through both Origin 200 flash views.
- Exercise autoselect, query, program, sector erase, reset, and migration
  through the QEMU pflash tests.
- Count one boot-flash QOM object per selected node at runtime. Object paths
  are not part of the model contract.
- Program each Origin 2000 node independently, reset, close, inspect the raw
  backing files, and reopen them without reseeding.
- Preserve physical node selection when descriptor records are reordered.
- Migrate an unfinished program command and autoselect mode using the same
  backing files after the source closes.
- Reject partial backing sets, a simultaneous `-bios`, and incorrect sizes.
