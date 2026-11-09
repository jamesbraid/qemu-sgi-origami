#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Fingerprint every SGI catalogue offering, and compare two catalogues.

A fingerprint has two hashes.  The first is a SHA-256 over the machine's
QOM tree, walked from /machine under -S -accel qtest after the offering
is launched from catalogue data alone (machine-options, one zeroed file
per storage item, the CPU model, -smp, the default memory).  The second
is a SHA-256 over the offering's hardware-bearing catalogue fields and
its topology record, with every naming field removed.  Together they say
whether two catalogues build the same machine for an offering even
though the offering was renamed.

    sgi-offering-fingerprints.py capture QEMU OUT.json
    sgi-offering-fingerprints.py compare OLD.json NEW.json

"capture" works on both catalogue layouts: offerings of version 3 carry
an ``id``; earlier ones are keyed by machine options, -smp and population.
"compare" maps each old key to its new id by the renaming rule and
requires both hashes to match.
"""

import argparse
import concurrent.futures
import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

# Properties of /machine that carry the selection, which is what is renamed.
SELECTION = {"topology", "board", "processor", "population", "type"}

# Offering fields that describe the hardware.  Anything else (names, ids,
# launcher inputs, settings, clocks, serial lines) is not hashed.
OFFERING_FIELDS = (
    "product", "nodes", "cpus-per-node", "dimms-per-node", "smp",
    "memory-per-node-mib", "processor", "firmware", "init-inputs",
    "storage", "scsi-adapters", "networks", "default-cpu-model",
    "supported-guests", "install-media", "netboot-adapter",
    "prom-dialogue", "console", "smdk", "launch-recipe",
    "needs-debug-leds-off",
)

# Topology fields that name or select, and fields added after the first
# fingerprint was taken; neither describes the machine.
TOPOLOGY_IGNORED = {
    "name", "description", "display-name", "machine-options", "board",
    "default-board", "machine-default", "id", "processor-options",
    "clock", "serial-lines", "hostid-source", "settings", "board-slots",
}

UNRESERVED_RAM = ["-object",
                  "memory-backend-ram,id=sgi-ram,size=%dM,reserve=off",
                  "-machine", "memory-backend=sgi-ram"]

# Origin 3000 clock topologies folded into processor options of a base.
FOLDED = {
    "onyx3000-r14000-500mhz": "onyx3000",
    "onyx3000-r14000-600mhz": "onyx3000",
    "onyx3000-r16000": "onyx3000",
    "origin3200-r14000-500mhz": "origin3200",
    "origin3200-r14000-600mhz": "origin3200",
    "origin3200-r16000": "origin3200",
    "origin3400-r14000-500mhz": "origin3400",
    "origin3400-r14000-600mhz": "origin3400",
    "origin3400-r16000": "origin3400",
}


class Qmp:
    """A synchronous QMP session on a child's standard streams."""

    def __init__(self, argv):
        self.proc = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True,
                                     bufsize=1)
        self.counter = 0
        greeting = self.proc.stdout.readline()
        if not greeting:
            self.stderr = self.proc.stderr.read()
            self.proc.wait()
            raise RuntimeError(self.stderr.strip())
        self.call("qmp_capabilities")

    def send(self, command, arguments=None):
        self.counter += 1
        message = {"execute": command, "id": self.counter}
        if arguments:
            message["arguments"] = arguments
        return self.counter, json.dumps(message)

    def read_reply(self):
        while True:
            line = self.proc.stdout.readline()
            if not line:
                self.proc.wait()
                raise RuntimeError(self.proc.stderr.read().strip() or
                                   "QEMU closed QMP")
            reply = json.loads(line)
            if "id" in reply:
                return reply

    def call(self, command, arguments=None):
        _, text = self.send(command, arguments)
        self.proc.stdin.write(text + "\n")
        self.proc.stdin.flush()
        return self.read_reply()

    def pipeline(self, commands):
        """Run (command, arguments) pairs in one write; replies in order."""
        ids = []
        lines = []
        for command, arguments in commands:
            i, text = self.send(command, arguments)
            ids.append(i)
            lines.append(text)
        replies = {}
        # Write from a thread-free loop in blocks small enough that the
        # reply pipe cannot fill while QEMU waits on a full input pipe.
        block = 200
        for start in range(0, len(lines), block):
            chunk = ids[start:start + block]
            self.proc.stdin.write("\n".join(lines[start:start + block]) + "\n")
            self.proc.stdin.flush()
            for _ in chunk:
                reply = self.read_reply()
                replies[reply["id"]] = reply
        return [replies[i] for i in ids]

    def close(self):
        try:
            self.proc.stdin.write('{"execute":"quit"}\n')
            self.proc.stdin.flush()
        except OSError:
            pass
        try:
            self.proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()


