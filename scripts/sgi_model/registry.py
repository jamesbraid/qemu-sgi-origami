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
    "onyx4-host": "onyx4-host-ip59",
    "onyx350": "onyx350-compat",
    "tezro": "tezro-tower-multi",
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
    "origin3400",
    "origin3900",
    "origin3900-ip59",
    "onyx3000",
    "onyx3400-ir",
    "onyx2-deskside",
    "origin300-numalink-pci-expansion",
    "origin300-pci-expansion",
    "origin350",
    "origin350-ip59",
    "onyx4-host-serial-compat",
    "onyx4-host-ip59",
    "onyx350-compat",
    "onyx350-ip59",
    "tezro-tower-one",
    "tezro-tower-multi",
    "tezro-rack",
    "tezro-tower-one-ip59",
    "tezro-tower-multi-ip59",
    "tezro-rack-ip59",
)

# Authored reference compositions are useful for comparing product topology,
# but their unresolved hardware inputs keep them out of the runtime catalogue.
PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY = ("onyx350", "tezro")

REFERENCE_TOPOLOGIES = (
    "origin350-reference",
    "onyx350-base",
    "onyx350-observed-module",
    "tezro-tower-four",
    "tezro-rack-two-enclosure",
    "onyx4-host-g2",
    "onyx4-host-g2n",
)

# The IP53-family runtime profiles: catalogue product id to the QEMU machine
# name and the emulation profile it must carry. The ip59 entries are the same
# boards with the 1 GHz generation's node board and processors.
IP53_RUNTIME_PROFILES = {
    "origin350": ("origin350", "ip53-origin350"),
    "origin350-ip59": ("origin350", "ip59-origin350"),
    "onyx4-host-serial-compat": (
        "onyx4-host", "ip53-onyx4-host-serial-compat"),
    "onyx4-host-ip59": ("onyx4-host", "ip59-onyx4-host"),
    "onyx350-compat": ("onyx350", "ip53-onyx350-compat"),
    "onyx350-ip59": ("onyx350", "ip59-onyx350"),
    "tezro-tower-one": ("tezro", "tezro-tower-one-serial-compat"),
    "tezro-tower-multi": ("tezro", "tezro-tower-multi-serial-compat"),
    "tezro-rack": ("tezro", "tezro-rack-serial-compat"),
    "tezro-tower-one-ip59": ("tezro", "ip59-tezro-tower-one"),
    "tezro-tower-multi-ip59": ("tezro", "ip59-tezro-tower-multi"),
    "tezro-rack-ip59": ("tezro", "ip59-tezro-rack"),
}
TEZRO_RUNTIME_PRODUCTS = frozenset(
    product for product, (machine, _) in IP53_RUNTIME_PROFILES.items()
    if machine == "tezro")
