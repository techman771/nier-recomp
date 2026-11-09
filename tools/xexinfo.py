"""Read-only XEX2 header dump. Usage: python -I xexinfo.py <path-to-xex>"""
import struct
import sys

NAMES = {
    0x000002FF: "RESOURCE_INFO", 0x000003FF: "FILE_FORMAT_INFO",
    0x000005FF: "DELTA_PATCH", 0x000080FF: "BOUNDING_PATH",
    0x00010001: "ORIGINAL_BASE", 0x00010100: "ENTRY_POINT",
    0x00010201: "IMAGE_BASE", 0x000103FF: "IMPORT_LIBRARIES",
    0x00018002: "CHECKSUM_TIMESTAMP", 0x000183FF: "ORIGINAL_PE_NAME",
    0x000200FF: "STATIC_LIBRARIES", 0x00020104: "TLS_INFO",
    0x00020200: "STACK_SIZE", 0x00020301: "FS_CACHE_SIZE",
    0x00020401: "HEAP_SIZE", 0x00030000: "SYSTEM_FLAGS",
    0x00040006: "EXECUTION_INFO", 0x00040201: "WORKSPACE_SIZE",
    0x00040310: "GAME_RATINGS", 0x00040404: "LAN_KEY",
    0x000405FF: "XBOX360_LOGO", 0x000406FF: "MULTIDISC_MEDIA_IDS",
    0x000407FF: "ALT_TITLE_IDS", 0x00040801: "ADDITIONAL_MEMORY",
    0x00E10402: "EXPORTS_BY_NAME",
}
ENC = {0: "none", 1: "AES (normal)"}
COMP = {0: "none", 1: "basic", 2: "LZX (normal)", 3: "delta"}


def u32(d, o):
    return struct.unpack_from(">I", d, o)[0]


def u16(d, o):
    return struct.unpack_from(">H", d, o)[0]


def main(path):
    with open(path, "rb") as f:
        d = f.read()
    print("magic", d[:4], "size", len(d))
    if d[:4] != b"XEX2":
        print("Not XEX2; stopping")
        return
    print("module_flags 0x%08X" % u32(d, 4))
    print("pe_offset 0x%X  security_offset 0x%X" % (u32(d, 8), u32(d, 0x10)))
    sec = u32(d, 0x10)
    print("image_size 0x%X  load_address 0x%08X" % (u32(d, sec + 4), u32(d, sec + 0x110)))
    count = u32(d, 0x14)
    print("optional headers:", count)
    for i in range(count):
        key, val = u32(d, 0x18 + i * 8), u32(d, 0x1C + i * 8)
        name = NAMES.get(key, "?")
        low = key & 0xFF
        inline = low in (0, 1)
        print("  0x%08X %-20s %s0x%08X" % (key, name, "" if inline else "@", val))
        if key == 0x000003FF:
            print("    encryption: %s  compression: %s" % (
                ENC.get(u16(d, val + 4), "?"), COMP.get(u16(d, val + 6), "?")))
        elif key == 0x00040006:
            media, ver, base, title = struct.unpack_from(">IIII", d, val)
            print("    media_id 0x%08X version 0x%08X base 0x%08X title_id 0x%08X" % (media, ver, base, title))
            print("    platform %d type %d disc %d/%d" % tuple(d[val + 16:val + 20]))
        elif key == 0x000183FF:
            size = u32(d, val)
            print("    name:", d[val + 4:val + size].split(b"\0")[0].decode("latin1"))
        elif key == 0x000200FF:
            size = u32(d, val)
            for o in range(val + 4, val + size, 16):
                n = d[o:o + 8].split(b"\0")[0].decode("latin1")
                print("    %-9s %d.%d.%d.%d" % (n, u16(d, o + 8), u16(d, o + 10), u16(d, o + 12), u16(d, o + 14) & 0xFF))
        elif key == 0x00020104:
            print("    tls slots %d raw_addr 0x%08X data 0x%X raw 0x%X" % struct.unpack_from(">IIII", d, val))
        elif key == 0x000103FF:
            strsize, libcount = u32(d, val + 4), u32(d, val + 8)
            raw = d[val + 12:val + 12 + strsize]
            names = [s.decode("latin1") for s in raw.split(b"\0") if s]
            print("    libraries:", names)
            o = val + 12 + strsize
            for _ in range(libcount):
                size = u32(d, o)
                idx, recs = u16(d, o + 36), u16(d, o + 38)
                ver = u32(d, o + 28)
                print("    lib[%d] %s version 0x%08X records %d" % (idx, names[idx] if idx < len(names) else "?", ver, recs))
                o += size


main(sys.argv[1])
