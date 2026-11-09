"""Converts saved frame dumps to PNG. Usage: python -I ppm2png.py <input.ppm> [output.png] [scale divisor]"""
import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    fields, position = [], 0
    while len(fields) < 4:
        end = position
        while data[end] not in b" \n\t\r":
            end += 1
        fields.append(data[position:end])
        position = end + 1
    return int(fields[1]), int(fields[2]), data[position:]


def write_png(path, width, height, rgb):
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def main(source, target=None, divisor=1):
    width, height, pixels = read_ppm(source)
    target = target or source.rsplit(".", 1)[0] + ".png"
    divisor = int(divisor)
    if divisor > 1:
        out_w, out_h = width // divisor, height // divisor
        small = bytearray(out_w * out_h * 3)
        for y in range(out_h):
            for x in range(out_w):
                offset = (y * divisor * width + x * divisor) * 3
                small[(y * out_w + x) * 3:(y * out_w + x) * 3 + 3] = pixels[offset:offset + 3]
        width, height, pixels = out_w, out_h, bytes(small)
    write_png(target, width, height, pixels)
    print(target, width, height)


main(*sys.argv[1:])
