// Per-site historical projection, its clipped-input boundary and the live
// modes that must remain progressive. All observations come from the host.
#include <pineforge/source/pine_strategy_host.hpp>

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#ifndef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
#error "security projection controls require the auxiliary-feed interface"
#endif

using pineforge::Bar;

namespace {

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t hour = 3'600'000;
constexpr std::int64_t day = 24 * hour;
constexpr std::int64_t jan1 = 1'704'067'200'000; // 2024-01-01, Monday, UTC

struct Dispatch {
    Bar bar;
    bool complete;
    bool slot_new;
};

enum class Registration { site, legacy5, legacy6, metadata };

class ProjectionHost final : public pineforge::source::PineStrategyHost {
public:
    ProjectionHost(const std::string& requested, const std::string& input,
                   Registration registration = Registration::site) {
        if (registration == Registration::site) {
            register_security_eval(0, requested, input, true, false, false, true);
        } else if (registration == Registration::legacy6) {
            register_security_eval(0, requested, input, true, false, false);
        } else {
            register_security_eval(0, requested, input, true, false);
        }
        if (registration == Registration::metadata)
            set_syminfo_metadata("historical_security_lookahead_projection", 1.0);
    }

    // This same source is a compile-control input. Defining
    // PINEFORGE_TEST_SECURITY_LITERAL_NEGATIVE must select the deleted
    // seven-argument foreign-literal overload; the ordinary build keeps the
    // explicit-string positive control. Neither branch is a runtime witness.
    void compile_foreign_input_control() {
#ifdef PINEFORGE_TEST_SECURITY_LITERAL_NEGATIVE
        register_security_eval(1, "SYMBOL", "D", "60", true, false, true);
#else
        register_security_eval(1, "SYMBOL", "D", std::string("60"),
                               true, false, true);
#endif
    }

    void evaluate_security(int id, const Bar& bar, bool complete) override {
        assert(id == 0);
        dispatches.push_back({bar, complete, security_series_slot_is_new(id)});
        visible = bar.close;
        previous = committed;
        if (complete) committed = bar.close;
    }

    void on_source_bar(const Bar& bar) override {
        for (const auto& state : security_eval_states_)
            saw_generated_periods = saw_generated_periods
                || (state.aggregator.has_native_periods() && state.native_feed_index < 0);
        chart_bars.push_back(bar);
        values.push_back(visible);
        previous_values.push_back(previous);
    }

    std::vector<Dispatch> dispatches;
    std::vector<Bar> chart_bars;
    std::vector<double> values;
    std::vector<double> previous_values;
    bool saw_generated_periods = false;

private:
    double visible = missing;
    double previous = missing;
    double committed = missing;
};

std::vector<Bar> make_feed(std::int64_t first, std::int64_t step, int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + i;
        bars.push_back({price, price + 1, price - 1, price, 1,
                        first + step * i});
    }
    return bars;
}

void check_clipped_prefix(const char* requested, const char* input,
                          std::int64_t first, std::int64_t step,
                          int prefix, int whole, int tail = 2,
                          bool supplied_daily = false) {
    // One clipped bucket, one whole bucket and an available shorter tail.
    assert(tail > 0 && tail < whole);
    const auto bars = make_feed(first, step, prefix + whole + tail);
    ProjectionHost host(requested, input);
    if (supplied_daily)
        assert(host.set_native_security_feed(input, bars.data(),
                                            static_cast<int>(bars.size())));
    host.run(bars.data(), static_cast<int>(bars.size()), input, input);
    assert(host.last_error().empty());
    assert(host.values.size() == bars.size());
    assert(host.previous_values.size() == bars.size());
    assert(host.dispatches.size() == 2);
    for (const auto& dispatch : host.dispatches) assert(dispatch.slot_new);
    assert(host.dispatches[0].bar.timestamp == bars[prefix].timestamp);
    assert(host.dispatches[0].bar.open == bars[prefix].open);
    assert(host.dispatches[0].bar.close == bars[prefix + whole - 1].close);
    assert(host.dispatches[0].bar.volume == whole);
    assert(host.dispatches[0].complete);
    assert(host.dispatches[1].bar.timestamp == bars[prefix + whole].timestamp);
    assert(host.dispatches[1].bar.close == bars.back().close);
    assert(!host.dispatches[1].complete);
    for (int i = 0; i < static_cast<int>(bars.size()); ++i) {
        if (i < prefix) {
            assert(std::isnan(host.values[i]));
        } else if (i < prefix + whole) {
            assert(host.values[i] == bars[prefix + whole - 1].close);
        } else {
            assert(host.values[i] == bars.back().close);
        }
        if (i < prefix + whole) {
            assert(std::isnan(host.previous_values[i]));
        } else {
            assert(host.previous_values[i] == bars[prefix + whole - 1].close);
        }
    }

    // An empty projection roster must not expose the clipped bucket through
    // the progressive fallback, even when its last input reaches the close.
    ProjectionHost clipped_only(requested, input);
    if (supplied_daily)
        assert(clipped_only.set_native_security_feed(input, bars.data(), prefix));
    clipped_only.run(bars.data(), prefix, input, input);
    assert(clipped_only.last_error().empty());
    assert(clipped_only.dispatches.empty());
    assert(clipped_only.values.size() == static_cast<std::size_t>(prefix));
    for (double value : clipped_only.values) assert(std::isnan(value));
}

