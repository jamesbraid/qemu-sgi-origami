# SPDX-License-Identifier: BSD-3-Clause

"""Supported product names and their authored model inputs."""

PRODUCT_MODELS = {
    "fuel": "fuel",
    "origin200": "origin200",
    "origin2000": "origin2000-rack",
    "origin300": "origin300",
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
    "onyx2-deskside", "origin300-v12-direct", "origin300-vbrick",
    "origin300-pci-expansion-experiment",
)
