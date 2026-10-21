// Pins the default-off historical batch projection for regular HTF
// request.security(..., gaps_off, lookahead_on) sites.

#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/na.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <vector>

using namespace pineforge;

namespace {

int failures = 0;

#define CHECK(cond, tag) do { \
    if (!(cond)) { \
        std::printf("FAIL: %s (line %d)\n", (tag), __LINE__); \
        ++failures; \
    } \
} while (0)

bool same(double lhs, double rhs) {
    if (is_na(lhs) && is_na(rhs)) return true;
    if (is_na(lhs) || is_na(rhs)) return false;
    return lhs == rhs;
}

struct Dispatch {
    Bar bar;
    bool complete;
};

struct ProjectionStorage {
    std::size_t population;
    std::size_t resident;
    std::size_t bytes;
};

// The existing test-only member-pointer pattern also reads the real base
// vectors, so the resident-bound assertion executes on both revisions.
template <class Tag, auto Member> struct PrivateMember {
    friend auto access(Tag) { return Member; }
};
struct PrepareSecurity { friend auto access(PrepareSecurity); };
struct Stamps { friend auto access(Stamps); };
struct Groups { friend auto access(Groups); };
template struct PrivateMember<PrepareSecurity,
    &source::PineStrategyHost::scheduler_prepare_security_sequence>;
template struct PrivateMember<Stamps, &TimeframeAggregator::native_stamps_>;
template struct PrivateMember<Groups, &TimeframeAggregator::native_group_open_>;

struct PartitionStorage {
    std::size_t population;
    std::size_t resident;
    std::size_t bytes;
    std::optional<std::size_t> held_days;
};
template <typename C>
auto partition_storage(const C& stamps, const C& groups, int)
    -> decltype(stamps.generated_source(), PartitionStorage{}) {
    const auto* source = stamps.generated_source();
    return {stamps.size(), stamps.capacity() + groups.capacity(),
            (stamps.capacity() + groups.capacity()) * sizeof(std::int64_t)
                + (source ? source->resident_bytes() : 0),
            source ? std::optional<std::size_t>(source->held_days()) : std::nullopt};
}
template <typename C>
PartitionStorage partition_storage(const C& stamps, const C& groups, long) {
    return {stamps.size(), stamps.size() + groups.size(),
            (stamps.capacity() + groups.capacity()) * sizeof(std::int64_t), std::nullopt};
}

class NDayHarness final : public source::PineStrategyHost {
public:
    void prepare(const std::vector<Bar>& chart, const std::vector<Bar>& aux,
                 const char* session, const char* type) {
        input_tf_ = script_tf_ = "1D";
        script_tf_seconds_ = 86400;
        syminfo_.timezone = "UTC";
        syminfo_.session = session;
        syminfo_.type = type;
        register_security_eval(0, "2D", "1D", true, false);
        register_security_eval(1, "8D", "1D", true, false);
        register_security_eval(2, "8D", "1D", true, false);
        CHECK(set_aux_security_feed(aux.data(), static_cast<int>(aux.size()), "60"),
              "N-day auxiliary feed installed");
        (this->*access(PrepareSecurity{}))(chart);
    }
    TimeframeAggregator& partition(int site) { return security_eval_states_[site].aggregator; }
    void on_source_bar(const Bar&) override {}
};

