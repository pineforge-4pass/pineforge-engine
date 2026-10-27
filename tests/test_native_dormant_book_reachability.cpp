// C3 / T8, REV2 P2-1. DESIGN section 5.3 names dormant_reachable but
// supplies no callable C++ signature or list enum. The requested test-only
// adapters below must call the ACTUAL join/wake computations, including their
// production margin, rather than an independent test implementation.
// Arguments: (bool ge, double level, double from, double to, GridThreshold).
// This explicit missing-seam diagnostic is intentional on the frozen base.
#include "../src/native_execution_consumer.hpp"
#include "../src/native_matching.hpp"
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>

#if !defined(PINEFORGE_TEST_DORMANT_JOIN_REACHABLE) || !defined(PINEFORGE_TEST_DORMANT_WAKE_REACHABLE)
#error "T8 needs C2 adapters for production join/wake dormant_reachable; see REPORT-s4e-391b-CT.md"
#else
namespace {
namespace nm = pineforge::native_matching;
namespace no = pineforge::native_order;
using nm::GeometricHit;
using nm::GridThreshold;

struct Rng {
    std::uint64_t state = 0xd1b54a32d192ed03ULL;
    std::uint64_t next() {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        return state;
    }
    double fraction() { return static_cast<double>(next() >> 11) * 0x1p-53; }
};

void require(bool value, const char* text) {
    if (!value) throw std::runtime_error(text);
}

// This dispatch follows BASE match_path::reach_trigger's priced states.
// Its only geometric oracle is the existing first_region_entry. It never
// calls either new predicate to determine an expected answer.
std::optional<GeometricHit> base_reach(const no::Trigger& trigger,
                                     const no::TriggerState& state, bool buy,
                                     double from, double to, const GeometricHit& start,
                                     const GridThreshold& grid) {
    if (std::holds_alternative<no::StopIdle>(state)) {
        const auto& stop = std::get<no::Stop>(trigger);
        return nm::first_region_entry(from, to, start, stop.price, !buy, true, grid);
    }
    if (std::holds_alternative<no::StopLimitPending>(state)) {
        const auto& sl = std::get<no::StopLimit>(trigger);
        return nm::first_region_entry(from, to, start, sl.stop, !buy, true, grid);
    }
    if (std::holds_alternative<no::StopLimitLive>(state)) {
        const auto& sl = std::get<no::StopLimit>(trigger);
        return nm::first_region_entry(from, to, start, sl.limit, buy, true, grid);
    }
    const auto& limit = std::get<no::Limit>(trigger);
    return nm::first_region_entry(from, to, start, limit.price, buy, true, grid);
}

struct Starts {
    // Fixed fixture geometry: five path fractions plus two prints for each
    // of 2 edges * 5 offsets * 2 step sizes * 2 orientations: <=85 starts.
    std::array<GeometricHit, 128> rows{};
    std::size_t size = 0;
    void add(GeometricHit start) {
        require(size < rows.size(), "T8 cursor fixture exceeded its derived bound");
        rows[size++] = start;
    }
};

Starts starts_for(double from, double to, const GridThreshold& grid) {
    Starts starts;
    for (const double t : {0.0, 0.125, 0.5, 0.875, 1.0})
        starts.add({t, nm::price_at(from, to, t), false});
    const double lo = std::min(from, to), hi = std::max(from, to);
    for (const double edge : {lo, hi}) {
        const double ulp = std::nextafter(edge, std::numeric_limits<double>::infinity()) - edge;
        for (const int k : {-2, -1, 0, 1, 2}) {
            for (const double step : {ulp, grid.tick}) {
                const double other = edge + k * step;
                for (const bool le : {false, true}) {
                    const auto hit = nm::first_region_entry(
                        from, to, {0, from, false}, other, le, true, grid);
                    if (!hit) continue;
                    starts.add(*hit); // Raw threshold crossing's booked level.
                    starts.add({hit->t, nm::grid_reached_print(hit->price, other, le, grid),
                                hit->at_level});
                }
            }
        }
    }
    return starts;
}

void one(double from, double to, double level, bool buy, int kind,
         const GridThreshold& grid, const Starts& starts, std::size_t& absent,
         std::size_t& reachable) {
    no::Trigger trigger;
    no::TriggerState state;
    bool ge = buy;
    if (kind == 0) {
        trigger = no::Limit{level}; state = no::LimitReady{};
    } else if (kind == 1) {
        trigger = no::Stop{level}; state = no::StopIdle{}; ge = !buy;
    } else if (kind == 2) {
        trigger = no::StopLimit{level, level + grid.tick};
        state = no::StopLimitPending{}; ge = !buy;
    } else {
        trigger = no::StopLimit{level - grid.tick, level};
        state = no::StopLimitLive{};
    }
    const bool join = PINEFORGE_TEST_DORMANT_JOIN_REACHABLE(ge, level, from, to, grid);
    const bool wake = PINEFORGE_TEST_DORMANT_WAKE_REACHABLE(ge, level, from, to, grid);
    require(join == wake, "T8 join/wake use different reach computations");

    // Two independent oracles:
    // (1) The specified conservative ENVELOPE, evaluated by BASE geometry.
    // Its padding is hand arithmetic from section 5.3: two tick excursions,
    // plus the chosen (not derived) 2^-20 relative safety margin. Requiring
    // the documented envelope is stronger than safety alone; this makes a
    // smaller but accidentally still-safe margin (M-22) observably red.
    const double lo = std::min(from, to), hi = std::max(from, to);
    const double relative = std::ldexp(std::max({1.0, std::abs(lo), std::abs(hi)}), -20);
    const double delta = (nm::grid_active(grid.tick) ? 2 * grid.tick : 0) + relative;
    const double padded_lo = lo - delta, padded_hi = hi + delta;
    const bool envelope = nm::first_region_entry(
        padded_lo, padded_hi, {0, padded_lo, false}, level, ge, true).has_value();
    if (join != envelope || wake != envelope) {
        std::fprintf(stderr, "T8 envelope ge=%d kind=%d from=%a to=%a level=%a tick=%a "
                     "join=%d wake=%d base-envelope=%d\n",
                     ge, kind, from, to, level, grid.tick, join, wake, envelope);
        throw std::runtime_error("T8 production predicate disagrees with the specified base envelope");
    }
    // (2) Safety against the UNPADDED base matcher and all generated legal
    // cursor prints. This is the one-way P2-1 property, not the new formula.
    if (!join || !wake) {
        ++absent;
        for (std::size_t i = 0; i < starts.size; ++i) {
            const auto hit = base_reach(trigger, state, buy, from, to, starts.rows[i], grid);
            if (hit) {
                std::fprintf(stderr, "T8 false dormancy ge=%d kind=%d from=%a to=%a level=%a "
                             "tick=%a start=(%a,%a) hit=(%a,%a)\n",
                             ge, kind, from, to, level, grid.tick,
                             starts.rows[i].t, starts.rows[i].price, hit->t, hit->price);
                throw std::runtime_error("T8 dormant_reachable=false but BASE reach_trigger hit");
            }
        }
    } else {
        ++reachable;
    }
}
} // namespace

