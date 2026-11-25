"""Synthetic PE fixtures: run with `py -3 -m unittest test_build_identity.py\`."""
from pathlib import Path
import shutil
import struct
import tempfile
import unittest

from build_identity import read_fingerprint, validate


def make_pe(path: Path) -> None:
    data = bytearray(0x400)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    coff = 0x80
    data[coff:coff + 4] = b'PE\x00\x00'
    struct.pack_into('<HHIIIHH', data, coff + 4, 0x8664, 1, 0x6B06FF0F, 0, 0, 0xF0, 0)
    opt = coff + 24
    struct.pack_into('<H', data, opt, 0x20B)
    struct.pack_into('<I', data, opt + 56, 0xF38000)
    section = opt + 0xF0
    data[section:section + 8] = b'.text\x00\x00\x00'
    struct.pack_into('<IIII', data, section + 8, 0x100, 0x1000, 0x100, 0x200)
    data[0x200:0x300] = bytes(i % 256 for i in range(256))
    path.write_bytes(data)


class BuildIdentityTests(unittest.TestCase):
    def test_reinstall_preserves_code_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            original = Path(tmp) / 'original.exe'
            modified = Path(tmp) / 'reinstalled.exe'
            make_pe(original)
            initial = read_fingerprint(original)
            profile = {k: initial[k] for k in ('timestamp', 'sizeOfImage', 'textRva',
                                               'textRawSize', 'textSha256', 'textFnv1a64')}
            profile['installationSha256Examples'] = [initial['sha256FullFile']]
            self.assertTrue(validate(initial, profile)['ok'])
            shutil.copyfile(original, modified)
            with modified.open('ab') as file:
                file.write(b'non-code-overlay')
            check = validate(read_fingerprint(modified), profile)
            self.assertTrue(check['ok'])
            self.assertEqual(check['status'], 'COMPATIBLE_CODE_DIFFERENT_INSTALLATION')
            self.assertFalse(check['nativeActionsReady'])

            with modified.open('r+b') as file:
                file.seek(0x220)
                file.write(b'\xAB')
            changed = validate(read_fingerprint(modified), profile)
            self.assertFalse(changed['ok'])
            self.assertFalse(changed['checks']['textSha256'])

    def test_metadata_change_blocks(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'test.exe'
            make_pe(path)
            f = read_fingerprint(path)
            profile = {k: f[k] for k in ('timestamp', 'sizeOfImage', 'textRva',
                                        'textRawSize', 'textSha256', 'textFnv1a64')}
            profile['timestamp'] = '0x1'
            self.assertFalse(validate(f, profile)['ok'])


if __name__ == '__main__':
    unittest.main()
