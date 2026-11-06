"""Source from an attached GDB to preserve Fallout 4 crash evidence.

FO4HUD_CRASH_DIR must name a unique directory. All monitored faults and
the known null inventory event leave the process stopped for inspection.
Some faults may be handled by Wine; resume those manually after inspection.

FO4HUD_PROTON_LOG can select a Proton log file. Otherwise, use
steam-377160.log in PROTON_LOG_DIR, falling back to the user's home directory.
Paths accept ~ and environment variables such as $HOME.
"""
import datetime
import json
import os
from pathlib import Path
import shutil

import gdb


def local_path(value):
    return Path(os.path.expandvars(value)).expanduser()


ROOT = local_path(os.environ['FO4HUD_CRASH_DIR'])
ROOT.mkdir(parents=True, exist_ok=True)
PID = gdb.selected_inferior().pid
PROTON = local_path(os.environ['FO4HUD_PROTON_LOG']) if os.environ.get('FO4HUD_PROTON_LOG') else (
    local_path(os.environ.get('PROTON_LOG_DIR') or str(Path.home())) / 'steam-377160.log'
)
sequence = 0


def capture(reason):
    global sequence
    sequence += 1
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    dest = ROOT / ('%04d-%s' % (sequence, stamp))
    dest.mkdir()
    commands = ['info threads', 'info all-registers', 'x/24i $pc',
                'x/128gx $sp', 'bt full 40', 'thread apply all bt 32',
                'info proc mappings', 'info sharedlibrary']
    with (dest / 'debugger.txt').open('w') as stream:
        stream.write('UTC=%s PID=%s reason=%s\n' % (stamp, PID, reason))
        for command in commands:
            stream.write('\n(gdb) %s\n' % command)
            try:
                stream.write(gdb.execute(command, to_string=True))
            except Exception as error:
                stream.write('Capture error: %s\n' % error)
    inferior = gdb.selected_inferior()
    for name, expression, size in [('stack', '$sp', 65536), ('instruction', '$pc', 256)]:
        try:
            address = int(gdb.parse_and_eval(expression))
            (dest / (name + '.bin')).write_bytes(bytes(inferior.read_memory(address, size)))
            (dest / (name + '.json')).write_text(json.dumps({'address': hex(address), 'size': size}))
        except Exception as error:
            (dest / (name + '-error.txt')).write_text(str(error))
    try:
        shutil.copy2('/proc/%s/maps' % PID, dest / 'maps.txt')
        shutil.copy2(PROTON, dest / 'proton.log')
    except OSError as error:
        (dest / 'copy-error.txt').write_text(str(error))
    (ROOT / 'latest-event.txt').write_text(str(dest) + '\n' + reason + '\n')
    gdb.write('Crash evidence saved: %s (%s)\n' % (dest, reason))


class KnownNullEvent(gdb.Breakpoint):
    def stop(self):
        return int(gdb.parse_and_eval('$rbx')) == 0


def stopped(event):
    if isinstance(event, gdb.SignalEvent):
        capture(event.stop_signal)
        # Do not auto-continue: preserve the live process even if this is a
        # first-chance fault that Wine might otherwise handle.
        gdb.write('Game left stopped for inspection; resume manually if appropriate.\n')
    elif isinstance(event, gdb.BreakpointEvent):
        capture('known null inventory event before Fallout4.exe+0x510C6F')
        gdb.write('Game left stopped before the null dereference.\n')


def exited(event):
    (ROOT / 'exit.txt').write_text('UTC=%s exit_code=%s\n' % (
        datetime.datetime.now(datetime.timezone.utc).isoformat(),
        getattr(event, 'exit_code', 'unavailable')))
    try:
        shutil.copy2(PROTON, ROOT / 'proton-at-exit.log')
    except OSError:
        pass


gdb.events.stop.connect(stopped)
gdb.events.exited.connect(exited)
# This probe is specific to the verified 1.11.240 executable.
probe = KnownNullEvent('*0x140510c6f')
probe.condition = '$rbx == 0'
gdb.write('Crash watcher ready for PID %s. Evidence: %s\n' % (PID, ROOT))
