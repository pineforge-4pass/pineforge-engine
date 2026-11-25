#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

using Values = std::vector<double>;
using Pair = std::array<double, 2>;
const double missing = na<double>();
constexpr std::int64_t minute = 60000;
constexpr int close15[][4] = {{1, 2, 3, 3}, {4, 5, 6, 6}, {7, 8, 9, 9},
                              {10, 11, 12, 12}, {13, 18, 15, 15}};

std::vector<Bar> bars(int minutes, int count) {
    std::vector<Bar> out;
    const int children = 60 / minutes;
    for (int i = 0; i < count; ++i) {
        const int hour = i / children, child = i % children;
        const double open = child ? out.back().close : 2 * hour + 1;
        const double close = close15[hour][(child + 1) * (4 / children) - 1];
        out.push_back({open, std::max(open, close), std::min(open, close), close, 1,
                       minute * minutes * i});
    }
    return out;
}

bool same(double a, double b) {
    return (is_na(a) && is_na(b)) || std::abs(a - b) < 1e-12;
}

bool same(const Values& a, const Values& b) {
    return a.size() == b.size()
        && std::equal(a.begin(), a.end(), b.begin(), [](double x, double y) { return same(x, y); });
}

int failures = 0;
void row(const char* name, const std::string& error, bool ok) {
    if (!error.empty()) std::printf("ROW %s NOT COMPARED: %s\n", name, error.c_str());
    else std::printf("ROW %s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok || !error.empty()) ++failures;
}

// Compile the same test on both headers. The earlier API can request the
// final candle, but delivers it through evaluate_security on the first child.
#ifdef PINEFORGE_HAS_SECURITY_CHART_PUBLICATION_V1
using Publisher = source::PineSecurityChartPublisher;
#else
struct Publisher {
    virtual void publish_security_chart(int, const Bar&) {}
};
#endif

enum class Mode { chart, once, six, seven_false, foreign };
struct Site {
    Mode mode = Mode::chart;
    bool ahead = true, gaps = false;
};
struct Step {
    Bar bar;
    source::PineRequestDelivery delivery;
    bool open, complete;
    Pair value;
};

// Literal tuple [close, f(ta.sma(close, 3))], where f(x) = x[2] - x.
// Both the SMA and the sampled argument have their own requested-bar history.
struct Helper {
    ta::SMA sma{3};
    Series<double> arg{5};
    Pair eval(const Bar& b, bool open) {
        const double x = open ? sma.compute(b.close) : sma.recompute(b.close);
        if (open) arg.push(x);
        else arg.update(x);
        return {b.close, arg[2] - x};
    }
};

struct Sample {
    Helper live;
    Pair current{missing, missing}, chart_cell{missing, missing};
    double previous = missing, offset = missing, chart_previous = missing;
    Values commits, chart_close, chart_diff, chart_offset;
    std::vector<Step> steps;
    std::vector<Bar> publications;
    bool scope_ok = true;
};

class Host : public source::PineStrategyHost, public Publisher {
public:
    explicit Host(std::vector<Site> sites = {{}}, std::string requested = "60")
        : samples(sites.size()), sites_(std::move(sites)), tf_(std::move(requested)) {
        attach_pine_execution_adapter();
        configure_pine_strategy(source::PineStrategyConfig{});
    }
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        for (int id = 0; id < static_cast<int>(sites_.size()); ++id) {
            const auto s = sites_[id];
            switch (s.mode) {
            case Mode::chart:
#ifdef PINEFORGE_HAS_SECURITY_CHART_PUBLICATION_V1
                register_security_chart_eval(id, tf_, input_tf_, s.ahead, s.gaps, false);
                break;
#else
                [[fallthrough]];
#endif
            case Mode::once:
                register_security_eval(id, tf_, input_tf_, s.ahead, s.gaps, false, true);
                break;
            case Mode::six:
                register_security_eval(id, tf_, input_tf_, s.ahead, s.gaps, false);
                break;
            case Mode::seven_false:
                register_security_eval(id, tf_, input_tf_, s.ahead, s.gaps, false, false);
                break;
            case Mode::foreign:
                register_security_eval(id, "TEST:OTHER", tf_, input_tf_, s.ahead, s.gaps, false);
                break;
            }
        }
    }
    void evaluate_security(int id, const Bar& b, bool complete) override {
        auto& s = samples.at(id);
        const bool open = security_series_slot_is_new(id);
        s.current = s.live.eval(b, open);
        s.chart_cell = s.current;  // also the fallback when no projection exists
        s.chart_previous = s.offset = s.previous; // close[1] before committing this close
        s.steps.push_back({b, current_request_delivery(id), open, complete, s.current});
        if (complete) {
            s.previous = b.close;
            s.commits.push_back(b.close);
        }
    }
    void publish_security_chart(int id, const Bar& final) override {
        auto& s = samples.at(id);
        const auto d = current_request_delivery(id);
        s.scope_ok = s.scope_ok && !s.steps.empty()
            && s.steps.size() == s.publications.size() + 1
            && d.caller_valid && d.caller == s.steps.back().delivery.caller
            && d.requested_slot == s.steps.back().delivery.requested_slot
            && ta::bar_context().installed && ta::bar_context().bar_index == d.requested_slot;
        s.publications.push_back(final);
        auto scratch = s.live;  // rewrite the real current slot in a disposable copy
        s.chart_cell = scratch.eval(final, false);
        s.chart_previous = s.offset;
    }
    void on_source_bar(const Bar&) override {
        for (auto& s : samples) {
            s.chart_close.push_back(s.chart_cell[0]);
            s.chart_diff.push_back(s.chart_cell[1]);
            s.chart_offset.push_back(s.chart_previous);
        }
    }
    void feed(const std::vector<Bar>& b, int minutes) {
        run(b.data(), static_cast<int>(b.size()), std::to_string(minutes), std::to_string(minutes));
    }
    std::vector<Sample> samples;
private:
    std::vector<Site> sites_;
    std::string tf_;
};

