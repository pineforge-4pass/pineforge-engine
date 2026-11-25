#include <pineforge/ta.hpp>

#include <climits>
#include <cstdio>

namespace {
using namespace pineforge;
static_assert(INT_MAX == 2147483647);

// These outer counters are saved for replay but do not determine the return
// value. Inspect the real objects in addition to checking their values.
template<class Tag, auto Field>
struct Access { friend auto member(Tag) { return Field; } };
struct AtrCount { friend auto member(AtrCount); };
struct TrCount { friend auto member(TrCount); };
struct TsiCount { friend auto member(TsiCount); };
template struct Access<AtrCount, &ta::ATR::bar_count>;
template struct Access<TrCount, &ta::TR::bar_count_>;
template struct Access<TsiCount, &ta::TSI::bar_count_>;

int failures = 0;

bool control(const char* name, const char* reason, bool ok) {
    std::printf("%s %s control %s\n", ok ? "PASS" : "FAIL", name, reason);
    failures += !ok;
    return ok;
}

template<class Next, class Replay>
void boundary(const char* name, bool warm, int& count, double initial, double revised,
              Next next, Replay replay) {
    auto observe = [&](const char* step, double value, double want) {
        const bool ok = count == INT_MAX && value == want;
        std::printf("%s %s %s: count=%d value=%g (want %d, %g)\n",
                    ok ? "PASS" : "FAIL", name, step, count, value, INT_MAX, want);
        failures += !ok;
        return ok;
    };
    bool at_limit = false;
    if (warm) {
        count = INT_MAX - 1;
        at_limit = observe("control last increment", next(), initial);
    } else {
        std::printf("NOT COMPARED %s control last increment: warm-up control failed\n", name);
    }
    if (at_limit) {
        at_limit = observe("control replay last increment", replay(false), initial);
    } else {
        std::printf("NOT COMPARED %s control replay last increment: no confirmed full count\n", name);
    }
    if (!at_limit) {
        std::printf("NOT COMPARED %s compute at limit: no confirmed full count\n", name);
        std::printf("NOT COMPARED %s recompute at limit: no confirmed full count\n", name);
        return;
    }
    // compute saves the confirmed full count; recompute restores it. They
    // each have to handle one more observation from the same starting count.
    observe("compute at limit", next(), initial);
    observe("recompute at limit", replay(true), revised);
}

void atr(bool chart) {
    const char* name = chart ? "ATR/4" : "ATR/3";
    ta::ATR value(2);
    int& count = value.*member(AtrCount{});
    auto next = [&](double high, double low, double previous) {
        return chart ? value.compute(high, low, 7, previous) : value.compute(high, low, 7);
    };
    const double first = next(8, 6, 12);
    const double missing = next(na<double>(), 6, 12);
    const int before_seed = count;
    const double seed = next(10, 4, 14);
    // The two valid ranges are 2, 6 (per-call close) or 6, 10 (chart
    // close). On the revised bar the replacement ranges are 8 and 14;
    // Wilder averages each with the preceding 4 or 8.
    const double expected = chart ? 8.0 : 4.0;
    const bool warm = control(name, "seed on second valid range",
                              is_na(first) && is_na(missing) && before_seed == 1
                              && count == 2 && seed == expected);
    boundary(name, warm, count, expected, chart ? 11.0 : 6.0, [&] { return next(9, 5, 13); },
             [&](bool revised) {
        const double high = revised ? 11 : 9, low = revised ? 3 : 5;
        return chart ? value.recompute(high, low, 7, revised ? 17 : 13)
                     : value.recompute(high, low, 7);
    });
}

void tr(bool chart) {
    const char* name = chart ? "TR/4" : "TR/3";
    ta::TR value(true);
    int& count = value.*member(TrCount{});
    const double missing = na<double>();
    const double first = chart ? value.compute(missing, 5, 7, missing)
                               : value.compute(missing, 5, 7);
    const bool warm = control(name, "missing range counts as a call", is_na(first) && count == 1);
    boundary(name, warm, count, chart ? 8.0 : 4.0, chart ? 14.0 : 8.0, [&] {
        return chart ? value.compute(9, 5, 7, 13) : value.compute(9, 5, 7);
    }, [&](bool revised) {
        const double high = revised ? 11 : 9, low = revised ? 3 : 5;
        return chart ? value.recompute(high, low, 7, revised ? 17 : 13)
                     : value.recompute(high, low, 7);
    });
}

void tsi() {
    ta::TSI value(1, 1);
    int& count = value.*member(TsiCount{});
    const double missing = value.compute(na<double>());
    const double first = value.compute(5);
    double price = 6;
    // With unit EMA lengths the ratio is the sign of the latest change.
    const double ratio = value.compute(price);
    const bool warm = control("TSI", "first change after missing source",
                              is_na(missing) && is_na(first) && ratio == 1 && count == 3);
    boundary("TSI", warm, count, 1, -1, [&] { return value.compute(++price); },
             [&](bool revised) { return value.recompute(revised ? price - 2 : price); });
}
}  // namespace

int main() {
    // Keep completed rows visible if a sanitizer stops at an overflowing call.
    std::setbuf(stdout, nullptr);
    atr(false);
    atr(true);
    tr(false);
    tr(true);
    tsi();
    return failures != 0;
}
