"""Finds the Xbox 360 shader containers (magic 102A1100 pixel / 102A1101 vertex) stored in the game's
files and compares their microcode with the shaders captured while playing (recomp/shaders/*.bin,
host-order dwords). Container header, big-endian dwords: magic, virtual size, physical size, 0,
constant table offset, definition table offset, shader record offset; the record starts with the
microcode's offset in the physical part (after the float literals) and its size. Usage: python -I shader_containers.py <shader dir> <file>...
Prints how many distinct stored shaders there are and how many captured shaders are among them."""
import os
import struct
import sys

shader_dir, files = sys.argv[1], sys.argv[2:]
MAGIC = bytes([0x10, 0x2A, 0x11])
stored = {"ps": {}, "vs": {}}   # microcode bytes -> (file, offset) of the first copy
copies = {"ps": 0, "vs": 0}
for path in files:
    with open(path, "rb") as f:
        position, tail = 0, b""
        while True:
            chunk = f.read(64 << 20)
            if not chunk:
                break
            data = tail + chunk
            base = position - len(tail)
            p = data.find(MAGIC)
            while p >= 0:
                if p + 12 <= len(data) and data[p + 3] in (0, 1):
                    kind = "vs" if data[p + 3] else "ps"
                    # virtual size (tables), physical size (literals + microcode), ..., shader record offset
                    block = data[p:p + 0x10000]
                    if len(block) < 0x28:
                        f_here = f.tell(); f.seek(base + p); block = f.read(0x10000); f.seek(f_here)
                    elif p + 0x10000 > len(data):
                        f_here = f.tell(); f.seek(base + p); block = f.read(0x10000); f.seek(f_here)
                    virtual, physical = struct.unpack(">II", block[4:12])
                    record = struct.unpack(">I", block[24:28])[0]
                    if 0x24 <= virtual < 0x8000 and 12 <= physical < 0x8000 and record + 8 <= virtual:
                        start, size = struct.unpack(">II", block[record:record + 8])
                        code = block[virtual + start:virtual + start + size]
                        if size >= 12 and size % 12 == 0 and start + size <= physical and len(code) == size:
                            copies[kind] += 1
                            stored[kind].setdefault(code, (os.path.basename(path), base + p))
                p = data.find(MAGIC, p + 1)
            tail = chunk[-(0x20000):]
            position += len(chunk)
            # containers wholly inside the tail were already counted: skip them next round
            # (setdefault makes the duplicate harmless for the distinct set; copies is approximate)
print("stored: %d distinct pixel shaders (%d copies), %d distinct vertex shaders (%d copies)" %
      (len(stored["ps"]), copies["ps"], len(stored["vs"]), copies["vs"]))

for kind in ("ps", "vs"):
    total = found = 0
    near = 0
    missing = []
    for name in sorted(os.listdir(shader_dir)):
        if not (name.startswith(kind) and name.endswith(".bin")):
            continue
        raw = open(os.path.join(shader_dir, name), "rb").read()
        words = struct.unpack("<%dI" % (len(raw) // 4), raw)
        code = struct.pack(">%dI" % len(words), *words)
        total += 1
        if code in stored[kind]:
            found += 1
            continue
        # same length and at most a few differing words: a stored shader patched at runtime
        best = None
        for candidate in stored[kind]:
            if len(candidate) == len(code):
                differing = sum(1 for i in range(0, len(code), 4) if candidate[i:i + 4] != code[i:i + 4])
                if best is None or differing < best:
                    best = differing
        if best is not None and best <= 24:
            near += 1
        else:
            missing.append((name, len(words), best))
    print("%s: %d captured, %d stored identically, %d stored with a few words patched, %d not found" % (kind, total, found, near, len(missing)))
    for name, count, best in missing[:40]:
        print("   %s (%d words)%s" % (name, count, "" if best is None else " closest same-length differs in %d words" % best))
