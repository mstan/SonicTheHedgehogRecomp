#!/usr/bin/env python3
"""Derive Sonic 1 native sound inputs from the original ROM and driver software."""
from pathlib import Path
import argparse, hashlib, json
EXPECTED='46160baa06362c711c9f1a5017cb7371026444936c8af5e93a78996cf32ff2a6'

def prepare(original, output, clone=None):
    rom=original.read_bytes()
    if hashlib.sha256(rom).hexdigest()!=EXPECTED:
        raise ValueError('Native sound requires original Sonic 1 JUE REV00')
    pc=0x72e7e;bits=int.from_bytes(rom[0x72e7c:0x72e7e],'little');left=16;out=bytearray()
    def byte():
        nonlocal pc
        if pc>=len(rom):raise ValueError('Truncated sound data')
        value=rom[pc];pc+=1;return value
    def bit():
        nonlocal bits,left
        value=bits&1;bits>>=1;left-=1
        if not left:bits=byte()|(byte()<<8);left=16
        return value
    while True:
        if bit():
            if len(out)>=8192:raise ValueError('Oversized sound program')
            out.append(byte());continue
        if not bit():count=2+(bit()<<1)+bit();offset=byte()-256
        else:
            a,b=byte(),byte();offset=(0xe000|((b&0xf8)<<5)|a)-65536;count=b&7
            if count:count+=2
            else:
                count=byte()
                if not count:break
                if count==1:continue
                count+=1
        if len(out)+offset<0 or len(out)+count>8192:raise ValueError('Invalid sound copy')
        for _ in range(count):out.append(out[len(out)+offset])
    output.mkdir(parents=True,exist_ok=True)
    def mapped(name, data):
        if len(data)>8192:raise ValueError('Native sound image exceeds RAM')
        memory=bytes(data).ljust(8192,b'\0')
        (output/name).write_bytes(memory+memory)
        return hashlib.sha256(memory).hexdigest()
    identities={'driver':mapped('driver.bin',out)}
    (output/'reset.bin').write_bytes(bytes(256))
    # Original register-clear program copied from ROM by reset. Its final
    # LD (HL),$E9 creates the wait loop at RAM $26; include that exact variant.
    bootstrap=bytearray(rom[0x2ca:0x2ca+38]).ljust(39,b'\0')
    if bootstrap[:4]!=bytes.fromhex('AF01D91F'):
        raise ValueError('Original register-clear bootstrap differs')
    bootstrap[0x26]=0xe9
    identities['bootstrap']=mapped('bootstrap.bin',bootstrap)
    if clone is not None:
        info=json.loads((clone/'probe.json').read_text(encoding='utf-8'))
        payload=(clone/'clone-driver.bin').read_bytes()
        if (info.get('source_commit')!='9c6e99358c510fa640868ce21b5fd6aea6cd01f5' or
            hashlib.sha256(payload).hexdigest()!=info.get('payload_sha256') or
            info.get('asset_mode')!='rom' or info.get('load_address')!=0x80000):
            raise ValueError('Clone Driver software identity differs')
        base=info['megapcm_load_driver']-info['load_address']
        program=payload[base+0x2b0:base+0x2117]
        if len(program)!=0x1e67 or program[:4]!=bytes.fromhex('F3ED56C3'):
            raise ValueError('Pinned Mega PCM loader bounds differ')
        identities['megapcm']=mapped('megapcm.bin',program)
        # The pinned driver installs its own entry template with LDIR during
        # startup. Compile that software-derived program shape as well as the
        # original upload; the running game's RAM remains freely writable.
        if (program[0x48:0x50]!=bytes.fromhex('216600010700EDB0') or
            program[0x66:0x6d]!=bytes.fromhex('3E00B7C8C36D00')):
            raise ValueError('Pinned Mega PCM entry copy differs')
        running=bytearray(program);running[:7]=program[0x66:0x6d]
        identities['megapcm_running']=mapped('megapcm-running.bin',running)
        identities['clone_software_sha256']=info['payload_sha256']
    else:
        (output/'megapcm.bin').write_bytes(bytes(256))
        (output/'megapcm-running.bin').write_bytes(bytes(256))
    (output/'game.toml').write_text('[game]\noutput_prefix="s1z80"\nplatform="sms"\nrom="driver.bin"\n',encoding='utf-8',newline='\n')
    (output/'input-identity.json').write_text(json.dumps({'original':str(original.resolve()),'original_sha256':EXPECTED,'decoded_bytes':len(out),'mapping':'8 KiB RAM mirrored at 0x2000','programs':identities},indent=2)+'\n',encoding='utf-8')
if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--rom',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--clone',type=Path)
    args=parser.parse_args();prepare(args.rom,args.output,args.clone)
