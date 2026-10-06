// A stream records what a batch over the same bars records. The batch
// runs over [0, T]; the stream begins on [0, W] and takes the rest bar by
// bar, its caller reading and clearing the events after every input. The
// series are equal bit for bit and the events equal in every field but
// `phase` (warm-up 1, realtime 2; a batch 0), for W = 1, T/2 and T-1, on the
// chart timeframe and on an aggregated chart (1m input, 5m script).
//
// The stale carried case: on a FeedTolerant stream whose last warm-up bar has
// an off-grid label, the first stream_advance_time dispatches a quiet carried
// calculation for the calendar slot holding that label -- an open not after
// the last bar the host published. The host skips it (the Pine host's rule),
// so the stream counts more calculations than it has rows, does not fail, and
// still records what a batch over the same bars (the carried flat bars
// included) records.
//
// Negative controls, kept in this file: a host that keys a row to every
// calculation the kernel counts breaks the equality on the row count, and a
// host that states the real open for every calculation fails the stream at
// the recorder's out-of-order refusal.
//
// Source-free: this TU runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <memory>
#include <sstream>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

struct ScriptState {
    std::unique_ptr<ta::SMA> sma;
};

// slot 0 close, slot 1 a kernel SMA(3), slot 2 a colour; output 0 a mark on
// up bars, output 1 a message on every third bar; constants 50 and a colour.
void install_script(OutputsHost& host, std::shared_ptr<ScriptState> state) {
    host.on_begin = [state](OutputsHost&) { state->sma = std::make_unique<ta::SMA>(3); };
    host.script = [state](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        const bool up = b.close > b.open;
        h.value(0, b.close);
        h.value(1, state->sma->compute(b.close));
        h.value(2, rgba(up ? 0xFF4CAF50u : 0xFFF23645u));
        if (up) h.event(0, b.close);
        if (h.published % 3 == 0) {
            std::ostringstream text;
            text << "bar " << h.published << " close " << b.close;
            h.event(1, b.volume, text.str());
        }
        h.constant(0, 50.0);
        h.constant(1, rgba(0xFF787B86u));
    };
}

struct Run {
    Record rows;                       // the record at the end (its queue unused)
    std::vector<EventRecord> events;   // every event the caller read, in order
    std::string error;
    std::int64_t processed = 0;
    std::int64_t warmup_rows = 0;      // rows recorded when stream_begin returned
};

std::unique_ptr<OutputsHost> make_host() {
    auto host = std::make_unique<OutputsHost>(OutputsHost::Shape{3, 2, 2});
    install_script(*host, std::make_shared<ScriptState>());
    return host;
}

std::string error_of(pf_strategy_t s) {
    const char* text = strategy_get_last_error(s);
    return text ? text : "";
}

Run batch_run(OutputsHost& host, const NativeRunSpec& spec, const std::vector<Bar>& bars) {
    Run out;
    pf_strategy_t s = host.handle();
    if (host.configure_native(spec).status != NativeSetupStatus::Applied
        || strategy_outputs_set_enabled(s, 1) != 0) {
        out.error = "setup refused";
        return out;
    }
    host.run(bars.data(), static_cast<int>(bars.size()));
    out.error = error_of(s);
    out.rows = read_record(s);
    out.events = out.rows.events;
    out.processed = strategy_script_bars_processed(s);
    return out;
}

void drain(pf_strategy_t s, Run& out) {
    for (auto& e : read_events(s)) out.events.push_back(e);
    strategy_outputs_events_clear(s);
}

