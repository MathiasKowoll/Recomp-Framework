#!/usr/bin/env python3
"""Extract the game's application icon (the first RT_GROUP_ICON's largest image) as a PNG.

usage: app_icon.py GAME_EXE OUT.png
Windows stores 256x256 icons as embedded PNG data; smaller ones are DIBs, which are wrapped in a
one-image .ico that sips can convert.
"""
import struct, subprocess, sys, tempfile, os
import lief

exe, out = sys.argv[1], sys.argv[2]
b = lief.parse(exe)
icons = {i.id: i for i in b.resources_manager.icons}
best = None
for t in b.resources.childs:
    if t.id != 14:  # RT_GROUP_ICON
        continue
    group = list(t.childs)[0]
    d = bytes(list(group.childs)[0].content)
    n = struct.unpack_from("<H", d, 4)[0]
    members = [struct.unpack_from("<BBBBHHIH", d, 6 + 14 * k) for k in range(n)]
    w, h, _, _, _, bits, _, iid = max(members, key=lambda m: ((m[0] or 256), m[5]))
    best = icons[iid]
    break
if best is None:
    sys.exit("no icon group in " + exe)
data = bytes(best.pixels)
if data[:8] == b"\x89PNG\r\n\x1a\n":
    open(out, "wb").write(data)
else:
    with tempfile.TemporaryDirectory() as tmp:
        ico = os.path.join(tmp, "icon.ico")
        best.save(ico)
        subprocess.run(["sips", "-s", "format", "png", ico, "--out", out], check=True, capture_output=True)
print("%s: %s icon -> %s" % (exe, "PNG" if data[:4] == b"\x89PNG" else "DIB", out))