Values closes(const Sample& s) {
    Values values;
    for (const auto& e : s.steps) values.push_back(e.bar.close);
    return values;
}

void cadence(int minutes, const Sample& s, const char* name, const std::string& error) {
    const int children = 60 / minutes, count = 5 * children;
    bool ok = s.steps.size() == static_cast<std::size_t>(count)
        && same(s.commits, {3, 6, 9, 12, 15});
    for (int i = 0; i < count && ok; ++i) {
        const auto& e = s.steps[i];
        const int hour = i / children, child = i % children;
        const bool complete = child + 1 == children;
        ok = e.bar.timestamp == hour * 60 * minute && e.delivery.caller_valid
            && e.delivery.requested_slot == hour && e.delivery.actual_complete == complete
            && e.delivery.publication == complete && !e.delivery.replay
            && e.open == (child == 0) && e.complete == complete
            && e.bar.close == close15[hour][(child + 1) * (4 / children) - 1];
    }
    row(name, error, ok);
}

void helper_rows() {
    const auto feed = bars(15, 20);
    Host host;
    host.feed(feed, 15);
    const Sample& s = host.samples[0];
    cadence(15, s, "hour-from-quarters", host.last_error());
    for (int minutes : {30, 60}) {
        Host other;
        other.feed(bars(minutes, 300 / minutes), minutes);
        cadence(minutes, other.samples[0], minutes == 30 ? "hour-from-halves" : "hour-on-hour",
                other.last_error());
    }

    // Fifth hour: SMA two slots ago = (3+6+9)/3 = 6. Now it is
    // (9+12+close)/3. Child 2 reaches 18; the hour finishes at 15.
    constexpr double live_diff[] = {-16.0/3, -7, -6, -6};
    constexpr double high[] = {13, 18, 18, 18};
    bool live = s.steps.size() == 20, published = s.publications.size() == 20 && s.scope_ok;
    for (int i = 0; i < 4 && live; ++i) {
        const auto& e = s.steps[16 + i];
        live = e.bar.open == 9 && e.bar.low == 9 && e.bar.high == high[i]
            && e.bar.volume == i + 1 && e.value[0] == close15[4][i]
            && same(e.value[1], live_diff[i]);
    }
    for (int i = 16; i < 20 && published; ++i) {
        const auto& b = s.publications[i];
        published = b.timestamp == 240 * minute && b.open == 9 && b.high == 18
            && b.low == 9 && b.close == 15 && b.volume == 4;
    }
    row("tuple-real-child", host.last_error(), live);
    Values final, diff, offset;
    for (int i = 0; i < 20; ++i) {
        final.push_back(3 * (i / 4 + 1));
        diff.push_back(i < 16 ? missing : -6.0);
        offset.push_back(i < 4 ? missing : 3 * (i / 4));
    }
    row("tuple-chart", host.last_error(), published && same(s.chart_close, final)
        && same(s.chart_diff, diff));
    row("previous-close", host.last_error(), same(s.chart_offset, offset));

    // End on the revision to 18, with a native final of 24: even the last
    // live SMA/argument and undo state must still belong to the real input.
    const auto cut = bars(15, 18);
    const Bar official{9, 24, 9, 24, 10, 240 * minute};
    Host projecting, plain(std::vector<Site>{{Mode::six}});
    const auto install_and_feed = [&](Host& h) {
        const bool installed = h.set_native_security_feed("60", &official, 1);
        if (installed) h.feed(cut, 15);
        return installed && h.last_error().empty();
    };
    const bool ready = install_and_feed(projecting) && install_and_feed(plain);
    const auto& a = projecting.samples[0];
    const auto& b = plain.samples[0];
    const auto snapshot = [](const Sample& sample) {
        Values v;
        for (int i = sample.live.arg.size() - 1; i >= 0; --i) v.push_back(sample.live.arg[i]);
        auto sma = sample.live.sma;
        v.push_back(sma.recompute(12)); // (9+12+12)/3
        v.push_back(sma.compute(18));   // (12+12+18)/3
        return v;
    };
    bool identities = a.steps.size() == 18 && b.steps.size() == 18;
    for (int i = 0; i < 18 && identities; ++i) {
        const auto& x = a.steps[i]; const auto& y = b.steps[i];
        identities = x.delivery.caller_valid && x.delivery.caller == y.delivery.caller
            && x.delivery.requested_slot == y.delivery.requested_slot
            && x.open == y.open && x.complete == y.complete && x.bar.close == y.bar.close;
    }
    row("live-state-and-identity", ready ? "" : "native feed or run failed",
        identities && same(snapshot(a), {missing, missing, 6, 9, 13, 11, 14})
        && same(snapshot(a), snapshot(b)) && same(a.commits, {3, 6, 9, 12})
        && same(a.commits, b.commits) && a.current[0] == 18 && same(a.current[1], -7)
        && a.chart_close.size() == 18 && a.chart_close[16] == 24 && a.chart_close[17] == 24
        && a.chart_diff.size() == 18 && same(a.chart_diff[17], -9));
}