void check_session_evidence() {
    // This session's daily stamp is 17:00, but trading starts at 18:00.
    // The stamp alone must not make an input starting at the open clipped.
    const auto bars = make_feed(jan1 + 18 * hour, hour, 2);
    ProjectionHost aligned("D", "60");
    aligned.set_syminfo_session("1800-1700");
    aligned.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
    assert(aligned.last_error().empty());
    assert(aligned.dispatches.size() == 1);
    assert(!aligned.dispatches[0].complete);
    assert(aligned.values.size() == bars.size());
    for (double value : aligned.values) assert(value == bars.back().close);

    ProjectionHost unknown_open("D", "60");
    unknown_open.set_syminfo_session("1800-1700");
    unknown_open.run(bars.data() + 1, 1, "60", "60");
    assert(unknown_open.last_error().empty());
    // A nominal session stamp alone does not prove earlier trading without
    // an explicitly supplied native bar or auxiliary evidence.
    assert(unknown_open.dispatches.size() == 1);
    assert(!unknown_open.dispatches[0].complete);
    assert(unknown_open.values.size() == 1);
    assert(unknown_open.values[0] == bars.back().close);
}

bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

bool same_value(double a, double b) {
    return a == b || (std::isnan(a) && std::isnan(b));
}

void check_same_trace(const ProjectionHost& a, const ProjectionHost& b) {
    assert(!a.dispatches.empty());
    assert(!a.chart_bars.empty());
    assert(a.dispatches.size() == b.dispatches.size());
    assert(a.chart_bars.size() == b.chart_bars.size());
    assert(a.values.size() == b.values.size());
    assert(a.previous_values.size() == b.previous_values.size());
    for (std::size_t i = 0; i < a.dispatches.size(); ++i) {
        assert(same_bar(a.dispatches[i].bar, b.dispatches[i].bar));
        assert(a.dispatches[i].complete == b.dispatches[i].complete);
        assert(a.dispatches[i].slot_new == b.dispatches[i].slot_new);
    }
    for (std::size_t i = 0; i < a.chart_bars.size(); ++i) {
        assert(same_bar(a.chart_bars[i], b.chart_bars[i]));
        assert(same_value(a.values[i], b.values[i]));
        assert(same_value(a.previous_values[i], b.previous_values[i]));
    }
}

void check_live_batch_modes() {
    // The last input is forming, and would complete the day if projected.
    const auto bars = make_feed(jan1, hour, 24);
    for (int mode : {1, 2, 3}) {
        ProjectionHost legacy("D", "60", Registration::legacy5);
        const bool realtime = (mode & 1) != 0;
        const bool suppress = (mode & 2) != 0;
        assert(legacy.set_realtime_tail(realtime, static_cast<int>(bars.size())));
        assert(legacy.set_probe_suppress_tail_logic(suppress));
        legacy.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
        assert(legacy.last_error().empty());
        assert(legacy.dispatches.size() == bars.size());
        const std::size_t callbacks = bars.size() - (suppress ? 1 : 0);
        assert(legacy.chart_bars.size() == callbacks);
        for (std::size_t i = 0; i < callbacks; ++i) {
            assert(same_bar(legacy.chart_bars[i], bars[i]));
            assert(legacy.values[i] == bars[i].close);
        }
        for (Registration registration : {Registration::site,
                                          Registration::legacy6,
                                          Registration::metadata}) {
            ProjectionHost host("D", "60", registration);
            assert(host.set_realtime_tail(realtime, static_cast<int>(bars.size())));
            assert(host.set_probe_suppress_tail_logic(suppress));
            host.run(bars.data(), static_cast<int>(bars.size()), "60", "60");
            assert(host.last_error().empty());
            check_same_trace(legacy, host);
        }
    }
}

