"""Lists (and optionally extracts) the files in an Xbox 360 STFS package (CON / LIVE / PIRS).
Usage: python -I stfs.py <package> [extract directory]
Read-only on the package. Layout per Xenia's stfs_container_device and the public STFS notes:
header (big-endian), a volume descriptor at 0x379, data blocks of 0x1000 bytes interleaved with
hash tables (one table per 0xAA blocks at level 0, per 0x70E4 at level 1)."""
import os
import struct
import sys

BLOCK = 0x1000


def u24le(data, offset):
    return data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16


class Package:
    def __init__(self, path):
        self.file = open(path, "rb")
        head = self.file.read(0xA000)
        self.magic = head[:4]
        self.header_size = struct.unpack_from(">I", head, 0x340)[0]
        self.content_type = struct.unpack_from(">I", head, 0x344)[0]
        self.title_id = struct.unpack_from(">I", head, 0x360)[0]
        self.display_name = head[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]
        self.description = head[0xD11:0xD11 + 0x80].decode("utf-16-be").split("\0")[0]
        vd = head[0x379:0x379 + 0x24]
        self.block_separation = vd[2]
        self.file_table_blocks = struct.unpack_from("<H", vd, 3)[0]
        self.file_table_block = u24le(vd, 5)
        self.allocated_blocks = struct.unpack_from(">I", vd, 0x1C)[0]
        self.first_table = (self.header_size + 0xFFF) & ~0xFFF
        # Packages whose header rounds to 0xB000 hold one hash table per level; others two.
        self.shift = 0 if (self.first_table == 0xB000 or (self.block_separation & 1)) else 1

    def block_offset(self, block):
        base = ((block + 0xAA) // 0xAA) << self.shift
        number = base + block
        if block >= 0xAA:
            number += ((block + 0x70E4) // 0x70E4) << self.shift
            if block >= 0x70E4:
                number += 1 << self.shift
        return self.first_table + number * BLOCK

    def hash_entry(self, block):
        """Level-0 hash table entry for a block: (next block in chain)."""
        record = block % 0xAA
        table_block = (block // 0xAA) * 0xAA
        # The level-0 table sits just before its first data block.
        offset = self.block_offset(table_block) - BLOCK * (1 << self.shift) if True else 0
        offset = self.level0_table(block)
        self.file.seek(offset + record * 0x18)
        entry = self.file.read(0x18)
        return u24le(entry[0x15:], 0) if False else (entry[0x15] << 16 | entry[0x16] << 8 | entry[0x17])

    def level0_table(self, block):
        index = block // 0xAA
        number = index * 0xAA + index * (1 << self.shift) if False else None
        # Position of the level-0 table for this group: computed like a block, then step back.
        first = index * 0xAA
        return self.block_offset(first) - (BLOCK << self.shift) if index == 0 or first % 0x70E4 else self.block_offset(first) - (BLOCK << self.shift)

    def read_blocks(self, start, count, consecutive):
        data = bytearray()
        block = start
        for i in range(count):
            self.file.seek(self.block_offset(block))
            data += self.file.read(BLOCK)
            block = block + 1 if consecutive else self.hash_entry(block)
        return bytes(data)

    def entries(self):
        table = self.read_blocks(self.file_table_block, self.file_table_blocks, True)
        result = []
        for i in range(0, len(table), 0x40):
            e = table[i:i + 0x40]
            if e[0] == 0:
                continue
            length = e[0x28] & 0x3F
            name = e[:length].decode("latin1")
            flags = e[0x28]
            blocks = u24le(e, 0x29)
            start = u24le(e, 0x2F)
            parent = struct.unpack_from(">h", e, 0x32)[0]
            size = struct.unpack_from(">I", e, 0x34)[0]
            result.append(dict(index=i // 0x40, name=name, dir=bool(flags & 0x80), consecutive=bool(flags & 0x40),
                               blocks=blocks, start=start, parent=parent, size=size))
        return result


def main():
    package = Package(sys.argv[1])
    print("magic %s, content type 0x%X, title 0x%08X, header 0x%X, block separation %d, shift %d" % (
        package.magic, package.content_type, package.title_id, package.header_size, package.block_separation, package.shift))
    print("display name:", package.display_name)
    print("description:", package.description[:120])
    print("file table: block %d, %d blocks; %d allocated blocks" % (package.file_table_block, package.file_table_blocks, package.allocated_blocks))
    entries = package.entries()
    names = {e["index"]: e["name"] for e in entries}

    def path(e):
        parts = [e["name"]]
        parent = e["parent"]
        while parent != -1 and parent in names:
            parts.insert(0, names[parent])
            parent = next(x["parent"] for x in entries if x["index"] == parent)
        return "/".join(parts)

    for e in entries:
        print("%-60s %s %10d bytes, start block %d%s" % (path(e), "dir " if e["dir"] else "file", e["size"], e["start"], "" if e["consecutive"] else " (chained)"))
    if len(sys.argv) > 2:
        out = sys.argv[2]
        for e in entries:
            target = os.path.join(out, *path(e).split("/"))
            if e["dir"]:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(package.read_blocks(e["start"], e["blocks"], e["consecutive"])[:e["size"]])
            print("extracted", target)


main()
