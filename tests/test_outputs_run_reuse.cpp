// The run boundary on a reused handle. One handle runs a long batch, a short
// batch, a zero-bar batch, a batch with recording switched off and on again,
// and finally a batch whose callback throws; after each completed run it
// records exactly what a fresh handle records over the same bars (the
// sequence starts at 1 again, no row of an earlier run survives). A failed
// run leaves its last error set, which is the caller's signal to read
// nothing.
//
// A C++ host must state the run boundary itself (output_run_begin(), in
// on_native_run_begin). One that never states it fails its first recorded
// bar; one that states it only on its first run appends a later run to the
// old record when the later bars open after it, and fails when they do not.
//
// Source-free: this TU runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <memory>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

void install(OutputsHost& host) {
    host.script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        h.value(0, b.close);
        if (b.close > b.open) h.event(0, b.high, "up");
        h.constant(0, b.open > 0.0 ? 7.0 : 0.0);
    };
}

std::unique_ptr<OutputsHost> make_host() {
    auto host = std::make_unique<OutputsHost>(OutputsHost::Shape{1, 1, 1});
    install(*host);
    return host;
}

std::string error_of(pf_strategy_t s) {
    const char* text = strategy_get_last_error(s);
    return text ? text : "";
}

bool same_record(const Record& a, const Record& b) {
    if (a.bars != b.bars || a.series_count != b.series_count) return false;
    if (a.open_ms != b.open_ms || a.close_ms != b.close_ms) return false;
    for (std::size_t i = 0; i < a.series.size() && i < b.series.size(); ++i)
        if (!same_values(a.series[i], b.series[i])) return false;
    return a.series.size() == b.series.size() && same_events(a.events, b.events, true)
        && same_values(a.constants, b.constants);
}

// What a fresh handle records over `bars`.
Record fresh(const std::vector<Bar>& bars, int n) {
    auto host = make_host();
    pf_strategy_t s = host->handle();
    CHECK(host->configure_native(make_spec("outputs-fresh", 1)).status == NativeSetupStatus::Applied);
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    host->run(bars.data(), n);
    CHECK(error_of(s).empty());
    return read_record(s);
}

void reused_handle() {
    const auto long_bars = make_bars(20, kStep);
    const auto short_bars = make_bars(6, kStep, kT0 + 100 * kStep, 250.0);
    auto host = make_host();
    pf_strategy_t s = host->handle();
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    std::uint64_t run = 0;
    auto configure = [&] {
        return host->configure_native(make_spec("outputs-reuse", ++run)).status
            == NativeSetupStatus::Applied;
    };

    // A long run, then a short one.
    CHECK(configure());
    host->run(long_bars.data(), static_cast<int>(long_bars.size()));
    CHECK(error_of(s).empty());
    const Record first = read_record(s);
    CHECK(first.bars == 20);
    CHECK(same_record(first, fresh(long_bars, 20)));
    CHECK(configure());
    host->run(short_bars.data(), static_cast<int>(short_bars.size()));
    CHECK(error_of(s).empty());
    const Record second = read_record(s);
    CHECK(second.bars == 6);
    CHECK(!second.events.empty() && second.events.front().sequence == 1);
    CHECK(same_record(second, fresh(short_bars, 6)));

    // A zero-bar run completes; its begin cleared the record, so nothing of
    // the run before it survives, as on a fresh handle.
    CHECK(configure());
    host->run(long_bars.data(), 0);
    CHECK(error_of(s).empty());
    CHECK(host->native_state().kind == NativeLifecycleKind::Completed);
    const Record zero = read_record(s);
    CHECK(zero.bars == 0 && zero.events.empty());
    CHECK(zero.constants.size() == 1 && std::isnan(zero.constants[0]));
    CHECK(same_record(zero, fresh(long_bars, 0)));
}

void off_and_on() {
    const auto bars = make_bars(9, kStep);
    auto host = make_host();
    pf_strategy_t s = host->handle();
    std::uint64_t run = 0;
    auto configure = [&] {
        return host->configure_native(make_spec("outputs-off-on", ++run)).status
            == NativeSetupStatus::Applied;
    };
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    CHECK(configure());
    host->run(bars.data(), 9);
    CHECK(read_record(s).bars == 9);
    // Off between runs: the record is cleared and nothing is recorded.
    CHECK(strategy_outputs_set_enabled(s, 0) == 0);
    CHECK(read_record(s).bars == 0);
    CHECK(configure());
    host->run(bars.data(), 9);
    CHECK(error_of(s).empty());
    const Record off = read_record(s);
    CHECK(off.bars == 0 && off.events.empty() && off.series_count == 0 && off.constants.empty());
    CHECK(host->published == 9);
    // On again: the next run records what a fresh handle records.
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    CHECK(configure());
    host->run(bars.data(), 9);
    CHECK(error_of(s).empty());
    CHECK(same_record(read_record(s), fresh(bars, 9)));

    // A run whose callback throws on its fourth bar: the run fails and keeps
    // its partial record; the caller, seeing the last error, reads nothing.
    host->script = [](OutputsHost& h, const Bar& b, const NativeDecisionContext&) {
        h.value(0, b.close);
        if (h.published == 4) throw std::runtime_error("host failure on bar 4");
    };
    CHECK(configure());
    host->run(bars.data(), 9);
    CHECK(host->native_state().kind == NativeLifecycleKind::Failed);
    CHECK(error_of(s) == "host failure on bar 4");
    const Record failed_run = read_record(s);
    CHECK(failed_run.bars == 4);
    CHECK(failed_run.events.empty());
}

// Q20: the host states the run boundary.
void host_without_run_begin() {
    const auto bars = make_bars(5, kStep);
    {
        auto host = make_host();
        host->begin_run = false;
        pf_strategy_t s = host->handle();
        CHECK(host->configure_native(make_spec("outputs-no-begin", 1)).status == NativeSetupStatus::Applied);
        CHECK(strategy_outputs_set_enabled(s, 1) == 0);
        host->run(bars.data(), 5);
        CHECK(host->native_state().kind == NativeLifecycleKind::Failed);
        CHECK(error_of(s).find("before any output_run_begin") != std::string::npos);
        CHECK(read_record(s).bars == 0);
    }
    {
        // output_run_begin on the first run only.
        auto host = make_host();
        pf_strategy_t s = host->handle();
        CHECK(strategy_outputs_set_enabled(s, 1) == 0);
        CHECK(host->configure_native(make_spec("outputs-once", 1)).status == NativeSetupStatus::Applied);
        host->run(bars.data(), 5);
        CHECK(read_record(s).bars == 5);
        host->begin_run = false;
        const auto later = make_bars(3, kStep, kT0 + 10 * kStep);
        CHECK(host->configure_native(make_spec("outputs-once", 2)).status == NativeSetupStatus::Applied);
        host->run(later.data(), 3);
        CHECK(error_of(s).empty());
        const Record appended = read_record(s);
        CHECK(appended.bars == 8);
        CHECK(appended.open_ms.size() == 8 && appended.open_ms[5] == kT0 + 10 * kStep);
        // The same bars again open below the last row: the run fails.
        CHECK(host->configure_native(make_spec("outputs-once", 3)).status == NativeSetupStatus::Applied);
        host->run(bars.data(), 5);
        CHECK(host->native_state().kind == NativeLifecycleKind::Failed);
        CHECK(error_of(s).find("is before the last recorded bar") != std::string::npos);
    }
}

}  // namespace

int main() {
    reused_handle();
    off_and_on();
    host_without_run_begin();
    return finish("test_outputs_run_reuse");
}
