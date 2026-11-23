#!/usr/bin/env python3
"""Write the time the app was linked into its app description.

ESP-IDF's build date and time are __DATE__ and __TIME__ in its own
esp_app_desc.c. An incremental build doesn't recompile that file, so the web
UI showed the date of the last clean build. The build runs this right after
the app is linked, before the image is made from the ELF, so the image's
checksum and ELF hash cover the new date.

    stamp_build_date.py <app.elf> <project name>

SOURCE_DATE_EPOCH, if set, is used instead of the current time.
"""
import os
import struct
import sys
import time

MAGIC = 0xABCD5432                  # ESP_APP_DESC_MAGIC_WORD
NAME_AT, TIME_AT, DATE_AT = 0x30, 0x50, 0x60   # esp_app_desc_t: project_name, time, date
MONTHS = 'Jan Feb Mar Apr May Jun Jul Aug Sep Oct Nov Dec'.split()


def find_section(elf, wanted):
    if elf[:4] != b'\x7fELF' or elf[4] != 1 or elf[5] != 1:
        sys.exit('stamp_build_date: not a little-endian 32-bit ELF file')
    shoff, = struct.unpack_from('<I', elf, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from('<HHH', elf, 0x2E)

    def header(i):
        return struct.unpack_from('<10I', elf, shoff + i * shentsize)

    names = header(shstrndx)[4]
    for i in range(shnum):
        h = header(i)
        start = names + h[0]
        if elf[start:elf.index(b'\0', start)] == wanted:
            return h[4], h[5]       # file offset, size
    sys.exit('stamp_build_date: no %s section' % wanted.decode())


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    path, project = sys.argv[1], sys.argv[2].encode()
    with open(path, 'r+b') as f:
        elf = f.read()
        off, size = find_section(elf, b'.flash.appdesc')
        name = elf[off + NAME_AT:off + NAME_AT + 32].split(b'\0')[0]
        if size < DATE_AT + 16 or struct.unpack_from('<I', elf, off)[0] != MAGIC or name != project:
            sys.exit('stamp_build_date: no app description for %s at the start of .flash.appdesc'
                     % project.decode())
        epoch = os.environ.get('SOURCE_DATE_EPOCH')
        now = time.gmtime(int(epoch)) if epoch else time.localtime()
        # the formats of __TIME__ ("08:29:05") and __DATE__ ("Nov  3 2026")
        stamp_time = '%02d:%02d:%02d' % (now.tm_hour, now.tm_min, now.tm_sec)
        stamp_date = '%s %2d %d' % (MONTHS[now.tm_mon - 1], now.tm_mday, now.tm_year)
        f.seek(off + TIME_AT)
        f.write(stamp_time.encode().ljust(16, b'\0'))
        f.seek(off + DATE_AT)
        f.write(stamp_date.encode().ljust(16, b'\0'))


if __name__ == '__main__':
    main()
