// The outputs recorder against hand values. A bare NativeStrategyHost records
// over 8 synthetic bars; everything is read back through the strategy_outputs_*
// C exports. Proves the values (NaN for a slot not written), the per-row
// ordinals, the 1-based sequence, message bytes and their FNV-1a 64, the
// size-prefixed event copy at 8/16/72/80/96 bytes, every -1 of the C readers,
// the row rules (append, restart, a lower open throws, a write with no row
// throws, a bar before output_run_begin throws, a restart after a clear
// throws, a clear keeps the sequence), the run constants (store, equal
// rewrite, changed throws, NaN, unwritten, reset by a run begin and by a flag
// change, off, index range), two calculations stating one open merging into
// one row, and a handle that declares nothing.
//
// Source-free: this TU runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <algorithm>
#include <stdexcept>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

bool throws_logic(const std::function<void()>& fn, const std::string& needle) {
    try {
        fn();
    } catch (const std::logic_error& e) {
        if (std::string(e.what()).find(needle) != std::string::npos) return true;
        std::fprintf(stderr, "  unexpected text: %s\n", e.what());
        return false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "  not a logic_error: %s\n", e.what());
        return false;
    }
    return false;
}

std::string last_error(pf_strategy_t s) {
    const char* text = strategy_get_last_error(s);
    return text ? std::string(text) : std::string();
}

// §1.7: a handle that never declared outputs, and a NULL handle.
void undeclared_and_null_handles() {
    OutputsHost host({0, 0, 0, false});
    CHECK(host.configure_native(make_spec("outputs-undeclared", 1)).status == NativeSetupStatus::Applied);
    host.script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        h.value(0, b.close);   // recording is off: returns at its first branch
        h.event(0, 1.0);
    };
    const auto bars = make_bars(8, kStep);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    pf_strategy_t s = host.handle();

    CHECK(strategy_outputs_set_enabled(s, 1) == -1);
    CHECK(last_error(s) == "outputs: this module declares no outputs");
    CHECK(strategy_outputs_set_enabled(s, 0) == 0);
    CHECK(strategy_outputs_series_count(s) == 0);
    CHECK(strategy_outputs_bars_len(s) == 0);
    CHECK(strategy_outputs_events_len(s) == 0);
    std::int64_t written = -7;
    CHECK(strategy_outputs_bar_times_copy(s, 0, nullptr, nullptr, 0, &written) == 0 && written == 0);
    double value = 0.0;
    written = -7;
    CHECK(strategy_outputs_series_copy(s, 0, 0, &value, 1, &written) == -1 && written == -7);
    pf_output_event_v1_t raw;
    CHECK(strategy_outputs_event_get(s, 0, &raw, sizeof(raw)) == -1);
    strategy_outputs_events_clear(s);
    CHECK(strategy_outputs_constants_copy(s, nullptr, 0) == 0);

    CHECK(strategy_outputs_set_enabled(nullptr, 1) == -1);
    CHECK(strategy_outputs_set_enabled(nullptr, 0) == -1);
    CHECK(strategy_outputs_series_count(nullptr) == -1);
    CHECK(strategy_outputs_bars_len(nullptr) == -1);
    CHECK(strategy_outputs_bar_times_copy(nullptr, 0, nullptr, nullptr, 0, &written) == -1);
    CHECK(strategy_outputs_series_copy(nullptr, 0, 0, &value, 1, &written) == -1);
    CHECK(strategy_outputs_events_len(nullptr) == -1);
    CHECK(strategy_outputs_event_get(nullptr, 0, &raw, sizeof(raw)) == -1);
    strategy_outputs_events_clear(nullptr);
    CHECK(strategy_outputs_constants_copy(nullptr, nullptr, 0) == -1);
}

// The run under test: slot 0 = close; slot 1 = close > open, except bar 2
// whose slot 1 is never written; output 0 = a mark on every up bar; output 1
// = two messages on bar 4; constant 0 = 50, constant 1 = a colour.
std::vector<Bar> recorded_run(OutputsHost& host) {
    host.script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        const int k = h.published - 1;
        h.value(0, b.close);
        if (k != 2) h.value(1, b.close > b.open ? 1.0 : 0.0);
        if (b.close > b.open) h.event(0, 1.0);
        if (k == 4) {
            h.event(1, kNaN, "first");
            h.event(1, kNaN, std::string("second\0bytes", 12));
        }
        h.constant(0, 50.0);
        h.constant(1, rgba(0xFF4CAF50u));
    };
    const auto bars = make_bars(8, kStep);
    host.run(bars.data(), static_cast<int>(bars.size()));
    return bars;
}

