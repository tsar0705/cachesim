#include "cache.hpp"

#include <cctype>
#include <limits>

namespace cachesim {

namespace {
bool is_pow2(uint64_t x) { return x && !(x & (x - 1)); }
unsigned log2u(uint64_t x) {
    unsigned n = 0;
    while (x > 1) { x >>= 1; ++n; }
    return n;
}
}  // namespace

Policy parse_policy(const std::string& s) {
    std::string t;
    for (char c : s) t += char(std::tolower(static_cast<unsigned char>(c)));
    if (t == "lru") return Policy::LRU;
    if (t == "fifo") return Policy::FIFO;
    if (t == "plru") return Policy::PLRU;
    if (t == "random" || t == "rand") return Policy::RANDOM;
    throw std::invalid_argument("unknown replacement policy '" + s + "' (lru|fifo|plru|random)");
}

const char* policy_name(Policy p) {
    switch (p) {
        case Policy::LRU: return "lru";
        case Policy::FIFO: return "fifo";
        case Policy::PLRU: return "plru";
        case Policy::RANDOM: return "random";
    }
    return "?";
}

void Config::validate() const {
    if (!is_pow2(block_bytes)) throw std::invalid_argument(name + ": block size must be a power of two");
    if (assoc == 0) throw std::invalid_argument(name + ": associativity must be >= 1");
    if (size_bytes == 0 || size_bytes % (uint64_t(assoc) * block_bytes) != 0)
        throw std::invalid_argument(name + ": size must be a multiple of assoc*block");
    if (!is_pow2(num_sets())) throw std::invalid_argument(name + ": number of sets must be a power of two");
    if (policy == Policy::PLRU && !is_pow2(assoc))
        throw std::invalid_argument(name + ": PLRU needs power-of-two associativity");
}

Level::Level(const Config& cfg) : cfg_(cfg) {
    cfg_.validate();
    sets_ = cfg_.num_sets();
    offset_bits_ = log2u(cfg_.block_bytes);
    index_bits_ = log2u(sets_);
    lines_.assign(sets_ * cfg_.assoc, Line{});
    if (cfg_.policy == Policy::PLRU && cfg_.assoc > 1) plru_.assign(sets_ * (cfg_.assoc - 1), 0);
}

int Level::find(uint64_t set, uint64_t tag) const {
    for (uint32_t w = 0; w < cfg_.assoc; ++w) {
        const Line& l = line(set, int(w));
        if (l.valid && l.tag == tag) return int(w);
    }
    return -1;
}

uint32_t Level::next_random() {  // xorshift32
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

int Level::choose_victim(uint64_t set) {
    for (uint32_t w = 0; w < cfg_.assoc; ++w)
        if (!line(set, int(w)).valid) return int(w);
    switch (cfg_.policy) {
        case Policy::LRU:
        case Policy::FIFO: {
            int best = 0;
            uint64_t best_stamp = std::numeric_limits<uint64_t>::max();
            for (uint32_t w = 0; w < cfg_.assoc; ++w) {
                if (line(set, int(w)).stamp < best_stamp) {
                    best_stamp = line(set, int(w)).stamp;
                    best = int(w);
                }
            }
            return best;
        }
        case Policy::PLRU: {
            if (cfg_.assoc == 1) return 0;
            const uint8_t* bits = &plru_[set * (cfg_.assoc - 1)];
            unsigned node = 0, way = 0;
            for (unsigned l = log2u(cfg_.assoc); l > 0; --l) {
                unsigned d = bits[node];  // 0 => victim in left half, 1 => right
                way = (way << 1) | d;
                node = 2 * node + 1 + d;
            }
            return int(way);
        }
        case Policy::RANDOM: return int(next_random() % cfg_.assoc);
    }
    return 0;
}

void Level::touch(uint64_t set, int way, bool is_fill) {
    switch (cfg_.policy) {
        case Policy::LRU: line(set, way).stamp = ++clock_; break;
        case Policy::FIFO:
            if (is_fill) line(set, way).stamp = ++clock_;
            break;
        case Policy::PLRU: {
            if (cfg_.assoc == 1) break;
            uint8_t* bits = &plru_[set * (cfg_.assoc - 1)];
            unsigned node = 0;
            for (unsigned l = log2u(cfg_.assoc); l > 0; --l) {
                unsigned d = (unsigned(way) >> (l - 1)) & 1u;
                bits[node] = d ? 0 : 1;  // point away from the half just used
                node = 2 * node + 1 + d;
            }
            break;
        }
        case Policy::RANDOM: break;
    }
}

Hierarchy::Hierarchy(const std::vector<Config>& cfgs) {
    if (cfgs.empty()) throw std::invalid_argument("hierarchy needs at least one level");
    levels_.reserve(cfgs.size());
    for (size_t i = 0; i < cfgs.size(); ++i) {
        if (i > 0 && cfgs[i].block_bytes < cfgs[i - 1].block_bytes)
            throw std::invalid_argument(cfgs[i].name + ": block size must be >= previous level's block size");
        levels_.emplace_back(cfgs[i]);
    }
}

void Hierarchy::access(uint64_t addr, bool is_write) { access_at(0, addr, is_write); }

void Hierarchy::access_at(size_t i, uint64_t addr, bool is_write) {
    if (i == levels_.size()) {
        (is_write ? mem_writes_ : mem_reads_)++;
        return;
    }
    Level& L = levels_[i];
    const Config& c = L.config();
    Stats& s = L.stats();
    (is_write ? s.writes : s.reads)++;

    const uint64_t set = L.set_index(addr), tag = L.tag_of(addr);
    const int hit_way = L.find(set, tag);

    if (hit_way >= 0) {
        (is_write ? s.write_hits : s.read_hits)++;
        L.touch(set, hit_way, false);
        if (is_write) {
            if (c.write_back) L.line(set, hit_way).dirty = true;
            else access_at(i + 1, addr, true);
        }
        return;
    }

    (is_write ? s.write_misses : s.read_misses)++;
    if (is_write && !c.write_allocate) {
        access_at(i + 1, addr, true);
        return;
    }

    const int way = L.choose_victim(set);
    Level::Line& victim = L.line(set, way);
    if (victim.valid) {
        s.evictions++;
        if (victim.dirty) {
            s.writebacks++;
            access_at(i + 1, L.block_addr(victim.tag, set), true);
        }
    }
    access_at(i + 1, addr, false);  // fill from next level
    Level::Line& fresh = L.line(set, way);
    fresh.valid = true;
    fresh.tag = tag;
    fresh.dirty = false;
    L.touch(set, way, true);
    if (is_write) {
        if (c.write_back) fresh.dirty = true;
        else access_at(i + 1, addr, true);
    }
}

double Hierarchy::amat(const std::vector<double>& hit_latency, double mem_latency) const {
    // AMAT_i = hit_i + local_miss_rate_i * AMAT_{i+1}
    double next = mem_latency;
    for (size_t k = levels_.size(); k-- > 0;) {
        double lat = k < hit_latency.size() ? hit_latency[k] : hit_latency.back();
        next = lat + levels_[k].stats().miss_rate() * next;
    }
    return next;
}

}  // namespace cachesim
