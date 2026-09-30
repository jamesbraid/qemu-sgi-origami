# SPDX-License-Identifier: BSD-3-Clause

"""Exceptions shared by the system-model resolver."""

class ResolutionError(ValueError):
    """A composition is incomplete, incompatible, or illegally populated."""
