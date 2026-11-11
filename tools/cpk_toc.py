"""Lists files in a CRI CPK archive from its TOC (CRI @UTF table). Usage:
python -I cpk_toc.py <archive.cpk> [name filter]
Prints directory, name, offset in the archive, stored size and extracted size."""
import struct
import sys
sys.stdout.reconfigure(encoding="utf-8", errors="replace")


def read_utf(data):
    """Parses one @UTF table (big-endian). Returns a list of row dicts."""
    assert data[:4] == b"@UTF", data[:4]
    table_size, rows_offset, strings_offset, data_offset, name_offset, columns, row_width, rows = struct.unpack_from(">IIIIIHHI", data, 4)
    base = 8
    strings = data[base + strings_offset: base + data_offset]

    def string(offset):
        end = strings.index(b"\0", offset)
        return strings[offset:end].decode("ascii", "replace")

    sizes = {0: 1, 1: 1, 2: 2, 3: 2, 4: 4, 5: 4, 6: 8, 7: 8, 8: 4, 0xA: 4, 0xB: 8}
    formats = {0: ">B", 1: ">b", 2: ">H", 3: ">h", 4: ">I", 5: ">i", 6: ">Q", 7: ">q", 8: ">f"}
    pos = 0x20
    cols = []
    for _ in range(columns):
        flags = data[pos]
        name = string(struct.unpack_from(">I", data, pos + 1)[0])
        pos += 5
        storage, kind = flags & 0xF0, flags & 0x0F
        constant = None
        if storage == 0x30:   # constant value stored in the schema
            constant, pos = read_value(data, pos, kind, sizes, formats, string)
        cols.append((name, storage, kind, constant))
    result = []
    for r in range(rows):
        pos = base + rows_offset + r * row_width
        row = {}
        for name, storage, kind, constant in cols:
            if storage == 0x50:   # per-row value
                row[name], pos = read_value(data, pos, kind, sizes, formats, string)
            elif storage == 0x30:
                row[name] = constant
            else:
                row[name] = None
        result.append(row)
    return result


def read_value(data, pos, kind, sizes, formats, string):
    if kind == 0xA:
        return string(struct.unpack_from(">I", data, pos)[0]), pos + 4
    if kind == 0xB:
        offset, size = struct.unpack_from(">II", data, pos)
        return (offset, size), pos + 8
    return struct.unpack_from(formats[kind], data, pos)[0], pos + sizes[kind]


path = sys.argv[1]
name_filter = sys.argv[2].upper() if len(sys.argv) > 2 else ""
archive_base = int(sys.argv[3], 0) if len(sys.argv) > 3 else 0   # for a CPK nested in another file
with open(path, "rb") as f:
    f.seek(archive_base)
    header = f.read(0x800)
    cpk = read_utf(header[0x10:])[0]
    toc_offset, content_offset = cpk["TocOffset"], cpk.get("ContentOffset") or 0
    f.seek(archive_base + toc_offset)
    chunk = f.read(16)
    size = struct.unpack_from("<I", chunk, 8)[0]
    toc = read_utf(f.read(size))
base = min(toc_offset, content_offset) if content_offset else toc_offset
print("TocOffset 0x%X ContentOffset 0x%X files %d (offsets relative to 0x%X)" % (toc_offset, content_offset, len(toc), base))
for row in toc:
    full = (row.get("DirName") or "") + "/" + (row.get("FileName") or "")
    if name_filter and name_filter not in full.upper():
        continue
    offset = row["FileOffset"] + base + archive_base
    print("%-40s offset 0x%010X stored %10d extracted %10d" % (full, offset, row["FileSize"], row.get("ExtractSize") or 0))