void hand_values() {
    OutputsHost host({2, 2, 2});
    pf_strategy_t s = host.handle();
    CHECK(host.configure_native(make_spec("outputs-recorder", 1)).status == NativeSetupStatus::Applied);
    CHECK(strategy_outputs_series_count(s) == 0);           // declared, still off
    CHECK(strategy_outputs_constants_copy(s, nullptr, 0) == 0);
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    CHECK(host.recording());
    CHECK(strategy_outputs_series_count(s) == 2);
    CHECK(strategy_outputs_constants_copy(s, nullptr, 0) == 2);
    const auto bars = recorded_run(host);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(last_error(s).empty());

    const Record r = read_record(s);
    CHECK(r.series_count == 2);
    CHECK(r.bars == 8);
    CHECK(r.bars <= strategy_script_bars_processed(s));
    CHECK(r.open_ms.size() == 8 && r.close_ms.size() == 8);
    for (int i = 0; i < 8 && r.open_ms.size() == 8; ++i) {
        CHECK(r.open_ms[i] == kT0 + i * kStep);
        CHECK(r.close_ms[i] == kT0 + (i + 1) * kStep);
    }
    CHECK(r.series.size() == 2 && r.series[0].size() == 8 && r.series[1].size() == 8);
    std::vector<EventRecord> expected;
    std::uint64_t sequence = 0;
    for (int i = 0; i < 8 && r.series.size() == 2 && r.series[0].size() == 8; ++i) {
        CHECK(bits(r.series[0][i]) == bits(bars[i].close));
        if (i == 2) {
            CHECK(std::isnan(r.series[1][i]));
        } else {
            CHECK(r.series[1][i] == (bars[i].close > bars[i].open ? 1.0 : 0.0));
        }
        EventRecord e;
        e.struct_version = 1;
        e.size = static_cast<std::uint32_t>(sizeof(pf_output_event_v1_t));
        e.bar_index = i;
        e.bar_open_ms = kT0 + i * kStep;
        e.bar_close_ms = kT0 + (i + 1) * kStep;
        e.phase = PF_OUTPUT_PHASE_BATCH;
        e.confirmed = 1;
        if (bars[i].close > bars[i].open) {
            e.sequence = ++sequence;
            e.output_index = 0;
            e.ordinal_in_bar = 0;
            e.value = 1.0;
            e.has_message = false;
            e.message.clear();
            e.message_hash64 = 0;
            expected.push_back(e);
        }
        if (i == 4) {
            const std::string messages[2] = {"first", std::string("second\0bytes", 12)};
            for (std::uint32_t m = 0; m < 2; ++m) {
                e.sequence = ++sequence;
                e.output_index = 1;
                e.ordinal_in_bar = m;
                e.value = kNaN;
                e.has_message = true;
                e.message = messages[m].c_str();   // a C reader sees the text to its first NUL
                e.message_hash64 = fnv1a64(messages[m]);
                expected.push_back(e);
            }
        }
    }
    CHECK(expected.size() >= 3);
    CHECK(same_events(r.events, expected, true));
    // A message is NUL-terminated C text: the copy stops at the first NUL, the
    // hash covers every recorded byte.
    CHECK(r.events.size() == expected.size());
    std::vector<EventRecord> messages;
    for (const auto& e : r.events)
        if (e.output_index == 1) messages.push_back(e);
    CHECK(messages.size() == 2);
    if (messages.size() == 2) {
        CHECK(messages[0].message == "first" && messages[0].ordinal_in_bar == 0);
        CHECK(messages[1].message == "second" && messages[1].ordinal_in_bar == 1);
        CHECK(messages[1].message_hash64 == fnv1a64(std::string("second\0bytes", 12)));
        CHECK(messages[1].sequence == messages[0].sequence + 1);
    }
    CHECK(r.constants.size() == 2);
    if (r.constants.size() == 2) {
        CHECK(r.constants[0] == 50.0);
        CHECK(r.constants[1] == 1286557951.0);   // color green #4CAF50FF
    }

    // The size-prefixed copy: refused below 8 bytes, otherwise exactly
    // min(size_in, sizeof) bytes.
    const int last = strategy_outputs_events_len(s) - 1;
    unsigned char buffer[128];
    pf_output_event_v1_t full;
    std::memset(&full, 0, sizeof(full));
    CHECK(strategy_outputs_event_get(s, last, &full, sizeof(full)) == 0);
    for (std::size_t size_in : {std::size_t{8}, std::size_t{16}, std::size_t{72},
                                std::size_t{80}, std::size_t{96}}) {
        std::memset(buffer, 0xAB, sizeof(buffer));
        CHECK(strategy_outputs_event_get(s, last, buffer, size_in) == 0);
        const std::size_t copied = std::min(size_in, sizeof(pf_output_event_v1_t));
        CHECK(std::memcmp(buffer, &full, copied) == 0);
        bool untouched = true;
        for (std::size_t i = copied; i < sizeof(buffer); ++i) untouched = untouched && buffer[i] == 0xAB;
        CHECK(untouched);
    }
    std::memset(buffer, 0xAB, sizeof(buffer));
    CHECK(strategy_outputs_event_get(s, last, buffer, 7) == -1);
    CHECK(buffer[0] == 0xAB);
    CHECK(strategy_outputs_event_get(s, -1, buffer, 80) == -1);
    CHECK(strategy_outputs_event_get(s, last + 1, buffer, 80) == -1);
    CHECK(strategy_outputs_event_get(s, 0, nullptr, 80) == -1);
    CHECK(full.struct_version == 1u && full.size == sizeof(pf_output_event_v1_t));

    // The bar-time copy.
    std::int64_t open[8], close[8], written = -7;
    CHECK(strategy_outputs_bar_times_copy(s, 2, open, close, 3, &written) == 0 && written == 3);
    CHECK(open[0] == kT0 + 2 * kStep && close[2] == kT0 + 5 * kStep);
    CHECK(strategy_outputs_bar_times_copy(s, 6, open, nullptr, 8, &written) == 0 && written == 2);
    CHECK(open[1] == kT0 + 7 * kStep);
    CHECK(strategy_outputs_bar_times_copy(s, 0, nullptr, nullptr, 8, &written) == 0 && written == 8);
    CHECK(strategy_outputs_bar_times_copy(s, 8, open, close, 8, &written) == 0 && written == 0);
    written = -7;
    CHECK(strategy_outputs_bar_times_copy(s, 9, open, close, 8, &written) == -1 && written == -7);
    CHECK(strategy_outputs_bar_times_copy(s, -1, open, close, 8, &written) == -1);
    CHECK(strategy_outputs_bar_times_copy(s, 0, open, close, -1, &written) == -1);
    CHECK(strategy_outputs_bar_times_copy(s, 0, open, close, 8, nullptr) == -1);

    // The series copy.
    double values[8];
    for (double& v : values) v = -1.0;
    CHECK(strategy_outputs_series_copy(s, 1, 1, values, 3, &written) == 0 && written == 3);
    CHECK(std::isnan(values[1]) && values[3] == -1.0);
    CHECK(strategy_outputs_series_copy(s, 0, 5, values, 8, &written) == 0 && written == 3);
    CHECK(bits(values[0]) == bits(bars[5].close));
    CHECK(strategy_outputs_series_copy(s, 0, 0, nullptr, 0, &written) == 0 && written == 0);
    CHECK(strategy_outputs_series_copy(s, 0, 8, values, 8, &written) == 0 && written == 0);
    written = -7;
    CHECK(strategy_outputs_series_copy(s, 0, 9, values, 8, &written) == -1 && written == -7);
    CHECK(strategy_outputs_series_copy(s, -1, 0, values, 8, &written) == -1);
    CHECK(strategy_outputs_series_copy(s, 2, 0, values, 8, &written) == -1);
    CHECK(strategy_outputs_series_copy(s, 0, 0, nullptr, 1, &written) == -1);
    CHECK(strategy_outputs_series_copy(s, 0, 0, values, -1, &written) == -1);
    CHECK(strategy_outputs_series_copy(s, 0, 0, values, 8, nullptr) == -1);

    // The constants copy.
    double constants[2] = {-1.0, -1.0};
    CHECK(strategy_outputs_constants_copy(s, constants, 1) == 2);
    CHECK(constants[0] == 50.0 && constants[1] == -1.0);
    CHECK(strategy_outputs_constants_copy(s, constants, -1) == -1);
    CHECK(strategy_outputs_constants_copy(s, nullptr, 1) == -1);

    // A clear drops the queue and nothing else.
    strategy_outputs_events_clear(s);
    CHECK(strategy_outputs_events_len(s) == 0);
    CHECK(strategy_outputs_bars_len(s) == 8);
    const Record after = read_record(s);
    CHECK(same_values(after.series[0], r.series[0]) && same_values(after.series[1], r.series[1]));
    CHECK(after.constants.size() == 2 && after.constants[1] == 1286557951.0);

    // Turning recording off clears the record and answers as undeclared.
    CHECK(strategy_outputs_set_enabled(s, 0) == 0);
    CHECK(!host.recording());
    CHECK(strategy_outputs_series_count(s) == 0 && strategy_outputs_bars_len(s) == 0);
    CHECK(strategy_outputs_constants_copy(s, nullptr, 0) == 0);
}

