"""Crome Antica: read-only PE code identity checker (no process memory access)."""
from __future__ import annotations

import argparse
import ctypes
from ctypes import wintypes
import hashlib
import json
from pathlib import Path
import struct
import sys

DEFAULT_PROFILE = Path(__file__).with_name('antica_identity.json')


def number(x):
    return int(x, 0) if isinstance(x, str) else int(x)


def checksum_fnv1a64(data: bytes) -> str:
    value = 0xCBF29CE484222325
    for byte in data:
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f'0x{value:016X}'


def image_path_from_pid(pid: int) -> Path:
    if sys.platform != 'win32':
        raise RuntimeError('--pid funciona apenas no Windows; utilize --file')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD,
                                                   wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    h = kernel.OpenProcess(0x1000, False, pid)  # Limited query only
    if not h:
        err = ctypes.get_last_error()
        raise OSError(err, f'OpenProcess para consulta limitada falhou (WinError {err})')
    try:
        buffer = ctypes.create_unicode_buffer(32768)
        size = wintypes.DWORD(len(buffer))
        if not kernel.QueryFullProcessImageNameW(h, 0, buffer, ctypes.byref(size)):
            err = ctypes.get_last_error()
            raise OSError(err, f'QueryFullProcessImageName falhou (WinError {err})')
        return Path(buffer.value)
    finally:
        kernel.CloseHandle(h)


def read_fingerprint(path: Path) -> dict:
    # Bounded PE parsing: do not trust section ranges from arbitrary files.
    filesize = path.stat().st_size
    if filesize < 512:
        raise ValueError('Executavel pequeno demais para conter PE valido')
    with path.open('rb') as file:
        dos = file.read(64)
        if dos[:2] != b'MZ':
            raise ValueError('MZ ausente')
        pe_offset, = struct.unpack_from('<I', dos, 0x3C)
        if not 64 <= pe_offset <= filesize - 24:
            raise ValueError('Offset do cabecalho PE invalido')
        file.seek(pe_offset)
        headers = file.read(24)
        if headers[:4] != b'PE\0\0':
            raise ValueError('PE signature ausente')
        machine, sections, timestamp, _, _, opt_size, _ = struct.unpack_from('<HHIIIHH', headers, 4)
        if machine != 0x8664 or sections < 1 or sections > 96:
            raise ValueError('Cliente nao e um PE x64 suportado')
        if opt_size < 60 or pe_offset + 24 + opt_size + sections * 40 > filesize:
            raise ValueError('Cabecalhos PE fora do arquivo')
        option = file.read(opt_size)
        if struct.unpack_from('<H', option, 0)[0] != 0x20B:
            raise ValueError('Opcional PE32+ nao encontrado')
        image_size, = struct.unpack_from('<I', option, 56)
        sections_data = file.read(sections * 40)
        found = None
        for i in range(sections):
            entry = sections_data[i * 40:(i + 1) * 40]
            if entry[:8].rstrip(b'\0') != b'.text':
                continue
            virtual_size, rva, raw_size, raw_offset = struct.unpack_from('<IIII', entry, 8)
            if not raw_size or raw_offset > filesize or raw_size > filesize - raw_offset:
                raise ValueError('Faixa .text fora do arquivo')
            file.seek(raw_offset)
            code = file.read(raw_size)
            if len(code) != raw_size:
                raise ValueError('Leitura da secao .text incompleta')
            found = dict(textRva=f'0x{rva:X}', textRawSize=f'0x{raw_size:X}',
                         textVirtualSize=f'0x{virtual_size:X}',
                         textSha256=hashlib.sha256(code).hexdigest(),
                         textFnv1a64=checksum_fnv1a64(code))
            break
        if found is None:
            raise ValueError('Secao .text nao encontrada')
    # SHA do EXE completo e somente informativo, nunca um criterio de compatibilidade.
    whole_hash = hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda: f.read(4 * 1024 * 1024), b''):
            whole_hash.update(chunk)
    return {'timestamp': f'0x{timestamp:08X}', 'sizeOfImage': f'0x{image_size:X}',
            'machine': f'0x{machine:04X}', 'sha256FullFile': whole_hash.hexdigest(),
            'fileSize': filesize, **found}


def validate(fingerprint: dict, profile: dict) -> dict:
    checks = {}
    for name in ('timestamp', 'sizeOfImage', 'textRva', 'textRawSize'):
        checks[name] = number(fingerprint[name]) == number(profile[name])
    checks['textSha256'] = fingerprint['textSha256'].lower() == profile['textSha256'].lower()
    checks['textFnv1a64'] = number(fingerprint['textFnv1a64']) == number(profile['textFnv1a64'])
    # A nova instalacao pode ter recursos, overlay ou outros bytes diferentes.
    previous = [v.lower() for v in profile.get('installationSha256Examples', [])]
    full_same = fingerprint['sha256FullFile'].lower() in previous
    compatible = all(checks.values())
    if not compatible:
        status = 'INCOMPATIBLE_CODE_OR_LAYOUT'
    elif full_same:
        status = 'COMPATIBLE_KNOWN_INSTALLATION'
    else:
        status = 'COMPATIBLE_CODE_DIFFERENT_INSTALLATION'
    return {'ok': compatible, 'codeCompatible': compatible, 'status': status,
            'checks': checks, 'fullFileHashKnown': full_same,
            'runtimeValidated': False, 'nativeActionsReady': False,
            'note': 'Verificacao de arquivo somente. Nao confirma permissoes de memoria nem ABI em execucao.'}


def main() -> int:
    parser = argparse.ArgumentParser(description='Identidade de codigo Antica x64, sem depurador')
    where = parser.add_mutually_exclusive_group(required=True)
    where.add_argument('--file', type=Path, help='Arquivo EXE para verificar offline')
    where.add_argument('--pid', type=int, help='PID a consultar sem ler memoria')
    parser.add_argument('--profile', type=Path, default=DEFAULT_PROFILE)
    parser.add_argument('--out', type=Path, default=Path('preflight_result.json'))
    args = parser.parse_args()
    try:
        profile = json.loads(args.profile.read_text(encoding='utf-8'))
        path = args.file if args.file else image_path_from_pid(args.pid)
        if not path.is_file():
            raise FileNotFoundError(path)
        fp = read_fingerprint(path)
        validation = validate(fp, profile)
        output = {'pid': args.pid, 'exe': str(path), 'profileId': profile.get('id'),
                  **fp, **validation}
        args.out.write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding='utf-8')
        print(json.dumps(output, ensure_ascii=False, indent=2))
        if validation['ok']:
            print('[OK] Codigo compativel. Nao habilita acoes nativas automaticamente.')
        else:
            print('[BLOCKED] Codigo/layout diferente do perfil. Nao executar acoes nativas.')
        return 0 if validation['ok'] else 2
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print('[FAIL]', type(exc).__name__, exc)
        return 3


if __name__ == '__main__':
    sys.exit(main())
