"""GDB probes for the Fallout 4 1.11.240 power-armor inventory crash.

Attach GDB to Fallout4.exe, then: source /absolute/path/to/this/file.py
Logs to /tmp/fo4hud-inventory-refresh-<pid>.log. Stops on a null/unreadable
item definition; ordinary refreshes continue. Does not change game data.
Use `disable` to remove the probes' overhead when finished.
"""
import datetime
import gdb
import struct


IMAGE = 0x140000000
LOG = '/tmp/fo4hud-inventory-refresh-%d.log' % gdb.selected_inferior().pid


def reg(name):
    return int(gdb.parse_and_eval('$' + name))


def memory(address, size):
    return bytes(gdb.selected_inferior().read_memory(address, size))


def log(text):
    with open(LOG, 'a') as stream:
        stream.write(datetime.datetime.now(datetime.timezone.utc).isoformat() +
                     ' thread=%s %s\n' % (gdb.selected_thread().ptid, text))


class ItemProbe(gdb.Breakpoint):
    def __init__(self, rva, label, owner_register, item_register):
        super().__init__('*%#x' % (IMAGE + rva), internal=False)
        self.label = label
        self.owner_register = owner_register
        self.item_register = item_register

    def stop(self):
        owner = reg(self.owner_register)
        item = reg(self.item_register)
        # Null item-structure pointers are valid for full-object refreshes.
        if not item:
            return False
        suspicious = False
        try:
            raw = memory(item, 16)
            obj, stack = struct.unpack('<QQ', raw)
            suspicious = not obj
            details = 'object=%#x stack=%#x' % (obj, stack)
            if obj:
                identity = memory(obj + 0x14, 7)
                details += ' form=%08x type=%02x' % (
                    struct.unpack_from('<I', identity)[0], identity[6])
        except (gdb.MemoryError, gdb.error) as error:
            suspicious = True
            details = 'unreadable: %s' % error
        log('%s owner=%#x item=%#x %s\n%s' % (
            self.label, owner, item, details,
            gdb.execute('bt 12', to_string=True)))
        if suspicious:
            gdb.write('Suspicious inventory refresh captured in %s; stopped.\n'
                      'Inspect memory or use generate-core-file before continuing.\n' % LOG)
        return suspicious


class NullEventProbe(gdb.Breakpoint):
    def __init__(self):
        super().__init__('*%#x' % (IMAGE + 0x510C6F), internal=False)
        self.condition = '$rbx == 0'

    def stop(self):
        # GDB can invoke Python stop callbacks before evaluating conditions.
        if reg('rbx') != 0:
            return False
        log('NULL EVENT\n' + gdb.execute('info registers', to_string=True) +
            gdb.execute('bt 30', to_string=True))
        gdb.write('Null inventory event caught before the crash. Log: %s\n' % LOG)
        return True


ItemProbe(0xC4B890, 'enqueue task 106', 'rdx', 'r9')
ItemProbe(0x98C470, 'execute refresh', 'rcx', 'r8')
ItemProbe(0x502B70, 'apply inventory item', 'rcx', 'rdx')
NullEventProbe()
gdb.write('Inventory refresh probes installed for Fallout 4 1.11.240. Log: %s\n' % LOG)
