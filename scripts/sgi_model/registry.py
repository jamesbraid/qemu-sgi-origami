# SPDX-License-Identifier: BSD-3-Clause

"""Supported product names and their authored model inputs."""

PRODUCT_MODELS = {
    "o2": "o2",
    "fuel": "fuel",
    "origin200": "origin200",
    "origin2000": "origin2000-rack",
    "origin300": "origin300",
    "origin3000": "origin3200",
    "origin350": "origin350",
    "onyx4": "onyx4",
    "onyx350": "onyx350",
    "tezro": "tezro",
    "onyx2": "onyx2-rack",
    "octane": "octane",
    "octane2": "octane2",
}
PRODUCT_NAMES = tuple(PRODUCT_MODELS)
IMPLEMENTED_TOPOLOGIES = (
    "o2", "o2plus",
    "fuel",
    "octane", "octane2", "origin200", "origin200-dual", "origin2000-rack", "origin300",
    "origin2000-deskside", "origin200-gigachannel-loopback",
    "origin300-two-chassis", "origin200-gigachannel", "onyx2-rack",
    "origin3200",
    "origin3200-two-ibricks",
    "origin3400",
    "origin3400-two-routers",
    "origin3400-two-partitions",
    "origin3800",
    "origin3800-high-bandwidth",
    "origin3800-64p",
    "origin3900-ip53",
    "origin3900-ip59",
    "origin3900-memory-node",
    "onyx3000",
    "onyx3400-ir",
    "onyx2-deskside",
    "origin300-numalink-pci-expansion",
    "origin300-pci-expansion",
    "origin350-ip53",
    "origin350-ip59",
    "onyx4",
    "onyx350-ip53",
    "onyx350-ip59",
    "tezro-one-processor",
    "tezro-ip53",
    "tezro-ip59",
    "tezro-rack-ip53",
    "tezro-rack-ip59",
)

# The compositions that declare variants, by file name, each compiling to
# several topologies; every other composition compiles to the topology of
# its own name.
VARIANT_COMPOSITIONS = {
    "origin350": ("origin350-ip53", "origin350-ip59"),
    "onyx350": ("onyx350-ip53", "onyx350-ip59"),
    "origin3900": ("origin3900-ip53", "origin3900-ip59"),
    "tezro": ("tezro-one-processor", "tezro-ip53", "tezro-ip59"),
    "tezro-rack": ("tezro-rack-ip53", "tezro-rack-ip59"),
}

# The authored compositions the implemented topologies compile from.
IMPLEMENTED_COMPOSITIONS = frozenset(
    name for name in IMPLEMENTED_TOPOLOGIES
    if not any(name in names for names in VARIANT_COMPOSITIONS.values())
) | frozenset(VARIANT_COMPOSITIONS)

# Authored reference compositions are useful for comparing product topology,
# but their unresolved hardware inputs keep them out of the runtime catalogue.
PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY = ("onyx350", "tezro")

REFERENCE_TOPOLOGIES = (
    "origin350-reference",
    "onyx350-base",
    "onyx350-observed-module",
    "tezro-reference",
    "tezro-rack-two-enclosure",
    "onyx4-g2",
    "onyx4-g2n",
)

# The IP53-family runtime profiles: topology name to the QEMU machine name
# and the emulation profile it must carry. The ip59 entries are the same
# products with the 1 GHz node board and processors.
IP53_RUNTIME_PROFILES = {
    "origin350-ip53": ("origin350", "ip53-origin350"),
    "origin350-ip59": ("origin350", "ip59-origin350"),
    "onyx4": ("onyx4", "ip59-onyx4"),
    "onyx350-ip53": ("onyx350", "ip53-onyx350"),
    "onyx350-ip59": ("onyx350", "ip59-onyx350"),
    "tezro-one-processor": ("tezro", "tezro-serial-compat"),
    "tezro-ip53": ("tezro", "tezro-serial-compat"),
    "tezro-ip59": ("tezro", "ip59-tezro"),
    "tezro-rack-ip53": ("tezro", "tezro-rack-serial-compat"),
    "tezro-rack-ip59": ("tezro", "ip59-tezro-rack"),
}
TEZRO_RUNTIME_PRODUCTS = frozenset(
    product for product, (machine, _) in IP53_RUNTIME_PROFILES.items()
    if machine == "tezro")


def ip53_runtime_profile(model: dict) -> tuple[str, str] | None:
    """The (machine, emulation profile) the IP53-family runtime profile of
    @model's topology must carry, or None for no such profile."""
    return IP53_RUNTIME_PROFILES.get(
        model.get("composition", {}).get("product"))


# What the guest's host id derives from, by QEMU machine name: the system
# serial text, a serial per module, the station address, or the O2's
# identity chip.
HOSTID_SOURCES = {
    "origin300": "text", "origin350": "text", "origin3000": "text",
    "onyx350": "text", "onyx4": "text", "tezro": "text",
    "origin2000": "module", "onyx2": "module",
    "octane": "address", "octane2": "address", "fuel": "address",
    "origin200": "address",
    "o2": "o2",
}

# The user-facing devices a launch may attach, by QEMU machine name.
# usb: an OHCI controller to put a keyboard and mouse on (read off each
# machine's device tree). tablet: a serial line a Wacom tablet can drive,
# the machines whose second IOC3 port a launch connects. audio: the
# device, whether it plays and captures, and the board product that fits
# it where the machine does not always have it.
DEVICES = {
    "fuel": {"usb": True, "tablet": True},
    "origin300": {"usb": True, "tablet": True},
    "origin3000": {"usb": True, "tablet": True},
    "origin200": {"usb": False, "tablet": True},
    "origin2000": {"usb": False, "tablet": True,
                   "audio": {"device": "sgi-rad1-audio", "output": True,
                             "input": False, "requires-product": "io6g"}},
    "onyx2": {"usb": False, "tablet": True,
              "audio": {"device": "sgi-rad1-audio", "output": True,
                        "input": False}},
    "octane": {"usb": False, "tablet": False,
               "audio": {"device": "sgi-rad1-octane", "output": True,
                         "input": True}},
    "octane2": {"usb": False, "tablet": False,
                "audio": {"device": "sgi-rad1-octane", "output": True,
                          "input": True}},
    "o2": {"usb": False, "tablet": False,
           "audio": {"device": "sgi-mace", "output": True, "input": True}},
    "origin350": {"usb": False, "tablet": False},
    "onyx350": {"usb": False, "tablet": False},
    "onyx4": {"usb": False, "tablet": False},
    "tezro": {"usb": False, "tablet": False},
}

_ICOUNT_SHIFTS = list(range(11))


def clock_record(machine_name: str) -> dict:
    """How a launch of @machine_name may choose the guest's time. The O2
    is accepted only at shift 3 (its identity chip and ec0 fail
    otherwise), so shift 3 is its default and only shift; counting may be
    off for the diagnostic that boots an installed disk without it. The
    Octane keeps the battery clock on the host's time unless asked. Every
    other machine's counted time runs on the virtual clock."""
    if machine_name == "o2":
        return {"default-icount-shift": 3, "icount-shifts": [3],
                "icount-off": True, "default-rtc": "vm",
                "rtc": ["host", "vm"], "vm-needs-icount": False}
    return {"icount-shifts": list(_ICOUNT_SHIFTS), "icount-off": True,
            "default-rtc": ("host" if machine_name in ("octane", "octane2")
                            else "vm"),
            "rtc": ["host", "vm"], "vm-needs-icount": False}
