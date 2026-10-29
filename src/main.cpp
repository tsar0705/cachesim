// cachesim CLI: replay a memory trace through a configurable cache hierarchy.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "cache.hpp"
#include "trace.hpp"

using namespace cachesim;

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s --l1 SIZE:ASSOC:BLOCK[:POLICY] [--l2 SIZE:ASSOC:BLOCK[:POLICY]] [options] TRACE|-\n"
        "  SIZE accepts K/M suffixes (32K, 1M). POLICY: lru|fifo|plru|random (default lru)\n"
        "Options:\n"
        "  --write-policy wb|wt   write-back (default) or write-through, all levels\n"
        "  --alloc wa|nwa         write-allocate (default) or no-write-allocate\n"
        "  --latency L1,L2,MEM    cycles for AMAT report (default 1,10,100)\n"
        "  --raw                  machine-readable 'key value' output\n",
        prog);
}

uint64_t parse_size(const std::string& s) {
    if (s.empty()) throw std::invalid_argument("empty size");
    size_t pos = 0;
    unsigned long long v = std::stoull(s, &pos, 10);
    if (pos < s.size()) {
        char u = char(std::toupper(static_cast<unsigned char>(s[pos])));
        if (pos + 1 != s.size()) throw std::invalid_argument("bad size '" + s + "'");
        if (u == 'K') v <<= 10;
        else if (u == 'M') v <<= 20;
        else throw std::invalid_argument("bad size suffix in '" + s + "'");
    }
    return v;
}

Config parse_level(const std::string& name, const std::string& spec) {
    std::vector<std::string> f;
    std::stringstream ss(spec);
    for (std::string tok; std::getline(ss, tok, ':');) f.push_back(tok);
    if (f.size() < 3 || f.size() > 4) throw std::invalid_argument(name + ": expected SIZE:ASSOC:BLOCK[:POLICY]");
    Config c;
    c.name = name;
    c.size_bytes = parse_size(f[0]);
    c.assoc = uint32_t(std::stoul(f[1]));
    c.block_bytes = uint32_t(std::stoul(f[2]));
    if (f.size() == 4) c.policy = parse_policy(f[3]);
    return c;
}

void print_raw(const Hierarchy& h) {
    for (size_t i = 0; i < h.num_levels(); ++i) {
        const auto& n = h.level(i).config().name;
        const auto& s = h.level(i).stats();
        std::printf("%s.reads %llu\n%s.writes %llu\n%s.read_hits %llu\n%s.write_hits %llu\n"
                    "%s.read_misses %llu\n%s.write_misses %llu\n%s.evictions %llu\n%s.writebacks %llu\n",
                    n.c_str(), (unsigned long long)s.reads, n.c_str(), (unsigned long long)s.writes,
                    n.c_str(), (unsigned long long)s.read_hits, n.c_str(), (unsigned long long)s.write_hits,
                    n.c_str(), (unsigned long long)s.read_misses, n.c_str(), (unsigned long long)s.write_misses,
                    n.c_str(), (unsigned long long)s.evictions, n.c_str(), (unsigned long long)s.writebacks);
    }
    std::printf("mem.reads %llu\nmem.writes %llu\n", (unsigned long long)h.mem_reads(),
                (unsigned long long)h.mem_writes());
}

void print_report(const Hierarchy& h, const std::vector<double>& lat, double mem_lat) {
    for (size_t i = 0; i < h.num_levels(); ++i) {
        const Config& c = h.level(i).config();
        const Stats& s = h.level(i).stats();
        std::printf("%s: %llu B, %u-way, %u B blocks, %s, %s, %s\n", c.name.c_str(),
                    (unsigned long long)c.size_bytes, c.assoc, c.block_bytes, policy_name(c.policy),
                    c.write_back ? "write-back" : "write-through",
                    c.write_allocate ? "write-allocate" : "no-write-allocate");
        std::printf("  accesses %llu (R %llu / W %llu)  hits %llu  misses %llu  miss rate %.4f\n",
                    (unsigned long long)s.accesses(), (unsigned long long)s.reads,
                    (unsigned long long)s.writes, (unsigned long long)s.hits(),
                    (unsigned long long)s.misses(), s.miss_rate());
        std::printf("  read misses %llu  write misses %llu  evictions %llu  writebacks %llu\n",
                    (unsigned long long)s.read_misses, (unsigned long long)s.write_misses,
                    (unsigned long long)s.evictions, (unsigned long long)s.writebacks);
    }
    std::printf("Memory: %llu reads, %llu writes\n", (unsigned long long)h.mem_reads(),
                (unsigned long long)h.mem_writes());
    std::printf("AMAT: %.3f cycles\n", h.amat(lat, mem_lat));
}

}  // namespace

int main(int argc, char** argv) {
    std::string l1, l2, trace_path;
    bool write_back = true, write_alloc = true, raw = false;
    std::vector<double> lat = {1, 10};
    double mem_lat = 100;
    try {
        for (int i = 1; i < argc; ++i) {
            std::string a = argv[i];
            auto need = [&](const char* flag) -> std::string {
                if (i + 1 >= argc) throw std::invalid_argument(std::string(flag) + " needs a value");
                return argv[++i];
            };
            if (a == "--l1") l1 = need("--l1");
            else if (a == "--l2") l2 = need("--l2");
            else if (a == "--write-policy") {
                std::string v = need("--write-policy");
                if (v == "wb") write_back = true;
                else if (v == "wt") write_back = false;
                else throw std::invalid_argument("--write-policy must be wb or wt");
            } else if (a == "--alloc") {
                std::string v = need("--alloc");
                if (v == "wa") write_alloc = true;
                else if (v == "nwa") write_alloc = false;
                else throw std::invalid_argument("--alloc must be wa or nwa");
            } else if (a == "--latency") {
                std::stringstream ss(need("--latency"));
                std::vector<double> v;
                for (std::string t; std::getline(ss, t, ',');) v.push_back(std::stod(t));
                if (v.size() < 2) throw std::invalid_argument("--latency needs at least L1,MEM");
                mem_lat = v.back();
                v.pop_back();
                lat = v;
            } else if (a == "--raw") raw = true;
            else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
            else if (!a.empty() && a[0] == '-' && a != "-") throw std::invalid_argument("unknown option " + a);
            else trace_path = a;
        }
        if (l1.empty() || trace_path.empty()) { usage(argv[0]); return 2; }

        std::vector<Config> cfgs;
        cfgs.push_back(parse_level("L1", l1));
        if (!l2.empty()) cfgs.push_back(parse_level("L2", l2));
        for (auto& c : cfgs) { c.write_back = write_back; c.write_allocate = write_alloc; }
        Hierarchy h(cfgs);

        std::ifstream file;
        std::istream* in = &std::cin;
        if (trace_path != "-") {
            file.open(trace_path);
            if (!file) { std::fprintf(stderr, "error: cannot open %s\n", trace_path.c_str()); return 1; }
            in = &file;
        }
        std::string line;
        uint64_t lineno = 0;
        while (std::getline(*in, line)) {
            ++lineno;
            Access acc;
            ParseResult r = parse_trace_line(line, acc);
            if (r == ParseResult::Skip) continue;
            if (r == ParseResult::Error) {
                std::fprintf(stderr, "error: %s:%llu: malformed trace line: %s\n", trace_path.c_str(),
                             (unsigned long long)lineno, line.c_str());
                return 1;
            }
            h.access(acc.addr, acc.is_write);
        }
        if (raw) print_raw(h);
        else print_report(h, lat, mem_lat);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 2;
    }
    return 0;
}
