#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <system_error>

namespace pineforge::internal::canonical_json {

// One UTF-8 sequence starting at `i`, or 0 when it is not well-formed.
inline std::size_t utf8_sequence(const std::string& text, std::size_t i) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    else if (lead == 0xE0) { length = 3; low = 0xA0; }
    else if (lead >= 0xE1 && lead <= 0xEC) length = 3;
    else if (lead == 0xED) { length = 3; high = 0x9F; }
    else if (lead >= 0xEE && lead <= 0xEF) length = 3;
    else if (lead == 0xF0) { length = 4; low = 0x90; }
    else if (lead >= 0xF1 && lead <= 0xF3) length = 4;
    else if (lead == 0xF4) { length = 4; high = 0x8F; }
    else return 0;
    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char c = byte(i + k);
        if (c < (k == 1 ? low : 0x80) || c > (k == 1 ? high : 0xBF)) return 0;
    }
    return length;
}

// A JSON string: quotes, backslashes and control characters escaped; a byte
// that is not well-formed UTF-8 becomes U+FFFD.
inline void append_json_string(std::string& out, const std::string& text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (c < 0x20 || c == 0x7F) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
            ++i;
        } else {
            const std::size_t length = utf8_sequence(text, i);
            if (length == 0) {
                out += "\xEF\xBF\xBD";
                ++i;
            } else {
                out.append(text, i, length);
                i += length;
            }
        }
    }
    out.push_back('"');
}

inline bool append_json_number(std::string& out, double value) {
    if (!std::isfinite(value)) return false;
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (converted.ec != std::errc()) return false;
    out.append(buffer, converted.ptr);
    return true;
}

} // namespace pineforge::internal::canonical_json
