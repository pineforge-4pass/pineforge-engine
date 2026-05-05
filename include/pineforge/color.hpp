#pragma once
#include <cstdint>

namespace pineforge {

namespace pine_color {
    constexpr int64_t red = 0xFFFF0000;
    constexpr int64_t green = 0xFF4CAF50;
    constexpr int64_t blue = 0xFF2196F3;
    constexpr int64_t white = 0xFFFFFFFF;
    constexpr int64_t black = 0xFF000000;
    constexpr int64_t yellow = 0xFFFFEB3B;
    constexpr int64_t orange = 0xFFFF9800;
    constexpr int64_t purple = 0xFF9C27B0;
    constexpr int64_t aqua = 0xFF00BCD4;
    constexpr int64_t gray = 0xFF787B86;
    constexpr int64_t lime = 0xFF00E676;
    constexpr int64_t maroon = 0xFF880E4F;
    constexpr int64_t navy = 0xFF311B92;
    constexpr int64_t olive = 0xFF808000;
    constexpr int64_t silver = 0xFFC0C0C0;
    constexpr int64_t teal = 0xFF00897B;
    constexpr int64_t fuchsia = 0xFFE040FB;
    inline int64_t new_color(int64_t c, int transp) {
        int alpha = (int)((100 - transp) * 2.55);
        return (c & 0x00FFFFFF) | ((int64_t)alpha << 24);
    }
    inline int r(int64_t c) { return (int)((c >> 16) & 0xFF); }
    inline int g(int64_t c) { return (int)((c >> 8) & 0xFF); }
    inline int b(int64_t c) { return (int)(c & 0xFF); }
    inline int t(int64_t c) { return 100 - (int)((c >> 24) & 0xFF) * 100 / 255; }
}

} // namespace pineforge
