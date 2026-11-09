#!/usr/bin/env python3
"""Generate vendor container identities and labels from installed plugin records.

FACT.VENC identifies vendor references and REFR.NAME confirms their bases. Plugin namespaces avoid load-order dependent IDs.
"""
import argparse
import json
import mmap
import re
import struct
import zlib
from pathlib import Path
from extract_dialogue_affinity import subrecords


def records(data, start=0, end=None):
    end = len(data) if end is None else end
    pos = start
    while pos < end:
        tag, size, flags, fid = struct.unpack_from('<4sIII', data, pos)
        if tag == b'GRUP':
            if size < 24 or pos + size > end:
                raise ValueError('Invalid group')
            yield from records(data, pos + 24, pos + size)
            pos += size
            continue
        if pos + 24 + size > end:
            raise ValueError('Truncated record')
        if tag in (b'TES4', b'CONT', b'FACT', b'REFR', b'NPC_'):
            body = data[pos+24:pos+24+size]
            if flags & 0x40000:
                body = zlib.decompress(body[4:])
            yield tag, fid, flags, list(subrecords(body))
        pos += 24 + size


def string_tables(directory, language):
    tables = {}
    for path in sorted(directory.glob('*')):
        if path.suffix.lower() != '.ba2':
            continue
        with path.open('rb') as f:
            magic, version, kind, count, names_at = struct.unpack('<4sI4sIQ', f.read(24))
            if magic != b'BTDX' or kind != b'GNRL' or version not in (1, 7, 8):
                continue
            f.seek(names_at)
            names = [f.read(struct.unpack('<H', f.read(2))[0]).decode('utf-8', errors='replace').replace('\\', '/').lower() for _ in range(count)]
            for i, name in enumerate(names):
                if not name.endswith('_' + language + '.strings'):
                    continue
                f.seek(24 + i*36)
                fields = struct.unpack('<I4sIIQIII', f.read(36))
                offset, packed, size = fields[4:7]
                f.seek(offset)
                data = f.read(packed or size)
                if packed:
                    data = zlib.decompress(data)
                n, length = struct.unpack_from('<II', data)
                begin = 8 + n*8
                table = {}
                for j in range(n):
                    sid, off = struct.unpack_from('<II', data, 8+j*8)
                    start = begin + off
                    table[sid] = data[start:data.index(b'\0', start)].decode('utf-8', errors='replace')
                tables[Path(name).name.rsplit('_', 1)[0]] = table
    return tables


def generate(directory, language='en'):
    tables = string_tables(directory, language)
    paths = {p.name.lower(): p for p in directory.iterdir() if p.suffix.lower() in ('.esm', '.esp', '.esl')}
    all_records = {}
    loaded = set()

    def load(plugin):
        if plugin in loaded:
            return
        loaded.add(plugin)
        with paths[plugin].open('rb') as f:
            data = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            header_size = struct.unpack_from('<I', data, 4)[0]
            header = list(subrecords(data[24:24+header_size]))
            masters = [v.rstrip(b'\0').decode().lower() for t, v in header if t == b'MAST']
            for master in masters:
                if master in paths:
                    load(master)
            owners = masters + [plugin]
            def key(fid):
                index = fid >> 24
                return (owners[index], fid & 0xFFFFFF) if index < len(owners) else None
            localized = bool(struct.unpack_from('<I', data, 8)[0] & 0x80)
            for tag, fid, flags, subs in records(data):
                if tag == b'TES4':
                    continue
                k = key(fid)
                if not k:
                    continue
                fields = dict(subs)
                name = fields.get(b'FULL', b'')
                if localized and len(name) == 4:
                    name = tables.get(Path(plugin).stem, {}).get(struct.unpack('<I', name)[0], '')
                else:
                    name = name.rstrip(b'\0').decode('utf-8', errors='replace')
                links = {t: key(struct.unpack('<I', fields[t][:4])[0]) for t in (b'NAME', b'VENC') if len(fields.get(t, b'')) >= 4}
                factions = [key(struct.unpack('<I', v[:4])[0]) for t, v in subs if t == b'SNAM' and len(v) == 5] if tag == b'NPC_' else []
                all_records[k] = (tag, fields.get(b'EDID', b'').rstrip(b'\0').decode(errors='replace'), name, links, factions, flags)
            data.close()
    for plugin in sorted(paths):
        load(plugin)
    result = {}
    for k, (tag, edid, name, links, factions, flags) in all_records.items():
        if tag != b'FACT' or flags & 0x20 or not links.get(b'VENC'):
            continue
        ref = links[b'VENC']
        record = all_records.get(ref)
        if not record or record[0] != b'REFR' or record[5] & 0x20:
            continue
        base = record[3].get(b'NAME')
        if not base or all_records.get(base, (None,))[0] != b'CONT':
            continue
        people = sorted({r[2] for r in all_records.values() if r[0] == b'NPC_' and k in r[4] and r[2] and 'Brahmin' not in r[2] and 'Guard' not in r[2]})
        label = ' / '.join(people) if people else name
        if not label:
            label = re.sub(r'(?<=[a-z])(?=[A-Z])', ' ', edid)
        result[(ref, base)] = label + ("' vendor chest" if label.endswith('s') else "'s vendor chest")
    bases = {}
    for (ref, base), label in result.items():
        bases.setdefault(base, set()).add(label)
    for k, (tag, edid, name, links, factions, flags) in all_records.items():
        if tag == b'CONT' and not flags & 0x20 and 'vendor' in edid.lower():
            bases.setdefault(k, set())
    labels = {}
    for k, names in bases.items():
        if len(names) == 1:
            labels[k] = next(iter(names))
        else:
            edid = all_records[k][1]
            text = re.sub(r'(?<=[a-z])(?=[A-Z0-9])', ' ', edid.replace('_', ' '))
            text = re.sub(r'^(?:DLC[0-9]+|DN[0-9]+|POI[A-Z0-9]+|REMerchant[0-9]+) *', '', text)
            labels[k] = text.strip() or 'Vendor chest'
    return result, labels


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--data', type=Path, default=Path.home()/'.local/share/Steam/steamapps/common/Fallout 4/Data')
    p.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent/'vendor_chest_records.h')
    p.add_argument('--language', default='en')
    a = p.parse_args()
    result, bases = generate(a.data, a.language)
    lines = ['// Generated by tools/extract_vendor_chests.py from FACT.VENC, REFR.NAME and vendor CONT records.', '#pragma once', '#include <cstdint>', 'struct Fo4VendorChestRecord { const char* reference_plugin; uint32_t reference_id; const char* base_plugin; uint32_t base_id; const char* name; };', 'inline constexpr Fo4VendorChestRecord kFo4VendorChests[] = {']
    for (ref, base), label in sorted(result.items()):
        lines.append('    {%s, 0x%06X, %s, 0x%06X, %s},' % (json.dumps(ref[0]), ref[1], json.dumps(base[0]), base[1], json.dumps(label, ensure_ascii=False)))
    lines += ['};', 'struct Fo4VendorChestBaseRecord { const char* plugin; uint32_t id; const char* name; };', 'inline constexpr Fo4VendorChestBaseRecord kFo4VendorChestBases[] = {']
    for base, label in sorted(bases.items()):
        lines.append('    {%s, 0x%06X, %s},' % (json.dumps(base[0]), base[1], json.dumps(label, ensure_ascii=False)))
    lines.append('};')
    a.output.write_text('\n'.join(lines)+'\n')
    print(f'Prepared {len(result)} vendor references and {len(bases)} stock bases in {a.output}')


if __name__ == '__main__':
    main()
