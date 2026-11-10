"""Disassembles captured Xenos shader microcode. Usage: python -I xenos_dis.py <shader.bin> [...]"""
import struct
import sys

CF = {0: "NOP", 1: "EXEC", 2: "EXEC_END", 3: "COND_EXEC", 4: "COND_EXEC_END", 5: "COND_PRED_EXEC", 6: "COND_PRED_EXEC_END",
      7: "LOOP_START", 8: "LOOP_END", 9: "COND_CALL", 10: "RETURN", 11: "COND_JMP", 12: "ALLOC", 13: "COND_EXEC_PC", 14: "COND_EXEC_PC_END", 15: "MARK_VS_FETCH_DONE"}
VEC = ["ADD", "MUL", "MAX", "MIN", "SETE", "SETGT", "SETGTE", "SETNE", "FRAC", "TRUNC", "FLOOR", "MULADD", "CNDE", "CNDGTE", "CNDGT", "DOT4", "DOT3", "DOT2ADD", "CUBE", "MAX4",
       "PRED_SETE_PUSH", "PRED_SETNE_PUSH", "PRED_SETGT_PUSH", "PRED_SETGTE_PUSH", "KILLE", "KILLGT", "KILLGTE", "KILLNE", "DST", "MAXA"]
SCA = ["ADDs", "ADD_PREV", "MULs", "MUL_PREV", "MUL_PREV2", "MAXs", "MINs", "SETEs", "SETGTs", "SETGTEs", "SETNEs", "FRACs", "TRUNCs", "FLOORs", "EXP", "LOGC", "LOG", "RECIPC", "RECIPF", "RECIP",
       "RSQC", "RSQF", "RSQ", "MAXAs", "MAXA_FLOOR", "SUBs", "SUB_PREV", "PRED_SETEs", "PRED_SETNEs", "PRED_SETGTs", "PRED_SETGTEs", "PRED_INV", "PRED_POP", "PRED_CLR", "PRED_RESTORE",
       "KILLEs", "KILLGTs", "KILLGTEs", "KILLNEs", "KILLONEs", "SQRT", "?41", "MULC0", "MULC1", "ADDC0", "ADDC1", "SUBC0", "SUBC1", "SIN", "COS", "RETAIN_PREV"]
CH = "xyzw"


def swizzle(bits):
    # Each 2-bit field is an offset from the identity channel.
    return "".join(CH[(((bits >> (i * 2)) & 3) + i) & 3] for i in range(4))


def mask(bits):
    return "".join(CH[i] if bits & (1 << i) else "_" for i in range(4))


def src(index, words):
    d1, d2 = words[1], words[2]
    fields = {1: (24, 26, 16, 31), 2: (8, 25, 8, 30), 3: (0, 24, 0, 29)}
    swiz_shift = {1: 16, 2: 8, 3: 0}[index]
    reg_shift = {1: 16, 2: 8, 3: 0}[index]
    negate_bit = {1: 26, 2: 25, 3: 24}[index]
    select_bit = {1: 31, 2: 30, 3: 29}[index]
    is_reg = (d2 >> select_bit) & 1
    reg = (d2 >> reg_shift) & 0xFF
    swz = swizzle((d1 >> swiz_shift) & 0xFF)
    neg = "-" if (d1 >> negate_bit) & 1 else ""
    if is_reg:
        absolute = "|" if reg & 0x80 else ""
        return "%s%sr%d.%s%s" % (neg, absolute, reg & 0x3F, swz, absolute)
    return "%sc%d.%s" % (neg, reg, swz)


