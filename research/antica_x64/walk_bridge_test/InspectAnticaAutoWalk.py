#!/usr/bin/env python3
"""Inspect Antica autoWalk callsite offline. No process access or injection."""
import argparse
import hashlib
import shutil
import struct
import subprocess
import sys
from pathlib import Path

EXPECTED = dict(timestamp=0x6B06FF0F, image_size=0xF38000,
                text_rva=0x1000, text_raw=0x980200,
                text_sha="6244c268981a4c46791fbc660c96f0f22644806f4e0494ad5244d48e32a94bb0",
                call_rva=0x15F71E, callee_rva=0x140D00)

def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]

def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]

def u64(data, offset):
    return struct.unpack_from("<Q", data, offset)[0]

def pe_sections(data):
    if data[:2] != b"MZ":
        raise ValueError("Nao e MZ")
    pe = u32(data, 0x3c)
    if data[pe:pe+4] != b"PE\x00\x00" or u16(data, pe+4) != 0x8664:
        raise ValueError("Nao e PE x64")
    count = u16(data, pe+6)
    timestamp = u32(data, pe+8)
    optional_size = u16(data, pe+20)
    opt = pe+24
    if u16(data, opt) != 0x20b:
        raise ValueError("Nao e PE32+")
    base = u64(data, opt+24)
    image_size = u32(data, opt+56)
    sections = {}
    for i in range(count):
        off = opt + optional_size + 40*i
        name = data[off:off+8].split(b"\x00")[0].decode("ascii","replace")
        size, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, off+8)
        if raw_ptr+raw_size > len(data):
            raise ValueError("Secao fora do arquivo")
        sections[name] = (va, raw_size, raw_ptr)
    return timestamp, image_size, base, sections

def get_bytes(data, sections, rva, size):
    for _, (va, raw_size, raw_ptr) in sections.items():
        if va <= rva and rva+size <= va+raw_size:
            off = raw_ptr+rva-va
            return data[off:off+size]
    raise ValueError(f"RVA 0x{rva:X} indisponivel")

def run():
    p = argparse.ArgumentParser()
    p.add_argument("--exe",type=Path,required=True)
    p.add_argument("--out",type=Path,required=True)
    p.add_argument("--extended",action="store_true",help="Desmontar regiao mais ampla da construcao do caminho.")
    a = p.parse_args()
    lines = ["CROME ANTICA X64 — AUTOWALK ABI OFFLINE",
             "NENHUMA INJECAO. NENHUM ACESSO A PROCESSO. NENHUM MOVIMENTO.",
             f"EXE: {a.exe}", ""]
    status = 1
    try:
        data = a.exe.read_bytes()
        stamp, image_size, base, sections = pe_sections(data)
        va, raw_size, raw_ptr = sections[".text"]
        sha = hashlib.sha256(data[raw_ptr:raw_ptr+raw_size]).hexdigest()
        lines += [f"Timestamp: 0x{stamp:X}",f"SizeOfImage: 0x{image_size:X}",
                  f"ImageBase preferencial: 0x{base:X}",
                  f".text RVA: 0x{va:X}",f".text raw: 0x{raw_size:X}",
                  f".text SHA256: {sha}"]
        if ((stamp,image_size,va,raw_size,sha) !=
            (EXPECTED["timestamp"],EXPECTED["image_size"],EXPECTED["text_rva"],
             EXPECTED["text_raw"],EXPECTED["text_sha"])):
            raise ValueError("Build desconhecida: analise das RVAs bloqueada")
        lines += ["[OK] Build correspondente.", ""]
        rva = EXPECTED["call_rva"]
        b = get_bytes(data,sections,rva,5)
        if b[0] != 0xE8:
            raise ValueError("Callsite nao inicia com CALL rel32")
        target = rva+5+struct.unpack("<i",b[1:])[0]
        lines += [f"CALL-site RVA: 0x{rva:X}",f"CALL bytes: {b.hex(' ')}",
                  f"CALL target RVA: 0x{target:X}"]
        if target != EXPECTED["callee_rva"]:
            raise ValueError("Alvo nao corresponde a autoWalk candidata")
        lines.append("[OK] CALL aponta para Game::autoWalk candidato.")
        for label,start,stop in [
            ("CALLER",rva-(0x700 if a.extended else 0xA0),rva+0x50),
            ("CALLEE",target,target+(0x800 if a.extended else 0xC0))
        ]:
            lines += ["",f"==== {label} RVA 0x{start:X}..0x{stop:X} ===="]
            window = get_bytes(data,sections,start,stop-start)
            lines.append("HEX: "+window.hex(" "))
            objdump = shutil.which("objdump")
            if not objdump:
                fallback = Path("C:/msys64/ucrt64/bin/objdump.exe")
                objdump = str(fallback) if fallback.is_file() else None
            if objdump:
                cmd = [objdump,"-d","-M","intel",
                       "--start-address",f"0x{base+start:X}",
                       "--stop-address",f"0x{base+stop:X}",str(a.exe)]
                proc = subprocess.run(cmd,capture_output=True,text=True,errors="replace")
                lines.append("OBJDUMP exit="+str(proc.returncode))
                lines.extend(proc.stdout.splitlines() if a.extended else proc.stdout.splitlines()[-220:])
                if proc.returncode:
                    lines.extend(proc.stderr.splitlines()[-10:])
            else:
                lines.append("[WARN] objdump ausente; instalar MSYS2 UCRT64.")
        lines += ["","[OK] Evidencia estatica coletada.",
                  "[PENDENTE] ABI, ponteiros, thread e carregamento nao validados."]
        status = 0
    except (OSError,ValueError,struct.error,IndexError,KeyError) as exc:
        lines.append("[BLOQUEADO] "+str(exc))
    a.out.write_text("\n".join(lines)+"\n",encoding="utf-8")
    print("\n".join(lines[:16]))
    print("Relatorio: "+str(a.out))
    return status

if __name__=="__main__":
    sys.exit(run())