void persistent_initializer() {
    class SeedHost final : public Host {
    public:
        SeedHost() : Host({{}}, "2") {}
        void evaluate_security(int id, const Bar& b, bool complete) override {
            const bool open = security_series_slot_is_new(id);
            if (state.size() == 0) { seed = b.close; state.push(seed); }
            else if (open) state.push(state[0]);
            else state.update(state.size() > 1 ? state[1] : seed);
            state.update(state[0] + 1);
            current = {b.close + 1, state[0], previous + 1, previous_var + 1};
            chart = current;
            real.push_back(current[1]);
            if (complete) { previous = current[2]; previous_var = current[3]; }
        }
        void publish_security_chart(int, const Bar& b) override {
            chart = {b.close + 1, current[1], current[2], current[3]};
        }
        void on_source_bar(const Bar&) override { seen.push_back(chart); }
        Series<double> state;
        double seed = missing, previous = 0, previous_var = 0;
        Values real;
        std::array<double, 4> current{}, chart{};
        std::vector<std::array<double, 4>> seen;
    } host;
    std::vector<Bar> inputs;
    for (int i = 0; i < 4; ++i) {
        const double price = i + 1;
        inputs.push_back({price, price, price, price, 1, minute * i});
    }
    host.feed(inputs, 1);
    row("persistent-first-child", host.last_error(), host.seed == 1 && host.state.size() == 2
        && same(host.real, {2, 2, 3, 3})
        && host.seen == std::vector<std::array<double, 4>>{
            {3, 2, 1, 1}, {3, 2, 1, 1}, {5, 3, 2, 2}, {5, 3, 2, 2}});
}