def alu(words):
    d0, d1, d2 = words
    vec_dest, sca_dest = d0 & 0x3F, (d0 >> 8) & 0x3F
    export = (d0 >> 15) & 1
    vmask, smask = (d0 >> 16) & 0xF, (d0 >> 20) & 0xF
    sop, vop = (d0 >> 26) & 0x3F, (d2 >> 24) & 0x1F
    prefix = "e" if export else "r"
    out = []
    if export:
        # Both results go to the vector destination; components in both masks get 1.0, and with
        # the scalar-relative bit, components in neither get 0.0.
        ones, zeros = vmask & smask, (0xF & ~(vmask | smask)) if (d0 >> 14) & 1 else 0
        vmask, smask, sca_dest = vmask & ~smask, smask & ~vmask, vec_dest
        if ones:
            out.append("%s%d.%s = 1.0" % (prefix, vec_dest, mask(ones)))
        if zeros:
            out.append("%s%d.%s = 0.0" % (prefix, vec_dest, mask(zeros)))
    if vmask or not smask:
        name = VEC[vop] if vop < len(VEC) else "?%d" % vop
        out.append("%s%d.%s = %s(%s, %s, %s)" % (prefix, vec_dest, mask(vmask), name, src(1, words), src(2, words), src(3, words)))
    if smask:
        name = SCA[sop] if sop < len(SCA) else "?%d" % sop
        out.append("%s%d.%s = %s(%s)" % (prefix, sca_dest, mask(smask), name, src(3, words)))
    return "  ;  ".join(out) or "alu-nop"


def fetch(words):
    d0, d1, d2 = words
    opcode = d0 & 0x1F
    src_reg, dst_reg = (d0 >> 5) & 0x3F, (d0 >> 12) & 0x3F
    dst_swiz = "".join("xyzw01?_"[(d1 >> (i * 3)) & 7] for i in range(4))
    if opcode == 0:
        const = (d0 >> 20) & 0x1F
        sel = (d0 >> 25) & 3
        fmt = (d1 >> 16) & 0x3F
        return "r%d.%s = vfetch(r%d.%s, slot %d) fmt %d stride %d offset %d signed %d norm %d" % (
            dst_reg, dst_swiz, src_reg, "xyzw"[(d0 >> 30) & 3], const * 3 + sel, fmt, d2 & 0xFF, (d2 >> 8) & 0x7FFFFF, (d1 >> 12) & 1, (d1 >> 13) & 1)
    const = (d0 >> 20) & 0x1F
    src_swiz = "".join("xyzw"[(d0 >> (26 + i * 2)) & 3] for i in range(3))   # absolute, unlike ALU swizzles
    return "r%d.%s = tfetch%d(r%d.%s, t%d)" % (dst_reg, dst_swiz, opcode, src_reg, src_swiz, const)


def main(path):
    with open(path, "rb") as f:
        data = f.read()
    words = list(struct.unpack("<%dI" % (len(data) // 4), data))
    print("===", path.replace("\\", "/").rsplit("/", 1)[-1], len(words), "dwords")
    pending = []
    index = 0
    end = len(words) // 3
    while index < end:
        w0, w1, w2 = words[index * 3:index * 3 + 3]
        pair = [(w0, w1 & 0xFFFF), ((w1 >> 16) | ((w2 & 0xFFFF) << 16), w2 >> 16)]
        finished = False
        for d0, d1 in pair:
            opcode = d1 >> 12
            if opcode in (1, 2, 3, 4, 5, 6):
                address, count, sequence = d0 & 0xFFF, (d0 >> 12) & 7, (d0 >> 16) & 0xFFF
                print("cf %s addr %d count %d seq %03X" % (CF[opcode], address, count, sequence))
                pending.append((address, count, sequence))
                end = min(end, address)
                finished = finished or opcode in (2, 4, 6)
            elif opcode == 12:
                print("cf ALLOC size %d type %d" % (d0 & 0xF, (d1 >> 9) & 3))
            else:
                print("cf %s %08X %04X" % (CF[opcode], d0, d1))
        index += 1
    for address, count, sequence in pending:
        for i in range(count):
            triple = words[(address + i) * 3:(address + i) * 3 + 3]
            is_fetch = (sequence >> (i * 2)) & 1
            print("  %3d: %s" % (address + i, fetch(triple) if is_fetch else alu(triple)))


for argument in sys.argv[1:]:
    main(argument)
