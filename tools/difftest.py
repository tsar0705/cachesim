#!/usr/bin/env python3
"""Differential tester: C++ cachesim vs. independent Python golden model.

For each case, draw a random legal configuration (1-2 levels, sizes, associativities,
block sizes, replacement policy, write policy, allocate policy) and a random trace from
the pattern library, run both implementations and compare every counter.
Exits non-zero on the first-ever mismatch count > 0 and prints a reproducer.
"""
import argparse
import os
import random
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import golden_model  # noqa: E402
import tracegen  # noqa: E402

POLICIES = ["lru", "fifo", "plru", "random"]
PATTERNS = ["seq", "stride", "random", "loop", "hotspot"]


def random_level(rng, min_block, two_level_l2=False):
    block = rng.choice([b for b in (16, 32, 64, 128) if b >= min_block])
    assoc = rng.choice([1, 2, 4, 8, 16])
    sets = rng.choice([1, 2, 4, 8, 16, 32, 64, 128]) * (4 if two_level_l2 else 1)
    policy = rng.choice(POLICIES)
    return (sets * assoc * block, assoc, block, policy)


def fmt_level(lv):
    return f"{lv[0]}:{lv[1]}:{lv[2]}:{lv[3]}"


def run_cpp(binary, levels, wb, wa, trace_path):
    cmd = [binary, "--l1", fmt_level(levels[0])]
    if len(levels) > 1:
        cmd += ["--l2", fmt_level(levels[1])]
    cmd += ["--write-policy", "wb" if wb else "wt", "--alloc", "wa" if wa else "nwa", "--raw", trace_path]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise RuntimeError(f"cachesim failed ({p.returncode}): {p.stderr.strip()}\n  {' '.join(cmd)}")
    out = {}
    for line in p.stdout.splitlines():
        k, v = line.split()
        out[k] = int(v)
    return out, " ".join(cmd)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", type=int, default=200)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--accesses", type=int, default=4000)
    ap.add_argument("--bin", default="build/cachesim")
    a = ap.parse_args()

    rng = random.Random(a.seed)
    mismatches = 0
    total_accesses = 0
    covered = {p: 0 for p in POLICIES}
    with tempfile.TemporaryDirectory() as td:
        for case in range(a.cases):
            l1 = random_level(rng, 16)
            levels = [l1]
            if rng.random() < 0.6:
                levels.append(random_level(rng, l1[2], two_level_l2=True))
            wb, wa = rng.random() < 0.7, rng.random() < 0.7
            pattern = rng.choice(PATTERNS)
            # Keep footprints comparable to cache size so hits, misses and evictions all occur.
            kw = {}
            if pattern == "loop":
                kw["ws"] = rng.choice([levels[0][0] // 2, levels[0][0] * 2, levels[0][0] * 5 // 2])
            if pattern == "hotspot":
                kw["hot"] = max(64, levels[0][0] * rng.choice([1, 2]))
            if pattern == "random":
                kw["span"] = levels[0][0] * rng.choice([2, 8])
            trace = list(tracegen.generate(pattern, a.accesses, rng.randrange(1 << 30),
                                           rng.choice([0.0, 0.3, 0.6]), **kw))
            path = os.path.join(td, "t.trace")
            with open(path, "w") as f:
                f.writelines(f"{'W' if w else 'R'} 0x{ad:x}\n" for w, ad in trace)
            got, cmd = run_cpp(a.bin, levels, wb, wa, path)
            want = golden_model.simulate(levels, trace, wb, wa)
            total_accesses += len(trace)
            for lv in levels:
                covered[lv[3]] += 1
            bad = {k: (got.get(k), want[k]) for k in want if got.get(k) != want[k]}
            bad.update({k: (got[k], None) for k in got if k not in want})
            if bad:
                mismatches += 1
                keep = f"/tmp/difftest_fail_{a.seed}_{case}.trace"
                os.replace(path, keep)
                print(f"MISMATCH case {case}: {cmd}\n  trace kept at {keep}")
                for k, (g, w) in sorted(bad.items())[:8]:
                    print(f"    {k}: C++={g} golden={w}")
    print(f"{a.cases} cases, {total_accesses} accesses, policy coverage (levels): "
          + ", ".join(f"{k}={v}" for k, v in covered.items()))
    print("PASS: C++ matches golden model on every counter" if not mismatches
          else f"FAIL: {mismatches} mismatching case(s)")
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
