#!/usr/bin/env python3
"""build_report.py - writes docs/review-3/layout.json from findings.py (review-3 LAYOUT).
The prose layout.md is written by hand; this keeps the JSON in one place."""
import json, sys
from pathlib import Path
here = Path(__file__).resolve().parent
sys.path.insert(0, str(here))
import findings as F  # noqa: E402

out = {"domain": "LAYOUT", "findings": F.FINDINGS, "round1_fixes_checked": F.ROUND1,
       "field_issues": F.FIELD, "tests_built": F.TESTS, "known_unknowns": F.KU}
(here.parent.parent / "layout.json").write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
c = {}
for f in F.FINDINGS:
    k = (f["severity"], "new" if f["repeat_of"] == "new" else "repeat")
    c[k] = c.get(k, 0) + 1
print("written", len(F.FINDINGS), "findings", c)