void check_metadata_and_epoch_routes() {
    const auto bars = make_feed(jan1 + hour + hour / 4, hour / 4, 9);
    ProjectionHost metadata("60", "15", Registration::metadata);
    metadata.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    assert(metadata.last_error().empty());
    assert(metadata.dispatches.size() == 3);
    assert(metadata.chart_bars.size() == bars.size());
    assert(metadata.dispatches.front().bar.timestamp == bars.front().timestamp);
    for (int i = 0; i < 3; ++i) assert(metadata.values[i] == bars[2].close);

    // The explicit epoch precedes the loaded input but includes its bucket.
    // Preserve that selector's existing bucket-open cut for the opted-in site.
    ProjectionHost explicit_epoch("60", "15");
    explicit_epoch.set_syminfo_metadata("security_range_start_na_warmup",
                                       static_cast<double>(jan1 + hour));
    explicit_epoch.run(bars.data(), static_cast<int>(bars.size()), "15", "15");
    assert(explicit_epoch.last_error().empty());
    check_same_trace(metadata, explicit_epoch);
}

void check_zero_month_boundary() {
    // January and February 1970 are whole, followed by two March inputs.
    const auto bars = make_feed(0, day, 31 + 28 + 2);
    ProjectionHost host("M", "D");
    host.run(bars.data(), static_cast<int>(bars.size()), "D", "D");
    assert(host.last_error().empty());
    assert(host.chart_bars.size() == bars.size());
    assert(host.dispatches.size() == 3);
    assert(host.dispatches[0].bar.timestamp == 0);
    assert(host.dispatches[0].complete);
    assert(host.dispatches[1].bar.timestamp == bars[31].timestamp);
    assert(host.dispatches[1].complete);
    assert(host.dispatches[2].bar.timestamp == bars[59].timestamp);
    assert(!host.dispatches[2].complete);
    for (std::size_t i = 0; i < bars.size(); ++i) {
        assert(same_bar(host.chart_bars[i], bars[i]));
        const std::size_t last = i < 31 ? 30 : i < 59 ? 58 : 60;
        assert(host.values[i] == bars[last].close);
        if (i < 31) assert(std::isnan(host.previous_values[i]));
        else assert(host.previous_values[i] == bars[i < 59 ? 30 : 58].close);
    }
}

std::vector<Bar> market_days() {
    std::vector<Bar> bars;
    // Monday is absent; the first loaded week begins on Tuesday at 09:00.
    for (int offset : {1, 2, 3, 4, 7, 8, 9, 10, 11, 14}) {
        const double price = 100.0 + offset;
        bars.push_back({price, price + 1, price - 1, price, 2,
                        jan1 + offset * day + 9 * hour});
    }
    return bars;
}

void check_nominal_n_day_evidence() {
    const auto chart = market_days();
    struct Case {
        const char* timeframe;
        std::vector<std::size_t> ends;
    };
    // Count weekdays from Monday January 1, restarting at the calendar year.
    // The unobserved Monday is a nominal date, not evidence it traded.
    const Case cases[] = {
        {"2D", {0, 2, 4, 6, 8, 9}},
        {"3D", {1, 4, 7, 9}},
    };
    for (const auto& selected : cases) {
        ProjectionHost host(selected.timeframe, "D");
        host.set_syminfo_session("0900-1100");
        host.set_syminfo_type("stock");
        host.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
        assert(host.last_error().empty());
        assert(host.saw_generated_periods);
        assert(host.chart_bars.size() == chart.size());
        assert(host.values.size() == chart.size());
        assert(host.dispatches.size() == selected.ends.size());
        std::size_t begin = 0;
        for (std::size_t group = 0; group < selected.ends.size(); ++group) {
            const auto end = selected.ends[group];
            assert(end >= begin && end < chart.size());
            assert(host.dispatches[group].bar.timestamp == chart[begin].timestamp);
            assert(host.dispatches[group].bar.close == chart[end].close);
            assert(host.dispatches[group].complete == (group + 1 < selected.ends.size()));
            assert(host.dispatches[group].slot_new);
            for (std::size_t i = begin; i <= end; ++i) {
                assert(same_bar(host.chart_bars[i], chart[i]));
                assert(host.values[i] == chart[end].close);
            }
            begin = end + 1;
        }
        assert(begin == chart.size());
        ProjectionHost metadata(selected.timeframe, "D", Registration::metadata);
        metadata.set_syminfo_session("0900-1100");
        metadata.set_syminfo_type("stock");
        metadata.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
        assert(metadata.last_error().empty());
        check_same_trace(host, metadata);
    }
}