// Begins on bars[0, warmup) and pushes the rest, then advances the clock to
// each of `advances`; the caller drains after every input.
Run stream_run(OutputsHost& host, const NativeRunSpec& spec, const std::vector<Bar>& bars,
               int warmup, const std::vector<std::int64_t>& advances = {}) {
    Run out;
    pf_strategy_t s = host.handle();
    if (host.configure_native(spec).status != NativeSetupStatus::Applied
        || strategy_outputs_set_enabled(s, 1) != 0) {
        out.error = "setup refused";
        return out;
    }
    std::vector<pf_bar_t> c_bars;
    for (const Bar& b : bars) c_bars.push_back({b.open, b.high, b.low, b.close, b.volume, b.timestamp});
    if (strategy_stream_begin(s, c_bars.data(), warmup, "", "") != 0) {
        out.error = error_of(s);
        return out;
    }
    out.warmup_rows = strategy_outputs_bars_len(s);
    drain(s, out);
    for (std::size_t i = static_cast<std::size_t>(warmup); i < c_bars.size(); ++i) {
        if (strategy_stream_push_bar(s, &c_bars[i]) != 0) {
            out.error = error_of(s);
            return out;
        }
        drain(s, out);
    }
    for (std::int64_t t : advances) {
        if (strategy_stream_advance_time(s, t) != 0) {
            out.error = error_of(s);
            return out;
        }
        drain(s, out);
    }
    out.rows = read_record(s);
    out.processed = strategy_script_bars_processed(s);
    strategy_stream_end(s, 0);
    return out;
}

// The first difference between a batch and a stream over the same bars, or "".
std::string stream_mismatch(const Run& batch, const Run& stream) {
    if (!batch.error.empty()) return "batch failed: " + batch.error;
    if (!stream.error.empty()) return "stream failed: " + stream.error;
    if (batch.rows.bars != stream.rows.bars)
        return "row count " + std::to_string(batch.rows.bars) + " != " + std::to_string(stream.rows.bars);
    if (batch.rows.open_ms != stream.rows.open_ms) return "bar open times";
    if (batch.rows.close_ms != stream.rows.close_ms) return "bar close times";
    if (batch.rows.series.size() != stream.rows.series.size()) return "series count";
    for (std::size_t slot = 0; slot < batch.rows.series.size(); ++slot)
        if (!same_values(batch.rows.series[slot], stream.rows.series[slot]))
            return "series slot " + std::to_string(slot);
    if (!same_values(batch.rows.constants, stream.rows.constants)) return "run constants";
    if (batch.events.size() != stream.events.size())
        return "event count " + std::to_string(batch.events.size()) + " != "
            + std::to_string(stream.events.size());
    for (std::size_t i = 0; i < batch.events.size(); ++i) {
        const EventRecord& b = batch.events[i];
        const EventRecord& s = stream.events[i];
        if (!same_event(b, s, false)) return "event " + std::to_string(i) + " (sequence "
            + std::to_string(b.sequence) + " vs " + std::to_string(s.sequence) + ")";
        if (b.phase != PF_OUTPUT_PHASE_BATCH) return "batch event phase";
        const std::uint32_t expected = s.bar_index < stream.warmup_rows ? PF_OUTPUT_PHASE_WARMUP
                                                                         : PF_OUTPUT_PHASE_REALTIME;
        if (s.phase != expected) return "stream event phase at bar " + std::to_string(s.bar_index);
    }
    return "";
}

void chart_timeframe() {
    const auto bars = make_bars(24, kStep);
    auto reference = make_host();
    const Run batch = batch_run(*reference, make_spec("outputs-d1", 1), bars);
    CHECK(batch.error.empty());
    CHECK(batch.rows.bars == 24);
    CHECK(batch.events.size() > 8);
    for (int warmup : {1, 12, 23}) {
        auto host = make_host();
        const Run stream = stream_run(*host, make_spec("outputs-d1", 1), bars, warmup);
        const std::string why = stream_mismatch(batch, stream);
        if (!why.empty()) std::fprintf(stderr, "  W=%d: %s\n", warmup, why.c_str());
        CHECK(why.empty());
        CHECK(stream.warmup_rows == warmup);
        bool warm = false, real = false;
        for (const auto& e : stream.events) {
            warm = warm || e.phase == PF_OUTPUT_PHASE_WARMUP;
            real = real || e.phase == PF_OUTPUT_PHASE_REALTIME;
        }
        CHECK(real);
        CHECK(warm == (warmup >= 3));   // the first message event is on bar 3
    }
}

