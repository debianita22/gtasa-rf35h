#!/usr/bin/env python3
"""check_fingerprints.py -- the instruction words in src/build_check.c must be
what binutils produces for the instructions named next to them.

Each entry's instruction is assembled with GNU as and compared with the
table's value under its mask. Branches use PC-relative targets (.+N), so the
encoding does not depend on where the instruction sits; masked rows (an
instruction class such as "any b.lt") get a concrete operand set that the
mask then ignores.
"""
import re
import subprocess
import sys
import tempfile
import os

src = open(os.path.join(os.path.dirname(__file__), "..", "src", "build_check.c")).read()
rows = re.findall(r'\{ "(\w+)", (0x[0-9a-f]+), (0x[0-9a-f]+), (0x[0-9a-f]+), "([^"]+)" \}', src)
if not rows:
    print("no fingerprint rows found")
    sys.exit(1)

# the masked rows name an instruction class; give each a concrete operand set
concrete = {
    "b.lt": "b.lt .+0x40",
    "fdiv": "fdiv s0, s1, s0",
    "b.le +0x98": "b.le .+0x98",
}

ok = True
with tempfile.TemporaryDirectory() as tmp:
    for sym, off, mask, value, insn in rows:
        asm = concrete.get(insn, insn)
        path = os.path.join(tmp, "f.s")
        with open(path, "w") as f:
            f.write(asm + "\n")
        subprocess.run(["aarch64-linux-gnu-as", "-o", path + ".o", path], check=True)
        dump = subprocess.run(["aarch64-linux-gnu-objdump", "-d", path + ".o"],
                              capture_output=True, text=True, check=True).stdout
        word = int(re.search(r"^\s+0:\s+([0-9a-f]{8})", dump, re.M).group(1), 16)
        good = (word & int(mask, 16)) == int(value, 16)
        ok &= good
        print(f"{'ok  ' if good else 'FAIL'} {sym}+{off}: {asm:28s} -> {word:08x} "
              f"(table {value} mask {mask})")
print("fingerprints:", "ok" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
