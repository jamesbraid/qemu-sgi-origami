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
    "onyx4-host": "onyx4-host",
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
    "origin3200-r16000",
    "origin3200-r14000-600mhz",
    "origin3400",
    "origin3400-r16000",
    "origin3400-r14000-600mhz",
    "origin3900",
    "origin3900-memory-node",
    "onyx3000",
    "onyx3000-r16000",
    "onyx3000-r14000-600mhz",
    "onyx3400-ir",
    "onyx2-deskside",
    "origin300-numalink-pci-expansion",
    "origin300-pci-expansion",
    "origin350",
    "onyx4-host",
    "onyx350",
    "tezro",
    "tezro-rack",
)

# Authored reference compositions are useful for comparing product topology,
# but their unresolved hardware inputs keep them out of the runtime catalogue.
PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY = ("onyx350", "tezro")

REFERENCE_TOPOLOGIES = (
    "origin350-reference",
    "onyx350-base",
    "onyx350-observed-module",
    "tezro-reference",
    "tezro-rack-two-enclosure",
    "onyx4-host-g2",
    "onyx4-host-g2n",
)

# The IP53-family runtime profiles: catalogue product id and board to the
# QEMU machine name and the emulation profile it must carry. The ip59
# entries are the same products with the 1 GHz node board and processors.
IP53_RUNTIME_PROFILES = {
    ("origin350", "ip53"): ("origin350", "ip53-origin350"),
    ("origin350", "ip59"): ("origin350", "ip59-origin350"),
    ("onyx4-host", "ip59"): ("onyx4-host", "ip59-onyx4-host"),
    ("onyx350", "ip53"): ("onyx350", "ip53-onyx350"),
    ("onyx350", "ip59"): ("onyx350", "ip59-onyx350"),
    ("tezro", "ip53"): ("tezro", "tezro-serial-compat"),
    ("tezro", "ip59"): ("tezro", "ip59-tezro"),
    ("tezro-rack", "ip53"): ("tezro", "tezro-rack-serial-compat"),
    ("tezro-rack", "ip59"): ("tezro", "ip59-tezro-rack"),
}
TEZRO_RUNTIME_PRODUCTS = frozenset(
    product for (product, _), (machine, _) in IP53_RUNTIME_PROFILES.items()
    if machine == "tezro")


def ip53_runtime_profile(model: dict) -> tuple[str, str] | None:
    """The (machine, emulation profile) the IP53-family runtime profile of
    @model's product and board must carry, or None for no such profile."""
    composition = model.get("composition", {})
    return IP53_RUNTIME_PROFILES.get(
        (composition.get("product"), composition.get("board", "ip53")))