void controls() {
    const auto cut = bars(15, 6);
    const Values real{1, 2, 3, 3, 4, 5}, final{3, 3, 3, 3, 5, 5};
    Host mixed({{Mode::chart}, {Mode::once}, {Mode::six}, {Mode::seven_false},
                {Mode::chart, false}, {Mode::chart, true, true}, {Mode::foreign}});
    const Bar foreign[] = {{51, 51, 51, 51, 1, 0}, {65, 65, 65, 65, 1, 60 * minute}};
    const std::int64_t ends[] = {60 * minute, 120 * minute};
    const bool installed = mixed.set_symbol_feed("TEST:OTHER", "60", foreign, ends, 2);
    if (installed) mixed.feed(cut, 15);
    const std::string error = installed ? mixed.last_error() : "foreign feed refused";
    const auto& sites = mixed.samples;
    row("truncated-hour", error, same(sites[0].chart_close, final)
        && same(closes(sites[0]), real) && same(sites[0].commits, {3})
        && sites[0].publications.size() == 6);
    row("seventh-true", error, same(sites[1].chart_close, final)
        && same(closes(sites[1]), {3, 5}) && same(sites[1].commits, {3})
        && sites[1].publications.empty());
    row("six-and-seventh-false", error, same(sites[2].chart_close, real)
        && same(sites[3].chart_close, real) && same(closes(sites[2]), real)
        && same(closes(sites[3]), real) && sites[2].publications.empty()
        && sites[3].publications.empty());
    row("lookahead-off", error, same(sites[4].chart_close, {missing, missing, missing, 3, 3, 3})
        && same(closes(sites[4]), {3}) && sites[4].publications.empty());
    row("gaps-on", error, same(sites[5].chart_close, real) && same(closes(sites[5]), real)
        && sites[5].publications.empty());
    row("foreign-symbol", error, same(sites[6].chart_close, {51, 51, 51, 51, 65, 65})
        && same(closes(sites[6]), {51, 65}) && sites[6].publications.empty());

    Host global({{Mode::six}, {Mode::chart}});
    global.set_syminfo_metadata("historical_security_lookahead_projection", 1);
    global.feed(cut, 15);
    row("run-switch", global.last_error(), same(global.samples[0].chart_close, final)
        && same(closes(global.samples[0]), {3, 5}) && global.samples[0].publications.empty()
        && same(global.samples[1].chart_close, final) && same(closes(global.samples[1]), real));

    class NoOp final : public Host {
        void publish_security_chart(int, const Bar&) override {}
    } noop;
    noop.feed(cut, 15);
    row("no-op-publisher", noop.last_error(), same(noop.samples[0].chart_close, real)
        && same(closes(noop.samples[0]), real));

    for (bool suppress : {false, true}) {
        Host tail;
        const auto seven = bars(15, 7); // last hour: confirmed 4,5; forming 6
        const Bar native[] = {{1, 30, 1, 30, 4, 0}, {3, 60, 3, 60, 4, 60 * minute}};
        const bool ready = tail.set_native_security_feed("60", native, 2)
            && (suppress ? tail.set_probe_suppress_tail_logic(true) : tail.set_realtime_tail(true, 7));
        if (ready) tail.feed(seven, 15);
        Values want{30, 30, 30, 30, 5, 5};
        if (!suppress) want.push_back(6);
        const auto& s = tail.samples[0];
        row(suppress ? "suppressed-tail" : "forming-tail", ready ? tail.last_error() : "tail setup failed",
            same(s.chart_close, want) && same(closes(s), {1, 2, 3, 30, 4, 5, 6})
            && same(s.commits, {30}) && s.publications.size() == 7
            && s.publications.back().close == 6);
    }
    Host stream;
    const bool accepted = stream.stream_begin(cut.data(), 4, "15", "15")
        && stream.stream_push_tick({cut[4].timestamp, 1, 4, 1})
        && stream.stream_advance_time(cut[5].timestamp)
        && stream.stream_push_tick({cut[5].timestamp, 2, 5, 1})
        && stream.stream_advance_time(cut[5].timestamp + 15 * minute)
        && stream.stream_end();
    row("stream", accepted ? stream.last_error() : "stream call refused",
        same(closes(stream.samples[0]), real) && stream.samples[0].publications.empty());

    Host chart, once(std::vector<Site>{{Mode::once}}), six(std::vector<Site>{{Mode::six}}), again;
    for (Host* h : {&chart, &once, &six, &again}) h->configure_security_evaluators();
    const auto hash = chart.broker_state_hash();
    row("registration-hash", "", hash == again.broker_state_hash()
        && hash != once.broker_state_hash() && hash != six.broker_state_hash());
}

