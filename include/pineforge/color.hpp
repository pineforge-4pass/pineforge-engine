#pragma once
#include <cmath>
#include <cstdint>

namespace pineforge {

// A color is 0xAARRGGBB: alpha is opacity, 0xFF opaque. The named constants
// are TradingView's Pine v6 values; v5 spells three differently (color.red
// #FF5252, color.teal #00897B, color.yellow #FFEB3B), which is the
// transpiler's to lower (lab tv tapes w11-color-v6-eth15 / w11-color-v5-eth15,
// tests/fixtures/color_tv).
namespace pine_color {
    constexpr int64_t red = 0xFFF23645;
    constexpr int64_t green = 0xFF4CAF50;
    constexpr int64_t blue = 0xFF2962FF;
    constexpr int64_t white = 0xFFFFFFFF;
    constexpr int64_t black = 0xFF363A45;
    constexpr int64_t yellow = 0xFFFDD835;
    constexpr int64_t orange = 0xFFFF9800;
    constexpr int64_t purple = 0xFF9C27B0;
    constexpr int64_t aqua = 0xFF00BCD4;
    constexpr int64_t gray = 0xFF787B86;
    constexpr int64_t lime = 0xFF00E676;
    constexpr int64_t maroon = 0xFF880E4F;
    constexpr int64_t navy = 0xFF311B92;
    constexpr int64_t olive = 0xFF808000;
    constexpr int64_t silver = 0xFFB2B5BE;
    constexpr int64_t teal = 0xFF089981;
    constexpr int64_t fuchsia = 0xFFE040FB;
    // Transparency t (0 opaque .. 100 invisible) is stored as the alpha byte
    // nearest 255 * (100 - t) / 100, and color.t reads a byte a back as
    // 100 - a * 100 / 255 rounded to the nearest whole number, so a whole t
    // reads back as itself (w11-color-v6-eth15's sweep; w11-color-alpha-eth15
    // reads all 256 bytes). A t outside 0 .. 100 stores the nearer end, and
    // an na t is 100, as a missing one is.
    inline int64_t new_color(int64_t c, double transp) {
        const double opacity = std::floor(255.0 * (100.0 - transp) / 100.0 + 0.5);
        const int64_t alpha = !(opacity > 0.0) ? 0 : opacity >= 255.0 ? 255
                                                   : static_cast<int64_t>(opacity);
        return (c & 0x00FFFFFF) | (alpha << 24);
    }
    inline int r(int64_t c) { return (int)((c >> 16) & 0xFF); }
    inline int g(int64_t c) { return (int)((c >> 8) & 0xFF); }
    inline int b(int64_t c) { return (int)(c & 0xFF); }
    inline int t(int64_t c) { return 100 - (int)((((c >> 24) & 0xFF) * 200 + 255) / 510); }
}

} // namespace pineforge
