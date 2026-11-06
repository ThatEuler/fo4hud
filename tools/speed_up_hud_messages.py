#!/usr/bin/env python3
"""Speed up queued messages in the installed HUDFramework HUDMenu.swf.

Requires JPEXS ffdec.jar. Only Messages.as is replaced; the last messages
retain their normal timing. Keeps an original backup next to the SWF.
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('hud', type=Path)
    parser.add_argument('--ffdec', required=True, type=Path)
    args = parser.parse_args()
    hud = args.hud.resolve()
    backup = hud.with_name(hud.name + '.fo4hud-original')
    if backup.exists():
        raise SystemExit(f'Backup already exists: {backup}; restore it before reapplying')
    original = hud.read_bytes()
    with tempfile.TemporaryDirectory(prefix='fo4hud-messages-') as directory:
        work = Path(directory)

        def ffdec(*arguments):
            subprocess.run(['java', '-jar', str(args.ffdec.resolve()), '-onerror',
                            'abort', *map(str, arguments)], check=True,
                           stdout=subprocess.DEVNULL)

        ffdec('-export', 'script', work / 'before', hud)
        ffdec('-swf2xml', hud, work / 'hud.xml')
        # Frame 66 skips most of the hold; the existing fade starts at 72.
        # Check this layout before touching a different HUD implementation.
        tree = ET.parse(work / 'hud.xml')
        sprite = next(e for e in tree.iter() if e.get('spriteId') == '270')
        frame, alphas = 1, {}
        for tag in sprite.find('subTags'):
            if tag.get('type') == 'ShowFrameTag':
                frame += 1
            for color in tag:
                if color.tag == 'colorTransform':
                    alphas[frame] = color.get('alphaMultTerm', '256')
        if sprite.get('frameCount') != '85' or alphas.get(72) != '218' or alphas.get(78) != '0':
            raise SystemExit('Unsupported HUD message animation')
        source = (work / 'before/scripts/Messages.as').read_text()
        replacements = {
            '_loc11_ = 1;': '_loc11_ = this.bqueuedMessage ? 4 : 1;',
            'this.ShownMessageArray[0].FadeOut();':
                'this.ShownMessageArray[0].FadeOut();\n'
                '                     if(this.bqueuedMessage)\n'
                '                     {\n'
                '                        this.ShownMessageArray[0].gotoAndPlay(66);\n'
                '                     }',
        }
        for old, new in replacements.items():
            if source.count(old) != 1:
                raise SystemExit('Unsupported Messages.as; expected one ' + old)
            source = source.replace(old, new)
        (work / 'Messages.as').write_text(source)
        ffdec('-replace', hud, work / 'HUDMenu.swf', 'Messages', work / 'Messages.as')
        ffdec('-export', 'script', work / 'after', work / 'HUDMenu.swf')
        for before in (work / 'before/scripts').rglob('*.as'):
            relative = before.relative_to(work / 'before/scripts')
            after = work / 'after/scripts' / relative
            if relative.as_posix() != 'Messages.as' and before.read_bytes() != after.read_bytes():
                raise SystemExit(f'Unexpected script change: {relative}')
        verified = (work / 'after/scripts/Messages.as').read_text()
        if 'gotoAndPlay(66)' not in verified or 'this.bqueuedMessage ? 4 : 1' not in verified:
            raise SystemExit('Compiled message timing verification failed')
        if hud.read_bytes() != original:
            raise SystemExit('HUD changed during patching; retry')
        shutil.copy2(hud, backup)
        pending = hud.with_name(hud.name + '.fo4hud-new')
        shutil.copyfile(work / 'HUDMenu.swf', pending)
        pending.replace(hud)
        print(f'Installed faster queued messages: {hud}\nOriginal backup: {backup}')


if __name__ == '__main__':
    main()
