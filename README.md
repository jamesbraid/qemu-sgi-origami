# Origami QEMU

A fork of [QEMU](https://www.qemu.org/) that emulates Silicon Graphics (SGI)
MIPS machines: the Origin, Onyx, Octane, Fuel and Tezro lines. The aim is to
boot SGI's own PROM, install IRIX from the original media, and end up with
something that feels like the real machine.

This is the emulator half of [Origami](https://github.com/jamesbraid/origami),
which adds a CLI for machines, disks, consoles and IRIX network installs.

* [Website](https://origami.irix.fans) ·
* [Origami CLI](https://github.com/jamesbraid/origami)

## Machines

Each family's spec covers its topologies, CPUs, memory sizes and boards.

- **Origin 200, Origin 2000, Onyx2** (IP27):
  one or two Origin 200 chassis, GIGAchannel, deskside and rack modules with
  up to eight R10000 CPUs. IRIX 6.5, Linux and OpenBSD.
  [Spec](docs/specs/sgi-sn.rst), [Onyx2](docs/specs/sgi-onyx2.rst).
- **Octane, Octane2** (IP30): IRIX 6.5 with IMPACT or VPro graphics.
  [Spec](docs/specs/sgi-ip30.rst).
- **Origin 300, Fuel** (IP35): the Bedrock generation. IRIX 6.5.
  [Spec](docs/specs/sgi-sn.rst), [Bedrock](docs/specs/sgi-bedrock.rst).
- **Origin 350, Onyx 350, Tezro, Onyx4** (IP53): IRIX 6.5 on an
  experimental R16000, with optional VPro graphics on some.
  [Spec](docs/specs/sgi-ip53.rst).
- **O2** (IP32): IRIX 6.5.

Graphics boards are IMPACT (MGRAS), InfiniteReality (KONA), VPro (Odyssey)
and the PsiTech RAD4. The [graphics spec](docs/specs/sgi-graphics.rst) says
which machines take which.

Some combinations boot to a desktop. Others are an opportunity to stare at a
serial console.

## Building

Only the big-endian 64-bit MIPS target is needed:

```sh
mkdir build && cd build
../configure --target-list=mips64-softmmu
make
```

## Running

No SGI firmware or IRIX media is included. Bring the stock PROM image for
your machine, and an IRIX disk or install media.

```sh
build/qemu-system-mips64 -M origin200 -bios ip27prom.img -nographic
```

That gets you to the PROM's maintenance menu. `qemu-sgi-machine-init`
creates a machine's flash and NVRAM files
([manual](docs/tools/qemu-sgi-machine-init.rst)), and the QMP command
`query-sgi-machines` lists every machine, topology and graphics slot. The
[Origami CLI](https://github.com/jamesbraid/origami) does all of this for
you.

The [SGI testing guide](docs/devel/testing/sgi.rst) covers the qtests and
functional tests.

## The machine catalogue

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

## Contributing

Bug reports and pull requests are welcome on GitHub.

None of this is headed upstream. Upstream QEMU's
[code provenance policy](docs/devel/code-provenance.rst) wouldn't take it
anyway: it's a vibe-coded melange of SGI nostalgia. Enjoy!

## License

QEMU as a whole is GPL version 2, and some files and bundled firmware have
their own licences ([summary](LICENSE)).

My work on the `sgi-origami` branch is [BSD 3-Clause](LICENSE.origami): the
files I wrote, listed in [`LICENSE.origami.paths`](LICENSE.origami.paths),
and my additions to QEMU's existing files. QEMU's own code keeps its
licences.

For QEMU itself, read the [upstream README](README.rst).

## Authors

James Braid (feeding the LLMs), on top of the original QEMU authors.
Standing on the shoulders of giants.
