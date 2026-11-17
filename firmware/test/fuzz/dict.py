#!/usr/bin/env python3
"""A libFuzzer dictionary from the C string literals in the given sources:
the paths, keys and commands the parsers compare against, which a fuzzer
would take a long time to guess byte by byte."""
import re
import sys

seen = set()
for path in sys.argv[1:]:
    src = open(path, errors="replace").read()
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', src):
        s = m.group(1)
        if not 2 <= len(s) <= 40 or "%" in s or "\\" in s:
            continue
        for tok in sorted({s, *re.findall(r"[A-Za-z_/][A-Za-z0-9_/.-]+", s)}):
            if tok not in seen:
                seen.add(tok)
                print(f'"{tok}"')
