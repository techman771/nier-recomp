"""Converts a texture dumped with NIER_DUMP_TEX (recomp/captures/tex_*.bin) to PNG.
Handles RGBA8 (CTX1 is dumped already expanded to RGBA8) and BC1/BC2/BC3; for BC2/BC3 and RGBA8 it
also writes <out>_alpha.png. Usage: python -I tex2png.py <tex.bin> <out.png>"""
import struct
import sys
import zlib

RGBA8, BC1, BC2, BC3 = 0, 1, 2, 3   # GuestTextureFormat order in renderer.h


def rgb565(c):
    return ((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31


def colour_block(block, rgb, width, bx, by, four_colour):
    c0, c1, bits = struct.unpack_from("<HHI", block, 0)
    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1 or four_colour:
        palette = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)), tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    else:
        palette = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
    for y in range(4):
        for x in range(4):
            rgb[(by * 4 + y) * width + bx * 4 + x] = palette[(bits >> (2 * (y * 4 + x))) & 3]


def bc3_alpha(block, alpha, width, bx, by):
    a0, a1 = block[0], block[1]
    bits = int.from_bytes(block[2:8], "little")
    if a0 > a1:
        palette = [a0, a1] + [((6 - i) * a0 + (1 + i) * a1) // 7 for i in range(6)]
    else:
        palette = [a0, a1] + [((4 - i) * a0 + (1 + i) * a1) // 5 for i in range(4)] + [0, 255]
    for i in range(16):
        alpha[(by * 4 + i // 4) * width + bx * 4 + i % 4] = palette[(bits >> (3 * i)) & 7]


def bc2_alpha(block, alpha, width, bx, by):
    bits = int.from_bytes(block[0:8], "little")
    for i in range(16):
        alpha[(by * 4 + i // 4) * width + bx * 4 + i % 4] = ((bits >> (4 * i)) & 15) * 17


def write_png(path, width, height, pixels, grey=False):
    if grey:
        raw = b"".join(b"\0" + bytes(pixels[y * width:(y + 1) * width]) for y in range(height))
        colour_type = 0
    else:
        raw = b"".join(b"\0" + bytes(c for p in pixels[y * width:(y + 1) * width] for c in p) for y in range(height))
        colour_type = 2
    chunk = lambda kind, payload: struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


def main(src, dst):
    data = open(src, "rb").read()
    fmt, width, height, pitch, rows, guest = struct.unpack_from("<6I", data, 0)
    body = data[24:]
    rgb = [(0, 0, 0)] * (width * height)
    alpha = None
    if fmt == RGBA8:
        alpha = [0] * (width * height)
        for y in range(height):
            for x in range(width):
                r, g, b, a = body[y * pitch + x * 4: y * pitch + x * 4 + 4]
                rgb[y * width + x] = (r, g, b)
                alpha[y * width + x] = a
    elif fmt in (BC1, BC2, BC3):
        size = 8 if fmt == BC1 else 16
        if fmt != BC1:
            alpha = [0] * (width * height)
        for by in range(rows):
            for bx in range(width // 4):
                block = body[by * pitch + bx * size: by * pitch + (bx + 1) * size]
                colour_block(block[-8:], rgb, width, bx, by, fmt != BC1)
                if fmt == BC3:
                    bc3_alpha(block, alpha, width, bx, by)
                elif fmt == BC2:
                    bc2_alpha(block, alpha, width, bx, by)
    else:
        sys.exit("format %d not handled" % fmt)
    write_png(dst, width, height, rgb)
    if alpha is not None:
        write_png(dst.replace(".png", "_alpha.png"), width, height, alpha, grey=True)
    print(dst, width, height, "guest format", guest)


main(sys.argv[1], sys.argv[2])
