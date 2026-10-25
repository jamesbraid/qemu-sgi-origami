# SPDX-License-Identifier: BSD-3-Clause

"""Supported product names and their authored model inputs."""

PRODUCT_MODELS = {
    "fuel": "fuel",
    "origin200": "origin200",
    "origin2000": "origin2000-rack",
    "origin300": "origin300",
    "origin350": "origin350",
    "onyx4-host": "onyx4-host-serial-compat",
    "onyx350": "onyx350-compat",
    "tezro": "tezro-deskside-700mhz-4mib-serial-trial",
    "onyx2": "onyx2-rack",
    "octane": "octane",
    "octane2": "octane2",
}
PRODUCT_NAMES = tuple(PRODUCT_MODELS)
IMPLEMENTED_TOPOLOGIES = (
    "fuel",
    "octane", "octane2", "origin200", "origin200-dual", "origin2000-rack", "origin300",
    "origin2000-deskside", "origin200-gigachannel-loopback",
    "origin300-two-chassis", "origin200-gigachannel", "onyx2-rack",
    "onyx2-deskside",
    "origin300-pci-expansion-experiment",
    "origin300-numalink-pci-expansion-experiment",
    "origin300-pci-expansion-configuration-storage-experiment",
    "origin300-pci-expansion-unconnected-uart-experiment",
    "origin300-pci-expansion-pending-register-experiment",
    "origin350",
    "onyx4-host-serial-compat",
    "onyx350-compat",
    "tezro-deskside-700mhz-4mib-serial-trial",
    "tezro-tower-one",
    "tezro-tower-multi",
    "tezro-rack",
)

# Authored reference compositions are useful for comparing product topology,
# but their unresolved hardware inputs keep them out of the runtime catalogue.
PRODUCTS_REQUIRING_EXPLICIT_TOPOLOGY = ("onyx350", "tezro")

REFERENCE_TOPOLOGIES = (
    "origin350-reference",
    "onyx350-base",
    "onyx350-observed-module",
    "tezro-tower-four",
    "tezro-deskside-compat",
    "tezro-rack-two-enclosure",
    "onyx4-host-g2",
    "onyx4-host-g2n",
)
