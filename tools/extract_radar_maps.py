#!/usr/bin/env python3
"""Extract installed Pip-Boy maps into the overlay's local texture cache.

Requires Pillow. Reads BA2 archives without modifying game files.
"""
import argparse
import io
from pathlib import Path
import struct
import zlib
from PIL import Image


def extract_archive(path, destination):
    with path.open('rb') as archive:
        magic, version, kind, count, names_offset = struct.unpack('<4sI4sIQ', archive.read(24))
        if magic != b'BTDX' or kind != b'DX10' or not names_offset:
            return 0
        archive.seek(names_offset)
        names = []
        for _ in range(count):
            length, = struct.unpack('<H', archive.read(2))
            names.append(archive.read(length).decode('utf-8').lower().replace('\\', '/'))
        # Recognize the header layout by the first texture record, since
        # installed archive versions use different header extensions.
        if version not in (1, 7, 8):
            raise ValueError(f'{path.name}: unsupported BA2 version {version}')
        archive.seek(24 if version == 1 else 32 if version == 7 else 36)
        # Installed v8 DX10 archives here retain the 24-byte header.
        if version != 1:
            for header_size in (24, 32, 36):
                archive.seek(header_size)
                first = archive.read(24)
                if len(first) == 24 and first[4:8].lower().rstrip(b'\0') == b'dds' and struct.unpack_from('<H', first, 14)[0] == 24:
                    archive.seek(header_size)
                    break
            else:
                raise ValueError(f'{path.name}: unrecognized texture header')
        extracted = 0
        for name in names:
            record = struct.unpack('<I4sIBBHHHBBH', archive.read(24))
            _, _, _, _, chunk_count, chunk_size, height, width, _, dxgi_format, _ = record
            if chunk_size != 24:
                raise ValueError(f'{path.name}: unsupported chunk record size {chunk_size}')
            chunks = [struct.unpack('<QIIHHI', archive.read(24)) for _ in range(chunk_count)]
            if 'interface/' not in name or 'map' not in name or '/note/' in name:
                continue
            cursor = archive.tell()
            mip = next((chunk for chunk in chunks if chunk[3] == 0), None)
            if mip is None:
                raise ValueError(f'{name}: no full-resolution mip')
            offset, packed, unpacked, _, _, _ = mip
            archive.seek(offset)
            pixels = archive.read(packed or unpacked)
            if packed:
                pixels = zlib.decompress(pixels)
            if len(pixels) != unpacked:
                raise ValueError(f'{name}: truncated texture data')
            # Wrap the archive's compressed GPU blocks in a standard DDS file.
            header = struct.pack('<7I', 124, 0x81007, height, width, unpacked, 0, 1)
            header += bytes(44) + struct.pack('<II4s5I', 32, 4, b'DX10', 0, 0, 0, 0, 0)
            header += struct.pack('<5I', 0x1000, 0, 0, 0, 0)
            header += struct.pack('<5I', dxgi_format, 3, 0, 1, 0)
            image = Image.open(io.BytesIO(b'DDS ' + header + pixels)).convert('RGBA')
            output = destination / (name.replace('/', '_') + '.fmap')
            output.write_bytes(b'FMAP' + struct.pack('<II', width, height) + zlib.compress(image.tobytes(), 9))
            print(f'{path.name}: {name} ({width}x{height})')
            extracted += 1
            archive.seek(cursor)
        return extracted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path.home() / '.local/share/Steam/steamapps/common/Fallout 4/Data')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent / 'assets/maps')
    args = parser.parse_args()
    if not args.data.is_dir():
        parser.error(f'game Data directory not found: {args.data}')
    args.output.mkdir(parents=True, exist_ok=True)
    # Patch archives override original textures. Avoid the optional HD pack:
    # these maps already contain all the detail available to the Pip-Boy.
    archives = sorted(args.data.glob('*.ba2'), key=lambda p: ('patch' in p.name.lower(), p.name.lower()))
    count = sum(extract_archive(path, args.output) for path in archives if 'ultrahighresolution' not in path.name.lower())
    if not count:
        parser.error('no Pip-Boy map textures found')
    print(f'Prepared {count} map textures in {args.output}')


if __name__ == '__main__':
    main()
