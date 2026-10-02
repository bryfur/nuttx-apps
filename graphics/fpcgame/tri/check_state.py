# The TRI fresh-start markers (tri_sdk.h) bracket every static of each
# file: check_state.py OBJDUMP FILE.o...   (after a build: games/*/ and the
# SDK's tri_*.o in apps/graphics/fpcgame; each line must say ok)
import subprocess, sys, glob
od = sys.argv[1]
bad = 0
for o in sys.argv[2:]:
    out = subprocess.run([od, "-t", o], capture_output=True, text=True).stdout
    sec = {".data": [], ".bss": []}
    for ln in out.splitlines():
        f = ln.split()
        if len(f) >= 5 and f[-3] in sec and f[1] in ("l", "g") or (len(f) >= 6 and f[-3] in sec):
            s = f[-3]
            if s in sec and not ln.strip().endswith(s):
                sec[s].append((int(f[0], 16), f[-1]))
    for s, lo_n, hi_pre in ((".data", "tri_state_d0", "d1"), (".bss", "tri_state_b0", "b1")):
        v = sorted(sec[s])
        if not v:
            continue
        ok = v[0][1] == lo_n and v[-1][1].split(".")[0] == hi_pre
        bad += not ok
        print(f"{o.split('/')[-1][:40]:40} {s:5} {len(v):3} statics: first {v[0][1]}, last {v[-1][1]}: {'ok' if ok else 'BAD'}")
sys.exit(bad != 0)
