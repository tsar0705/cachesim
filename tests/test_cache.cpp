// Self-contained unit tests (no external framework). Expected values are hand-derived.
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "cache.hpp"
#include "trace.hpp"

using namespace cachesim;

static int g_checks = 0, g_failed = 0;

#define CHECK_EQ(actual, expected)                                                                \
    do {                                                                                          \
        ++g_checks;                                                                               \
        unsigned long long a_ = (unsigned long long)(actual), e_ = (unsigned long long)(expected); \
        if (a_ != e_) {                                                                           \
            ++g_failed;                                                                           \
            std::printf("  FAIL %s:%d: %s = %llu, expected %llu\n", __FILE__, __LINE__, #actual, a_, e_); \
        }                                                                                         \
    } while (0)

#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        ++g_checks;                                                              \
        bool threw_ = false;                                                     \
        try { expr; } catch (const std::exception&) { threw_ = true; }           \
        if (!threw_) { ++g_failed; std::printf("  FAIL %s:%d: expected throw: %s\n", __FILE__, __LINE__, #expr); } \
    } while (0)

static Config cfg(const char* name, uint64_t size, uint32_t assoc, uint32_t block, Policy p = Policy::LRU,
                  bool wb = true, bool wa = true) {
    Config c;
    c.name = name;
    c.size_bytes = size;
    c.assoc = assoc;
    c.block_bytes = block;
    c.policy = p;
    c.write_back = wb;
    c.write_allocate = wa;
    return c;
}

using Op = std::pair<char, uint64_t>;
static void run(Hierarchy& h, const std::vector<Op>& ops) {
    for (auto& o : ops) h.access(o.second, o.first == 'W');
}

static void test_direct_mapped_conflict() {
    // 256 B, 1-way, 16 B blocks => 16 sets; 0 and 256 collide in set 0.
    Hierarchy h({cfg("L1", 256, 1, 16)});
    run(h, {{'R', 0}, {'R', 256}, {'R', 0}});
    const Stats& s = h.level(0).stats();
    CHECK_EQ(s.misses(), 3);
    CHECK_EQ(s.hits(), 0);
    CHECK_EQ(s.evictions, 2);
    CHECK_EQ(h.mem_reads(), 3);
}

static void test_spatial_locality() {
    Hierarchy h({cfg("L1", 1024, 2, 64)});
    run(h, {{'R', 0}, {'R', 8}, {'R', 63}, {'R', 64}});
    const Stats& s = h.level(0).stats();
    CHECK_EQ(s.read_misses, 2);
    CHECK_EQ(s.read_hits, 2);
}

static void test_lru_vs_fifo() {
    // One set, 2 ways. A=0, B=64, C=128. Sequence A B A C A.
    std::vector<Op> seq = {{'R', 0}, {'R', 64}, {'R', 0}, {'R', 128}, {'R', 0}};
    Hierarchy lru({cfg("L1", 128, 2, 64, Policy::LRU)});
    run(lru, seq);
    CHECK_EQ(lru.level(0).stats().hits(), 2);  // A at #3 and #5 (B was evicted, not A)
    Hierarchy fifo({cfg("L1", 128, 2, 64, Policy::FIFO)});
    run(fifo, seq);
    CHECK_EQ(fifo.level(0).stats().hits(), 1);  // A evicted by C because it entered first
}

static void test_plru_differs_from_lru() {
    // One set, 4 ways. Fill 0,64,128,192 then touch 0, then miss on 256.
    // LRU evicts 64; tree-PLRU evicts the block in way 2 (128).
    std::vector<Op> seq = {{'R', 0}, {'R', 64}, {'R', 128}, {'R', 192}, {'R', 0}, {'R', 256}, {'R', 64}, {'R', 128}};
    Hierarchy plru({cfg("L1", 256, 4, 64, Policy::PLRU)});
    run(plru, seq);
    CHECK_EQ(plru.level(0).stats().hits(), 2);  // R0 and R64 hit; R128 was the PLRU victim
    CHECK_EQ(plru.level(0).stats().misses(), 6);
    Hierarchy lru({cfg("L1", 256, 4, 64, Policy::LRU)});
    run(lru, seq);
    CHECK_EQ(lru.level(0).stats().hits(), 1);  // only R0 hits
    CHECK_EQ(lru.level(0).stats().misses(), 7);
}

static void test_write_back_dirty_eviction() {
    Hierarchy h({cfg("L1", 64, 1, 64)});  // one line
    run(h, {{'W', 0}, {'R', 64}});
    const Stats& s = h.level(0).stats();
    CHECK_EQ(s.write_misses, 1);
    CHECK_EQ(s.evictions, 1);
    CHECK_EQ(s.writebacks, 1);
    CHECK_EQ(h.mem_reads(), 2);
    CHECK_EQ(h.mem_writes(), 1);
}

static void test_clean_eviction_no_writeback() {
    Hierarchy h({cfg("L1", 64, 1, 64)});
    run(h, {{'R', 0}, {'R', 64}});
    CHECK_EQ(h.level(0).stats().writebacks, 0);
    CHECK_EQ(h.mem_writes(), 0);
}

static void test_write_through() {
    Hierarchy h({cfg("L1", 64, 1, 64, Policy::LRU, /*wb=*/false)});
    run(h, {{'W', 0}, {'W', 0}});
    CHECK_EQ(h.mem_reads(), 1);   // allocate on first write miss
    CHECK_EQ(h.mem_writes(), 2);  // every write goes through
    CHECK_EQ(h.level(0).stats().writebacks, 0);
}

static void test_no_write_allocate() {
    Hierarchy h({cfg("L1", 64, 1, 64, Policy::LRU, true, /*wa=*/false)});
    run(h, {{'W', 0}, {'R', 0}, {'R', 0}});
    const Stats& s = h.level(0).stats();
    CHECK_EQ(s.write_misses, 1);
    CHECK_EQ(s.read_misses, 1);  // the write did not allocate
    CHECK_EQ(s.read_hits, 1);
    CHECK_EQ(h.mem_writes(), 1);
    CHECK_EQ(h.mem_reads(), 1);
}

static void test_two_level() {
    Hierarchy h({cfg("L1", 64, 1, 64), cfg("L2", 256, 4, 64)});
    run(h, {{'R', 0}, {'R', 64}, {'R', 0}});
    CHECK_EQ(h.level(0).stats().misses(), 3);
    CHECK_EQ(h.level(1).stats().accesses(), 3);
    CHECK_EQ(h.level(1).stats().read_hits, 1);  // third access finds block 0 in L2
    CHECK_EQ(h.mem_reads(), 2);
}

static void test_writeback_lands_in_l2() {
    Hierarchy h({cfg("L1", 64, 1, 64), cfg("L2", 256, 4, 64)});
    run(h, {{'W', 0}, {'R', 64}});
    // L1 evicts dirty block 0 into L2, where it already resides from the first fill.
    CHECK_EQ(h.level(0).stats().writebacks, 1);
    CHECK_EQ(h.level(1).stats().write_hits, 1);
    CHECK_EQ(h.mem_writes(), 0);
}

static void test_validation() {
    CHECK_THROWS(Hierarchy({cfg("L1", 1024, 2, 48)}));                 // block not pow2
    CHECK_THROWS(Hierarchy({cfg("L1", 1000, 2, 64)}));                 // size not multiple
    CHECK_THROWS(Hierarchy({cfg("L1", 192, 1, 64)}));                  // 3 sets
    CHECK_THROWS(Hierarchy({cfg("L1", 192, 3, 64, Policy::PLRU)}));    // PLRU, 3-way
    CHECK_THROWS(Hierarchy({cfg("L1", 1024, 0, 64)}));                 // zero ways
    CHECK_THROWS(Hierarchy({cfg("L1", 1024, 2, 64), cfg("L2", 4096, 2, 32)}));  // shrinking block
    CHECK_THROWS(parse_policy("mru"));
}

static void test_random_is_deterministic() {
    std::vector<Op> seq;
    for (uint64_t i = 0; i < 2000; ++i) seq.push_back({i % 7 == 0 ? 'W' : 'R', (i * 2654435761ULL) % 8192});
    Hierarchy a({cfg("L1", 512, 4, 32, Policy::RANDOM)});
    Hierarchy b({cfg("L1", 512, 4, 32, Policy::RANDOM)});
    run(a, seq);
    run(b, seq);
    CHECK_EQ(a.level(0).stats().misses(), b.level(0).stats().misses());
    CHECK_EQ(a.level(0).stats().evictions, b.level(0).stats().evictions);
}

static void test_conservation_invariants() {
    // For write-back + write-allocate, every L1 miss triggers exactly one L2 read and every
    // L1 writeback one L2 write; the same relation holds between L2 and memory.
    for (Policy p : {Policy::LRU, Policy::FIFO, Policy::PLRU, Policy::RANDOM}) {
        Hierarchy h({cfg("L1", 1024, 4, 32, p), cfg("L2", 8192, 8, 64, p)});
        uint64_t x = 88172645463325252ULL;
        for (int i = 0; i < 20000; ++i) {
            x ^= x << 13; x ^= x >> 7; x ^= x << 17;
            h.access(x % 65536, (x >> 40) % 3 == 0);
        }
        const Stats& l1 = h.level(0).stats();
        const Stats& l2 = h.level(1).stats();
        CHECK_EQ(l1.hits() + l1.misses(), l1.accesses());
        CHECK_EQ(l2.reads, l1.misses());
        CHECK_EQ(l2.writes, l1.writebacks);
        CHECK_EQ(h.mem_reads(), l2.misses());
        CHECK_EQ(h.mem_writes(), l2.writebacks);
        CHECK_EQ(l1.evictions >= l1.writebacks, 1);
    }
}

static void test_trace_parser() {
    Access a;
    CHECK_EQ(parse_trace_line("R 0x1f", a) == ParseResult::Ok, 1);
    CHECK_EQ(a.addr, 0x1f);
    CHECK_EQ(a.is_write, 0);
    CHECK_EQ(parse_trace_line("  w   64   # comment", a) == ParseResult::Ok, 1);
    CHECK_EQ(a.addr, 64);
    CHECK_EQ(a.is_write, 1);
    CHECK_EQ(parse_trace_line("W deadbeef", a) == ParseResult::Ok, 1);
    CHECK_EQ(a.addr, 0xdeadbeefULL);
    CHECK_EQ(parse_trace_line("", a) == ParseResult::Skip, 1);
    CHECK_EQ(parse_trace_line("# only a comment", a) == ParseResult::Skip, 1);
    CHECK_EQ(parse_trace_line("X 10", a) == ParseResult::Error, 1);
    CHECK_EQ(parse_trace_line("R", a) == ParseResult::Error, 1);
    CHECK_EQ(parse_trace_line("R 0xZZ", a) == ParseResult::Error, 1);
    CHECK_EQ(parse_trace_line("R 12 34", a) == ParseResult::Error, 1);
    CHECK_EQ(parse_trace_line("R -5", a) == ParseResult::Error, 1);
    CHECK_EQ(parse_trace_line("R 0xFFFFFFFFFFFFFFFF", a) == ParseResult::Ok, 1);
    CHECK_EQ(parse_trace_line("R 0x1FFFFFFFFFFFFFFFF", a) == ParseResult::Error, 1);
}

static void test_amat() {
    Hierarchy h({cfg("L1", 64, 1, 64)});
    run(h, {{'R', 0}, {'R', 0}, {'R', 0}, {'R', 0}});  // 1 miss, 3 hits => miss rate 0.25
    double amat = h.amat({1.0}, 100.0);                 // 1 + 0.25*100
    CHECK_EQ(amat == 26.0, 1);
}

int main() {
    struct T { const char* name; std::function<void()> fn; };
    std::vector<T> tests = {
        {"direct_mapped_conflict", test_direct_mapped_conflict},
        {"spatial_locality", test_spatial_locality},
        {"lru_vs_fifo", test_lru_vs_fifo},
        {"plru_differs_from_lru", test_plru_differs_from_lru},
        {"write_back_dirty_eviction", test_write_back_dirty_eviction},
        {"clean_eviction_no_writeback", test_clean_eviction_no_writeback},
        {"write_through", test_write_through},
        {"no_write_allocate", test_no_write_allocate},
        {"two_level", test_two_level},
        {"writeback_lands_in_l2", test_writeback_lands_in_l2},
        {"validation", test_validation},
        {"random_is_deterministic", test_random_is_deterministic},
        {"conservation_invariants", test_conservation_invariants},
        {"trace_parser", test_trace_parser},
        {"amat", test_amat},
    };
    for (auto& t : tests) {
        int before = g_failed;
        t.fn();
        std::printf("[%s] %s\n", g_failed == before ? " ok " : "FAIL", t.name);
    }
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