// The row rules, the writers' preconditions and the run constants, called
// directly on a declared host outside any run.
void row_rules() {
    OutputsHost host({2, 2, 2});
    pf_strategy_t s = host.handle();
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);

    CHECK(throws_logic([&] { host.bar(kT0, kT0 + kStep); }, "before any output_run_begin"));
    CHECK(throws_logic([&] { host.constant(0, 1.0); }, "before any output_run_begin"));
    host.run_begin();
    host.run_begin();   // idempotent
    CHECK(throws_logic([&] { host.value(0, 1.0); }, "with no open bar"));
    CHECK(throws_logic([&] { host.event(0, 1.0); }, "with no open bar"));

    host.bar(kT0, kT0 + kStep);                       // append row 0
    host.value(0, 1.0);
    host.value(0, 2.0);                               // the last write wins
    CHECK(throws_logic([&] { host.value(2, 1.0); }, "series slot 2 is out of range"));
    CHECK(throws_logic([&] { host.value(-1, 1.0); }, "series slot -1 is out of range"));
    CHECK(throws_logic([&] { host.event(2, 1.0); }, "output 2 is out of range"));
    host.event(0, 1.0);                               // sequence 1
    host.bar(kT0 + kStep, kT0 + 2 * kStep);           // append row 1
    host.value(1, 3.0);
    host.event(0, 1.0);                               // sequence 2, ordinal 0
    host.event(0, 1.0);                               // sequence 3, ordinal 1
    host.event(1, 1.0, "x");                          // sequence 4, ordinal 0
    // A recalculation of row 1: its values, its close time, its ordinals and
    // its queued events start again; its sequences are issued again.
    host.bar(kT0 + kStep, kT0 + 2 * kStep + 1);
    Record r = read_record(s);
    CHECK(r.bars == 2);
    CHECK(r.series[0][0] == 2.0 && std::isnan(r.series[1][0]));
    CHECK(std::isnan(r.series[0][1]) && std::isnan(r.series[1][1]));
    CHECK(r.close_ms[1] == kT0 + 2 * kStep + 1);
    CHECK(r.events.size() == 1 && r.events[0].sequence == 1);
    host.event(0, 5.0);
    r = read_record(s);
    CHECK(r.events.size() == 2 && r.events[1].sequence == 2 && r.events[1].ordinal_in_bar == 0
          && r.events[1].value == 5.0 && r.events[1].bar_index == 1
          && r.events[1].bar_close_ms == kT0 + 2 * kStep + 1);
    CHECK(throws_logic([&] { host.bar(kT0, kT0 + kStep); },
                       "bar opened at 1704067200000 is before the last recorded bar"));
    CHECK(read_record(s).bars == 2);

    // A clear raises the cleared sequence; the sequence runs on.
    strategy_outputs_events_clear(s);
    host.bar(kT0 + 2 * kStep, kT0 + 3 * kStep);       // row 2: nothing of it was cleared
    host.bar(kT0 + 2 * kStep, kT0 + 3 * kStep);       // so it may restart
    host.event(1, 7.0, "after clear");
    r = read_record(s);
    CHECK(r.events.size() == 1 && r.events[0].sequence == 3 && r.events[0].bar_index == 2);
    // Row 2's event is cleared now: recalculating row 2 would retract it.
    strategy_outputs_events_clear(s);
    CHECK(throws_logic([&] { host.bar(kT0 + 2 * kStep, kT0 + 3 * kStep); },
                       "bar 2 was recalculated after its events were cleared"));
    host.bar(kT0 + 3 * kStep, kT0 + 4 * kStep);
    host.event(0, 1.0);
    r = read_record(s);
    CHECK(r.events.size() == 1 && r.events[0].sequence == 4 && r.events[0].bar_index == 3);

    // Run constants.
    Record c = read_record(s);
    CHECK(c.constants.size() == 2 && std::isnan(c.constants[0]) && std::isnan(c.constants[1]));
    host.constant(0, 50.0);
    host.constant(0, 50.0);                           // an equal rewrite
    CHECK(throws_logic([&] { host.constant(0, 50.5); }, "run constant 0 changed within a run"));
    CHECK(throws_logic([&] { host.constant(0, -50.0); }, "run constant 0 changed within a run"));
    host.constant(1, kNaN);
    host.constant(1, -kNaN);                          // NaN again: equal
    CHECK(throws_logic([&] { host.constant(1, 0.0); }, "run constant 1 changed within a run"));
    CHECK(throws_logic([&] { host.constant(2, 1.0); }, "run constant 2 is out of range"));
    CHECK(throws_logic([&] { host.constant(-1, 1.0); }, "run constant -1 is out of range"));
    c = read_record(s);
    CHECK(c.constants.size() == 2 && c.constants[0] == 50.0 && std::isnan(c.constants[1]));
    // A constant does not need an open row, and a row restart keeps it.
    host.bar(kT0 + 3 * kStep, kT0 + 4 * kStep);
    CHECK(read_record(s).constants[0] == 50.0);
    // A run begin resets them, and so does a flag change.
    host.run_begin();
    c = read_record(s);
    CHECK(c.bars == 0 && c.events.empty());
    CHECK(c.constants.size() == 2 && std::isnan(c.constants[0]));
    host.constant(0, 0.0);                            // unwritten again: any value stores
    CHECK(throws_logic([&] { host.constant(0, -0.0); }, "run constant 0 changed within a run"));
    CHECK(read_record(s).constants[0] == 0.0 && !std::signbit(read_record(s).constants[0]));
    CHECK(strategy_outputs_set_enabled(s, 0) == 0);
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    c = read_record(s);
    CHECK(c.constants.size() == 2 && std::isnan(c.constants[0]) && c.bars == 0);
    host.constant(0, 61.0);
    CHECK(read_record(s).constants[0] == 61.0);
    // A run begin restarts the sequence at 1.
    host.run_begin();
    host.bar(kT0, kT0 + kStep);
    host.event(0, 1.0);
    CHECK(read_record(s).events.size() == 1 && read_record(s).events[0].sequence == 1);

    // Recording off: every writer returns at its first branch.
    CHECK(strategy_outputs_set_enabled(s, 0) == 0);
    host.bar(kT0 - kStep, kT0);
    host.value(9, 1.0);
    host.event(9, 1.0, "ignored");
    host.constant(9, 1.0);
    CHECK(read_record(s).bars == 0 && strategy_outputs_events_len(s) == 0);

    // declare_outputs: non-negative counts, repeated only with the same counts.
    OutputsHost bare({0, 0, 0, false});
    CHECK(throws_logic([&] { bare.declare(-1, 0); }, "declared counts must not be negative"));
    CHECK(throws_logic([&] { bare.declare(0, 0, -1); }, "declared counts must not be negative"));
    bare.declare(3, 1, 2);
    bare.declare(3, 1, 2);
    CHECK(throws_logic([&] { bare.declare(3, 1); }, "repeated with other counts"));
    CHECK(throws_logic([&] { bare.declare(4, 1, 2); }, "repeated with other counts"));
    CHECK(strategy_outputs_set_enabled(bare.handle(), 1) == 0);
    CHECK(strategy_outputs_series_count(bare.handle()) == 3);
}

