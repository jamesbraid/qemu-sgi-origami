# Contributing to Origami QEMU

This repository contains Origami's experimental SGI emulator branch of QEMU.
Open issues and pull requests on GitHub against `sgi-origami`.

Describe the problem, how to reproduce it, and the QEMU revision you tested.
For emulation changes, identify the machine, device and guest version. Include
public references and tests for the behavior being changed. Keep proprietary
firmware, IRIX media, vendor source and snapshots out of commits and uploads.

GitHub runs builds and MIPS64 qtests for proposed changes. Accepted changes
retain contributor authorship. The maintainer closes the pull request with a
link to the resulting commit.

Origami is developed largely with AI assistance. Upstream QEMU has its own
contribution and code provenance policies in `docs/devel/code-provenance.rst`.
A change accepted here is not approval to submit it upstream. Report issues with
Origami's SGI behavior here. Follow upstream QEMU's reporting procedures for
independently reproduced upstream problems.
