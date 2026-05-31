#!/usr/bin/env python3
"""Print the raw per-FRAME (not deduped) monkey-parent sequence around the
spawn for native and oracle, so the code-pointer evolution is visible."""
import json, sys
d = sys.argv[1] if len(sys.argv) > 1 else "_mk_cap2"
def load(lbl):
    return json.load(open(f"{d}/{lbl}_trace.json"))
def show(lbl, rows):
    print(f"\n=== {lbl} (per frame) ===")
    print("  idx |  sx  | code  | rout | t2e  | rend | f34  | s9   s10 | rng")
    prev=None
    for i,r in enumerate(rows):
        sig=(r['clo'],r['rout'],r['rend'],r['s9'],r['s10'])
        # print only when something monkey-relevant changes, plus context
        if sig!=prev:
            print("  %3d | %4d | $%04X |  %d   | %4d | 0x%02X | $%04X| $%04X $%04X | %d" % (
                i, r['sx'], r['clo'], r['rout'], r['t2e'], r['rend'], r['f34'],
                r['s9'], r['s10'], r['rng']))
            prev=sig
for lbl in ("native","oracle"):
    show(lbl, load(lbl))
