"""Lists recompiled functions whose last instruction can fall through (a conditional return or branch,
or any non-branch), i.e. functions XenonRecomp ended too early. Execution then "returns" from the
end of the C++ function instead of continuing into the next guest instructions.
Usage: python -I find_fallthrough.py <recomp/out directory>"""
import os
import re
import sys

UNCONDITIONAL = re.compile(r"^(b|blr|bctr|bctrl?|bl)\b")   # b/blr/bctr end; bl returns (tail calls handled separately)
func_re = re.compile(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)")
comment_re = re.compile(r"^\s*// (.+)$")

results = []
out = sys.argv[1]
for name in sorted(os.listdir(out)):
    if not name.startswith("ppc_recomp.") or not name.endswith(".cpp"):
        continue
    current, last = None, None
    for line in open(os.path.join(out, name), encoding="utf-8", errors="replace"):
        m = func_re.search(line)
        if m:
            current, last = m.group(1), None
            continue
        if current is None:
            continue
        c = comment_re.match(line)
        if c:
            last = c.group(1).strip()
        elif line.startswith("}"):
            mnemonic = last.split()[0] if last else ""
            ends = mnemonic in ("b", "blr", "bctr", "blr+", "blr-") or mnemonic.startswith("b ") or last in ("blr",)
            if not ends and not (mnemonic == "b" or mnemonic == "bctr"):
                results.append((current, last))
            current = None

for address, last in results:
    print(address, last)
print(len(results), "functions end with an instruction that can fall through", file=sys.stderr)
