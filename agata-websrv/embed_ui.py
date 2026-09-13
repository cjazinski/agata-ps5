#!/usr/bin/env python3
"""Embed ui.html into ui.h as a C string for the PS5 payload."""
import sys

src, dst = sys.argv[1], sys.argv[2]
data = open(src, 'rb').read()

# minimize a touch: strip leading/trailing blank lines only (keep it simple/safe)
text = data.decode('utf-8')

out = ['/* auto-generated from ui.html — do not edit */',
       '#ifndef AGATA_UI_H', '#define AGATA_UI_H', '',
       'static const char UI_HTML[] =']

# chunk into C string literals, 200 chars per line
CHUNK = 200
i = 0
lines = []
while i < len(text):
    chunk = text[i:i+CHUNK]
    esc = chunk.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n').replace('\r', '')
    esc = esc.replace('?', '\\?')  # avoid C trigraph warnings
    lines.append('  "%s"' % esc)
    i += CHUNK
out.append('\n'.join(lines))
out.append(';')
out.append('#define UI_HTML_LEN %d' % len(text.encode('utf-8')))
out.append('#endif')

open(dst, 'w').write('\n'.join(out) + '\n')
print('ui.h: %d bytes embedded' % len(data))
