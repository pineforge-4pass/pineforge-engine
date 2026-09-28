// Colors as TradingView reads them back (lane W11-ENG-TIME-COLOR,
// tests/fixtures/color_tv/README.md): color.r / g / b / t of every named
// constant, color.t of color.new and color.rgb over a transparency sweep, and
// color.t of a hex literal for every alpha byte. Each reading of each tape is
// replayed through include/pineforge/color.hpp as generated code calls it.
#include <pineforge/color.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "exit_comment_tape.hpp"

#ifndef PINEFORGE_COLOR_TV_FIXTURE_DIR
#error "PINEFORGE_COLOR_TV_FIXTURE_DIR must name tests/fixtures/color_tv"
#endif

using namespace pineforge;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

namespace {

struct Named {
    const char* name;
    int64_t v6;  // pine_color's constant
    int64_t v5;  // TradingView's Pine v5 value
};

// The probes' order: n0 aqua..green, n1 lime..purple, n2 red..yellow. v5
// spells red, teal and yellow differently; the other fourteen agree.
const Named kNamed[] = {
    {"aqua", pine_color::aqua, pine_color::aqua},
    {"black", pine_color::black, pine_color::black},
    {"blue", pine_color::blue, pine_color::blue},
    {"fuchsia", pine_color::fuchsia, pine_color::fuchsia},
    {"gray", pine_color::gray, pine_color::gray},
    {"green", pine_color::green, pine_color::green},
    {"lime", pine_color::lime, pine_color::lime},
    {"maroon", pine_color::maroon, pine_color::maroon},
    {"navy", pine_color::navy, pine_color::navy},
    {"olive", pine_color::olive, pine_color::olive},
    {"orange", pine_color::orange, pine_color::orange},
    {"purple", pine_color::purple, pine_color::purple},
    {"red", pine_color::red, 0xFFFF5252},
    {"silver", pine_color::silver, pine_color::silver},
    {"teal", pine_color::teal, 0xFF00897B},
    {"white", pine_color::white, pine_color::white},
    {"yellow", pine_color::yellow, 0xFFFFEB3B},
};
constexpr std::size_t kNamedCount = sizeof(kNamed) / sizeof(kNamed[0]);

std::string spelled_rgbt(int64_t c) {
    return std::to_string(pine_color::r(c)) + "," + std::to_string(pine_color::g(c)) + ","
        + std::to_string(pine_color::b(c)) + "," + std::to_string(pine_color::t(c));
}

// color.rgb(r, g, b, t) as the transpiler lowers it.
int64_t rgb(int64_t r, int64_t g, int64_t b, double transp) {
    return pine_color::new_color(static_cast<int64_t>((static_cast<uint64_t>(r) & 0xFFULL) << 16
                                                      | (static_cast<uint64_t>(g) & 0xFFULL) << 8
                                                      | (static_cast<uint64_t>(b) & 0xFFULL)),
                                 transp);
}

// The named constants: "n0 r,g,b,t|r,g,b,t|..." for six names, n1 six, n2 five.
void check_named(const std::string& slug, bool v5) {
    bool ok = true;
    const auto readings =
        exit_comment_tape::read(PINEFORGE_COLOR_TV_FIXTURE_DIR, slug, ok);
    CHECK(ok);
    static const std::size_t first[] = {0, 6, 12};
    static const std::size_t count[] = {6, 6, 5};
    int groups = 0, wrong = 0;
    for (const auto& reading : readings) {
        if (reading.signal.size() < 3 || reading.signal[0] != 'n') continue;
        const int group = reading.signal[1] - '0';
        if (group < 0 || group > 2 || reading.signal[2] != ' ') { ++wrong; continue; }
        const auto spelled = exit_comment_tape::split(reading.signal.substr(3), '|');
        ++groups;
        if (spelled.size() != count[group]) { ++wrong; continue; }
        for (std::size_t k = 0; k < spelled.size(); ++k) {
            const Named& n = kNamed[first[group] + k];
            const std::string engine = spelled_rgbt(v5 ? n.v5 : n.v6);
            if (spelled[k] != engine) {
                if (++wrong <= 6) {
                    std::printf("  %s color.%s: tv %s engine %s\n", slug.c_str(), n.name,
                                spelled[k].c_str(), engine.c_str());
                }
            }
        }
    }
    std::printf("  %s: %d named readings, %d differ\n", slug.c_str(), groups, wrong);
    CHECK(groups > 3);
    CHECK(wrong == 0);
}

}  // namespace

static void test_named_constants() {
    std::printf("test_named_constants\n");
    CHECK(kNamedCount == 17);
    check_named("w11-color-v6-eth15", false);
    check_named("w11-color-v5-eth15", true);
}

