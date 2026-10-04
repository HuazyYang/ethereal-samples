"""Shader permutation tooling for the NvVolumetricLighting reconstruction.

usage:
  python shaders.py check [-f FAMILY ...] [-i INDEX ...] [-v] [-j N]
      Compile every permutation with fxc into recon/build/nvvl_check/<family>/<idx>.dxbc,
      disassemble it (fxc /dumpbin) and compare it with the blob embedded in the
      original DLL (recon/build/nvvl_decomp/dxbc/<va>.dxbc).
  python shaders.py diff FAMILY INDEX
      Print a unified diff between the original and the rebuilt disassembly.
  python shaders.py gen
      Write shaders/permutations.json (merged) and shaders/Permutations.cmake
      (consumed by CMakeLists.txt to compile and embed every permutation).

Permutation definitions live in shaders/permutations/<family>.json:
  {"name", "source", "entry", "profile", "table_size", "axes": [{"macro", "shift", "bits", "values"}]}
The table index of a permutation is sum(field << shift); field selects values[field] for the
macro (-D MACRO=values[field]). Indices whose fields are out of range are empty (nullptr) in
the DLL's tables.
"""
import argparse
import concurrent.futures
import difflib
import glob
import itertools
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # recon/NvVolumetricLighting
RECON = os.path.dirname(ROOT)                     # recon
SHADER_DIR = os.path.join(ROOT, "shaders")
PERM_DIR = os.path.join(SHADER_DIR, "permutations")
ORIG_DIR = os.path.join(RECON, "build", "nvvl_decomp", "dxbc")
CHECK_DIR = os.path.join(RECON, "build", "nvvl_check")
TABLES = os.path.join(HERE, "shader_tables.json")
FXC = r"C:\Program Files (x86)\Windows Kits\10\bin\10.0.19041.0\x64\fxc.exe"
FXC_FLAGS = ["/nologo", "/WX"]


def load_families():
    fams = []
    for f in sorted(glob.glob(os.path.join(PERM_DIR, "*.json"))):
        fams.append(json.load(open(f)))
    return fams


def permutations(fam):
    """Yield (index, [(macro, value), ...]) for every valid permutation of a family."""
    axes = fam["axes"]
    ranges = [range(len(a["values"])) for a in axes]
    out = []
    for fields in itertools.product(*ranges):
        idx = 0
        defines = []
        for a, v in zip(axes, fields):
            assert v < (1 << a["bits"])
            idx |= v << a["shift"]
            defines.append((a["macro"], a["values"][v]))
        out.append((idx, defines))
    out.sort()
    return out


def fxc_cmd(fam, defines, out_path):
    cmd = [FXC] + FXC_FLAGS + ["/T", fam["profile"], "/E", fam.get("entry", "main")]
    for m, v in defines:
        cmd += ["/D", "%s=%s" % (m, v)]
    cmd += ["/Fo", out_path, os.path.join(SHADER_DIR, fam["source"])]
    return cmd


def compile_one(fam, idx, defines):
    d = os.path.join(CHECK_DIR, fam["name"])
    os.makedirs(d, exist_ok=True)
    dxbc = os.path.join(d, "%d.dxbc" % idx)
    asm = os.path.join(d, "%d.asm" % idx)
    for p in (dxbc, asm):
        if os.path.exists(p):
            os.remove(p)
    r = subprocess.run(fxc_cmd(fam, defines, dxbc), capture_output=True, text=True)
    if r.returncode != 0:
        return idx, None, (r.stdout + r.stderr).strip()
    subprocess.run([FXC, "/nologo", "/dumpbin", "/Fc", asm, dxbc], capture_output=True, text=True)
    return idx, dxbc, None


def split_asm(text):
    """Split an fxc listing into (header comments, instruction lines)."""
    lines = text.splitlines()
    for i, l in enumerate(lines):
        if re.match(r"^(vs|hs|ds|gs|ps|cs)_\d_\d", l):
            head = lines[:i]
            code = [x for x in lines[i:] if not x.startswith("// Approximately")]
            return head, code
    return lines, []


def compare(orig_dxbc, new_dxbc):
    a = open(orig_dxbc, "rb").read()
    b = open(new_dxbc, "rb").read()
    res = {"binary": a == b}
    oa = open(orig_dxbc[:-5] + ".asm").read()
    na = open(new_dxbc[:-5] + ".asm").read()
    ha, ca = split_asm(oa)
    hb, cb = split_asm(na)
    res["header"] = ha == hb
    res["code"] = ca == cb
    res["orig_instr"] = len(ca)
    res["new_instr"] = len(cb)
    res["similarity"] = 1.0 if ca == cb else round(difflib.SequenceMatcher(None, ca, cb, autojunk=False).ratio(), 4)
    return res


def orig_map():
    t = json.load(open(TABLES))
    return {fam: {i: va for i, va, sz in ents if va} for fam, ents in t.items()}


