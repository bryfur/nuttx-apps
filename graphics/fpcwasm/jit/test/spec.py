#!/usr/bin/env python3
# apps/graphics/fpcwasm/jit/test/spec.py
#
# SPDX-License-Identifier: Apache-2.0
#
# The WebAssembly spec tests on the JIT: each .wast through wast2json (a
# JSON list of commands and the modules), the commands this harness runs
# written as lines for jitrun (module, invoke, assert_return,
# assert_trap, assert_exhaustion, get), run by jitrun spec.  Prints each
# file's summary and the totals; exit status 1 if the JIT failed a command
# the interpreter passed, or the two differ.

import argparse
import json
import os
import shlex
import subprocess
import sys


def val(v):
    t = v["type"]
    x = v.get("value")
    if t in ("i32", "i64", "f32", "f64"):
        if x is None:
            return None
        if x.startswith("nan:"):
            return "%s:%s" % (t, x)
        return "%s:%#x" % (t, int(x))
    if t in ("funcref", "externref"):
        if x is None:
            return "%s:any" % t
        if x == "null":
            return "%s:null" % t
        return "%s:%#x" % (t, int(x))
    return None                           # v128 and the rest: not here


def action(a):
    if a.get("type") != "invoke" and a.get("type") != "get":
        return None
    if "module" in a:
        return None                       # (another named module)
    args = [val(x) for x in a.get("args", [])]
    if None in args:
        return None
    return a["field"].encode().hex(), args


def convert(js, base):
    out = []
    for c in js["commands"]:
        t = c["type"]
        if t == "module":
            if c.get("module_type", "binary") != "binary":
                out.append("module /nonexistent")
                continue
            out.append("module %s" % os.path.join(base, c["filename"]))
        elif t in ("assert_return", "action", "assert_trap",
                   "assert_exhaustion"):
            a = action(c["action"])
            if a is None:
                continue
            name, args = a
            if c["action"]["type"] == "get":
                if t != "assert_return" or len(c["expected"]) != 1:
                    continue
                e = val(c["expected"][0])
                if e is None:
                    continue
                out.append("get %s 1 %s" % (name, e))
                continue
            line = "%s %s %d %s" % ({"action": "invoke"}.get(t, t), name,
                                     len(args), " ".join(args))
            if t == "assert_return":
                exp = [val(x) for x in c["expected"]]
                if None in exp:
                    continue
                line += " %d %s" % (len(exp), " ".join(exp))
            elif t == "assert_trap":
                line += " " + c["text"]
            out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--wast2json", required=True)
    ap.add_argument("--jitrun", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("wast", nargs="+")
    o = ap.parse_args()

    totals = [0, 0, 0, 0, 0, 0]
    bad = []
    for w in sorted(o.wast):
        name = os.path.splitext(os.path.basename(w))[0]
        d = os.path.join(o.out, name)
        os.makedirs(d, exist_ok=True)
        js = os.path.join(d, name + ".json")
        r = subprocess.run([o.wast2json, w, "-o", js], capture_output=True,
                           text=True)
        if r.returncode != 0:
            print("%s: wast2json failed: %s" % (name,
                  r.stderr.strip().splitlines()[0] if r.stderr else "?"))
            continue
        cmds = convert(json.load(open(js)), d)
        cf = os.path.join(d, name + ".cmds")
        open(cf, "w").write("\n".join(cmds) + "\n")
        try:
            r = subprocess.run(shlex.split(o.jitrun) + ["spec", cf],
                               capture_output=True, text=True, timeout=600)
        except subprocess.TimeoutExpired:
            print("%s: TIMEOUT" % name)
            bad.append(name)
            continue
        line = r.stdout.strip().splitlines()[-1] if r.stdout.strip() else \
            "%s: crashed (%d)" % (name, r.returncode)
        print(line.replace(cf, name))
        if r.returncode != 0:
            bad.append(name)
            for l in r.stderr.strip().splitlines()[:30]:
                print("    " + l.replace(d + "/", ""))
        try:
            p = line.split(": ", 1)[1]
            nums = [int(x) for x in
                    p.replace(",", " ").replace("(", " ").replace(")", " ")
                    .replace(";", " ").split() if x.isdigit()]
            # modules, skipped, refused, commands, interp, jit, jit only,
            # mismatches
            totals[0] += nums[3]
            totals[1] += nums[4]
            totals[2] += nums[5]
            totals[3] += nums[6]
            totals[4] += nums[7]
            totals[5] += nums[2]
        except (IndexError, ValueError):
            pass
    print("TOTAL: %d commands; failed: interp %d, JIT %d (JIT only %d); "
          "mismatches %d; modules the JIT refused %d" % tuple(totals))
    if bad:
        print("files with problems: " + " ".join(bad))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
