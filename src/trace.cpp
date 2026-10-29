#include "trace.hpp"

#include <cctype>
#include <cerrno>
#include <cstdlib>

namespace cachesim {

ParseResult parse_trace_line(const std::string& raw, Access& out) {
    std::string line = raw.substr(0, raw.find('#'));
    size_t i = 0;
    auto is_ws = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    while (i < line.size() && is_ws(line[i])) ++i;
    if (i == line.size()) return ParseResult::Skip;

    char op = char(std::toupper(static_cast<unsigned char>(line[i++])));
    if (op != 'R' && op != 'W') return ParseResult::Error;
    if (i == line.size() || !is_ws(line[i])) return ParseResult::Error;
    while (i < line.size() && is_ws(line[i])) ++i;
    if (i == line.size()) return ParseResult::Error;

    const char* start = line.c_str() + i;
    if (start[0] == '-') return ParseResult::Error;
    int base = 10;
    if (start[0] == '0' && (start[1] == 'x' || start[1] == 'X')) base = 16;
    else {
        for (const char* p = start; *p && !is_ws(*p); ++p)
            if (std::isxdigit(static_cast<unsigned char>(*p)) && !std::isdigit(static_cast<unsigned char>(*p))) base = 16;
    }
    char* end = nullptr;
    errno = 0;
    unsigned long long v = std::strtoull(start, &end, base);
    if (end == start || errno == ERANGE) return ParseResult::Error;
    while (*end && is_ws(*end)) ++end;
    if (*end != '\0') return ParseResult::Error;

    out.is_write = (op == 'W');
    out.addr = v;
    return ParseResult::Ok;
}

}  // namespace cachesim
