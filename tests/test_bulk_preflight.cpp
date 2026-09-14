// Structural chart-input admission, independent of strategy/broker decisions.
// --baseline-safe runs only a finite malformed tail, never null/extreme-time UB.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <stdexcept>
#include <vector>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
int failures = 0;
int rejections = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)

void write_bar(std::ostream& out, const Bar& b) {
    out << b.open << ',' << b.high << ',' << b.low << ',' << b.close
        << ',' << b.volume << ',' << b.timestamp << ';';
}
bool same_number(double a, double b) {
    return (std::isnan(a) && std::isnan(b)) || a == b;
}
bool same_bar(const Bar& a, const Bar& b) {
    return same_number(a.open, b.open) && same_number(a.high, b.high)
        && same_number(a.low, b.low) && same_number(a.close, b.close)
        && same_number(a.volume, b.volume) && a.timestamp == b.timestamp;
}
class Probe final : public pineforge::source::PineStrategyHost {
public:
    int preparations = 0;
    int configurations = 0;
    int callbacks = 0;
    bool abort_on_prepare = false;
    bool throw_on_prepare = false;
    std::vector<Bar> observed;
    void prepare_script_run(const Bar*, int, bool) override {
        ++preparations;
        observed.clear();
        if (abort_on_prepare) request_abort();
        if (throw_on_prepare) throw std::runtime_error("sentinel preparation failure");
    }
    void configure_security_evaluators() override { ++configurations; }
    void on_source_bar(const Bar& b) override { ++callbacks; observed.push_back(b); }
    bool abort_pending() const { return abort_requested_.load(std::memory_order_relaxed); }
    size_t curve_size() const { return equity_curve_.size(); }
    double capital() const { return initial_capital_; }
    double pointvalue() const { return syminfo_.pointvalue; }
    std::string input_value() const { return get_input_string("audit_value", ""); }
    void seed_retained_queues() {
        PendingOrder order{};
        order.id = "retained";
        pending_orders_.push_back(order);
        StreamOrderAction action{};
        action.sequence = 42;
        action.order_id = "retained";
        stream_order_actions_.push_back(action);
    }
    // Owned values, not raw object bytes or a hash-only state oracle.
    std::string snapshot() const {
        std::ostringstream out;
        out << std::hexfloat << preparations << ',' << configurations << ',' << callbacks << ';';
        for (const auto& b : observed) write_bar(out, b);
        out << '|' << initial_capital_ << ',' << pyramiding_ << ',' << slippage_
            << ',' << commission_value_ << ',' << static_cast<int>(commission_type_)
            << ',' << default_qty_value_ << ',' << static_cast<int>(default_qty_type_)
            << ',' << process_orders_on_close_ << ',' << calc_on_order_fills_
            << ',' << close_entries_rule_any_ << ',' << qty_step_ << ',' << syminfo_mintick_;
        for (const auto& s : {syminfo_.ticker, syminfo_.tickerid, syminfo_.currency,
             syminfo_.basecurrency, syminfo_.type, syminfo_.timezone, syminfo_.session,
             syminfo_.volumetype, syminfo_.description}) out << '|' << s;
        out << '|' << syminfo_.mintick << ',' << syminfo_.pointvalue << ',' << syminfo_.qty_step;
        for (const auto& kv : std::map<std::string, std::string>(inputs_.begin(), inputs_.end()))
            out << '|' << kv.first << '=' << kv.second;
        out << '|' << input_tf_ << ',' << script_tf_ << ',' << security_input_tf_
            << ',' << script_tf_seconds_ << ',' << bar_magnifier_enabled_
            << ',' << magnifier_samples_ << ',' << static_cast<int>(magnifier_dist_)
            << ',' << bar_index_ << ',' << last_bar_index_ << ',' << last_bar_time_
            << ',' << diag_input_bars_processed_ << ',' << diag_script_bars_processed_
            << ',' << diag_magnifier_sub_bars_processed_ << ',' << diag_magnifier_sample_ticks_processed_
            << ',' << diag_script_tf_ratio_ << ',' << diag_needs_aggregation_;
        write_bar(out, current_bar_);
        for (const auto* series : {&_src_open_, &_src_high_, &_src_low_, &_src_close_, &_src_volume_}) {
            out << '|' << series->size() << ':';
            for (int i = 0; i < series->size(); ++i) out << (*series)[i] << ',';
        }
        out << '|' << equity_curve_.size();
        for (const auto& e : equity_curve_) out << ';' << e.time_ms << ',' << e.equity << ',' << e.open_profit;
        out << '|' << trades_.size() << ',' << range_end_trades_.size()
            << ',' << signed_position_size() << ',' << position_entry_price_ << ',' << net_profit_sum_;
        for (const auto& o : pending_orders_) out << '|' << o.id << ',' << o.qty;
        for (const auto& a : stream_order_actions_) out << '|' << a.sequence << ',' << a.order_id;
        for (const auto h : broker_state_hashes_) out << '|' << h;
        out << '|' << stream_state_hash(); // supplementary, includes stream/aggregator cursors
        return out.str();
    }
};

