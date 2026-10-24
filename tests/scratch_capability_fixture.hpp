#pragma once

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace s4e_tmp {

struct ScratchEffectiveCapabilities {
    bool known = false;
    bool setuid = false;
    bool dac_override = false;
    bool dac_read_search = false;

    bool may_setuid() const { return !known || setuid; }
    bool may_bypass_dac() const { return !known || dac_override || dac_read_search; }
};

inline ScratchEffectiveCapabilities parse_scratch_effective_capabilities(std::string_view line) {
    if (line.substr(0, 7) != "CapEff:") return {};
    const auto first = line.find_first_not_of(" \t", 7);
    if (first == std::string_view::npos) return {};
    const auto last = line.find_last_not_of(" \t\r\n");
    if (last < first) return {};
    const char* end = line.data() + last + 1;
    std::uint64_t mask = 0;
    const auto parsed = std::from_chars(line.data() + first, end, mask, 16);
    if (parsed.ec != std::errc{} || parsed.ptr != end) return {};
    return {true,
            (mask & (UINT64_C(1) << 7)) != 0, // CAP_SETUID
            (mask & (UINT64_C(1) << 1)) != 0, // CAP_DAC_OVERRIDE
            (mask & (UINT64_C(1) << 2)) != 0}; // CAP_DAC_READ_SEARCH
}

inline ScratchEffectiveCapabilities read_scratch_effective_capabilities() {
    // No caller-supplied paths: only this fixed procfs file can be opened.
    std::puts("S4E capability inspection path: /proc/self/status (fixed path only)");
    std::ifstream status("/proc/self/status");
    if (!status) return {};
    std::string line;
    while (std::getline(status, line)) {
        if (line.compare(0, 7, "CapEff:") == 0)
            return parse_scratch_effective_capabilities(line);
    }
    // Missing, unreadable and malformed CapEff all mean capabilities may be present.
    return {};
}

} // namespace s4e_tmp
