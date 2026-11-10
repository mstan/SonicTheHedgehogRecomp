#!/usr/bin/env python3
"""Fetch pinned driver software/tools and assemble a software-only build input."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import platform
import stat
import sys
import tempfile
import urllib.parse
import urllib.request
from build import SOURCE_COMMIT, DRIVER_SOURCES, assemble, sha256


def fetch_file(root: Path, name: str, spec: dict, offline: bool) -> Path:
    target=root/name
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError("source cache path escapes its root")
    if target.is_file() and target.stat().st_size==spec['size'] and sha256(target)==spec['sha256']:
        return target
    if offline:raise RuntimeError(f"Missing or damaged cached driver input: {name}")
    url='https://raw.githubusercontent.com/TheBlad768/s1disasm-clone-driver/'+SOURCE_COMMIT+'/'+urllib.parse.quote(name,safe='/')
    request=urllib.request.Request(url,headers={'User-Agent':'Sonic1-driver-build/1'})
    with urllib.request.urlopen(request,timeout=60) as response:
        data=response.read(spec['size']+1)
    if len(data)!=spec['size'] or hashlib.sha256(data).hexdigest()!=spec['sha256']:
        raise RuntimeError(f"Pinned driver input hash mismatch: {name}")
    target.parent.mkdir(parents=True,exist_ok=True)
    target.write_bytes(data)
    return target


def main() -> None:
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cache',type=Path,required=True)
    ap.add_argument('--cmake-out',type=Path,required=True)
    ap.add_argument('--offline',action='store_true')
    ap.add_argument('--assembler',type=Path)
    ap.add_argument('--p2bin',type=Path)
    a=ap.parse_args()
    here=Path(__file__).resolve().parent
    lock=json.loads((here/'sources.lock.json').read_text())
    if lock['commit']!=SOURCE_COMMIT or set(lock['sources'])!=set(DRIVER_SOURCES):
        raise ValueError('Driver source lock does not match the assembler inputs')
    cache=a.cache.resolve();source=cache/'source';source.mkdir(parents=True,exist_ok=True)
    for name in DRIVER_SOURCES:fetch_file(source,name,lock['sources'][name],a.offline)
    if bool(a.assembler)!=bool(a.p2bin):ap.error('--assembler and --p2bin must be supplied together')
    if a.assembler:
        assembler=a.assembler.resolve(strict=True);p2bin=a.p2bin.resolve(strict=True)
    else:
        host=platform.system()+'-'+platform.machine().lower()
        host={'Windows-amd64':'Windows-x86_64','Linux-x86_64':'Linux-x86_64'}.get(host)
        if not host:ap.error('Supply native --assembler and --p2bin tools for this build host')
        names=['build_tools/'+host+'/'+name for name in (('asw.exe','p2bin.exe') if host.startswith('Windows') else ('asl','p2bin'))]
        assembler,p2bin=[fetch_file(source,name,lock['tools'][name],a.offline) for name in names]
        if not host.startswith('Windows'):
            for tool in (assembler,p2bin):tool.chmod(tool.stat().st_mode|stat.S_IXUSR)
    key=hashlib.sha256(b''.join((here/name).read_bytes() for name in ('sources.lock.json','driver_rom.asm','build.py','fetch_build.py'))+
                       bytes.fromhex(sha256(assembler)+sha256(p2bin))).hexdigest()[:20]
    pack=cache/('program-'+key)
    if not pack.exists():
        with tempfile.TemporaryDirectory(prefix='.assemble-',dir=cache) as work:
            staging=Path(work)/'program'
            try:
                manifest=assemble(source,assembler,p2bin,staging,rom_assets=True)
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
