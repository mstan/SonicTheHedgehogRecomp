#!/usr/bin/env python3
"""Assemble an isolated Clone Driver image; never modify or launch a game."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

SOURCE_URL = "https://github.com/TheBlad768/s1disasm-clone-driver"
SOURCE_COMMIT = "9c6e99358c510fa640868ce21b5fd6aea6cd01f5"
DRIVER_SOURCES = (
    "MacroSetup.asm", "sound/Definitions.asm", "sound/Sound IDs.asm", "sound/Settings.asm",
    "sound/SMPS2ASM - PSG Volume Envelope Equates.asm", "sound/SMPS2ASM - DAC Sample Equates.asm",
    *("sound/engine/"+name for name in ("Constants.asm", "Functions.asm", "Macros.asm",
       "MegaPCM.asm", "MegaPCM.Macros.asm", "RAM.asm", "Sonic 2 Clone Driver v2.asm", "_smps2asm_inc.asm")),
)
LOAD_ADDRESS = 0x80000
HEADER = struct.Struct(">4s14I")
FIELDS = (
    "payload_size", "initialise", "update", "queue_music", "queue_sfx",
    "queue_extra", "ram_address", "ram_size", "queue_offset", "pause_offset",
    "megapcm_load_driver", "megapcm_load_table", "sample_table", "guarded_update",
)


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(argv: list[str], cwd: Path, log: Path) -> None:
    with log.open("w", encoding="utf-8") as stream:
        try:
            subprocess.run(argv, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT,
                           check=True, timeout=120)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            raise RuntimeError(f"build tool failed; see {log} and driver-errors.log") from exc


def assemble(source: Path, assembler: Path, p2bin: Path, output: Path,
             rom_assets: bool = False) -> dict:
    """Assemble already verified inputs; callers own their source verification."""
    wrapper = Path(__file__).with_name("driver_rom.asm" if rom_assets else "driver.asm").resolve(strict=True)
    output = output.resolve()
    if output.is_relative_to(source):
        raise ValueError("output must be outside the upstream checkout")
    if output.exists():
        raise ValueError("output already exists; choose a new directory")
    output.mkdir(parents=True)
    include_root=source
    if rom_assets:
        include_root=output / "inputs"
        # A bounded software-only include tree makes asset inclusion impossible,
        # independent of the assembler's include-directory precedence.
        for name in DRIVER_SOURCES:
            target=include_root / name;target.parent.mkdir(parents=True,exist_ok=True)
            target.write_bytes((source/name).read_bytes())
        overrides = include_root / "sound"
        # Tables are populated from the player's ROM, never from game assets
        # in the upstream repository. Their driver-relative locations are fixed.
        for name,label,ids,end in (
                ("Music.asm","MusicIndex",[f"ptr_mus{i:02X}" for i in range(0x81,0x94)],"ptr_musend"),
                ("SFX.asm","SoundIndex",[f"ptr_snd{i:02X}" for i in range(0xA0,0xD0)],"ptr_sndend"),
                ("Special SFX.asm","SpecSoundIndex",["ptr_sndD0"],"ptr_specend"),
                ("PSG Volume Envelopes.asm","PSG_Index",[f"ptr_s1psg{i:02X}" for i in range(1,10)],"")):
            text=f"{label}:\n"+"".join(f"{item}: dc.l 0\n" for item in ids)
            if end:text+=f"{end}:\n"
            (overrides / name).write_text(text,encoding="utf-8")
    obj = output / "driver.p"
    image = output / "driver-image.bin"
    # Absolute output names avoid AS's basename handling for ../source paths.
    run([str(assembler), "-xx", "-n", "-q", "-A", "-L", "-U",
         "-i", str(include_root), "-o", str(obj), "-olist", str(output / "driver.lst"),
         "-E", str(output / "driver-errors.log"), str(wrapper)],
        output, output / "assembler.log")
    run([str(p2bin), "-p=FF", str(obj), str(image)], output, output / "p2bin.log")
    raw = image.read_bytes()
    payload = raw[LOAD_ADDRESS:]
    if (len(payload) < HEADER.size or len(raw) > 0x400000 or
            raw[:LOAD_ADDRESS] != b"\xff" * LOAD_ADDRESS):
        raise ValueError("expected an isolated sound image beginning at $80000")
    magic, *values = HEADER.unpack_from(payload)
    info = dict(zip(FIELDS, values))
    if magic != (b"S1CR" if rom_assets else b"S1CD") or info["payload_size"] != len(payload):
        raise ValueError("invalid probe header or payload extent")
    if info["ram_address"] & 0xFFFFFF != 0xFFF000 or not 0 < info["ram_size"] <= 0x400:
        raise ValueError("driver does not fit the proposed $FFF000-$FFF3FF RAM range")
    for name in ("initialise", "update", "queue_music", "queue_sfx", "queue_extra",
                 "megapcm_load_driver", "megapcm_load_table", "sample_table", "guarded_update"):
        if not LOAD_ADDRESS + HEADER.size <= info[name] < len(raw) or info[name] & 1:
            raise ValueError(f"invalid image pointer: {name}")
    if not 0 <= info["queue_offset"] <= info["ram_size"] - 8:
        raise ValueError("four word-sized queues do not fit driver RAM")
    if not 0 <= info["pause_offset"] < info["ram_size"]:
        raise ValueError("pause byte does not fit driver RAM")
    binary = output / "clone-driver.bin"
    binary.write_bytes(payload)
    manifest = {
        "status": "assembled-only; not a playable or runtime-qualified mod",
        "source_url": SOURCE_URL, "source_commit": SOURCE_COMMIT,
        "asset_mode": "rom" if rom_assets else "upstream",
        "wrapper_sha256": sha256(wrapper),
        "assembler_sha256": sha256(assembler), "p2bin_sha256": sha256(p2bin),
        "load_address": LOAD_ADDRESS, "payload_sha256": sha256(binary), **info,
    }
    (output / "probe.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                       encoding="utf-8")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("source", "assembler", "p2bin", "output"):
        parser.add_argument("--"+name,type=Path,required=True)
    parser.add_argument("--rom-assets",action="store_true")
    args=parser.parse_args()
    source=args.source.resolve(strict=True)
    commit=subprocess.check_output(["git","-C",str(source),"rev-parse","HEAD"],text=True).strip()
    if commit!=SOURCE_COMMIT:parser.error(f"expected upstream commit {SOURCE_COMMIT}; got {commit}")
    dirty=subprocess.check_output(["git","-C",str(source),"status","--porcelain","--untracked-files=all","--","MacroSetup.asm","sound"],text=True)
    if dirty.strip():parser.error("upstream MacroSetup.asm or sound/ has local changes")
    print(json.dumps(assemble(source,args.assembler.resolve(strict=True),args.p2bin.resolve(strict=True),args.output,args.rom_assets),indent=2))


if __name__ == "__main__":
    main()