void test_nday_real_dates_stamps_and_fixed_resident_bound() {
    namespace cal = native_calendar;
    constexpr std::int64_t day_ms = 86'400'000;
    const auto first = cal::native_civil_days(2023, 12, 29);
    const auto last = cal::native_civil_days(2026, 1, 5);
    const auto chart_first = cal::native_civil_days(2024, 1, 3);
    const auto chart_last = cal::native_civil_days(2025, 12, 30);
    const auto good_friday = cal::native_civil_days(2025, 4, 18);
    const auto christmas = cal::native_civil_days(2025, 12, 25);
    // The year's first trading date opens every N-day group, so its late
    // chart stamp must win over an earlier auxiliary bar in all three sites.
    const auto late_day = cal::native_civil_days(2025, 1, 1);
    for (int mode = 0; mode < 3; ++mode) {
        const bool every_day = mode == 0;
        const bool exchange = mode == 2;
        const char* session = every_day ? "24x7" : exchange ? "0930-1600" : "1700-1700";
        const char* type = every_day ? "crypto" : exchange ? "stock" : "forex";
        const auto nominal = [&](std::int64_t d) {
            return d * day_ms + (every_day ? 0 : exchange ? 570 * 60'000LL : -7 * 3'600'000LL);
        };
        std::vector<Bar> chart, aux;
        std::map<std::int64_t, std::int64_t> held;
        for (auto d = first; d <= last; ++d) {
            const auto weekday = ((d % 7) + 7 + 4) % 7;
            if (!every_day && (weekday == 0 || weekday == 6)) continue;
            if (d == christmas || d == good_friday) continue;
            const bool own = d >= chart_first && d <= chart_last
                && (d % 11 != 0 || d == late_day);
            const bool finer = d == first || d == last || d % 5 == 0 || d == late_day;
            const auto own_stamp = nominal(d) + (exchange && d == late_day ? 255 * 60'000LL : 0);
            if (own) {
                chart.push_back({100, 102, 99, 101, 1, own_stamp});
                held.emplace(d, own_stamp);
            }
            if (finer) {
                aux.push_back({100, 102, 99, 101, 1, nominal(d) + 30 * 60'000});
                held.emplace(d, nominal(d));
            }
        }
        CHECK(held.size() > 450, "hundreds of real chart/aux trading dates");
        CHECK(held.count(christmas) == 0 && held.count(good_friday) == 0,
              "missing exchange/OTC holidays are real input holes");
        NDayHarness harness;
        harness.prepare(chart, aux, session, type);
        for (int site = 0; site < 3; ++site) {
            const int days = site == 0 ? 2 : 8;
            std::vector<std::int64_t> dates;
            const auto first_held = held.begin()->first;
            const auto last_held = held.rbegin()->first;
            for (auto year = cal::native_civil_date(first_held).year;
                 year <= cal::native_civil_date(last_held).year + 1; ++year) {
                int count = 0;
                for (auto d = cal::native_civil_days(year, 1, 1);
                     d < cal::native_civil_days(year + 1, 1, 1); ++d) {
                    const auto weekday = ((d % 7) + 7 + 4) % 7;
                    const bool trades = exchange && d >= first_held && d <= last_held
                        ? held.count(d) != 0 : every_day || (weekday >= 1 && weekday <= 5);
                    if (trades && count++ % days == 0) dates.push_back(d);
                }
            }
            auto begin = std::upper_bound(dates.begin(), dates.end(), first_held);
            CHECK(begin != dates.begin(), "first held date has a containing N-day bucket");
            if (begin == dates.begin()) continue;
            std::vector<std::int64_t> expected;
            for (--begin; begin != dates.end(); ++begin) {
                const auto found = held.find(*begin);
                expected.push_back(found == held.end() ? nominal(*begin) : found->second);
            }
            if (exchange)
                CHECK(std::find(expected.begin(), expected.end(), held.at(late_day)) != expected.end(),
                      "late chart open owns the N-day stamp despite earlier auxiliary trading");
            auto& agg = harness.partition(site);
            const auto storage = partition_storage(agg.*access(Stamps{}), agg.*access(Groups{}), 0);
            CHECK(storage.population == expected.size(), "full N-day stamp population including next year");
            if (storage.held_days)
                CHECK(*storage.held_days == held.size(), "merged real held-date population");
            CHECK(storage.resident == 0, "no resident N-day stamp/group table on either site");
            CHECK(storage.bytes <= 4096, "fixed N-day cursor bytes including owned buffers");
            for (std::size_t k = 0; k < expected.size(); ++k) {
                const auto index = (k % 2 == 0) ? k / 2 : expected.size() - 1 - k / 2;
                const auto stamp = expected[index];
                CHECK(agg.bar_label_ms(stamp) == stamp, "N-day stamp survives nonmonotone seeks");
                CHECK(agg.bucket_open_ms(stamp + 1) == stamp, "N-day membership matches every stamp");
                if (index + 1 < expected.size())
                    CHECK(agg.period_changes(stamp, expected[index + 1]), "every N-day boundary survives");
            }
            const auto last_bound = session_period_close_ms(
                expected.back(), "UTC", session, CalendarPeriod::DAY);
            CHECK(agg.bucket_open_ms(last_bound)
                      == session_period_open_ms(last_bound, "UTC", session, CalendarPeriod::DAY),
                  "nominal fallback after the final next-year stamp");
            NativeDayPartition other;
            other.tz = "UTC";
            other.session = session;
            other.stamps = {expected.front()};
            other.trade_day = {first_held};
            other.week_open = other.month_open = other.stamps;
            other.last_bound = last_bound;
            NativeDayPartitionScope ambient(&other);
            CHECK(agg.bar_label_ms(expected[expected.size() / 2])
                      == expected[expected.size() / 2], "recompute keeps preparation's calendar context");
            const auto final = partition_storage(agg.*access(Stamps{}), agg.*access(Groups{}), 0);
            CHECK(final.resident == 0 && final.bytes <= 4096, "seeking never grows resident storage");
        }
    }
}

template <typename C>
auto projection_storage(const C& projections, int)
    -> decltype(projections.capacity(), ProjectionStorage{}) {
    return {projections.size(), projections.size(),
            sizeof(C) + projections.capacity() * sizeof(source::HistoricalSecurityProjection)};
}

template <typename C>
ProjectionStorage projection_storage(const C& projections, long) {
    return {projections.size(), projections.resident_size(), sizeof(C)};
}

class ProjectionHarness final : public pineforge::source::PineStrategyHost {
public:
    explicit ProjectionHarness(bool lookahead_on = true, bool gaps_on = false,
                               const char* requested_tf = "60",
                               bool heikinashi = false) {
        register_security_eval(0, requested_tf, "15", lookahead_on, gaps_on,
                               heikinashi);
    }

    void evaluate_security(int sec_id, const Bar& bar,
                           bool is_complete) override {
        CHECK(sec_id == 0, "security id");
        dispatches.push_back(Dispatch{bar, is_complete});
        visible_close = bar.close;
    }

    void on_source_bar(const Bar&) override {
        chart_values.push_back(visible_close);
        if (measure_projection) {
            const auto storage = projection_storage(pine_security_states_[0].historical_projections, 0);
            projection_population = storage.population;
            peak_resident = std::max(peak_resident, storage.resident);
            peak_bytes = std::max(peak_bytes, storage.bytes);
        }
    }

    std::vector<Dispatch> dispatches;
    std::vector<double> chart_values;
    double visible_close = na<double>();
    bool measure_projection = false;
    std::size_t projection_population = 0;
    std::size_t peak_resident = 0;
    std::size_t peak_bytes = 0;
};

void test_projection_population_has_one_resident_bucket() {
    constexpr int n = 4098;
    std::vector<Bar> bars;
    for (int i = 0; i < n; ++i) {
        const double value = static_cast<double>(i + 1);
        bars.push_back({value, value + 2, value - 1, value, value,
                        3'600'000 + static_cast<std::int64_t>(i) * 900'000});
    }
    for (const bool cut_first : {false, true}) {
        ProjectionHarness harness;
        harness.measure_projection = true;
        harness.set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
        if (cut_first)
            harness.set_syminfo_metadata("security_range_start_na_warmup", 4'500'000.0);
        harness.run(bars.data(), n, "15", "15");
        CHECK(harness.last_error().empty(), "bounded source projection succeeds");
        const std::size_t population = cut_first ? 1024 : 1025;
        CHECK(harness.projection_population == population, "real retained bucket population");
        CHECK(harness.dispatches.size() == population, "every retained bucket dispatches");
        CHECK(harness.peak_resident == 1, "one resident source projection (base retains every bucket)");
        CHECK(harness.peak_bytes <= 512, "fixed source projection bytes including owned capacity");
        CHECK(harness.chart_values.size() == bars.size(), "every source child is observed");
        for (std::size_t i = 0; i < harness.dispatches.size(); ++i) {
            const std::size_t begin = (i + (cut_first ? 1 : 0)) * 4;
            const std::size_t end = std::min(begin + 4, bars.size());
            const auto& got = harness.dispatches[i];
            double volume = bars[begin].volume;
            for (std::size_t j = begin + 1; j < end; ++j) volume += bars[j].volume;
            CHECK(got.bar.timestamp == bars[begin].timestamp, "projected first label");
            CHECK(got.bar.open == bars[begin].open
                      && got.bar.low == bars[begin].low
                      && got.bar.high == bars[end - 1].high
                      && got.bar.close == bars[end - 1].close
                      && got.bar.volume == volume, "exact projected OHLCV fold");
            CHECK(got.complete == (end - begin == 4), "complete buckets and partial tail");
        }
        for (std::size_t i = 0; i < harness.chart_values.size(); ++i) {
            const double want = cut_first && i < 4 ? na<double>()
                : bars[std::min((i / 4 + 1) * 4, bars.size()) - 1].close;
            CHECK(same(harness.chart_values[i], want), "FINAL projection held on every child");
        }
    }
}

std::vector<Bar> make_feed() {
    // One complete 60m bucket (01:00..01:45) and one incomplete tail
    // (02:00..02:15), both composed from 15m chart bars.
    return {
        Bar{10.0, 11.0,  9.0, 10.0, 1.0,  3'600'000},
        Bar{10.0, 22.0,  8.0, 20.0, 2.0,  4'500'000},
        Bar{20.0, 33.0,  7.0, 30.0, 3.0,  5'400'000},
        Bar{30.0, 44.0,  6.0, 40.0, 4.0,  6'300'000},
        Bar{40.0, 55.0, 35.0, 50.0, 5.0,  7'200'000},
        Bar{50.0, 66.0, 34.0, 60.0, 6.0,  8'100'000},
    };
}

std::vector<Bar> make_5m_feed() {
    std::vector<Bar> bars;
    bars.reserve(12);
    for (int i = 0; i < 12; ++i) {
        const double close = static_cast<double>(i + 1);
        const int64_t timestamp = 3'600'000
            + static_cast<int64_t>(i) * 300'000;
        bars.push_back(Bar{close, close, close, close, 1.0, timestamp});
    }
    return bars;
}

void test_default_remains_progressive() {
    ProjectionHarness harness;
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "default run succeeds");
    CHECK(harness.dispatches.size() == 6,
          "default lookahead dispatches every progressive child");
    const double expected[] = {10, 20, 30, 40, 50, 60};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 6; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, expected[i]),
              "default progressive close sequence");
        CHECK(harness.dispatches[i].complete == (i == 3),
              "default completion cadence");
        CHECK(same(harness.chart_values[i], expected[i]),
              "default chart sees progressive value");
    }
}

void test_flag_projects_full_bucket_then_holds() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "projected run succeeds");
    CHECK(harness.dispatches.size() == 2,
          "one projected dispatch per HTF bucket");
    CHECK(harness.dispatches[0].complete,
          "full historical bucket is committed");
    CHECK(same(harness.dispatches[0].bar.open, 10.0), "projected full open");
    CHECK(same(harness.dispatches[0].bar.high, 44.0), "projected full high");
    CHECK(same(harness.dispatches[0].bar.low, 6.0), "projected full low");
    CHECK(same(harness.dispatches[0].bar.close, 40.0), "projected full close");
    CHECK(same(harness.dispatches[0].bar.volume, 10.0), "projected full volume");

    const double expected_chart[] = {40, 40, 40, 40, 60, 60};
    CHECK(harness.chart_values.size() == 6, "all chart children dispatched");
    for (std::size_t i = 0; i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "projection is visible on first child and held");
    }
}

