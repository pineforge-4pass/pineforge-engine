// A batch cut at bar t records, for every bar up to t, what the batch
// over the whole input records. Five cut points over 48 five-minute bars; the
// host records the close, a kernel ta:: average, a declared 15-minute
// series (its delivered bucket, NaN until the first) and a colour, plus a
// mark and a message event and a run constant. The rows, their values and
// the events at bars <= t are equal, and so are the run constants.
//
// Negative control: a host value that depends on the batch's last bar (1 on
// the bar it knows is the input's last) breaks the prefix, and the harness names the
// cut bar it differs on.
//
// Source-free: this TU runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <memory>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;
constexpr int kTotal = 48;

struct Truncation {
    Record rows;
    std::string error;
};

NativeRunSpec spec_with_series(std::uint64_t run) {
    NativeRunSpec spec = make_spec("outputs-d2", run);
    NativeTimeframeSubscription fifteen;
    fifteen.tf = "15";
    spec.subscriptions.push_back(fifteen);
    return spec;
}

// `last_aware`: the negative control's value, 1 on the bar the host was told
// is the batch's last.
Truncation run_prefix(const std::vector<Bar>& all, int last_bar, bool last_aware) {
    OutputsHost host({5, 2, 1});
    auto sma = std::make_shared<std::unique_ptr<ta::SMA>>();
    host.on_begin = [sma](OutputsHost&) { *sma = std::make_unique<ta::SMA>(4); };
    const int n = last_bar + 1;
    host.script = [sma, last_aware, n](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        h.value(0, b.close);
        h.value(1, (*sma)->compute(b.close));
        const std::optional<Bar> bucket = h.native_series_bar(0);
        h.value(2, bucket ? bucket->close : kNaN);
        if (last_aware) h.value(3, h.published == n ? 1.0 : 0.0);
        h.value(4, rgba(b.close >= b.open ? 0xFF4CAF50u : 0xFFF23645u));
        if (b.close < b.open) h.event(0, b.low);
        if (bucket && h.published % 4 == 0) h.event(1, bucket->high, "bucket");
        h.constant(0, 21.0);
    };
    Truncation out;
    pf_strategy_t s = host.handle();
    if (host.configure_native(spec_with_series(1)).status != NativeSetupStatus::Applied
        || strategy_outputs_set_enabled(s, 1) != 0) {
        out.error = "setup refused";
        return out;
    }
    host.run(all.data(), n);
    const char* text = strategy_get_last_error(s);
    out.error = text ? text : "";
    out.rows = read_record(s);
    return out;
}

// The first bar <= cut whose rows or events differ, or -1. `why` names it.
int prefix_mismatch(const Record& cut, const Record& full, int last, std::string& why) {
    for (int i = 0; i <= last; ++i) {
        if (i >= cut.bars || i >= full.bars) {
            why = "missing row";
            return i;
        }
        if (cut.open_ms[i] != full.open_ms[i] || cut.close_ms[i] != full.close_ms[i]) {
            why = "bar times";
            return i;
        }
        for (std::size_t slot = 0; slot < cut.series.size(); ++slot) {
            if (!same_value(cut.series[slot][i], full.series[slot][i])) {
                why = "series slot " + std::to_string(slot);
                return i;
            }
        }
    }
    if (cut.bars != last + 1) {
        why = "extra row";
        return last + 1;
    }
    std::size_t f = 0;
    for (const EventRecord& e : cut.events) {
        if (f >= full.events.size() || !same_event(e, full.events[f], true)) {
            why = "event sequence " + std::to_string(e.sequence);
            return e.bar_index;
        }
        ++f;
    }
    if (f < full.events.size() && full.events[f].bar_index <= last) {
        why = "missing event";
        return full.events[f].bar_index;
    }
    if (!same_values(cut.constants, full.constants)) {
        why = "run constants";
        return last;
    }
    return -1;
}

void prefixes_equal() {
    const auto bars = make_bars(kTotal, kStep);
    const Truncation full = run_prefix(bars, kTotal - 1, false);
    CHECK(full.error.empty());
    CHECK(full.rows.bars == kTotal);
    // The 15-minute series is delivered on the bar that completes its bucket.
    CHECK(full.rows.series.size() == 5);
    CHECK(full.rows.constants.size() == 1 && full.rows.constants[0] == 21.0);
    if (full.rows.series.size() == 5 && full.rows.bars == kTotal) {
        CHECK(full.rows.series[4][0] == rgba(bars[0].close >= bars[0].open ? 0xFF4CAF50u
                                                                             : 0xFFF23645u));
        CHECK(std::isnan(full.rows.series[2][0]) && std::isnan(full.rows.series[2][1]));
        CHECK(full.rows.series[2][2] == bars[2].close);
        CHECK(full.rows.series[2][4] == bars[2].close);
        CHECK(full.rows.series[2][5] == bars[5].close);
        CHECK(std::isnan(full.rows.series[3][0]));   // not written in this run
    }
    CHECK(full.rows.events.size() > 10);
    // Bucket ends (t % 3 == 2) and bars inside a bucket.
    for (int cut : {0, 8, 19, 44, 46}) {
        const Truncation part = run_prefix(bars, cut, false);
        CHECK(part.error.empty());
        std::string why;
        const int bar = prefix_mismatch(part.rows, full.rows, cut, why);
        if (bar >= 0) std::fprintf(stderr, "  cut %d: bar %d, %s\n", cut, bar, why.c_str());
        CHECK(bar == -1);
    }
}

void last_bar_control() {
    const auto bars = make_bars(kTotal, kStep);
    const Truncation full = run_prefix(bars, kTotal - 1, true);
    CHECK(full.error.empty());
    int failures = 0;
    for (int cut : {0, 8, 19, 44, 46}) {
        const Truncation part = run_prefix(bars, cut, true);
        std::string why;
        const int bar = prefix_mismatch(part.rows, full.rows, cut, why);
        std::fprintf(stderr, "  control, cut %d: %s at bar %d\n", cut,
                     bar >= 0 ? why.c_str() : "no difference", bar);
        CHECK(bar == cut && why == "series slot 3");
        if (bar >= 0) ++failures;
    }
    CHECK(failures == 5);
}

}  // namespace

int main() {
    prefixes_equal();
    last_bar_control();
    return finish("test_outputs_truncation");
}
