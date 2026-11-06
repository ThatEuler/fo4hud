#!/usr/bin/env python3
"""Build a local dialogue-affinity index from installed plugins and scripts.

Requires the Champollion PEX decompiler (--champollion). No game assets are
downloaded or modified. Generated data is local, not a redistributed wiki table.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import tempfile
import zlib
from pathlib import Path


class Reader:
    def __init__(self, data):
        self.data, self.pos = data, 0

    def take(self, n):
        if n < 0 or self.pos + n > len(self.data):
            raise ValueError('Truncated record')
        out = self.data[self.pos:self.pos+n]
        self.pos += n
        return out

    def number(self, fmt):
        return struct.unpack('<' + fmt, self.take(struct.calcsize('<' + fmt)))[0]

    def text(self):
        return self.take(self.number('H')).decode('utf-8')


def subrecords(data):
    r = Reader(data)
    extended = None
    while r.pos < len(data):
        tag, size = r.take(4), r.number('H')
        if tag == b'XXXX':
            if size != 4:
                raise ValueError('Bad extended subrecord')
            extended = r.number('I')
            continue
        yield tag, r.take(extended if extended is not None else size)
        extended = None


def records(data, start=0, end=None, topic=None):
    end = len(data) if end is None else end
    pos = start
    while pos < end:
        if pos + 24 > end:
            raise ValueError('Truncated plugin header')
        tag, size, flags, fid = struct.unpack_from('<4sIII', data, pos)
        if tag == b'GRUP':
            if size < 24 or pos + size > end:
                raise ValueError('Bad group size')
            group_type = struct.unpack_from('<I', data, pos+12)[0]
            yield from records(data, pos+24, pos+size, flags if group_type == 7 else topic)
            pos += size
            continue
        if pos + 24 + size > end:
            raise ValueError('Bad record size')
        if tag in (b'TES4', b'QUST', b'DIAL', b'INFO', b'KYWD'):
            body = data[pos+24:pos+24+size]
            if flags & 0x40000:
                unpacked = struct.unpack_from('<I', body)[0]
                body = zlib.decompress(body[4:])
                if len(body) != unpacked:
                    raise ValueError('Bad compressed record')
            yield tag, fid, topic, dict(subrecords(body))
        pos += 24 + size


def vmad(data, resolve):
    r = Reader(data)
    version, fmt, count = r.number('H'), r.number('H'), r.number('H')
    if version > 6 or fmt not in (1, 2) or count > 1024:
        raise ValueError('Unsupported VMAD')

    def value(kind):
        if kind == 1:
            raw = r.take(8)
            return resolve(struct.unpack_from('<I', raw, 4 if fmt == 2 else 0)[0])
        if kind == 2:
            return r.text()
        if kind == 3:
            return r.number('i')
        if kind == 4:
            return r.number('f')
        if kind == 5:
            return bool(r.number('B'))
        if 11 <= kind <= 15:
            n = r.number('I')
            if n > 65536:
                raise ValueError('Oversized VMAD array')
            return [value(kind-10) for _ in range(n)]
        raise ValueError('Unsupported VMAD property type')

    scripts = {}
    for _ in range(count):
        name = r.text().lower()
        r.number('B')
        props = {}
        try:
            for _ in range(r.number('H')):
                prop, kind = r.text().lower(), r.number('B')
                r.number('B')
                props[prop] = value(kind)
        except ValueError:
            scripts['unsupported'] = {}
            return scripts
        scripts[name] = props
    return scripts


def archive_scripts(path):
    with path.open('rb') as f:
        magic, version, kind, count, names_at = struct.unpack('<4sI4sIQ', f.read(24))
        if magic != b'BTDX' or kind != b'GNRL' or version not in (1, 7, 8):
            return
        f.seek(names_at)
        names = []
        for _ in range(count):
            n = struct.unpack('<H', f.read(2))[0]
            names.append(f.read(n).decode().replace('\\', '/').lower())
        for i, name in enumerate(names):
            if not name.endswith('.pex') or 'affinityevent' not in name:
                continue
            f.seek(24 + i*36)
            fields = struct.unpack('<I4sIIQIII', f.read(36))
            offset, packed, size = fields[4:7]
            f.seek(offset)
            data = f.read(packed or size)
            data = zlib.decompress(data) if packed else data
            if len(data) != size:
                raise ValueError('Truncated script archive')
            yield name, data


NAMES = {'valentine': 'Nick Valentine', 'x688': 'X6-88', 'longfellow': 'Old Longfellow',
         'oldlongfellow': 'Old Longfellow', 'gage': 'Porter Gage', 'danse': 'Paladin Danse'}


def event_reactions(source):
    """Extract explicit dialogue dispatch branches; retain nested uncertainty.

    Quest-stage handlers deliberately do not count as immediate dialogue effects.
    No guessing from response text or companion personality.
    """
    out = {}
    for match in re.finditer(r'(?ims)^Function HandleDialogueBump_(\w+)\([^\n]*\n(.*?)^EndFunction', source):
        quest, body = match.group(1).lower(), match.group(2)
        stack = []
        for raw in body.splitlines():
            line = raw.split(';', 1)[0].strip()
            branch = re.match(r'(?i)^(if|elseif)\s+(.+)', line)
            if branch:
                if branch[1].lower() == 'elseif' and stack:
                    stack.pop()
                condition = branch[2].strip()
                event = re.fullmatch(r'(?i)eventID\s*==\s*(\d+)', condition)
                wrapper = re.fullmatch(r'(?i)eventQuest\s*==\s*\w+(?:\s+as\s+Quest)?', condition)
                stack.append(('event', int(event[1])) if event else ('wrapper', None) if wrapper else ('condition', condition))
            elif re.match(r'(?i)^else\b', line):
                if stack:
                    stack[-1] = ('condition', 'else')
            elif re.match(r'(?i)^endif\b', line):
                if stack:
                    stack.pop()
            reaction = re.fullmatch(r'(?i)Self\.(\w+)_(Likes|Loves|Dislikes|Hates)\((?:True|False)\)', line)
            events = [v for k, v in stack if k == 'event']
            if reaction and len(events) == 1:
                who, effect = reaction[1], reaction[2].lower()
                name = NAMES.get(who.lower(), who)
                entry = {'companion': name, 'reaction': effect,
                         'conditional': any(k == 'condition' for k, _ in stack)}
                out.setdefault((quest, events[0]), []).append(entry)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=Path.home()/'.local/share/Steam/steamapps/common/Fallout 4/Data')
    parser.add_argument('--plugins', type=Path, default=Path.home()/'.local/share/Steam/steamapps/compatdata/377160/pfx/drive_c/users/steamuser/AppData/Local/Fallout4/Plugins.txt')
    parser.add_argument('--champollion', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parent.parent/'assets/dialogue_affinity.json')
    args = parser.parse_args()
    paths = {p.name.lower(): p for p in args.data.iterdir() if p.is_file()}
    order, seen, evidence, forms = [], set(), {}, {}

    def track(path):
        evidence[str(path.resolve())] = {'size': path.stat().st_size, 'mtime_ns': path.stat().st_mtime_ns,
                                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}
        if path.resolve() == args.plugins.resolve():
            evidence[str(path.resolve())]['active_plugins'] = [line[1:].strip().lower()
                for line in path.read_text(encoding='utf-8-sig').splitlines() if line.startswith('*')]

    def load(name):
        name = name.lower()
        if name in seen:
            return
        seen.add(name)
        path = paths.get(name)
        if path is None:
            raise ValueError('Missing active plugin/master: ' + name)
        items = list(records(path.read_bytes()))
        masters = [v.rstrip(b'\0').decode().lower() for t, v in subrecords(path.read_bytes()[24:24+struct.unpack_from('<I', path.read_bytes(), 4)[0]]) if t == b'MAST']
        for master in masters:
            load(master)
        owners = masters + [name]

        def resolve(raw):
            if not raw:
                return ''
            idx = raw >> 24
            if idx >= len(owners):
                raise ValueError('Invalid plugin-local form index')
            return f'{owners[idx]}|{raw & 0xFFFFFF:06x}'

        for tag, fid, topic, fields in items:
            if tag == b'TES4':
                continue
            scripts = {}
            if b'VMAD' in fields:
                try:
                    scripts = vmad(fields[b'VMAD'], resolve)
                except ValueError:
                    scripts = {'unsupported': {}}
            forms[resolve(fid)] = {'type': tag.decode(), 'edid': fields.get(b'EDID', b'').rstrip(b'\0').decode(errors='replace'),
                'topic': resolve(topic) if topic else '', 'quest': resolve(struct.unpack('<I', fields[b'QNAM'])[0]) if tag == b'DIAL' and b'QNAM' in fields else '',
                'scripts': scripts, 'last': name}
        order.append(name)
        track(path)

    for name in ['fallout4.esm', 'dlcrobot.esm', 'dlcworkshop01.esm', 'dlcworkshop02.esm', 'dlccoast.esm', 'dlcworkshop03.esm', 'dlcnukaworld.esm', 'dlcultrahighresolution.esm']:
        if name in paths:
            load(name)
    ccc = args.data.parent/'Fallout4.ccc'
    if ccc.exists():
        track(ccc)
        for name in ccc.read_text(encoding='utf-8-sig').splitlines():
            if name.strip().lower() in paths:
                load(name.strip())
    track(args.plugins)
    for line in args.plugins.read_text(encoding='utf-8-sig').splitlines():
        if line.startswith('*'):
            load(line[1:].strip())

    scripts = {}
    for name in order:
        stem = Path(name).stem
        archives = ['fallout4 - misc.ba2'] if name == 'fallout4.esm' else [stem+' - main.ba2']
        for archive in archives:
            path = paths.get(archive)
            if path:
                found = dict(archive_scripts(path))
                if found:
                    track(path)
                    scripts.update(found)
    for p in (args.data/'Scripts').rglob('*.pex'):
        if 'affinityevent' in p.name.lower():
            track(p)
            scripts['scripts/'+str(p.relative_to(args.data/'Scripts')).lower()] = p.read_bytes()
    reactions = {}
    with tempfile.TemporaryDirectory(prefix='fo4hud-affinity-') as tmp:
        pex, psc = Path(tmp)/'pex', Path(tmp)/'psc'
        pex.mkdir(); psc.mkdir()
        for i, (name, data) in enumerate(scripts.items()):
            (pex/f'{i}.pex').write_bytes(data)
        subprocess.run([str(args.champollion.resolve()), '-r', '-p', str(psc), str(pex)], check=True, capture_output=True)
        for p in psc.rglob('*.psc'):
            for key, values in event_reactions(p.read_text()).items():
                reactions.setdefault(key, []).extend(values)

    # EDIDs in the dispatcher normally match the property names. Resolve aliases
    # through the installed quest VMAD where a property uses another EDID.
    aliases, companions = {}, {}
    ref_names = {'caitref': 'Cait', 'curieref': 'Curie', 'codsworthref': 'Codsworth',
                 'bospaladindanseref': 'Paladin Danse', 'deaconref': 'Deacon', 'hancockref': 'Hancock',
                 'compmaccreadyref': 'MacCready', 'piperref': 'Piper', 'prestonref': 'Preston',
                 'strongref': 'Strong', 'valentineref': 'Nick Valentine', 'x688ref': 'X6-88',
                 'dlc03longfellowref': 'Old Longfellow', 'dlc03_companionoldlongfellowref': 'Old Longfellow', 'dlc04gageref': 'Porter Gage'}
    for form in forms.values():
        if form['type'] == 'QUST':
            for script, props in form['scripts'].items():
                if 'affinityevent' in script:
                    for prop, key in props.items():
                        if prop in ref_names and isinstance(key, str) and key:
                            companions[key] = ref_names[prop]
                        if isinstance(key, str) and key in forms and forms[key]['type'] == 'QUST':
                            aliases[prop] = forms[key]['edid'].lower()
    for (quest, event), values in list(reactions.items()):
        actual = aliases.get(quest, quest)
        if actual != quest:
            reactions.setdefault((actual, event), []).extend(values)

    indexed = {}
    for key, form in forms.items():
        if form['type'] != 'INFO':
            continue
        topic = forms.get(form['topic'], {})
        quest = forms.get(topic.get('quest', ''), {}).get('edid', '').lower()
        values = []
        for script, props in form['scripts'].items():
            match = re.fullmatch(r'ca_db_event(\d+)', script)
            if match:
                values.extend(reactions.get((quest, int(match[1])), []))
        # Include both original and overriding plugin identities, so runtime
        # rejects an unindexed override instead of applying a vanilla prediction.
        if values:
            unique = {json.dumps(v, sort_keys=True): v for v in values}
            indexed[key] = {'last': form['last'], 'reactions': list(unique.values())}
    if not indexed:
        raise ValueError('No affinity dialogue records found; refusing an empty index')
    result = {'version': 1, 'sources': evidence, 'records': indexed, 'companions': companions,
              'coverage': 'Explicit CA_DB_Event dialogue dispatch only; potential reactions, not guaranteed gains.'}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(f'Indexed {len(indexed)} dialogue records, {len(reactions)} event branches from {len(scripts)} scripts.')
    print(args.output)


if __name__ == '__main__':
    main()