void test_incomplete_tail_projects_available_aggregate() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 2, "tail projection exists");
    if (harness.dispatches.size() == 2) {
        const Dispatch& tail = harness.dispatches[1];
        CHECK(!tail.complete, "incomplete tail does not commit history");
        CHECK(same(tail.bar.open, 40.0), "tail available open");
        CHECK(same(tail.bar.high, 66.0), "tail available high");
        CHECK(same(tail.bar.low, 34.0), "tail available low");
        CHECK(same(tail.bar.close, 60.0), "tail available close");
        CHECK(same(tail.bar.volume, 11.0), "tail available volume");
    }
}

void test_lookahead_off_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/false, /*gaps_on=*/false);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 1,
          "lookahead_off keeps completion-only behavior");
    CHECK(harness.dispatches.empty() || harness.dispatches[0].complete,
          "lookahead_off dispatch is committed");
    CHECK(harness.dispatches.empty()
              || same(harness.dispatches[0].bar.close, 40.0),
          "lookahead_off completed close unchanged");
    const double expected_chart[] = {
        na<double>(), na<double>(), na<double>(), 40.0, 40.0, 40.0,
    };
    for (std::size_t i = 0; i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "lookahead_off chart sequence unchanged");
    }
}

void test_gaps_on_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/true);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "gaps_on keeps progressive lookahead behavior");
    const double expected[] = {10, 20, 30, 40, 50, 60};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 6; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, expected[i]),
              "gaps_on progressive close sequence unchanged");
    }
}

