// The carried-trail tick reach's copy of the kernel's trailing-stop level.
//
// PineExecutionAdapter::retune_carried_trails_for_tick_reach walks a bar as
// the kernel will and needs the level the kernel's generic Trail rests at for
// every best the walk moves to, not only for the carried best it can read back
// (trail_state's current_level). It computes those levels with
// compat::pine::kernel_trail_stop_level (pineforge/compat/pine/trail_ticks.hpp),
// a copy of native_matching::checked_trail_stop: best +/- distance, except that a whole
// number of ticks from a best on the run's price ladder is that ladder point,
// spelled on the reachable side (ladder_trail_stop). This unit holds the copy
// to the kernel, bit for bit, on:
//
// - every best a sell trail and a buy trail move to on the bars of the tape
//   tests/fixtures/callback_lifecycle/carried-trail-14 (NYSE:F 15m, half-cent
//   prints such as 10.345 and 10.485), with whole-tick distances spelled as
//   the kernel resolves them (ticks x price tick) and the fractional
//   distances a re-priced bar carries (a print minus the best, and the
//   doubles beside it);
// - ladder bests in both decimal spellings (k x tick and k / n) on four price
//   ticks (0.01, 0.001, 1e-5 and 0.05), half-tick bests and off-grid bests;
// - the zero distance (the "ride the best" spelling).
//
// Where the kernel refuses a level (checked_trail_stop false: not strictly
// past the best), the adapter's walk never reaches the copy with it; the case
// is counted and skipped.

#include <pineforge/compat/pine/trail_ticks.hpp>

#include "../src/native_matching.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

int tests_passed = 0;
int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_CALLBACK_LIFECYCLE_FIXTURE_DIR
#error "PINEFORGE_CALLBACK_LIFECYCLE_FIXTURE_DIR must name tests/fixtures/callback_lifecycle"
#endif

struct Bar {
    double open = 0.0, high = 0.0, low = 0.0, close = 0.0;
};

std::vector<Bar> read_bars(const std::string& path) {
    std::vector<Bar> bars;
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        std::stringstream row(line);
        std::string cell;
        std::vector<std::string> cells;
        while (std::getline(row, cell, ',')) cells.push_back(cell);
        if (cells.size() < 5) continue;
        bars.push_back({std::strtod(cells[1].c_str(), nullptr), std::strtod(cells[2].c_str(), nullptr),
                        std::strtod(cells[3].c_str(), nullptr), std::strtod(cells[4].c_str(), nullptr)});
    }
    return bars;
}

std::size_t compared = 0;
std::size_t refused = 0;

// One level, the copy against the kernel, bit for bit.
void agree(double best, double distance, bool is_buy, double tick) {
    double kernel = 0.0;
    const bool representable = pineforge::native_matching::checked_trail_stop(
        best, distance, is_buy, &kernel, tick);
    if (!representable) {
        ++refused;
        return;
    }
    ++compared;
    const double copy = pineforge::compat::pine::kernel_trail_stop_level(best, distance, is_buy, tick);
    const bool same = std::memcmp(&copy, &kernel, sizeof(double)) == 0;
    if (!same) {
        std::printf("  differs: best=%.17g distance=%.17g %s tick=%.17g kernel=%.17g copy=%.17g\n",
                    best, distance, is_buy ? "buy" : "sell", tick, kernel, copy);
    }
    CHECK(same);
}

// The distances a bar can carry for one best: whole ticks as acceptance
// resolves them (ticks x price tick), and the fractional distance from the
// best to a print, with the doubles on either side of it.
void distances_for(double best, double print, bool is_buy, double tick) {
    for (const double ticks : {1.0, 5.0, 13.0, 14.0, 24.0, 1000.0})
        agree(best, ticks * tick, is_buy, tick);
    const double gap = is_buy ? print - best : best - print;
    if (gap > 0.0) {
        agree(best, gap, is_buy, tick);
        agree(best, std::nextafter(gap, 0.0), is_buy, tick);
        agree(best, std::nextafter(gap, std::numeric_limits<double>::infinity()), is_buy, tick);
    }
    agree(best, 0.0, is_buy, tick);
}

// Every best the walk moves to on the tape's bars: the open, then the two
// extremes, then the close, a buy trail's best falling with the lows and a
// sell trail's rising with the highs.
void walk_tape_bests() {
    const std::string path =
        std::string(PINEFORGE_CALLBACK_LIFECYCLE_FIXTURE_DIR) + "/carried-trail-14/bars.csv";
    const std::vector<Bar> bars = read_bars(path);
    CHECK(bars.size() == 78u);
    const double tick = 0.01;
    for (const bool is_buy : {true, false}) {
        double best = is_buy ? bars.front().high : bars.front().low;
        for (const Bar& bar : bars) {
            const bool high_first = std::abs(bar.open - bar.high) < std::abs(bar.open - bar.low);
            const double points[4] = {bar.open, high_first ? bar.high : bar.low,
                                      high_first ? bar.low : bar.high, bar.close};
            for (const double print : points) {
                distances_for(best, print, is_buy, tick);
                const bool improves = is_buy ? print < best : print > best;
                if (improves) best = print;
            }
        }
    }
}

// Ladder bests in both spellings, half-tick and off-grid bests, on four ticks.
void grid_bests() {
    struct Grid {
        double tick;
        double n;  // 1 / tick
        double anchor;
    };
    const Grid grids[] = {{0.01, 100.0, 10.35}, {0.001, 1000.0, 3331.65},
                          {1e-5, 100000.0, 1.13785}, {0.05, 20.0, 24300.05}};
    for (const Grid& g : grids) {
        const double k0 = std::round(g.anchor / g.tick);
        for (int step = -40; step <= 40; ++step) {
            const double k = k0 + step;
            const double spellings[] = {k * g.tick, k / g.n, (k + 0.5) * g.tick, (k + 0.5) / g.n,
                                        (k + 0.25) * g.tick, (k + 0.75) / g.n};
            for (const double best : spellings) {
                for (const bool is_buy : {true, false}) {
                    const double print = is_buy ? best + 14.5 * g.tick : best - 14.5 * g.tick;
                    distances_for(best, print, is_buy, g.tick);
                }
            }
        }
    }
}

}  // namespace

int main() {
    std::printf("=== the carried-trail walk's copy of the kernel's trailing-stop level ===\n");
    walk_tape_bests();
    grid_bests();
    std::printf("levels compared %zu, refused by the kernel (never read by the walk) %zu\n",
                compared, refused);
    CHECK(compared > 10000u);
    std::printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed ? 1 : 0;
}