void check_week_values(const ProjectionHost& host, const std::vector<Bar>& chart,
                       bool prior_trading) {
    assert(host.last_error().empty());
    assert(host.chart_bars.size() == chart.size());
    assert(host.values.size() == chart.size());
    assert(host.dispatches.size() == (prior_trading ? 2 : 3));
    for (std::size_t i = 0; i < chart.size(); ++i) {
        assert(same_bar(host.chart_bars[i], chart[i]));
        if (i < 4) {
            if (prior_trading) assert(std::isnan(host.values[i]));
            else assert(host.values[i] == chart[3].close);
        } else if (i < 9) {
            assert(host.values[i] == chart[8].close);
        } else {
            assert(host.values[i] == chart.back().close);
        }
    }
    const auto first = prior_trading ? 4 : 0;
    assert(host.dispatches.front().bar.timestamp == chart[first].timestamp);
    assert(host.dispatches.front().complete);
    for (const auto& dispatch : host.dispatches) assert(dispatch.slot_new);
}

void check_native_period_evidence() {
    const auto chart = market_days();
    ProjectionHost without_evidence("W", "D");
    without_evidence.set_syminfo_session("0900-1100");
    without_evidence.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
    check_week_values(without_evidence, chart, false);
    for (bool prior_trading : {false, true}) {
        auto native = chart;
        if (prior_trading)
            native.insert(native.begin(), Bar{99, 100, 98, 99, 2, jan1 + 9 * hour});
        ProjectionHost host("W", "D");
        host.set_syminfo_session("0900-1100");
        assert(host.set_native_security_feed("D", native.data(),
                                             static_cast<int>(native.size())));
        host.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
        check_week_values(host, chart, prior_trading);
    }
}

void check_symbol_calendar_evidence() {
    const auto chart = market_days();
    std::vector<std::pair<std::int64_t, std::int64_t>> sessions;
    for (const auto& bar : chart)
        sessions.emplace_back(bar.timestamp, bar.timestamp + 2 * hour);
    ProjectionHost host("W", "D");
    assert(host.set_symbol_calendar(std::move(sessions)));
    host.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
    // An explicit market calendar prevents inferring trading from the
    // default continuous clock outside the calendar's supplied sessions.
    check_week_values(host, chart, false);
}

#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
void check_split_feed_evidence() {
    const auto chart = market_days();
    for (bool prior_trading : {false, true}) {
        std::vector<Bar> aux;
        // Coverage before the requested week does not prove Monday traded.
        for (int offset : {-3, 0, 1, 2, 3, 4, 7, 8, 9, 10, 11, 14}) {
            if (offset == 0 && !prior_trading) continue;
            for (int sub = 9; sub < 11; ++sub) {
                const double price = 100.0 + offset;
                aux.push_back({price, price + 1, price - 1, price, 1,
                               jan1 + offset * day + sub * hour});
            }
        }
        ProjectionHost host("W", "D");
        host.set_syminfo_session("0900-1100");
        assert(host.set_aux_security_feed(aux.data(), static_cast<int>(aux.size()), "60"));
        host.run(chart.data(), static_cast<int>(chart.size()), "D", "D");
        // The implicit daily partition starts on Tuesday in both cases.
        // Only the actual auxiliary Monday bars can make its week absent.
        check_week_values(host, chart, prior_trading);
    }
}
#endif

} // namespace

int main() {
    check_clipped_prefix("60", "15", jan1 + hour + hour / 4, hour / 4, 3, 4);
    check_clipped_prefix("D", "60", jan1 + hour, hour, 23, 24);
    check_clipped_prefix("W", "D", jan1 + day, day, 6, 7);
    check_clipped_prefix("M", "D", jan1 + day, day, 30, 29); // leap February
    // Supplied daily partitions begin at the same clipped first input.
    // They must not mask the continuous clock's requested-bucket open.
    check_clipped_prefix("W", "D", jan1 + day, day, 6, 7, 2, true);
    check_clipped_prefix("M", "D", jan1 + day, day, 30, 29, 2, true);
    check_clipped_prefix("2D", "D", jan1 + day, day, 1, 2, 1);
    check_clipped_prefix("3D", "D", jan1 + day, day, 2, 3);
    // Timestamp zero is actual input, not an absent-first-input sentinel.
    check_clipped_prefix("W", "D", 0, day, 4, 7); // Thursday, 1970-01-01
    check_session_evidence();
    check_live_batch_modes();
    check_metadata_and_epoch_routes();
    check_zero_month_boundary();
    check_native_period_evidence();
    check_nominal_n_day_evidence();
    check_symbol_calendar_evidence();
#ifdef PINEFORGE_HAS_AUX_SECURITY_FEED_V1
    check_split_feed_evidence();
#endif
    return 0;
}