void test_equal_timeframe_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/false,
                              /*requested_tf=*/"15");
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "equal timeframe remains passthrough");
    for (std::size_t i = 0; i < harness.dispatches.size() && i < bars.size(); ++i) {
        CHECK(same(harness.dispatches[i].bar.close, bars[i].close),
              "equal timeframe close unchanged");
        CHECK(harness.dispatches[i].complete,
              "equal timeframe dispatch stays complete");
    }
}

void test_heikinashi_ignores_projection_flag() {
    ProjectionHarness harness(/*lookahead_on=*/true, /*gaps_on=*/false,
                              /*requested_tf=*/"60", /*heikinashi=*/true);
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.dispatches.size() == 6,
          "Heikin-Ashi security remains on its established progressive path");
}

void test_input_tf_below_script_tf_ignores_projection_flag() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_5m_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "5", "15");

    CHECK(harness.last_error().empty(), "5m-to-15m run succeeds");
    CHECK(harness.dispatches.size() == 12,
          "input-to-script aggregation keeps security progressive");
    for (std::size_t i = 0;
         i < harness.dispatches.size() && i < bars.size(); ++i) {
        CHECK(same(harness.dispatches[i].bar.close, bars[i].close),
              "raw input security close remains progressive");
    }

    const double expected_chart[] = {3.0, 6.0, 9.0, 12.0};
    CHECK(harness.chart_values.size() == 4,
          "5m input produces four 15m script bars");
    for (std::size_t i = 0;
         i < harness.chart_values.size() && i < 4; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "15m script sees latest progressive security close");
    }
}

