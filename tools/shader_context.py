"""Shows where a captured shader sits in a game file: aligns the whole captured microcode against the file
around a known window offset, reports how many dwords match (patched dwords differ), and dumps the
bytes before the microcode (the container header). Usage:
python -I shader_context.py <shader.bin> <file> <window offset (hex)>"""
import struct
import sys

shader, path, window_offset = sys.argv[1], sys.argv[2], int(sys.argv[3], 16)
data = open(shader, "rb").read()
words = struct.unpack("<%dI" % (len(data) // 4), data)
middle = len(words) // 2
start = window_offset - middle * 4
with open(path, "rb") as f:
    f.seek(max(0, start - 256))
    before = f.read(min(256, start))
    f.seek(start)
    stored = struct.unpack(">%dI" % len(words), f.read(len(words) * 4))
diff = [i for i in range(len(words)) if words[i] != stored[i]]
print("%s: %d dwords, microcode starts at 0x%X; %d dwords differ" % (shader.split("/")[-1].split("\\")[-1], len(words), start, len(diff)))
for i in diff[:12]:
    print("   dword %d: runtime %08X stored %08X" % (i, words[i], stored[i]))
print("256 bytes before:")
for row in range(0, len(before), 32):
    print("   -%03X %s" % (len(before) - row, " ".join(before[row + k:row + k + 4].hex() for k in range(0, 32, 4))))