int main() {
    try {
        // M10: public bars/ticks reject nonfinite prices before matching.
        // Exercise the SAME finite-segment predicate used by eligible(),
        // just as the rest of T8 exercises its unreachable grid envelope.
        // Independent classification: the first four values are finite
        // (including the largest finite double); the last three are not.
        const std::array<double, 7> endpoints{{
            0.0, 100.0, -100.0, std::numeric_limits<double>::max(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()
        }};
        for (std::size_t i = 0; i < endpoints.size(); ++i)
            for (std::size_t j = 0; j < endpoints.size(); ++j)
                require(pineforge::NativeExecutionConsumer::dormant_segment_finite(
                            endpoints[i], endpoints[j]) == (i < 4 && j < 4),
                        "T8 production finite-segment guard disagrees with endpoint classification");
        Rng rng;
        std::size_t absent = 0, reachable = 0;
        for (int trial = 0; trial < 64; ++trial) {
            const double scale = std::ldexp(1.0, static_cast<int>(rng.next() % 40) - 15);
            const double center = (1 + 8 * rng.fraction()) * scale;
            const double width = trial % 8 == 0 ? 0 : rng.fraction() * scale;
            double from = center, to = center + width;
            if (trial & 1) std::swap(from, to);
            for (const double tick : {0.0, 0.01, 0.25, scale / 8}) {
                for (const bool half_up : {false, true}) {
                    const GridThreshold grid{tick, half_up};
                    const Starts starts = starts_for(from, to, grid);
                    const double lo = std::min(from, to), hi = std::max(from, to);
                    const double relative = std::ldexp(
                        std::max({1.0, std::abs(lo), std::abs(hi)}), -20);
                    const double delta = (nm::grid_active(tick) ? 2 * tick : 0) + relative;
                    for (int kind = 0; kind < 4; ++kind) {
                        for (const bool buy : {false, true}) {
                            for (const double edge : {lo, hi}) {
                                const double ulp = std::nextafter(
                                    edge, std::numeric_limits<double>::infinity()) - edge;
                                for (const int k : {-4, -2, -1, 0, 1, 2, 4}) {
                                    for (const double step : {ulp, tick, relative})
                                        one(from, to, edge + k * step, buy, kind, grid,
                                            starts, absent, reachable);
                                }
                            }
                            // Closed envelope boundaries catch >= versus >.
                            for (const double level : {lo - delta, hi + delta,
                                     lo - 1.5 * tick - relative, hi + 1.5 * tick + relative,
                                     center + (rng.fraction() - 0.5) * 16 * scale})
                                one(from, to, level, buy, kind, grid, starts, absent, reachable);
                        }
                    }
                }
            }
        }
        require(absent > 0 && reachable > 0, "T8 generated only one predicate outcome");
        std::printf("T8 base-geometry oracle absent=%zu reachable=%zu\n", absent, reachable);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "test_native_dormant_book_reachability: %s\n", e.what());
        return 1;
    }
}
#endif