void test_range_start_warmup_composes_with_projection() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    // Range start on the 60m grid (the 01:00 bucket open): nothing precedes
    // it, so the finite projection aggregates the same feed as the plain
    // projection — the four-child historical 60m bucket, then the available
    // two-child tail — through the shared range-start suffix logic.
    harness.set_syminfo_metadata(
        "security_range_start_na_warmup", 3'600'000.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "range-start composition run succeeds");
    CHECK(harness.dispatches.size() == 2,
          "range-start feed projects once per retained HTF bucket");
    if (harness.dispatches.size() == 2) {
        const Dispatch& historical = harness.dispatches[0];
        CHECK(historical.complete, "grid-aligned historical bucket is complete");
        CHECK(same(historical.bar.open, 10.0), "grid-aligned projection open");
        CHECK(same(historical.bar.high, 44.0), "grid-aligned projection high");
        CHECK(same(historical.bar.low, 6.0), "grid-aligned projection low");
        CHECK(same(historical.bar.close, 40.0), "grid-aligned projection close");
        CHECK(same(historical.bar.volume, 10.0), "grid-aligned projection volume");

        const Dispatch& tail = harness.dispatches[1];
        CHECK(!tail.complete, "grid-aligned tail remains incomplete");
        CHECK(same(tail.bar.close, 60.0), "grid-aligned tail available close");
    }

    const double expected_chart[] = {40.0, 40.0, 40.0, 40.0, 60.0, 60.0};
    CHECK(harness.chart_values.size() == 6,
          "range-start composition preserves every chart child");
    for (std::size_t i = 0;
         i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "range-start projected chart sequence");
    }
}