void sparse_periods() {
    // The input after a hole emits the old slot's completion AND the new
    // slot's first peek. Each publication must belong to its own delivery.
    auto feed = bars(15, 6);
    feed = {feed[1], feed[4], feed[5]}; // first observation is inside the first hour
    Host host;
    host.feed(feed, 15);
    const auto& s = host.samples[0];
    Values published;
    std::vector<std::int64_t> stamps;
    for (const auto& b : s.publications) {
        published.push_back(b.close);
        stamps.push_back(b.timestamp);
    }
    row("late-completion", host.last_error(), same(closes(s), {2, 2, 4, 5})
        && same(s.commits, {2}) && same(published, {2, 2, 5, 5}) && s.scope_ok
        && stamps == std::vector<std::int64_t>{0, 0, 60 * minute, 60 * minute}
        && same(s.chart_close, {2, 5, 5}) && same(s.chart_offset, {missing, 2, 2}));

    // The first 20-minute native row is missing. It has two real children;
    // the following periods already have an authoritative delivery at chart open.
    const auto quarters = bars(15, 6);
    const Bar native[] = {{71, 71, 71, 71, 1, 20 * minute}, {72, 72, 72, 72, 1, 40 * minute},
                          {73, 73, 73, 73, 1, 60 * minute}};
    Host fractional({{}}, "20"), plain({{Mode::six}}, "20");
    bool ready = fractional.set_native_security_feed("20", native, 3)
        && plain.set_native_security_feed("20", native, 3);
    if (ready) {
        fractional.feed(quarters, 15);
        plain.feed(quarters, 15);
        ready = fractional.last_error().empty() && plain.last_error().empty();
    }
    const auto& f = fractional.samples[0];
    row("fractional-native", ready ? "" : "native feed or run failed",
        same(closes(f), {1, 2, 71, 72, 73}) && same(f.commits, {2, 71, 72, 73})
        && same(closes(plain.samples[0]), closes(f)) && same(plain.samples[0].commits, f.commits)
        && same(f.chart_close, {2, 2, 71, 72, 73, 73})
        && same(plain.samples[0].chart_close, {1, 2, 71, 72, 73, 73})
        && f.publications.size() == 5 && f.scope_ok);
}

void counted_calendar(const char* tf, const char* name, std::initializer_list<int> days,
                      const Values& finals) {
    std::vector<Bar> daily;
    Values real;
    for (int day : days) {
        const double price = static_cast<double>(daily.size() + 1);
        daily.push_back({price, price, price, price, 1, day * 1440 * minute});
        real.push_back(price);
    }
    Host host({{}}, tf);
    host.run(daily.data(), static_cast<int>(daily.size()), "D", "D");
    const auto& s = host.samples[0];
    row(name, host.last_error(), same(s.chart_close, finals) && same(closes(s), real)
        && same(s.commits, {finals.front()}) && s.publications.size() == days.size());
}

} // namespace

int main() {
    try {
        helper_rows();
        persistent_initializer();
        controls();
        sparse_periods();
        // The fortnight starting Mon 1969-12-29 ends before Jan 12; Q1
        // ends before Apr 1. Crossing the intervening week/month is a rewrite.
        counted_calendar("2W", "two-week-chart", {3, 4, 10, 11}, {3, 3, 3, 4});
        counted_calendar("3M", "quarter-chart", {30, 31, 58, 89, 90}, {4, 4, 4, 4, 5});
    } catch (const std::exception& e) {
        std::printf("NOT COMPARED: %s\n", e.what());
        return 1;
    }
    return failures == 0 ? 0 : 1;
}
