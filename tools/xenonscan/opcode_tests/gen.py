"""Extracts one real emitted instance of each added opcode from the recompiled output.

Usage: python -I gen.py <recomp/ppc dir> <output .inc>

Each instance becomes a function that runs the exact C++ the recompiler produced, so the
harness in harness.cpp tests the output rather than a copy of the intended behaviour.
"""
import glob
import os
import re
import sys

OPCODES = [
    "bdzf", "bdnzt", "lfsu", "lfsux", "lfdu", "stfsu", "stfsux", "stfdu", "sthu", "sthux",
    "lhzu", "lhzux", "lhau", "lwzux", "lbzux", "ldux", "stdux", "addme", "addc", "eqv",
    "subfze", "subfze.", "vcmpgtuw", "vcmpgtuw.", "vnor128", "vsel128", "lvehx",
    "vrfip128", "vpkswss128", "vcmpbfp128", "vupkd3d128", "vpkd3d128",
]
COMMENT = re.compile(r"^\t// (\S+) (.*)$")
BOUNDARY = re.compile(r"^\t// |^loc_[0-9A-F]+:|^}")
BI = {"lt": 0, "gt": 1, "eq": 2, "so": 3}


def bi_of(text):
    match = re.fullmatch(r"(?:4\*cr(\d)\+)?(lt|gt|eq|so)", text)
    return (int(match.group(1) or 0) * 4 + BI[match.group(2)]) if match else None


def operands(args):
    """Returns register numbers and immediates in written order; 'd(rA)' yields d then A."""
    result = []
    for part in args.split(","):
        part = part.strip()
        mem = re.fullmatch(r"(-?\d+)\(r(\d+)\)", part)
        if mem:
            result += [int(mem.group(1)), int(mem.group(2))]
            continue
        reg = re.fullmatch(r"[rfv](\d+)", part)
        if reg:
            result.append(int(reg.group(1)))
            continue
        bi = bi_of(part)
        if bi is not None:
            result.append(bi)
        elif part.startswith("0x"):
            result.append(0)
        else:
            result.append(int(part))
    return result


def usable(op, args, body):
    text = "\n".join(body)
    if op == "vupkd3d128":
        return args.endswith(",16")
    if op == "vpkd3d128":
        return args.endswith(",3,1,3")
    if op in ("bdzf", "bdnzt"):
        return "goto loc_" in text
    if op.endswith("ux") or op.endswith("u"):
        regs = operands(args)
        return len(set(regs[-2:] if op.endswith("ux") else regs[-1:] + regs[:1])) == (2 if op.endswith("ux") else 2)
    return True


def main(ppc_dir, out_path):
    found = {}
    for path in sorted(glob.glob(os.path.join(ppc_dir, "ppc_recomp.*.cpp"))):
        with open(path, encoding="latin1") as f:
            lines = f.read().split("\n")
        for i, line in enumerate(lines):
            match = COMMENT.match(line)
            if not match or match.group(1) not in OPCODES or match.group(1) in found:
                continue
            body = []
            for following in lines[i + 1:]:
                if BOUNDARY.match(following):
                    break
                body.append(following)
            if usable(match.group(1), match.group(2), body):
                found[match.group(1)] = (match.group(2), body)
        if len(found) == len(OPCODES):
            break

    missing = sorted(set(OPCODES) - set(found))
    if missing:
        sys.exit("no instance found for: " + ", ".join(missing))

    with open(out_path, "w") as out:
        for op, (args, body) in found.items():
            name = op.replace(".", "_rc")
            out.write("// %s %s\n" % (op, args))
            out.write("static const int ops_%s[] = { %s };\n" % (name, ", ".join(map(str, operands(args)))))
            out.write("static void emit_%s(PPCContext& ctx, uint8_t* base, bool& taken) {\n" % name)
            out.write("\tPPCRegister temp{}; PPCVRegister vTemp{}; uint32_t ea{}; (void)temp; (void)vTemp; (void)ea; (void)base; (void)taken;\n")
            for line in body:
                out.write(re.sub(r"goto loc_[0-9A-F]+;", "{ taken = true; return; }", line) + "\n")
            out.write("}\n\n")
    print("extracted", len(found), "opcodes")


main(sys.argv[1], sys.argv[2])
