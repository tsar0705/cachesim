#!/usr/bin/env python3
"""Independent reference model for cachesim.

Deliberately written with different data structures than the C++ code (recency lists and
queues instead of timestamps, a recursive PLRU tree instead of a flat bit array, dict-free
slot arrays) so that a shared bug is unlikely. Only the *specification* is shared:

  * victim = lowest-index invalid way, otherwise the policy's choice
  * allocating miss: write back dirty victim -> read block from next level -> install
  * RANDOM = xorshift32 seeded 0x12345678 (one generator per level), victim = x % assoc
"""
import argparse
import sys

MASK32 = 0xFFFFFFFF


class PlruNode:
    """Binary tree over ways [lo, hi). go_right=True means the next victim is in the right half."""

    def __init__(self, lo, hi):
        self.lo, self.hi, self.go_right = lo, hi, False
        self.left = self.right = None
        if hi - lo > 1:
            mid = (lo + hi) // 2
            self.left, self.right = PlruNode(lo, mid), PlruNode(mid, hi)

    def touch(self, way):
        if self.left is None:
            return
        mid = (self.lo + self.hi) // 2
        if way < mid:
            self.go_right = True
            self.left.touch(way)
        else:
            self.go_right = False
            self.right.touch(way)

    def victim(self):
        if self.left is None:
            return self.lo
        return (self.right if self.go_right else self.left).victim()


class GoldenLevel:
    def __init__(self, name, size, assoc, block, policy, write_back, write_alloc):
        assert size % (assoc * block) == 0
        self.name, self.assoc, self.block = name, assoc, block
        self.policy, self.wb, self.wa = policy, write_back, write_alloc
        self.nsets = size // (assoc * block)
        self.slots = [[None] * assoc for _ in range(self.nsets)]  # None | [tag, dirty]
        self.recency = [[] for _ in range(self.nsets)]  # LRU: way ids, most recent first
        self.queue = [[] for _ in range(self.nsets)]    # FIFO: way ids, oldest first
        self.trees = [PlruNode(0, assoc) for _ in range(self.nsets)]
        self.rng = 0x12345678
        self.c = dict(reads=0, writes=0, read_hits=0, write_hits=0, read_misses=0,
                      write_misses=0, evictions=0, writebacks=0)

    def _rand(self):
        x = self.rng
        x ^= (x << 13) & MASK32
        x ^= x >> 17
        x ^= (x << 5) & MASK32
        self.rng = x
        return x

    def locate(self, addr):
        blk = addr // self.block
        return blk % self.nsets, blk // self.nsets

    def touch(self, s, way, fill):
        if self.policy == "lru":
            if way in self.recency[s]:
                self.recency[s].remove(way)
            self.recency[s].insert(0, way)
        elif self.policy == "fifo":
            if fill:
                self.queue[s].append(way)
        elif self.policy == "plru":
            self.trees[s].touch(way)

    def victim(self, s):
        for w, slot in enumerate(self.slots[s]):
            if slot is None:
                return w
        if self.policy == "lru":
            return self.recency[s][-1]
        if self.policy == "fifo":
            return self.queue[s].pop(0)  # leaves the queue; touch(fill) re-appends
        if self.policy == "plru":
            return self.trees[s].victim()
        return self._rand() % self.assoc


class GoldenHierarchy:
    def __init__(self, levels):
        self.levels = levels
        self.mem_reads = self.mem_writes = 0

    def access(self, addr, is_write):
        self._at(0, addr, is_write)

    def _at(self, i, addr, is_write):
        if i == len(self.levels):
            if is_write:
                self.mem_writes += 1
            else:
                self.mem_reads += 1
            return
        L = self.levels[i]
        L.c["writes" if is_write else "reads"] += 1
        s, tag = L.locate(addr)
        way = next((w for w, sl in enumerate(L.slots[s]) if sl is not None and sl[0] == tag), None)
        if way is not None:
            L.c["write_hits" if is_write else "read_hits"] += 1
            L.touch(s, way, False)
            if is_write:
                if L.wb:
                    L.slots[s][way][1] = True
                else:
                    self._at(i + 1, addr, True)
            return
        L.c["write_misses" if is_write else "read_misses"] += 1
        if is_write and not L.wa:
            self._at(i + 1, addr, True)
            return
        way = L.victim(s)
        old = L.slots[s][way]
        if old is not None:
            L.c["evictions"] += 1
            if old[1]:
                L.c["writebacks"] += 1
                self._at(i + 1, (old[0] * L.nsets + s) * L.block, True)
        self._at(i + 1, addr, False)
        L.slots[s][way] = [tag, False]
        L.touch(s, way, True)
        if is_write:
            if L.wb:
                L.slots[s][way][1] = True
            else:
                self._at(i + 1, addr, True)

    def counters(self):
        out = {}
        for L in self.levels:
            for k, v in L.c.items():
                out[f"{L.name}.{k}"] = v
        out["mem.reads"], out["mem.writes"] = self.mem_reads, self.mem_writes
        return out


def parse_size(s):
    s = s.strip().upper()
    mult = {"K": 1 << 10, "M": 1 << 20}.get(s[-1], 1)
    return int(s[:-1] if s[-1] in "KM" else s) * mult


def build(levels, write_back=True, write_alloc=True):
    """levels: list of (size, assoc, block, policy)."""
    gl = [GoldenLevel(f"L{i + 1}", sz, a, b, p, write_back, write_alloc)
          for i, (sz, a, b, p) in enumerate(levels)]
    return GoldenHierarchy(gl)


def simulate(levels, trace, write_back=True, write_alloc=True):
    """trace: iterable of (is_write, addr). Returns the raw counter dict."""
    h = build(levels, write_back, write_alloc)
    for is_write, addr in trace:
        h.access(addr, is_write)
    return h.counters()


def read_trace(fp):
    for line in fp:
        line = line.split("#")[0].strip()
        if not line:
            continue
        op, addr = line.split()
        yield op.upper() == "W", int(addr, 16) if addr.lower().startswith("0x") else int(addr, 0)


def main():
    ap = argparse.ArgumentParser(description="Golden model; same flags/output as cachesim --raw")
    ap.add_argument("--l1", required=True)
    ap.add_argument("--l2")
    ap.add_argument("--write-policy", default="wb", choices=["wb", "wt"])
    ap.add_argument("--alloc", default="wa", choices=["wa", "nwa"])
    ap.add_argument("trace")
    a = ap.parse_args()
    levels = []
    for spec in filter(None, [a.l1, a.l2]):
        f = spec.split(":")
        levels.append((parse_size(f[0]), int(f[1]), int(f[2]), (f[3] if len(f) > 3 else "lru").lower()))
    fp = sys.stdin if a.trace == "-" else open(a.trace)
    res = simulate(levels, read_trace(fp), a.write_policy == "wb", a.alloc == "wa")
    for k, v in res.items():
        print(k, v)


if __name__ == "__main__":
    main()