enum class Route { Single, TF, Auto, Aggregate, Magnifier, Full, FullAuto, FullAggregate, FullMagnifier };
const Route routes[] = {Route::Single, Route::TF, Route::Auto, Route::Aggregate, Route::Magnifier,
                       Route::Full, Route::FullAuto, Route::FullAggregate, Route::FullMagnifier};
const char* name(Route r) {
    const char* names[] = {"single", "tf", "auto", "aggregate", "magnifier", "full", "full-auto", "full-aggregate", "full-magnifier"};
    return names[static_cast<int>(r)];
}
bool aggregated(Route r) { return r == Route::Aggregate || r == Route::FullAggregate; }
void invoke(Probe& p, Route r, const Bar* bars, int n) {
    if (r == Route::Single) { p.run(bars, n); return; }
    const bool autodetect = r == Route::Auto || r == Route::FullAuto;
    const bool magnifier = r == Route::Magnifier || r == Route::FullMagnifier;
    const std::string input_tf = autodetect ? "" : "1";
    const std::string script_tf = autodetect ? "" : (aggregated(r) ? "3" : "1");
    if (r < Route::Full) {
        p.run(bars, n, input_tf, script_tf, magnifier);
    } else {
        SymInfo symbol;
        symbol.ticker = "changed";
        symbol.pointvalue = 50;
        symbol.mintick = 0.25;
        symbol.qty_step = 0.5;
        source::StrategyOverrides overrides;
        overrides.initial_capital = 54321;
        overrides.commission_value = 0.2;
        overrides.commission_type = 0;
        overrides.default_qty_value = 2;
        overrides.default_qty_type = 0;
        overrides.pyramiding = 2;
        overrides.slippage = 1;
        overrides.process_orders_on_close = 1;
        overrides.calc_on_order_fills = 0;
        overrides.close_entries_rule = 1;
        p.run(bars, n, input_tf, script_tf, {{"audit_value", "changed"}}, symbol,
              &overrides, magnifier);
    }
}
std::vector<Bar> bars(int n) {
    std::vector<Bar> result;
    for (int i = 0; i < n; ++i)
        result.push_back(Bar{100.0+i, 102.0+i, 99.0+i, 101.0+i, 1.0+i, int64_t(i)*60000});
    return result;
}
void seed(Probe& p) {
    p.set_input("audit_value", "original");
    p.set_broker_state_hash_recording(true);
    const Bar prior[] = {{20,22,19,21,1,600000}, {21,23,20,22,2,660000}, {22,24,21,23,3,720000}};
    p.run(prior, 3);
    CHECK(p.last_error().empty());
    p.seed_retained_queues();
}
void rejected(Route r, const Bar* data, int n, const std::string& rule, bool seeded = true) {
    Probe p;
    if (seeded) seed(p);
    p.request_abort(); // idle request must be cleared once, even on rejection
    const auto before = p.snapshot();
    invoke(p, r, data, n);
    if (p.last_error().find(rule) == std::string::npos || before != p.snapshot()) {
        std::fprintf(stderr, "route=%s n=%d expected=%s error=%s state_equal=%d\n",
                     name(r), n, rule.c_str(), p.last_error().c_str(), before == p.snapshot());
    }
    CHECK(p.last_error().find(rule) != std::string::npos);
    CHECK(p.snapshot() == before);
    CHECK(!p.abort_pending());
    CHECK(p.last_run_status() == 0);
    CHECK(std::string(strategy_get_last_error(&p)) == p.last_error());
    ++rejections;
}
void safe_tail_failure() {
    auto input = bars(257);
    input.back().high = input.back().close - 1;
    for (auto r : routes) rejected(r, input.data(), int(input.size()), "bar[256].high");
}
void malformed_matrix() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    double Bar::*fields[] = {&Bar::open, &Bar::high, &Bar::low, &Bar::close};
    const char* names[] = {"open", "high", "low", "close"};
    for (auto r : routes) {
        auto input = bars(257);
        rejected(r, nullptr, 1, "bars");
        rejected(r, nullptr, 2, "bars");
        rejected(r, input.data(), -1, "count");
        rejected(r, nullptr, -1, "count");
        for (int pos : {0, 128, 256}) {
            const auto good = input[pos];
            const auto prefix = "bar[" + std::to_string(pos) + "].";
            for (int f = 0; f < 4; ++f) for (double v : {nan, inf, -inf}) {
                input[pos] = good;
                input[pos].*fields[f] = v;
                rejected(r, input.data(), int(input.size()), prefix + names[f]);
            }
            for (double v : {-1.0, inf, -inf}) {
                input[pos] = good;
                input[pos].volume = v;
                rejected(r, input.data(), int(input.size()), prefix + "volume");
            }
            const Bar shapes[] = {{102,101,100,103,3,good.timestamp}, {102,104,103,103,3,good.timestamp},
                {103,104,102,101,3,good.timestamp}, {103,102,100,101,3,good.timestamp},
                {102,100,104,103,3,good.timestamp}};
            for (const auto& bad : shapes) {
                input[pos] = bad;
                rejected(r, input.data(), int(input.size()), prefix);
            }
            input[pos] = good;
        }
        input[256].timestamp = input[255].timestamp;
        rejected(r, input.data(), int(input.size()), "bar[256].timestamp");
        --input[256].timestamp;
        rejected(r, input.data(), int(input.size()), "bar[256].timestamp");
        const Bar extreme[] = {{1,1,1,1,0,std::numeric_limits<int64_t>::min()},
                               {1,1,1,1,0,std::numeric_limits<int64_t>::max()}};
        rejected(r, extreme, 2, "bar[1].timestamp");
        const Bar crossing[] = {{1,1,1,1,0,-1}, {1,1,1,1,0,std::numeric_limits<int64_t>::max()}};
        rejected(r, crossing, 2, "bar[1].timestamp");
        auto short_input = bars(3);
        short_input[2].low = short_input[2].high + 1;
        rejected(r, short_input.data(), 3, "bar[2].", false);
    }
}
void positive_controls() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const Bar controls[] = {{0,0,0,0,0,0}, {-10,-8,-12,-9,nan,0},
        {100.003,100.007,100.001,100.005,0.125,0},
        {100,102,99,101,std::numeric_limits<double>::max(),0}};
    for (auto r : routes) {
        for (const auto& b : controls) {
            Probe p;
            invoke(p, r, &b, 1);
            CHECK(p.last_error().empty());
            CHECK(p.preparations == 1);
            if (!aggregated(r)) {
                CHECK(p.observed.size() == 1);
                if (p.observed.size() == 1) CHECK(same_bar(p.observed[0], b));
            }
        }
        auto input = bars(3);
        input[0].volume = 0;
        input[1].volume = 0.125;
        input[2].volume = nan;
        Probe p;
        invoke(p, r, input.data(), 3);
        CHECK(p.last_error().empty());
        CHECK(p.observed.size() == (aggregated(r) ? 1u : 3u));
        if (!aggregated(r) && p.observed.size() == 3)
            for (size_t i = 0; i < input.size(); ++i) CHECK(same_bar(p.observed[i], input[i]));
        if (aggregated(r) && p.observed.size() == 1) {
            CHECK(p.observed[0].open == 100 && p.observed[0].close == 103);
            CHECK(std::isnan(p.observed[0].volume));
        }
        input[2].timestamp = 240000;
        invoke(p, r, input.data(), 3);
        CHECK(p.last_error().empty()); // gaps admitted; existing aggregation semantics retained
        input = bars(2);
        input[0].timestamp = -120000;
        input[1].timestamp = -60000;
        invoke(p, r, input.data(), 2);
        CHECK(p.last_error().empty()); // modest pre-epoch domain, no extreme-calendar claim
        seed(p);
        invoke(p, r, nullptr, 0);
        CHECK(p.last_error().empty());
        CHECK(p.observed.empty() && p.curve_size() == 0);
        invoke(p, r, input.data(), 0);
        CHECK(p.last_error().empty());
        CHECK(p.observed.empty() && p.curve_size() == 0);
        if (r >= Route::Full) CHECK(p.pointvalue() == 50 && p.capital() == 54321 && p.input_value() == "changed");
    }
}
void abort_and_error_transport() {
    auto input = bars(6);
    for (auto r : routes) {
        Probe p;
        p.request_abort();
        invoke(p, r, input.data(), 6);
        CHECK(p.last_error().empty() && p.last_run_status() == 0);
        p.abort_on_prepare = true;
        const auto count = p.callbacks;
        invoke(p, r, input.data(), 6);
        CHECK(p.last_error().empty() && p.last_run_status() == 1);
        CHECK(p.callbacks == count);
        p.abort_on_prepare = false;
        p.throw_on_prepare = true;
        invoke(p, r, input.data(), 6);
        CHECK(p.last_error() == "sentinel preparation failure");
        CHECK(p.last_run_status() == 0);
        p.throw_on_prepare = false;
        invoke(p, r, input.data(), 6);
        CHECK(p.last_error().empty() && p.last_run_status() == 0);
    }
}
} // namespace
int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--valid-receipt") == 0) {
        const auto input = bars(6);
        for (auto r : routes) {
            Probe p;
            invoke(p, r, input.data(), int(input.size()));
            CHECK(p.last_error().empty());
            std::printf("%s %s\n", name(r), p.snapshot().c_str());
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--controls") == 0) {
        positive_controls();
        abort_and_error_transport();
        return failures ? 1 : 0;
    }
    safe_tail_failure();
    if (!(argc == 2 && std::strcmp(argv[1], "--baseline-safe") == 0)) {
        malformed_matrix();
        positive_controls();
        abort_and_error_transport();
    }
    std::printf("bulk preflight: %d rejection cases, %d failures\n", rejections, failures);
    return failures ? 1 : 0;
}