def sha(text):
    return hashlib.sha256(text.encode()).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def read_catalogue(binary):
    q = Qmp([binary, "-M", "none", "-nodefaults", "-display", "none", "-S",
             "-accel", "qtest", "-qmp", "stdio"])
    try:
        return q.call("query-sgi-machines")["return"]
    finally:
        q.close()


def topology_named(catalogue, offering):
    """The topology record an offering resolves to, as QEMU resolves it."""
    name = offering["topology"]
    board = offering.get("board")
    smp = offering["smp"]
    records = [t for t in catalogue["topologies"]
               if t["name"] == name and t["product"] == offering["product"]]
    if board is not None:
        by_board = [t for t in records if t.get("board") == board]
        records = by_board or records
    for t in records:
        for pop in t["node-populations"]:
            for cpus in pop["supported-cpu-populations"]:
                if sum(cpus["cpus-per-node"]) == smp and \
                        cpus["cpus-per-node"] == offering["cpus-per-node"]:
                    return t
    return records[0]


def fit_clock_option(topology, offering):
    """@topology as an Origin 3000 clock option fits it: the option's
    processor in every fitted slot, its board identity in each Bedrock and
    its flash word, as the machine applies them. Any other topology comes
    back as it is."""
    for option in topology.get("processor-options", []):
        if (option["assembly"]["id"] != offering["processor"]["id"]
                or "board-id-word" not in option):
            continue
        fitted = copy.deepcopy(topology)
        for node in fitted["nodes"]:
            node["bedrock"]["board-id-word"] = option["board-id-word"]
            for slot in node["cpu-slots"]:
                if slot.get("assembly"):
                    slot["assembly"] = option["assembly"]
        fitted["ip35-firmware"]["cpu-mode-word"] = option["firmware-word"]
        return fitted
    return topology


def record_hash(catalogue, offering, topology=None):
    topology = topology or topology_named(catalogue, offering)
    topology = fit_clock_option(topology, offering)
    part = {k: offering[k] for k in OFFERING_FIELDS if k in offering}
    # An adapter's boot target was added after the first fingerprint.
    part["scsi-adapters"] = [
        {k: v for k, v in adapter.items() if k != "boot-target"}
        for adapter in part.get("scsi-adapters", [])]
    top = {k: v for k, v in topology.items() if k not in TOPOLOGY_IGNORED}
    # The one product renamed (onyx4-host to onyx4) appears in its records.
    return sha(canonical([part, top]).replace("onyx4-host", "onyx4"))


def normalise(text, directory):
    text = text.replace(directory, "<dir>")
    # The one machine type renamed with its product (onyx4-host to onyx4).
    text = text.replace("onyx4-host-machine", "onyx4-machine")
    return re.sub(r"#block\d+", "#blockN", text)


def walk(q, directory, path="/machine", top=True):
    """Every scalar and link property below @path, as sorted text lines."""
    listing = q.call("qom-list", {"path": path})
    if "error" in listing:
        return [f"{path}:!list"]
    lines = []
    names = []
    children = []
    for entry in listing["return"]:
        name, kind = entry["name"], entry["type"]
        if kind.startswith("child<"):
            children.append(name)
        elif name == "type" or not (top and name in SELECTION):
            names.append(name)
    replies = q.pipeline([("qom-get", {"path": path, "property": n})
                          for n in names])
    for name, reply in zip(names, replies):
        if "error" in reply:
            value = "!" + reply["error"]["class"]
        else:
            value = canonical(reply["return"])
        lines.append(normalise(f"{path}:{name}={value}", directory))
    for child in children:
        lines += walk(q, directory, f"{path}/{child}", False)
    return lines


