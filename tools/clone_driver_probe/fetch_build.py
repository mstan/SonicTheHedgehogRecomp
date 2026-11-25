#!/usr/bin/env python3
"""Fetch pinned driver software/tools and assemble a software-only build input."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import tempfile
from build import DEFAULT_ENGINE, HERE, LOCK, PROFILE, SOURCE_COMMIT, shared_builder, sha256


def main() -> None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cache',type=Path,required=True)
    ap.add_argument('--cmake-out',type=Path,required=True)
    ap.add_argument('--offline',action='store_true')
    ap.add_argument('--engine',type=Path,default=DEFAULT_ENGINE)
    ap.add_argument('--assembler',type=Path)
    ap.add_argument('--p2bin',type=Path)
    a=ap.parse_args()
    here=HERE
    builder=shared_builder(a.engine)
    cache=a.cache.resolve()
    source,assembler,p2bin=builder.fetch_inputs(LOCK,cache,a.assembler,a.p2bin,a.offline)
    shared_path=a.engine.resolve()/'tools/clone_driver/build.py'
    key=hashlib.sha256(b''.join((here/name).read_bytes() for name in ('sources.lock.json','profile.json','driver_rom.asm','build.py','fetch_build.py'))+
                       shared_path.read_bytes()+bytes.fromhex(sha256(assembler)+sha256(p2bin))).hexdigest()[:20]
    pack=cache/('program-'+key)
    if not pack.exists():
        with tempfile.TemporaryDirectory(prefix='.assemble-',dir=cache) as work:
            staging=Path(work)/'program'
            try:
                manifest=builder.assemble_legacy(source,assembler,p2bin,staging,PROFILE,here/'driver_rom.asm',LOCK)
            except Exception as exc:
                log=cache/'assembly-failure.log'
                text='\n'.join((staging/name).read_text(errors='replace')[-65536:]
                               for name in ('assembler.log','driver-errors.log','p2bin.log') if (staging/name).is_file())
                log.write_text(text or str(exc))
                raise RuntimeError(f'Driver assembly failed; see {log}') from exc
            staging.rename(pack)
    else:
        manifest=json.loads((pack/'probe.json').read_text())
    if (manifest.get('asset_mode')!='rom' or manifest.get('source_commit')!=SOURCE_COMMIT or
            manifest.get('wrapper_sha256')!=sha256(here/'driver_rom.asm') or
            manifest.get('payload_sha256')!=sha256(pack/'clone-driver.bin')):
        raise RuntimeError('Cached driver program identity mismatch')
    a.cmake_out.parent.mkdir(parents=True,exist_ok=True)
    # Bracket quoting preserves spaces, semicolons and Windows paths literally.
    value=pack.as_posix();eq='='
    while ']'+eq+']' in value:eq+='='
    text='set(SONIC_CLONE_DRIVER_PROBE_DIR ['+eq+'['+value+']'+eq+'])\n'
    if not a.cmake_out.exists() or a.cmake_out.read_text()!=text:a.cmake_out.write_text(text)
    print(f"Clone Driver software prepared: {manifest['payload_size']} bytes; game assets come from the player ROM")


if __name__=='__main__':
    main()
