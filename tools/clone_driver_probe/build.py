#!/usr/bin/env python3
"""Assemble the pinned Sonic 1 legacy profile with the shared Genesis generator."""
from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent
DEFAULT_ENGINE = HERE.parents[1] / "segagenesisrecomp"
LOCK = json.loads((HERE / "sources.lock.json").read_text())
PROFILE = json.loads((HERE / "profile.json").read_text())
SOURCE_COMMIT = LOCK["commit"]


def shared_builder(engine: Path):
    path = engine.resolve() / "tools/clone_driver/build.py"
    spec = importlib.util.spec_from_file_location("genesis_clone_driver", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "assembler", "p2bin", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--engine", type=Path, default=DEFAULT_ENGINE)
    parser.add_argument("--rom-assets", action="store_true")
    args = parser.parse_args()
    if not args.rom_assets:
        parser.error("Only isolated software-only --rom-assets builds are supported")
    source = args.source.resolve(strict=True)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if commit != SOURCE_COMMIT or args.output.resolve().is_relative_to(source):
        parser.error("Expected the pinned upstream checkout and separate output")
    paths = {name: source / name for name in LOCK["sources"]}
    for name, path in paths.items():
        spec = LOCK["sources"][name]
        if not path.is_file() or path.stat().st_size != spec["size"] or sha256(path) != spec["sha256"]:
            parser.error("Pinned source identity mismatch: " + name)
    builder = shared_builder(args.engine)
    print(json.dumps(builder.assemble_legacy(
        paths, args.assembler.resolve(strict=True), args.p2bin.resolve(strict=True),
        args.output.resolve(), PROFILE, HERE / "driver_rom.asm", LOCK), indent=2))


if __name__ == "__main__":
    main()