def launch_arguments(binary, offering, directory, machine_options):
    storage = []
    machine = machine_options
    args = [binary]
    for item in offering["storage"]:
        name = item["name"]
        path = os.path.join(directory, f"{name}.raw")
        with open(path, "wb") as f:
            f.truncate(item["size"])
        machine += f",{name}={name}"
        args += ["-blockdev",
                 f"driver=raw,node-name={name},file.driver=file,"
                 f"file.filename={path}" +
                 (",read-only=on" if item.get("read-only") else "")]
        storage.append(path)
    for n in offering["networks"]:
        mac = n["mac"]
        if mac["derived"] and mac["offset"] == 0:
            machine += f",{mac['machine-property']}={mac['default-address']}"
            break
    nics = []
    for n in offering["networks"]:
        if not n["default"]:
            continue
        nics += ["-netdev", "user,id=catalogue-net", "-net"]
        if n["mac"]["derived"]:
            nics.append(f"nic,model={n['nic-model']},netdev=catalogue-net,"
                        f"{n['nic-mac-option']}={n['mac']['default-address']}")
        else:
            nics.append(f"nic,model={n['nic-model']},netdev=catalogue-net")
    return args, machine, nics


def o2_machine(directory, machine):
    """The synthetic ROM and identity inputs the O2 launch test supplies."""
    rom = os.path.join(directory, "synthetic-rom")
    ident = os.path.join(directory, "synthetic-identity")
    data = bytearray(512 * 1024)
    data[0] = 0x5a
    data[-1] = 0xa5
    with open(rom, "wb") as f:
        f.write(data)
    with open(ident, "wb") as f:
        f.write(bytes(((i * 37) ^ 0xa5) & 0xff for i in range(128)))
    options = (f"o2,rom-image={rom},rom-size=524288,rom-offset=0,"
               "rom-pad-byte=238,rom-at29c040a-protection=disabled,"
               "rom-at29c040a-lower-lock=unlocked,"
               "rom-at29c040a-upper-lock=unlocked,"
               "identity-test-serial=1032547698ba,"
               f"identity-eprom-file={ident},"
               "identity-status=0123456789abcd,password-clear-input=0")
    return options + machine[machine.index(","):] if "," in machine \
        else options


def fingerprint(binary, offering, dump_to=None):
    directory = tempfile.mkdtemp(prefix="sgi-fp-")
    try:
        options = offering["machine-options"]
        for attempt in range(2):
            args, machine, nics = launch_arguments(binary, offering,
                                                   directory, options)
            if offering["product"] == "o2":
                machine = o2_machine(directory, machine)
            memory = offering["memory-per-node-mib"]["default"] * \
                offering["nodes"]
            ram = (["-m", f"{memory}M"] if memory <= 16 * 1024 else
                   [UNRESERVED_RAM[0], UNRESERVED_RAM[1] % memory,
                    *UNRESERVED_RAM[2:]])
            args += ["-M", machine, "-cpu", offering["default-cpu-model"],
                     "-smp", str(offering["smp"]), *ram, "-nodefaults",
                     "-display", "none", "-S", "-accel", "qtest", "-qmp",
                     "stdio", *nics]
            try:
                q = Qmp(args)
                break
            except RuntimeError as error:
                # Before population= was an option's own, the four
                # ambiguous Origin 300 offerings named it by hand.
                if attempt == 0 and "population=" in str(error):
                    options += ",population=" + ":".join(
                        str(c) for c in offering["cpus-per-node"])
                    continue
                return {"error": str(error)}
        try:
            lines = sorted(walk(q, directory))
        finally:
            q.close()
        if dump_to:
            with open(dump_to, "w") as f:
                f.write("\n".join(lines) + "\n")
        return {"qom": sha("\n".join(lines)), "lines": len(lines)}
    finally:
        shutil.rmtree(directory, ignore_errors=True)


def offering_key(offering):
    if "id" in offering:
        return offering["id"]
    return "|".join([offering["machine-options"], str(offering["smp"]),
                     "-".join(str(c) for c in offering["cpus-per-node"]),
                     offering["processor"]["id"]])