// Inside a run: a violation fails the run with its text; the switch is
// refused while the host is Running; a stream reads phases 1 and 2.
void in_run() {
    {
        OutputsHost host({1, 1, 0});
        pf_strategy_t s = host.handle();
        CHECK(host.configure_native(make_spec("outputs-below", 1)).status == NativeSetupStatus::Applied);
        CHECK(strategy_outputs_set_enabled(s, 1) == 0);
        host.script = [](OutputsHost& h, const Bar&, const NativeDecisionContext& c) {
            if (h.published == 3) h.bar(c.script_bar_open_ms - 2 * kStep, c.script_bar_open_ms);
        };
        const auto bars = make_bars(6, kStep);
        host.run(bars.data(), static_cast<int>(bars.size()));
        CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
        CHECK(last_error(s).find("is before the last recorded bar") != std::string::npos);
    }
    {
        OutputsHost host({1, 1, 0});
        pf_strategy_t s = host.handle();
        CHECK(host.configure_native(make_spec("outputs-switch", 1)).status == NativeSetupStatus::Applied);
        CHECK(strategy_outputs_set_enabled(s, 1) == 0);
        host.script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
            h.value(0, b.close);
            h.event(0, b.close);
        };
        const auto bars = make_bars(6, kStep);
        std::vector<pf_bar_t> c_bars;
        for (const Bar& b : bars) c_bars.push_back({b.open, b.high, b.low, b.close, b.volume, b.timestamp});
        CHECK(strategy_stream_begin(s, c_bars.data(), 3, "", "") == 0);
        // Between the inputs of a stream the host is Running: a change of the
        // switch is refused, a call that changes nothing is not.
        for (int i = 3; i <= 6; ++i) {
            CHECK(strategy_outputs_set_enabled(s, 0) == -1);
            CHECK(last_error(s) == "outputs: recording cannot change during a run");
            CHECK(strategy_outputs_set_enabled(s, 1) == 0);
            if (i < 6) CHECK(strategy_stream_push_bar(s, &c_bars[i]) == 0);
        }
        Record r = read_record(s);
        CHECK(r.bars == 6);
        CHECK(r.events.size() == 6);
        for (const auto& e : r.events)
            CHECK(e.phase == (e.bar_index < 3 ? PF_OUTPUT_PHASE_WARMUP : PF_OUTPUT_PHASE_REALTIME));
        CHECK(strategy_stream_end(s, 0) == 0);
        CHECK(strategy_outputs_set_enabled(s, 0) == 0);
        CHECK(strategy_outputs_bars_len(s) == 0);
    }
    {
        // Two calculations that state one open time, with no event cleared
        // between them, are one row: the second recalculates it. Every later
        // row's bar_index runs one short of its calculation's count. This
        // host states the open itself: calculation 3 repeats bar 2's.
        struct EqualStamp : OutputsHost {
            using OutputsHost::OutputsHost;
            void on_native_bar(const Bar& b, const NativeDecisionContext& c) override {
                const int k = calculations++;
                const std::int64_t open = k == 3 ? c.script_bar_open_ms - kStep : c.script_bar_open_ms;
                bar(open, open + kStep);
                value(0, b.close);
                event(0, static_cast<double>(k));
            }
        };
        EqualStamp merging({1, 1, 0});
        pf_strategy_t m = merging.handle();
        CHECK(merging.configure_native(make_spec("outputs-equal-stamp", 1)).status
              == NativeSetupStatus::Applied);
        CHECK(strategy_outputs_set_enabled(m, 1) == 0);
        const auto bars = make_bars(6, kStep);
        merging.run(bars.data(), 6);
        CHECK(last_error(m).empty());
        const Record r = read_record(m);
        CHECK(merging.calculations == 6);
        CHECK(r.bars == 5);
        CHECK(r.open_ms.size() == 5 && r.open_ms[2] == kT0 + 2 * kStep && r.open_ms[3] == kT0 + 4 * kStep);
        CHECK(r.series.size() == 1 && r.series[0].size() == 5 && bits(r.series[0][2]) == bits(bars[3].close));
        // Calculation 2's event was retracted with its row; calculation 3's
        // took its sequence. Calculations 4 and 5 are rows 3 and 4.
        CHECK(r.events.size() == 5);
        if (r.events.size() == 5) {
            CHECK(r.events[2].value == 3.0 && r.events[2].bar_index == 2 && r.events[2].sequence == 3);
            CHECK(r.events[3].value == 4.0 && r.events[3].bar_index == 3);
            CHECK(r.events[4].value == 5.0 && r.events[4].bar_index == 4 && r.events[4].sequence == 5);
        }
    }
}

}  // namespace

int main() {
    undeclared_and_null_handles();
    hand_values();
    row_rules();
    in_run();
    return finish("test_outputs_recorder");
}
