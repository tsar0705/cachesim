#pragma once
#include <cstdint>
#include <string>

namespace cachesim {

struct Access {
    bool is_write = false;
    uint64_t addr = 0;
};

enum class ParseResult { Ok, Skip, Error };

// Format: "<R|W> <address>" (hex with/without 0x, or decimal). '#' starts a comment.
ParseResult parse_trace_line(const std::string& line, Access& out);

}  // namespace cachesim
