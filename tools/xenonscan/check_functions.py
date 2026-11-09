"""Audits the recompiled output for dropped branches and overlapping function symbols.

Usage: python -I check_functions.py <recomp/ppc dir> <dis.exe> <xex> <work dir>
Exits non-zero if the generated code has any problem this script knows how to detect.
"""
import glob
import os
import re
import subprocess
import sys


def main(ppc_dir, dis, xex, work):
    errors = 0
    references = {}
    for path in glob.glob(os.path.join(ppc_dir, "ppc_recomp.*.cpp")):
        with open(path, encoding="latin1") as f:
            text = f.read()
        errors += text.count("// ERROR")
        for address in re.findall(r"\bsub_([0-9A-F]+)\(ctx, base\);", text):
            references[address] = references.get(address, 0) + 1

    with open(os.path.join(ppc_dir, "ppc_func_mapping.cpp"), encoding="latin1") as f:
        addresses = re.findall(r"\{ 0x([0-9A-F]+), sub_", f.read())

    listing = os.path.join(work, "all_functions.txt")
    with open(listing, "w") as f:
        for address in addresses:
            f.write("%s 1\n" % address)

    output = subprocess.run([dis, xex, listing], capture_output=True, text=True, check=True).stdout.split("\n")

    in_pdata = overlapping = gap = 0
    overlap_examples = []
    gap_first_words = {}
    for i, line in enumerate(output):
        match = re.match(r"== ([0-9A-F]{8}): (INSIDE|after) pdata .*offset 0x([0-9A-F]+)\)", line)
        if not match:
            continue
        if match.group(2) == "INSIDE" and match.group(3) == "0":
            in_pdata += 1
        elif match.group(2) == "INSIDE":
            overlapping += 1
            overlap_examples.append(match.group(1))
        else:
            gap += 1
            parts = output[i + 1].split()
            mnemonic = parts[2] if len(parts) > 2 else "?"
            gap_first_words[mnemonic] = gap_first_words.get(mnemonic, 0) + 1

    print("functions:", len(addresses))
    print("  at a .pdata start:", in_pdata)
    print("  in gaps (no .pdata, e.g. leaf functions):", gap)
    entries = [a for a in overlap_examples if references.get(a, 0)]
    ghosts = [a for a in overlap_examples if not references.get(a, 0)]
    print("  overlapping a .pdata function:", overlapping)
    print("    referenced by code (secondary entry points):", len(entries), [(a, references[a]) for a in entries])
    print("    never referenced (ghosts):", len(ghosts), ghosts)
    print("// ERROR markers:", errors)
    print("first instruction of gap functions:", sorted(gap_first_words.items(), key=lambda kv: -kv[1])[:14])
    with open(os.path.join(work, "ghosts.txt"), "w") as f:
        for address in ghosts:
            f.write("%s 6\n" % address)
    sys.exit(1 if errors or ghosts else 0)


main(*sys.argv[1:5])
