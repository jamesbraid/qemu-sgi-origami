# Origami QEMU

This is a fork of [QEMU](https://www.qemu.org/) with experimental SGI
machine and device emulation. `sgi-origami` is its maintained development
branch. The [original QEMU README](README.rst) covers QEMU itself and its
build instructions.

The branch contains SGI machine descriptions, device models, register tests,
and guest boot tests. Many device behaviors remain approximate where public
hardware evidence is incomplete. A guest boot does not establish that a
model matches a physical machine. The [SGI testing guide](docs/devel/testing/sgi.rst)
describes the available checks.

## SGI model catalogue

The inputs and schema are in [`hw/mips/sgi/models/`](hw/mips/sgi/models/).
`scripts/sgi-model.py` validates them and regenerates the checked-in
`hw/mips/sgi/sgi-machines.json` and `hw/mips/sgi/sn-model.json` files.
From the source root, configure a MIPS64 build and install the generator's
Python dependencies:

```sh
mkdir build
cd build
../configure --target-list=mips64-softmmu --disable-docs
cd ..
build/pyvenv/bin/python3 python/scripts/mkvenv.py ensuregroup \
  --online pythondeps.toml sgi-model
```

Regenerate the files and run the catalogue tests with:

```sh
build/pyvenv/bin/python3 scripts/sgi-model.py
build/pyvenv/bin/python3 -m unittest discover \
  -s scripts/tests -p 'test_sgi_model_*.py'
```

SGI firmware and IRIX media are not included. To run an SGI guest,
supply the images it requires under their applicable terms. The
[contribution guide](.github/CONTRIBUTING.md) explains how to report issues
and propose changes to this branch.

QEMU as a whole is distributed under GPL version 2. Some files and bundled
firmware have separate licenses. See the [QEMU license summary](LICENSE).
The Origami-authored files in the [BSD 3-Clause file list](LICENSE.origami.paths)
carry the [separate notice](LICENSE.origami).

Origami uses AI assistance in development. This branch is maintained
separately from upstream QEMU, whose [code provenance policy](docs/devel/code-provenance.rst)
governs upstream contributions.
