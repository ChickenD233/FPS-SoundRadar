# embed_html.py - embeds gui_web.html into a C++ header (chunked raw string
# literals; MSVC caps a single string literal token at 16380 characters).
# Run by CMake at configure time. Usage: embed_html.py <in.html> <out.h>
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, encoding='utf-8').read()
assert ')HTML"' not in data, 'raw-string delimiter collision'

# split on line boundaries into <=3000-char chunks
chunks = []
cur = ''
for line in data.split('\n'):
    if len(cur) + len(line) > 3000:
        chunks.append(cur)
        cur = ''
    cur += line + '\n'
if cur:
    chunks.append(cur)

with open(dst, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// generated from gui_web.html - do not edit\n')
    f.write('#pragma once\n')
    f.write('static const wchar_t* kGuiHtml =\n')
    for c in chunks:
        f.write('LR"HTML(' + c + ')HTML"\n')
    f.write(';\n')