// "t<k>:a,b,c,d,e": color.t of color.new(color.red, k), color.new(color.red,
// k + 0.5), color.rgb(10, 20, 30, k), color.new(#123456, k) and
// color.new(color.new(color.blue, 50), k), for k = 0 .. 100.
static void test_transparency_sweep() {
    std::printf("test_transparency_sweep\n");
    for (const char* slug : {"w11-color-v6-eth15", "w11-color-v5-eth15"}) {
        bool ok = true;
        const auto readings =
            exit_comment_tape::read(PINEFORGE_COLOR_TV_FIXTURE_DIR, slug, ok);
        CHECK(ok);
        std::vector<bool> seen(101, false);
        int compared = 0, wrong = 0;
        for (const auto& reading : readings) {
            const std::string& s = reading.signal;
            const std::size_t colon = s.find(':');
            if (s.empty() || s[0] != 't' || colon == std::string::npos) continue;
            const int k = std::atoi(s.substr(1, colon - 1).c_str());
            const auto spelled = exit_comment_tape::split(s.substr(colon + 1), ',');
            if (k < 0 || k > 100 || spelled.size() != 5) { ++wrong; continue; }
            seen[static_cast<std::size_t>(k)] = true;
            const int engine[5] = {
                pine_color::t(pine_color::new_color(pine_color::red, k)),
                pine_color::t(pine_color::new_color(pine_color::red, k + 0.5)),
                pine_color::t(rgb(10, 20, 30, k)),
                pine_color::t(pine_color::new_color(0xFF123456, k)),
                pine_color::t(pine_color::new_color(pine_color::new_color(pine_color::blue, 50), k)),
            };
            for (int q = 0; q < 5; ++q) {
                ++compared;
                if (spelled[static_cast<std::size_t>(q)] != std::to_string(engine[q])) {
                    if (++wrong <= 6) {
                        std::printf("  %s t=%d form %d: tv %s engine %d\n", slug, k, q,
                                    spelled[static_cast<std::size_t>(q)].c_str(), engine[q]);
                    }
                }
            }
        }
        int covered = 0;
        for (bool b : seen) covered += b;
        std::printf("  %s: %d readings, t 0..100 covered %d, %d differ\n", slug, compared, covered,
                    wrong);
        CHECK(covered == 101);
        CHECK(wrong == 0);
    }
}

// "a<j>:t,t,t,t": color.t of #FF0000xx for the alpha bytes j .. j + 3.
static void test_alpha_bytes() {
    std::printf("test_alpha_bytes\n");
    bool ok = true;
    const auto readings =
        exit_comment_tape::read(PINEFORGE_COLOR_TV_FIXTURE_DIR, "w11-color-alpha-eth15", ok);
    CHECK(ok);
    std::vector<bool> seen(256, false);
    int wrong = 0;
    for (const auto& reading : readings) {
        const std::string& s = reading.signal;
        const std::size_t colon = s.find(':');
        if (s.empty() || s[0] != 'a' || colon == std::string::npos) { ++wrong; continue; }
        const int j = std::atoi(s.substr(1, colon - 1).c_str());
        const auto spelled = exit_comment_tape::split(s.substr(colon + 1), ',');
        if (j < 0 || j > 252 || spelled.size() != 4) { ++wrong; continue; }
        for (int q = 0; q < 4; ++q) {
            const int64_t alpha = j + q;
            seen[static_cast<std::size_t>(alpha)] = true;
            // A hex literal #RRGGBBAA is packed 0xAARRGGBB.
            const int engine = pine_color::t((alpha << 24) | 0xFF0000);
            if (spelled[static_cast<std::size_t>(q)] != std::to_string(engine)) {
                if (++wrong <= 6) {
                    std::printf("  alpha %lld: tv %s engine %d\n", static_cast<long long>(alpha),
                                spelled[static_cast<std::size_t>(q)].c_str(), engine);
                }
            }
        }
    }
    int covered = 0;
    for (bool b : seen) covered += b;
    std::printf("  w11-color-alpha-eth15: bytes covered %d, %d differ\n", covered, wrong);
    CHECK(covered == 256);
    CHECK(wrong == 0);
}

// What the tapes cannot reach: an na transparency (the transpiler passes 100,
// color.hpp reads an na itself the same way) and one outside 0 .. 100.
static void test_transparency_edges() {
    std::printf("test_transparency_edges\n");
    CHECK(pine_color::t(pine_color::new_color(pine_color::red, 100)) == 100);
    CHECK(pine_color::t(pine_color::new_color(pine_color::red, std::nan(""))) == 100);
    CHECK(pine_color::t(pine_color::new_color(pine_color::red, 250)) == 100);
    CHECK(pine_color::t(pine_color::new_color(pine_color::red, -40)) == 0);
    // The channels are the base color's, whatever the transparency.
    const int64_t c = pine_color::new_color(pine_color::teal, 37);
    CHECK(pine_color::r(c) == 8 && pine_color::g(c) == 153 && pine_color::b(c) == 129);
}

int main() {
    test_named_constants();
    test_transparency_sweep();
    test_alpha_bytes();
    test_transparency_edges();
    std::printf("\ncolor_tapes: %d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