void test_range_start_inside_bucket_drops_whole_bucket_from_projection() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    // Range start INSIDE the 01:00 bucket (01:15). The KI-55 cut is taken on
    // HTF-bucket opens (finding 452): the whole 01:00 bucket opened before the
    // range start and is absent, not a three-child partial. The projection is
    // built from that same retained suffix — only the two-child 02:00 tail —
    // and its child indexes line up with the per-state feed cursor, so the
    // four skipped chart children stay na and the tail projects on its first
    // retained child.
    harness.set_syminfo_metadata(
        "security_range_start_na_warmup", 4'500'000.0);
    const auto bars = make_feed();
    harness.run(bars.data(), static_cast<int>(bars.size()), "15", "15");

    CHECK(harness.last_error().empty(), "mid-bucket composition run succeeds");
    CHECK(harness.dispatches.size() == 1,
          "mid-bucket range start projects only the retained tail bucket");
    if (harness.dispatches.size() == 1) {
        const Dispatch& tail = harness.dispatches[0];
        CHECK(!tail.complete, "retained tail remains incomplete");
        CHECK(same(tail.bar.open, 40.0), "retained tail open is the 02:00 child");
        CHECK(same(tail.bar.high, 66.0), "retained tail high");
        CHECK(same(tail.bar.low, 34.0), "retained tail low");
        CHECK(same(tail.bar.close, 60.0), "retained tail available close");
        CHECK(same(tail.bar.volume, 11.0), "retained tail volume");
    }

    const double expected_chart[] = {
        na<double>(), na<double>(), na<double>(), na<double>(), 60.0, 60.0,
    };
    CHECK(harness.chart_values.size() == 6,
          "mid-bucket composition preserves every chart child");
    for (std::size_t i = 0;
         i < harness.chart_values.size() && i < 6; ++i) {
        CHECK(same(harness.chart_values[i], expected_chart[i]),
              "mid-bucket projected chart sequence");
    }
}

void test_stream_warmup_and_continuation_stay_progressive() {
    ProjectionHarness harness;
    harness.set_syminfo_metadata(
        "historical_security_lookahead_projection", 1.0);
    const auto bars = make_feed();

    CHECK(harness.stream_begin(bars.data(), 4, "15", "15"),
          "stream begin succeeds");
    CHECK(harness.dispatches.size() == 4,
          "stream warmup ignores historical projection");
    const double warmup_expected[] = {10, 20, 30, 40};
    for (std::size_t i = 0; i < harness.dispatches.size() && i < 4; ++i) {
        CHECK(same(harness.dispatches[i].bar.close, warmup_expected[i]),
              "stream warmup stays progressive");
    }

    CHECK(harness.stream_push_tick(
              TradeTick{7'200'000, 1, 50.0, 5.0}),
          "first realtime tick accepted");
    CHECK(harness.stream_advance_time(8'100'000),
          "first realtime input bar finalized");
    CHECK(harness.dispatches.size() == 5,
          "realtime continuation dispatches next partial");
    CHECK(same(harness.dispatches.back().bar.close, 50.0),
          "realtime continuation exposes available close");
    CHECK(!harness.dispatches.back().complete,
          "realtime continuation remains partial");

    CHECK(harness.stream_push_tick(
              TradeTick{8'100'000, 2, 60.0, 6.0}),
          "second realtime tick accepted");
    CHECK(harness.stream_advance_time(9'000'000),
          "second realtime input bar finalized");
    CHECK(harness.dispatches.size() == 6,
          "second realtime partial dispatched");
    CHECK(same(harness.dispatches.back().bar.close, 60.0),
          "realtime aggregation advances progressively");
    CHECK(!harness.dispatches.back().complete,
          "second realtime bar is still partial");
    CHECK(harness.stream_end(), "stream ends cleanly");
}

}  // namespace

int main() {
    test_nday_real_dates_stamps_and_fixed_resident_bound();
    test_projection_population_has_one_resident_bucket();
    test_default_remains_progressive();
    test_flag_projects_full_bucket_then_holds();
    test_incomplete_tail_projects_available_aggregate();
    test_lookahead_off_ignores_projection_flag();
    test_gaps_on_ignores_projection_flag();
    test_equal_timeframe_ignores_projection_flag();
    test_heikinashi_ignores_projection_flag();
    test_input_tf_below_script_tf_ignores_projection_flag();
    test_range_start_warmup_composes_with_projection();
    test_range_start_inside_bucket_drops_whole_bucket_from_projection();
    test_stream_warmup_and_continuation_stay_progressive();
    if (failures != 0) {
        std::printf("%d check(s) FAILED\n", failures);
        return 1;
    }
    std::printf("test_historical_security_lookahead_projection passed.\n");
    return 0;
}
