#!/usr/bin/env python3
# apps/graphics/fpcwasm/jit/test/fuzz.py
#
# SPDX-License-Identifier: Apache-2.0
#
# Differential fuzzing of the JIT: random modules (wasm-tools smith, the
# features WAMR's board configuration has, loops bounded by fuel), every
# exported function called by jitrun fuzz interpreted and compiled, the
# results, traps, memory compared.  A module that differs is kept in the
# output directory (its seed in the name) with jitrun's report.

import argparse
import os
import random
import shlex
import subprocess
import sys

SMITH = [
    "--ensure-termination",
    "--export-everything", "true",
    "--allow-start-export", "false",
    "--simd-enabled", "false",
    "--relaxed-simd-enabled", "false",
    "--exceptions-enabled", "false",
    "--gc-enabled", "false",
    "--tail-call-enabled", "false",
    "--threads-enabled", "false",
    "--memory64-enabled", "false",
    "--custom-page-sizes-enabled", "false",
    "--wide-arithmetic-enabled", "false",
    "--extended-const-enabled", "false",
    "--shared-everything-threads-enabled", "false",
    "--multi-value-enabled", "true",
    "--bulk-memory-enabled", "true",
    "--reference-types-enabled", "true",
    "--saturating-float-to-int-enabled", "true",
    "--sign-extension-ops-enabled", "true",
    "--max-memories", "1",
    "--max-memory32-bytes", "1048576",
    "--max-imports", "0",
    "--min-funcs", "1",
    "--max-funcs", "20",
    "--max-instructions", "2000",
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wasm-tools", required=True)
    ap.add_argument("--jitrun", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("-n", type=int, default=100)
    ap.add_argument("--seed", type=int, default=None)
    o = ap.parse_args()

    base = o.seed if o.seed is not None else random.randrange(1 << 30)
    os.makedirs(o.out, exist_ok=True)
    stats = {"ok": 0, "bad": 0, "skipped": 0, "refused": 0, "timeout": 0}
    for i in range(o.n):
        seed = base + i
        rnd = random.Random(seed)
        data = bytes(rnd.randrange(256) for _ in range(rnd.randrange(64, 4096)))
        wasm = os.path.join(o.out, "m%d.wasm" % seed)
        r = subprocess.run([o.wasm_tools, "smith"] + SMITH + ["-o", wasm],
                           input=data, capture_output=True)
        if r.returncode != 0:
            stats["skipped"] += 1
            continue
        try:
            r = subprocess.run(shlex.split(o.jitrun) +
                               ["fuzz", wasm, str(seed)],
                               capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            stats["timeout"] += 1
            print("seed %d: TIMEOUT (kept %s)" % (seed, wasm))
            continue
        out = r.stdout.strip()
        if r.returncode == 0:
            if "skipped" in out:
                stats["skipped"] += 1
            elif "refused" in out:
                stats["refused"] += 1
            else:
                stats["ok"] += 1
            os.remove(wasm)
        else:
            stats["bad"] += 1
            open(wasm + ".txt", "w").write(out + "\n" + r.stderr)
            print("seed %d: %s" % (seed, out.splitlines()[-1] if out else
                                   "crashed (%d) %s" % (r.returncode,
                                                         r.stderr[-300:])))
    print("fuzz from seed %d: %d modules agree, %d differ, %d timeouts, "
          "%d refused by the JIT, %d skipped" %
          (base, stats["ok"], stats["bad"], stats["timeout"],
           stats["refused"], stats["skipped"]))
    sys.exit(1 if stats["bad"] or stats["timeout"] else 0)


if __name__ == "__main__":
    main()
