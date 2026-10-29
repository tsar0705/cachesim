# cachesim: trace-driven CPU cache hierarchy simulator (C++17)

A configurable 1- or 2-level cache simulator that replays memory traces and reports hit/miss
behaviour, evictions, write-backs, memory traffic and AMAT. It is verified against an
independently written Python reference model with randomized differential testing.

## Features
- Set-associative caches (direct-mapped to fully associative), power-of-two geometry validated
- Replacement: **LRU, FIFO, tree-PLRU, seeded random (xorshift32)**
- **Write-back / write-through** and **write-allocate / no-write-allocate**
- L1 + optional L2 (non-inclusive); dirty evictions are propagated down the hierarchy as writes
- Trace format: `R|W <addr>` (hex/decimal, `#` comments); malformed lines are reported with line numbers
- Report mode and `--raw` machine-readable mode; AMAT from configurable latencies

## Build and run
```
make            # build/cachesim  (-Wall -Wextra -Wpedantic -Werror)
make test       # unit tests (85 checks, hand-derived expectations)
make diff       # 300 randomized cases vs. the Python golden model
make asan       # unit + differential tests under AddressSanitizer + UBSan
make sweep      # parameter sweeps -> results/*.csv, results/SUMMARY.md
```
```
python3 tools/tracegen.py matmul --dim 128 -o mm.trace
./build/cachesim --l1 32K:8:64:plru --l2 256K:8:64:lru --latency 1,10,100 mm.trace
```
Level spec: `SIZE:ASSOC:BLOCK[:POLICY]` (`32K`, `1M` suffixes). Global flags: `--write-policy wb|wt`,
`--alloc wa|nwa`, `--raw`.

## Behavioural specification (shared by both implementations)
- Victim: lowest-index invalid way, otherwise per policy. LRU = least recent touch, FIFO = oldest fill,
  PLRU = binary tree pointing away from the last-used half, RANDOM = `xorshift32 % assoc` (seed `0x12345678` per level).
- Allocating miss: (1) if the victim is valid and dirty, write it to the next level; (2) read the block from the
  next level; (3) install; if the access is a write apply the write policy.
- Write-through forwards every write; no-write-allocate forwards write misses without filling.

## Verification
| Layer | What it checks |
|---|---|
| `tests/test_cache.cpp` | Hand-computed scenarios: conflict misses, LRU vs FIFO vs PLRU divergence, dirty-eviction write-back, write-through, no-write-allocate, L1->L2 write-back, config validation, trace parser edge cases, conservation invariants (L2 reads == L1 misses, L2 writes == L1 write-backs, mem reads == L2 misses, ...) |
| `tools/golden_model.py` | Independent reference model. Different data structures (recency lists / queues / recursive PLRU tree vs. timestamps / flat bit array), same written spec |
| `tools/difftest.py` | Random legal configs (1-2 levels, sizes, ways, blocks, policies, write/alloc policies) x random traces; compares **every counter** |
| Mutation check | 7 deliberately injected bugs (flipped PLRU direction, FIFO updating on hits, dirty bit not set on write hit, wrong write-back address, off-by-one random victim, write-through dropping a forwarded write, counting invalid-slot fills as evictions) were all caught by `difftest.py` |
| Sanitizers | Unit + differential tests clean under ASan + UBSan |

Note: the golden model and simulator share one author, so the differential test catches implementation
bugs but not a misreading of the specification. The spec above is what the unit tests encode by hand.

## Results (from `make sweep`, see `results/SUMMARY.md`)
- **Loop tiling:** 128x128 double matmul, 64 B blocks, LRU. On a 32 KB 4-way L1 the L1 miss rate falls from
  **0.2526 (naive i-j-k) to 0.0045 (16x16 tiles), a 98.2% reduction**. With 4 KB and 8 KB caches the 16x16 tiles
  (3 x 2 KB) do not fit, and tiling gives no benefit, as expected.
- **Replacement policy:** on a 9 KB cyclic loop over an 8 KB 4-way cache, LRU/FIFO/PLRU all miss 7.0% of
  accesses while random replacement misses 2.8% (the textbook LRU pathology). On a hot/cold workload LRU (14.4%)
  beats PLRU (15.8%) beats FIFO (21.9%) and random (22.0%).
- **Hierarchy:** adding a 256 KB L2 to an 8 KB L1 cuts AMAT on the hotspot trace from 15.36 to 8.10 cycles
  (latencies 1/10/100).
- Throughput about 7 M simulated accesses/s including trace parsing (5 M-access trace, 32K PLRU L1 + 1M L2).

## Limitations
Single-core, no coherence, no timing/pipeline model, no prefetching, no inclusive/exclusive policies,
virtual == physical addresses, 64-bit addresses only. AMAT uses local miss rates and ignores write-buffer effects.

## Layout
```
src/cache.{hpp,cpp}   core model (Level, Hierarchy)      src/trace.{hpp,cpp}  trace parser
src/main.cpp          CLI                                tests/test_cache.cpp unit tests
tools/golden_model.py tools/tracegen.py tools/difftest.py tools/sweep.py
.github/workflows/ci.yml   Makefile   results/
```
