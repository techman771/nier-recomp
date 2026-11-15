"""Reads a frame capture (recomp/captures/frame_f<N>.nfrm, see recomp/runtime/framecapture.cpp).

    nfrm.py <capture>                      one line per command
    nfrm.py <capture> vertices <command>   the draw's vertex streams: base offsets and the first vertices as hex
    nfrm.py <capture> field <command> <slot> <stride> <offset> [bits=30:2]
                                           histogram of a bit field of one dword per vertex
                                           (slot = vertex fetch slot, i.e. the vbBase index printed by `vertices`)

As a module: load(path) returns (frame, shaders, textures, commands); a draw is a dict.
"""
import struct
import sys


class Reader:
    def __init__(self, data):
        self.d, self.o = data, 0

    def get(self, fmt):
        size = struct.calcsize("<" + fmt)
        v = struct.unpack_from("<" + fmt, self.d, self.o)
        self.o += size
        return v[0] if len(v) == 1 else v

    def raw(self, n):
        b = self.d[self.o:self.o + n]
        self.o += n
        return b


def load(path):
    r = Reader(open(path, "rb").read())
    magic, version = r.get("II")
    assert magic == 0x4D52464E and version in (1, 2, 3, 4, 5), "not a frame capture this script can read"
    frame, scanout = r.get("Q"), r.get("I")
    shaders = []
    for _ in range(r.get("I")):
        h, pixel = r.get("Q"), r.get("B")
        words = r.raw(r.get("I") * 4)
        hlsl = r.raw(r.get("I"))
        shaders.append({"hash": h, "pixel": bool(pixel), "words": words, "hlsl": hlsl})
    textures = []
    for _ in range(r.get("I")):
        key, content, w, h, pitch, faces, levels, fmt = r.get("QQIIIIII")
        data = r.raw(r.get("Q"))
        subs = [r.get("QII") for _ in range(r.get("I"))]
        textures.append({"key": key, "width": w, "height": h, "pitch": pitch, "faces": faces, "levels": levels, "format": fmt, "data": data, "subs": subs})
    commands = []
    for _ in range(r.get("I")):
        kind = r.get("B")
        if kind == 0:
            d = {"type": "draw"}
            d["vs"], d["ps"] = r.get("ii")
            d["target"] = r.get("III")
            d["hasColor"], d["depthBound"] = r.get("BB")
            d["depth"] = r.get("III")
            d["depthControl"], d["stencilRefMask"], d["blendControl"], d["writeMask"] = r.get("IIII")
            d["rectList"] = r.get("B")
            d["cull"], d["frontCCW"] = r.get("BB") if version >= 2 else (0, 1)
            d["blendConstant"] = r.get("ffff") if version >= 3 else (0.0, 0.0, 0.0, 0.0)
            d["alphaFunc"], d["alphaRef"] = r.get("Bf") if version >= 3 else (7, 0.0)
            d["paramGen"] = r.get("B") if version >= 4 else 255
            d["strip"] = r.get("B") if version >= 5 else 0
            d["vertexCount"] = r.get("I")
            d["indices"] = struct.unpack_from("<%dI" % r.get("I"), r.d, r.o) if True else ()
            r.o += len(d["indices"]) * 4
            d["vertexData"] = r.raw(r.get("I"))
            d["vertexBase"] = r.get("96I")
            d["constants"] = r.raw(2048 * 4)
            d["boolConstants"] = r.get("8I")
            d["textures"] = {}
            for slot in range(32):
                if not r.get("B"):
                    continue
                b = {"texture": r.get("i"), "resolvedBase": r.get("I")}
                r.raw(14 * 4 + 4)
                b["swizzle"] = r.raw(4)
                b["point"], b["addressU"], b["addressV"], b["cube"], b["baseOnly"] = r.get("BBBBB")
                b["scale"] = r.get("ff")
                b["sign"], b["expScale"] = r.get("If")
                d["textures"][slot] = b
            d["bypassViewport"] = r.get("B")
            d["ndcScale"], d["ndcOffset"] = r.get("ff"), r.get("ff")
            d["viewport"], d["scissor"] = r.get("ffff"), r.get("iiii")
            commands.append(d)
        else:
            v = {"type": "resolve"}
            v["source"] = r.get("III")
            v["copyColor"], v["copyDepth"], v["scale"] = r.get("BBI")
            v["destBase"], v["width"], v["height"] = r.get("III")
            v["rect"] = r.get("iiiiII")
            v["swapRedBlue"], v["writeBack"], v["destEndian"], v["destFormat"] = r.get("BBII")
            v["clearColor"], v["clearDepth"], v["colorClear"], v["depthClear"] = r.get("BBII")
            v["depth"] = r.get("III")
            commands.append(v)
    return {"frame": frame, "scanout": scanout}, shaders, textures, commands


def constant(draw, index):
    """Float constant register `index` (0-511; pixel shader constants start at 256) as 4 floats."""
    return struct.unpack_from("<4f", draw["constants"], index * 16)


def main():
    info, shaders, textures, commands = load(sys.argv[1])
    mode = sys.argv[2] if len(sys.argv) > 2 else "list"
    if mode == "list":
        for i, c in enumerate(commands):
            if c["type"] == "draw":
                vs = shaders[c["vs"]]["hash"] if c["vs"] >= 0 else 0
                ps = shaders[c["ps"]]["hash"] if c["ps"] >= 0 else 0
                print("%4d draw vs %016X ps %016X verts %d idx %d target %X vdata %d" % (i, vs, ps, c["vertexCount"], len(c["indices"]), c["target"][0], len(c["vertexData"])))
            else:
                print("%4d resolve -> %08X %dx%d" % (i, c["destBase"], c["width"], c["height"]))
        return
    d = commands[int(sys.argv[3])]
    if mode == "vertices":
        print("vertex data bytes:", len(d["vertexData"]), "indices:", len(d["indices"]), "min/max index:", min(d["indices"], default=0), max(d["indices"], default=0))
        for slot in range(96):
            if d["vertexBase"][slot] or slot == 95:
                base = d["vertexBase"][slot]
                print("slot %d base %d: %s" % (slot, base, d["vertexData"][base:base + 64].hex()))
    elif mode == "field":
        slot, stride, offset = int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6])
        shift, bits = (int(x) for x in (sys.argv[7] if len(sys.argv) > 7 else "30:2").split(":"))
        base = d["vertexBase"][slot]
        counts = {}
        used = sorted(set(d["indices"])) if d["indices"] else range(d["vertexCount"])
        for i in used:
            at = base + i * stride + offset
            if at + 4 > len(d["vertexData"]):
                continue
            (w,) = struct.unpack_from("<I", d["vertexData"], at)
            v = (w >> shift) & ((1 << bits) - 1)
            counts[v] = counts.get(v, 0) + 1
        print("field bits %d..%d over %d vertices:" % (shift, shift + bits - 1, len(used)), dict(sorted(counts.items())))


if __name__ == "__main__":
    main()
