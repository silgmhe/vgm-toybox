"""Removes the duplicate `rate` declaration the previous patch left in the three ROM-check decoders.

The duplicate sits between the blank line that follows the original declaration and the new
`const bool measured` line, so that span is what gets cut.
"""
import io

BASE = r'h:\CbProject\vgmplay-libvgm\plugin\Source\chips'
FILES = ['QSoundDecoder.cpp', 'K053260Decoder.cpp', 'K054539Decoder.cpp']

for name in FILES:
    path = BASE + '\\' + name
    text = io.open(path, encoding='utf-8', newline='').read().replace('\r\n', '\n')
    if 'const bool measured' not in text:
        print(name, 'no marker')
        continue

    marker = text.index('const bool measured')
    blank = text.rindex('\n\n', 0, marker)
    span = text[blank + 2:marker]
    if 'const double rate' not in span:
        print(name, 'nothing to remove')
        continue

    text = text[:blank + 2] + text[marker:]
    io.open(path, 'w', encoding='utf-8', newline='').write(text.replace('\n', '\r\n'))
    print(name, 'fixed')
