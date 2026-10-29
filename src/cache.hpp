// cachesim: trace-driven, configurable multi-level CPU cache simulator.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cachesim {

enum class Policy { LRU, FIFO, PLRU, RANDOM };

Policy parse_policy(const std::string& s);  // throws std::invalid_argument
const char* policy_name(Policy p);

struct Config {
    std::string name = "L1";
    uint64_t size_bytes = 32 * 1024;
    uint32_t assoc = 8;
    uint32_t block_bytes = 64;
    Policy policy = Policy::LRU;
    bool write_back = true;      // false => write-through
    bool write_allocate = true;  // false => no-write-allocate

    // Throws std::invalid_argument on an impossible geometry.
    void validate() const;
    uint64_t num_sets() const { return size_bytes / (uint64_t(assoc) * block_bytes); }
};

struct Stats {
    uint64_t reads = 0, writes = 0;
    uint64_t read_hits = 0, write_hits = 0;
    uint64_t read_misses = 0, write_misses = 0;
    uint64_t evictions = 0;   // valid lines replaced
    uint64_t writebacks = 0;  // dirty lines written to the next level

    uint64_t accesses() const { return reads + writes; }
    uint64_t hits() const { return read_hits + write_hits; }
    uint64_t misses() const { return read_misses + write_misses; }
    double miss_rate() const { return accesses() ? double(misses()) / double(accesses()) : 0.0; }
};

// One cache level: tag store + replacement state. Knows nothing about other levels.
class Level {
public:
    explicit Level(const Config& cfg);

    const Config& config() const { return cfg_; }
    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }

    uint64_t set_index(uint64_t addr) const { return (addr >> offset_bits_) & (sets_ - 1); }
    uint64_t tag_of(uint64_t addr) const { return addr >> (offset_bits_ + index_bits_); }
    uint64_t block_addr(uint64_t tag, uint64_t set) const {
        return ((tag << index_bits_) | set) << offset_bits_;
    }

    int find(uint64_t set, uint64_t tag) const;  // way or -1
    int choose_victim(uint64_t set);             // invalid way first, else policy
    void touch(uint64_t set, int way, bool is_fill);

    struct Line {
        uint64_t tag = 0;
        uint64_t stamp = 0;
        bool valid = false;
        bool dirty = false;
    };
    Line& line(uint64_t set, int way) { return lines_[set * cfg_.assoc + way]; }
    const Line& line(uint64_t set, int way) const { return lines_[set * cfg_.assoc + way]; }

private:
    uint32_t next_random();

    Config cfg_;
    Stats stats_;
    uint64_t sets_;
    unsigned offset_bits_, index_bits_;
    std::vector<Line> lines_;
    std::vector<uint8_t> plru_;  // (assoc-1) bits per set
    uint64_t clock_ = 0;
    uint32_t rng_ = 0x12345678u;
};

// Non-inclusive, non-exclusive hierarchy ending in main memory.
//
// Miss handling order on an allocating miss at level i:
//   1. choose victim; if valid & dirty -> write victim block to level i+1
//   2. read the requested block from level i+1
//   3. install line; if the access is a write apply the write policy
class Hierarchy {
public:
    explicit Hierarchy(const std::vector<Config>& levels);

    void access(uint64_t addr, bool is_write);

    size_t num_levels() const { return levels_.size(); }
    const Level& level(size_t i) const { return levels_[i]; }
    uint64_t mem_reads() const { return mem_reads_; }
    uint64_t mem_writes() const { return mem_writes_; }

    // AMAT in cycles from per-level hit latencies and memory latency.
    double amat(const std::vector<double>& hit_latency, double mem_latency) const;

private:
    void access_at(size_t i, uint64_t addr, bool is_write);

    std::vector<Level> levels_;
    uint64_t mem_reads_ = 0, mem_writes_ = 0;
};

}  // namespace cachesim