def job(args):
    binary, offering, record, dump_dir = args
    key = offering_key(offering)
    dump = None
    if dump_dir:
        dump = os.path.join(dump_dir, re.sub(r"[^a-z0-9.-]", "_",
                                             key.lower()) + ".txt")
    result = fingerprint(binary, offering, dump)
    result["record"] = record
    result["meta"] = {k: offering.get(k) for k in (
        "product", "topology", "board", "machine-options", "smp",
        "cpus-per-node", "nodes")}
    result["meta"]["processor"] = offering["processor"]["id"]
    return key, result


def capture(args):
    catalogue = read_catalogue(args.qemu)
    jobs = []
    for o in catalogue["offerings"]:
        if args.only and not re.search(args.only, offering_key(o)):
            continue
        jobs.append((args.qemu, o, record_hash(catalogue, o), args.dump_dir))
    results = {}
    with concurrent.futures.ProcessPoolExecutor(args.jobs) as pool:
        for key, result in pool.map(job, jobs):
            results[key] = result
            print(f"{key}: {result.get('qom', result.get('error'))[:16]}",
                  file=sys.stderr)
    with open(args.out, "w") as f:
        json.dump({"version": catalogue["version"], "entries": results}, f,
                  indent=1, sort_keys=True)


def topology_name(meta, old_entries):
    """The version 3 topology name of an old entry, by the renaming rule."""
    product, topology = meta["product"], meta["topology"]
    name = FOLDED.get(topology, topology)
    if name == "onyx4-host":
        name = "onyx4"
    boards = {e["meta"]["board"] for e in old_entries.values()
              if e["meta"]["product"] == product
              and e["meta"]["topology"] == topology and e["meta"]["board"]}
    if len(boards) > 1:
        if name == "tezro":
            name = ("tezro-one-processor" if meta["smp"] == 1
                    else "tezro-" + meta["board"])
        else:
            name += "-" + meta["board"]
    return name


def new_id(entry, old_entries):
    """The version 3 id of an old entry, by the renaming rule."""
    meta = entry["meta"]
    name = topology_name(meta, old_entries)
    processors = {e["meta"]["processor"] for e in old_entries.values()
                  if topology_name(e["meta"], old_entries) == name}
    result = name + "." + "-".join(str(c) for c in meta["cpus-per-node"])
    if len(processors) > 1:
        result += "." + meta["processor"]
    return result


def compare(args):
    old = json.load(open(args.old))["entries"]
    new = json.load(open(args.new))["entries"]
    mapping = {k: new_id(v, old) for k, v in old.items()}
    matched = mismatched = 0
    bad = []
    if len(set(mapping.values())) != len(mapping):
        print("two old offerings map to one id", file=sys.stderr)
        return 1
    if set(mapping.values()) != set(new):
        print("old ids not in new:",
              sorted(set(mapping.values()) - set(new)), file=sys.stderr)
        print("new ids not mapped:",
              sorted(set(new) - set(mapping.values())), file=sys.stderr)
        return 1
    for key, target in sorted(mapping.items()):
        o, n = old[key], new[target]
        same = o.get("qom") == n.get("qom") and o["record"] == n["record"]
        if "error" in o or "error" in n or not same:
            mismatched += 1
            bad.append((key, target, o.get("qom") == n.get("qom"),
                        o["record"] == n["record"],
                        o.get("error") or n.get("error")))
        else:
            matched += 1
    print(f"matched {matched} mismatched {mismatched} of {len(mapping)}")
    for key, target, qom, record, error in bad[:20]:
        print(f"  {key} -> {target}: qom {'ok' if qom else 'DIFF'}, "
              f"record {'ok' if record else 'DIFF'}"
              + (f", {error}" if error else ""))
    if args.map_out:
        with open(args.map_out, "w") as f:
            json.dump(mapping, f, indent=1, sort_keys=True)
    return 1 if mismatched else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = parser.add_subparsers(dest="mode", required=True)
    c = sub.add_parser("capture")
    c.add_argument("qemu")
    c.add_argument("out")
    c.add_argument("--jobs", type=int, default=16)
    c.add_argument("--only", help="regular expression on the offering key")
    c.add_argument("--dump-dir", help="also write each QOM dump here")
    k = sub.add_parser("compare")
    k.add_argument("old")
    k.add_argument("new")
    k.add_argument("--map-out")
    args = parser.parse_args()
    return capture(args) or 0 if args.mode == "capture" else compare(args)


if __name__ == "__main__":
    sys.exit(main())