def cmd_check(args):
    fams = load_families()
    omap = orig_map()
    jobs = []
    for fam in fams:
        if args.family and fam["name"] not in args.family:
            continue
        perms = permutations(fam)
        valid = set(omap.get(fam["name"], {}).keys())
        mine = set(i for i, _ in perms)
        if valid != mine and not args.index:
            print("WARNING %s: permutation index set differs from DLL table (missing %s, extra %s)" % (
                fam["name"], sorted(valid - mine)[:10], sorted(mine - valid)[:10]))
        for idx, defs in perms:
            if args.index and idx not in args.index:
                continue
            jobs.append((fam, idx, defs))
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = {ex.submit(compile_one, fam, idx, defs): (fam, idx) for fam, idx, defs in jobs}
        for f in concurrent.futures.as_completed(futs):
            fam, idx = futs[f]
            _, dxbc, err = f.result()
            name = fam["name"]
            r = {"index": idx}
            if err:
                r["error"] = err
            else:
                va = omap.get(name, {}).get(idx)
                if va is None:
                    r["error"] = "no original blob for this index"
                else:
                    r["va"] = va
                    r.update(compare(os.path.join(ORIG_DIR, va + ".dxbc"), dxbc))
            results.setdefault(name, []).append(r)
    summary = {}
    for name in sorted(results):
        rs = sorted(results[name], key=lambda r: r["index"])
        n = len(rs)
        nb = sum(1 for r in rs if r.get("binary"))
        nh = sum(1 for r in rs if r.get("header"))
        nc = sum(1 for r in rs if r.get("code"))
        ne = sum(1 for r in rs if "error" in r)
        sim = sum(r.get("similarity", 0) for r in rs) / max(n, 1)
        summary[name] = {"count": n, "binary_identical": nb, "header_identical": nh,
                         "code_identical": nc, "errors": ne, "mean_similarity": round(sim, 4)}
        print("%-22s %3d perms: binary %3d  header %3d  code %3d  errors %3d  mean-sim %.4f" % (
            name, n, nb, nh, nc, ne, sim))
        if args.verbose:
            for r in rs:
                if "error" in r:
                    print("   [%d] ERROR %s" % (r["index"], r["error"][:400]))
                elif not r["binary"]:
                    print("   [%d] va=%s header=%s code=%s instr %d/%d sim=%.4f" % (
                        r["index"], r["va"], r["header"], r["code"], r["new_instr"], r["orig_instr"], r["similarity"]))
        results[name] = rs
    if not args.family and not args.index:
        tot = {k: sum(s[k] for s in summary.values()) for k in
               ("count", "binary_identical", "header_identical", "code_identical", "errors")}
        print("TOTAL %(count)d perms: binary %(binary_identical)d  header %(header_identical)d  "
              "code %(code_identical)d  errors %(errors)d" % tot)
        json.dump({"summary": summary, "total": tot, "results": results},
                  open(os.path.join(CHECK_DIR, "report.json"), "w"), indent=1)


def cmd_diff(args):
    omap = orig_map()
    fam = [f for f in load_families() if f["name"] == args.family][0]
    defs = dict(permutations(fam))[args.index]
    _, dxbc, err = compile_one(fam, args.index, defs)
    if err:
        print(err)
        return
    va = omap[fam["name"]][args.index]
    oa = open(os.path.join(ORIG_DIR, va + ".asm")).read().splitlines()
    na = open(dxbc[:-5] + ".asm").read().splitlines()
    if not args.full:
        oa = split_asm("\n".join(oa))[1] if not args.header else split_asm("\n".join(oa))[0]
        na = split_asm("\n".join(na))[1] if not args.header else split_asm("\n".join(na))[0]
    sys.stdout.writelines(l + "\n" for l in difflib.unified_diff(oa, na, "original/" + va, "rebuilt", n=args.context))


def cmd_gen(args):
    fams = load_families()
    merged = []
    lines = ["# Generated by tools/shaders.py gen -- do not edit.",
             "# NVVL_SHADER_FAMILIES: family names; per family: _SOURCE, _PROFILE, _ENTRY, _SIZE, _INDICES,",
             "# and per permutation NVVL_<family>_<index>_DEFINES (list of MACRO=VALUE).", ""]
    lines.append("set(NVVL_SHADER_FAMILIES %s)" % " ".join(f["name"] for f in fams))
    for fam in fams:
        perms = permutations(fam)
        n = fam["name"]
        lines.append("set(NVVL_%s_SOURCE %s)" % (n, fam["source"]))
        lines.append("set(NVVL_%s_PROFILE %s)" % (n, fam["profile"]))
        lines.append("set(NVVL_%s_ENTRY %s)" % (n, fam.get("entry", "main")))
        lines.append("set(NVVL_%s_SIZE %d)" % (n, fam["table_size"]))
        lines.append("set(NVVL_%s_INDICES %s)" % (n, " ".join(str(i) for i, _ in perms)))
        for idx, defs in perms:
            lines.append("set(NVVL_%s_%d_DEFINES %s)" % (n, idx, " ".join("%s=%s" % d for d in defs)))
        lines.append("")
        m = dict(fam)
        m["permutations"] = [{"index": idx, "defines": {k: v for k, v in defs}} for idx, defs in perms]
        merged.append(m)
    open(os.path.join(SHADER_DIR, "Permutations.cmake"), "w").write("\n".join(lines) + "\n")
    json.dump({"compiler": "fxc 10.1 (Windows SDK 10.0.19041.0)", "flags": FXC_FLAGS,
               "families": merged}, open(os.path.join(SHADER_DIR, "permutations.json"), "w"), indent=1)
    print("wrote shaders/Permutations.cmake and shaders/permutations.json (%d families, %d permutations)" % (
        len(fams), sum(len(f["permutations"]) for f in merged)))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("check")
    c.add_argument("-f", "--family", action="append")
    c.add_argument("-i", "--index", action="append", type=int)
    c.add_argument("-v", "--verbose", action="store_true")
    c.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    d = sub.add_parser("diff")
    d.add_argument("family")
    d.add_argument("index", type=int)
    d.add_argument("--full", action="store_true")
    d.add_argument("--header", action="store_true")
    d.add_argument("-c", "--context", type=int, default=3)
    sub.add_parser("gen")
    args = ap.parse_args()
    {"check": cmd_check, "diff": cmd_diff, "gen": cmd_gen}[args.cmd](args)


if __name__ == "__main__":
    main()
