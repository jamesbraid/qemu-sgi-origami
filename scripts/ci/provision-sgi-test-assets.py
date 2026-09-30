#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Fetch only the LocalAsset files used by a selected MIPS64 test set."""

import argparse
import hashlib
import importlib.util
import os
from pathlib import Path, PurePosixPath
import sys
import tempfile
import unittest
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen


def selected_assets(source, target_names):
    source = Path(source).resolve()
    functional = source / "tests/functional"
    mips64 = functional / "mips64"
    sys.path[:0] = [str(source / "python"), str(functional), str(mips64)]

    from qemu_test.asset import LocalAsset

    assets = {}
    for target in target_names:
        prefix = "func-mips64-"
        if not target.startswith(prefix):
            raise ValueError(f"unexpected MIPS64 test target: {target}")
        name = target[len(prefix):]
        module_path = mips64 / f"test_{name}.py"
        if not module_path.is_file():
            module_path = mips64 / "generic" / f"test_{name}.py"
        if not module_path.is_file():
            raise ValueError(f"no test module found for {target}")

        module_name = f"_nightly_asset_{name}"
        spec = importlib.util.spec_from_file_location(module_name, module_path)
        if spec is None or spec.loader is None:
            raise ValueError(f"cannot load test module for {target}")
        module = importlib.util.module_from_spec(spec)
        sys.modules[module_name] = module
        spec.loader.exec_module(module)

        for candidate in vars(module).values():
            if (not isinstance(candidate, type) or
                    not issubclass(candidate, unittest.TestCase)):
                continue
            for attr in dir(candidate):
                if not attr.startswith("ASSET_"):
                    continue
                asset = getattr(candidate, attr)
                if not isinstance(asset, LocalAsset):
                    continue
                path = asset.path.as_posix()
                digest = asset.hash
                if (not path or path.startswith("/") or
                        ".." in PurePosixPath(path).parts):
                    raise ValueError(
                        f"unsafe LocalAsset path in {target}: {path}")
                if not digest or len(digest) != 64:
                    raise ValueError(
                        f"LocalAsset lacks SHA-256 in {target}: {path}")
                previous = assets.setdefault(path, digest)
                if previous != digest:
                    raise ValueError(
                        f"conflicting LocalAsset hashes for {path}")

    return sorted(assets.items())


def validate_asset_base_url(base_url):
    parsed = urlsplit(base_url)
    if (parsed.scheme not in {"http", "https"} or not parsed.netloc or
            parsed.query or parsed.fragment):
        raise ValueError("asset base URL must be HTTP(S) without query or fragment")
    return base_url.rstrip("/")


def package_url(path, digest, base_url):
    rel = PurePosixPath(path)
    if rel.parent == PurePosixPath("."):
        raise ValueError(f"LocalAsset has no package directory: {path}")
    package = str(rel.parent).replace("/", "-")
    base_url = validate_asset_base_url(base_url)
    coordinate = (package, digest, rel.name)
    encoded = "/".join(quote(part, safe="") for part in coordinate)
    return f"{base_url}/{encoded}"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def fetch(path, digest, root, token, base_url):
    destination = root / path
    if destination.is_file() and sha256(destination) == digest:
        print(f"verified {path}")
        return

    destination.parent.mkdir(parents=True, exist_ok=True)
    request = Request(package_url(path, digest, base_url),
                      headers={"Authorization": f"token {token}"})
    fd, temporary = tempfile.mkstemp(dir=destination.parent,
                                     prefix=f"{destination.name}.")
    temp_path = Path(temporary)
    try:
        running = hashlib.sha256()
        with (os.fdopen(fd, "wb") as output,
              urlopen(request, timeout=600) as response):
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                running.update(chunk)
                if chunk.count(0) == len(chunk):
                    output.seek(len(chunk), os.SEEK_CUR)
                else:
                    output.write(chunk)
            output.truncate(output.tell())
        output_hash = running.hexdigest()
        if output_hash != digest:
            raise ValueError(f"SHA-256 mismatch for {path}: {output_hash}")
        temp_path.replace(destination)
        print(f"fetched {path}")
    finally:
        temp_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", default="/src")
    parser.add_argument("--names", required=True,
                        help="file containing selected func-mips64-* targets")
    parser.add_argument("--asset-root", required=True)
    parser.add_argument("--asset-base-url",
                        default=os.environ.get("CI_ASSET_BASE_URL"),
                        help="generic package endpoint base URL (or CI_ASSET_BASE_URL)")
    parser.add_argument("--list-only", action="store_true")
    args = parser.parse_args()
    if not args.asset_base_url:
        parser.error("--asset-base-url or CI_ASSET_BASE_URL is required")
    try:
        args.asset_base_url = validate_asset_base_url(args.asset_base_url)
    except ValueError as error:
        parser.error(str(error))

    names = [line.strip() for line in Path(args.names).read_text().splitlines()
             if line.strip()]
    assets = selected_assets(args.source, names)
    print(f"selected {len(assets)} distinct LocalAsset files")
    if args.list_only:
        for path, digest in assets:
            print(f"{package_url(path, digest, args.asset_base_url)}  {digest}  {path}")
        return 0

    token = os.environ.get("CI_BOT_TOKEN")
    if not token:
        parser.error("CI_BOT_TOKEN is required")
    root = Path(args.asset_root)
    for path, digest in assets:
        fetch(path, digest, root, token, args.asset_base_url)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
