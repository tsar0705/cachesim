#!/usr/bin/env python3
"""Deterministic memory-trace generator (seeded) for cachesim.

Patterns: seq, stride, random, loop, hotspot, matmul (naive i-j-k), matmul-blocked.
Output format: one 'R 0x...' / 'W 0x...' per line.
"""
import argparse
import random
import sys


def gen_seq(n, rng, wf, base=0x10000000, step=8, **_):
    for i in range(n):
        yield rng.random() < wf, base + i * step


def gen_stride(n, rng, wf, base=0x10000000, stride=4096, span=1 << 24, **_):
    for i in range(n):
        yield rng.random() < wf, base + (i * stride) % span


def gen_random(n, rng, wf, base=0x10000000, span=1 << 22, **_):
    for _i in range(n):
        yield rng.random() < wf, base + rng.randrange(span) & ~7


def gen_loop(n, rng, wf, base=0x10000000, ws=24 * 1024, step=8, **_):
    for i in range(n):
        yield rng.random() < wf, base + (i * step) % ws


def gen_hotspot(n, rng, wf, base=0x10000000, hot=8 * 1024, span=1 << 22, p_hot=0.9, **_):
    for _i in range(n):
        off = rng.randrange(hot) if rng.random() < p_hot else rng.randrange(span)
        yield rng.random() < wf, base + (off & ~7)


def gen_matmul(n, rng, wf, dim=64, **_):
    A, B, C = 0x10000000, 0x20000000, 0x30000000
    e = 8
    for i in range(dim):
        for j in range(dim):
            for k in range(dim):
                yield False, A + (i * dim + k) * e
                yield False, B + (k * dim + j) * e
                yield False, C + (i * dim + j) * e
                yield True, C + (i * dim + j) * e


def gen_matmul_blocked(n, rng, wf, dim=64, tile=8, **_):
    A, B, C = 0x10000000, 0x20000000, 0x30000000
    e = 8
    for ii in range(0, dim, tile):
        for jj in range(0, dim, tile):
            for kk in range(0, dim, tile):
                for i in range(ii, ii + tile):
                    for j in range(jj, jj + tile):
                        for k in range(kk, kk + tile):
                            yield False, A + (i * dim + k) * e
                            yield False, B + (k * dim + j) * e
                            yield False, C + (i * dim + j) * e
                            yield True, C + (i * dim + j) * e


PATTERNS = {"seq": gen_seq, "stride": gen_stride, "random": gen_random, "loop": gen_loop,
            "hotspot": gen_hotspot, "matmul": gen_matmul, "matmul-blocked": gen_matmul_blocked}


def generate(pattern, n=10000, seed=0, write_frac=0.3, **kw):
    rng = random.Random(seed)
    return PATTERNS[pattern](n, rng, write_frac, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern", choices=sorted(PATTERNS))
    ap.add_argument("-n", type=int, default=100000, help="accesses (ignored by matmul patterns)")
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--write-frac", type=float, default=0.3)
    ap.add_argument("--dim", type=int, default=64)
    ap.add_argument("--tile", type=int, default=8)
    ap.add_argument("-o", "--output", default="-")
    a = ap.parse_args()
    out = sys.stdout if a.output == "-" else open(a.output, "w")
    for w, addr in generate(a.pattern, a.n, a.seed, a.write_frac, dim=a.dim, tile=a.tile):
        out.write(f"{'W' if w else 'R'} 0x{addr:x}\n")


if __name__ == "__main__":
    main()
