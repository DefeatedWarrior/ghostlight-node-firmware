#!/usr/bin/env python3
"""Regenerate components/web_server/web_files.h from web_ui/index.html and main.js.

Run from the project root after editing anything in components/web_server/web_ui/:

    python3 tools/gen_web_files.py

The files are stored gzip-compressed (served with Content-Encoding: gzip),
which cuts flash use and page-load time over WiFi by about 75 %.
"""
import gzip
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UI = os.path.join(ROOT, 'components', 'web_server', 'web_ui')
OUT = os.path.join(ROOT, 'components', 'web_server', 'web_files.h')


def file_to_c_array(filename, var_name):
    with open(filename, 'rb') as f:
        raw = f.read()
    data = gzip.compress(raw, compresslevel=9, mtime=0)   # mtime=0: reproducible
    lines = [f'/* {os.path.basename(filename)}: {len(raw)} bytes, {len(data)} gzipped */',
             f'static const uint8_t {var_name}[] = {{']
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    lines.append('};')
    lines.append(f'static const size_t {var_name}_len = sizeof({var_name});')
    print(f'{os.path.basename(filename)}: {len(raw)} -> {len(data)} bytes')
    return '\n'.join(lines)


html_arr = file_to_c_array(os.path.join(UI, 'index.html'), 'index_html')
js_arr = file_to_c_array(os.path.join(UI, 'main.js'), 'main_js')

content = '''/**
 * @file web_files.h
 * @brief Embedded web files (auto-generated, gzip-compressed)
 * DO NOT EDIT MANUALLY - regenerate with: python3 tools/gen_web_files.py
 */

#ifndef WEB_FILES_H
#define WEB_FILES_H

#include <stdint.h>
#include <stddef.h>

#define WEB_FILES_GZIPPED 1

''' + html_arr + '\n\n' + js_arr + '\n\n#endif // WEB_FILES_H\n'

with open(OUT, 'w') as f:
    f.write(content)
print(f'wrote {os.path.relpath(OUT, ROOT)}')