void aggregated() {
    const auto bars = make_bars(60, kMinute);
    auto reference = make_host();
    const Run batch = batch_run(*reference, make_spec("outputs-d1-agg", 1, "1", "5"), bars);
    CHECK(batch.error.empty());
    CHECK(batch.rows.bars == 12);
    if (batch.rows.open_ms.size() == 12) {
        CHECK(batch.rows.open_ms[1] == kT0 + kStep && batch.rows.close_ms[1] == kT0 + 2 * kStep);
    }
    for (int warmup : {1, 30, 59}) {
        auto host = make_host();
        const Run stream = stream_run(*host, make_spec("outputs-d1-agg", 1, "1", "5"), bars, warmup);
        const std::string why = stream_mismatch(batch, stream);
        if (!why.empty()) std::fprintf(stderr, "  aggregated W=%d: %s\n", warmup, why.c_str());
        CHECK(why.empty());
    }
}

NativeRunSpec tolerant_spec(const char* key) {
    NativeRunSpec spec = make_spec(key, 1);
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    return spec;
}

// Four warm-up bars, the last labelled one minute into its slot, then the
// clock advanced over three quiet slots.
struct StaleCase {
    std::vector<Bar> warmup;
    std::vector<std::int64_t> advances;
    std::vector<Bar> batch;   // the warm-up bars and the three carried flat bars
};

StaleCase stale_case() {
    StaleCase c;
    c.warmup = make_bars(4, kStep);
    c.warmup[3].timestamp = kT0 + 3 * kStep + kMinute;
    const double last = c.warmup[3].close;
    c.advances = {kT0 + 5 * kStep, kT0 + 6 * kStep, kT0 + 7 * kStep};
    c.batch = c.warmup;
    for (int k = 4; k <= 6; ++k) c.batch.push_back({last, last, last, last, 0.0, kT0 + k * kStep});
    return c;
}

void stale_carried() {
    const StaleCase c = stale_case();
    auto reference = make_host();
    const Run batch = batch_run(*reference, tolerant_spec("outputs-d1-stale"), c.batch);
    CHECK(batch.error.empty());
    CHECK(batch.rows.bars == 7);
    CHECK(reference->stale == 0);

    auto host = make_host();
    const Run stream = stream_run(*host, tolerant_spec("outputs-d1-stale"), c.warmup, 4, c.advances);
    CHECK(stream.error.empty());
    // The trigger fired: the kernel dispatched a calculation the host did not
    // publish, for the slot that holds the last warm-up label.
    CHECK(host->stale == 1);
    CHECK(stream.processed == 8);
    CHECK(stream.rows.bars == 7);
    CHECK(stream.processed > stream.rows.bars);
    const std::string why = stream_mismatch(batch, stream);
    if (!why.empty()) std::fprintf(stderr, "  stale carried: %s\n", why.c_str());
    CHECK(why.empty());

    // Control: a host that keys a row to every calculation the kernel counts
    // (keyed by the count). The equality fails on the row count.
    struct CountKeyed : OutputsHost {
        using OutputsHost::OutputsHost;
        Script inner;
        void on_native_bar(const Bar& b, const NativeDecisionContext& context) override {
            ++calculations;
            ++published;
            if (outputs_enabled_) output_bar(kT0 + calculations, kT0 + calculations + 1);
            if (inner) inner(*this, b, context);
        }
    };
    CountKeyed counted(OutputsHost::Shape{3, 2, 2});
    {
        auto state = std::make_shared<ScriptState>();
        install_script(counted, state);
        counted.inner = counted.script;
    }
    const Run control_a = stream_run(counted, tolerant_spec("outputs-d1-stale"), c.warmup, 4, c.advances);
    const std::string why_a = stream_mismatch(batch, control_a);
    std::fprintf(stderr, "  control, rows keyed to the count: %s\n", why_a.c_str());
    CHECK(why_a.rfind("row count 7 != 8", 0) == 0);

    // A host that states the real open for every calculation is refused by
    // the recorder at the stale calculation: the stream fails.
    auto every = make_host();
    every->skip_stale = false;
    const Run control_open = stream_run(*every, tolerant_spec("outputs-d1-stale"), c.warmup, 4, c.advances);
    std::fprintf(stderr, "  control, every calculation published: %s\n",
                 control_open.error.c_str());
    CHECK(control_open.error.find("is before the last recorded bar") != std::string::npos);
    CHECK(!stream_mismatch(batch, control_open).empty());
}

}  // namespace

int main() {
    chart_timeframe();
    aggregated();
    stale_carried();
    return finish("test_outputs_stream_equivalence");
}
