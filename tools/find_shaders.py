"""Searches files for captured shader microcode (recomp/shaders/*.bin, host-order dwords), to find where
the game stores its shaders. For each shader a 24-byte window from its middle is searched in big-endian
order, as the console stores it. Usage: python -I find_shaders.py <shader dir> <file>...
Prints, per file, how many shaders were found and the first offsets."""
import mmap
import os
import re
import struct
import sys

shader_dir, files = sys.argv[1], sys.argv[2:]
patterns = {}
for name in sorted(os.listdir(shader_dir)):
    if not name.endswith(".bin"):
        continue
    data = open(os.path.join(shader_dir, name), "rb").read()
    words = struct.unpack("<%dI" % (len(data) // 4), data)
    if len(words) < 24:
        continue
    middle = len(words) // 2
    window = struct.pack(">6I", *words[middle:middle + 6])
    if len(set(window)) < 6:   # skip windows of mostly zeros or repeats
        continue
    patterns[window] = name

regex = re.compile(b"|".join(re.escape(p) for p in patterns))
print("searching for %d shaders" % len(patterns))
for path in files:
    found = {}
    with open(path, "rb") as f:
        size = os.fstat(f.fileno()).st_size
        if size == 0:
            continue
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as view:
            chunk, overlap = 256 << 20, 64
            for start in range(0, size, chunk):
                end = min(size, start + chunk + overlap)
                for m in regex.finditer(view[start:end]):
                    found.setdefault(patterns[m.group(0)], []).append(start + m.start())
    kinds = {"ps": 0, "vs": 0}
    for name in found:
        kinds[name[:2]] += 1
    print("%s: %d shaders found (%d pixel, %d vertex)" % (os.path.basename(path), len(found), kinds["ps"], kinds["vs"]))
    for name, offsets in sorted(found.items(), key=lambda item: item[1][0])[:8]:
        print("   %s at 0x%X%s" % (name, offsets[0], " (+%d more)" % (len(offsets) - 1) if len(offsets) > 1 else ""))
