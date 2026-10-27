# Origami QEMU

This is a fork of [QEMU](https://www.qemu.org/) that emulates Silicon Graphics
(SGI) machines: the Origin, Onyx2/3, Octane, O2, Fuel and Tezro systems. The
goal is to boot SGI's own PROM, and install & run IRIX with reasonable
performance.

This is the emulator half of [Origami](https://github.com/jamesbraid/origami),
which adds a CLI for machines, disks, consoles and IRIX network installs.

* [Website](https://origami.irix.fans)
* [Origami CLI](https://github.com/jamesbraid/origami)

## Machines

Each family's model covers its topologies, CPUs, memory sizes and boards.

- **Origin 200, Origin 2000, Onyx2** (IP27): one or two Origin 200 chassis, GIGAchannel, deskside and rack. Boots IRIX 6.5, Linux and OpenBSD.
  [Spec](docs/system/mips/sgi/ip27.rst).
- **Octane, Octane2** (IP30): Boots IRIX 6.5 with IMPACT or VPro graphics. 
  [Spec](docs/system/mips/sgi/ip30.rst).
- **Origin 300, Fuel** (IP35): Boots IRIX 6.5 with VPro graphics.
  [Spec](docs/system/mips/sgi/ip35.rst), [Bedrock](docs/system/mips/sgi/hardware/bedrock.rst).
- **Origin 350, Onyx 350, Tezro, Onyx4** (IP53): Boots IRIX 6.5 with VPro graphics.
  [Spec](docs/system/mips/sgi/ip35.rst).
- **O2** (IP32): WIP - boots IRIX 6.5.

Graphics boards are IMPACT (MGRAS), InfiniteReality (KONA), VPro (Odyssey)
and the PsiTech RAD4. The [graphics spec](docs/system/mips/sgi/hardware/graphics.rst) says
which machines take which.

There is a flexible configuration model to allow different configurations of
systems including graphics/non-graphics (e.g. Onyx is an Origin with graphics),
as well as realistic CPU/memory and add-in board configurations.

## Building

Only the big-endian 64-bit MIPS target is needed:

```sh
mkdir build && cd build
../configure --target-list=mips64-softmmu
make
```

## Running

A very minimal system can be launched directly from qemu:

```sh
build/qemu-system-mips64 -M origin200 -bios ip27prom.img -nographic
```

That should get you to the PROM's maintenance menu. 

The [Origami CLI](https://github.com/jamesbraid/origami) wraps qemu into an
easier to use front-end including networking and installation. 


## Contributing

Bug reports and pull requests are welcome on GitHub.

The [SGI testing guide](docs/devel/testing/sgi.rst) covers the qtests and
functional tests.

### The machine catalogue

Machines, boards and their populations are described as data in
[`hw/mips/sgi/models/`](hw/mips/sgi/models/). `scripts/sgi-model.py`
validates those files and regenerates the checked-in
`hw/mips/sgi/sgi-machines.json` and `hw/mips/sgi/sn-model.json`. After
configuring a build, install the generator's Python dependencies, then
regenerate and test:

```sh
build/pyvenv/bin/python3 python/scripts/mkvenv.py ensuregroup \
  --online pythondeps.toml sgi-model
build/pyvenv/bin/python3 scripts/sgi-model.py
build/pyvenv/bin/python3 -m unittest discover \
  -s scripts/tests -p 'test_sgi_model_*.py'
```

## License

My work on the `sgi-origami` branch is [BSD 3-Clause](LICENSE.origami).

QEMU as a whole is GPL version 2, and some files and bundled firmware have
their own licences ([summary](LICENSE)), which is unchanged by my new work.

None of this is headed upstream. Upstream QEMU's
[code provenance policy](docs/devel/code-provenance.rst) wouldn't take it
anyway: it's a vibe-coded melange of SGI nostalgia. Enjoy!

## Authors

James Braid (feeding the LLMs), on top of the original QEMU authors.
Standing on the shoulders of giants.
