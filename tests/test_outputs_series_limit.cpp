// The recorded-series boundary through a real host and the C output readers.
// The literal 64 is Pine's plot-count limit, independent of engine constants.
// Source-free: this TU also runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <pineforge/run_failure.hpp>

#include <climits>
#include <stdexcept>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

Record record_64_series(OutputsHost& host) {
    const pf_strategy_t s = host.handle();
    CHECK(host.configure_native(make_spec("outputs-series-limit", 1)).status
          == NativeSetupStatus::Applied);
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    CHECK(strategy_outputs_series_count(s) == 64);
    host.script = [](OutputsHost& h, const Bar& bar, const NativeDecisionContext&) {
        for (int slot = 0; slot < 64; ++slot) h.value(slot, bar.close + slot);
        h.event(1, bar.close);
        h.constant(0, 17.0);
    };
    const auto bars = make_bars(3, kStep);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(strategy_last_run_status(s) == 0);
    CHECK(std::string(strategy_get_last_error(s)).empty());
    CHECK(std::string(strategy_get_last_error_code(s)).empty());
    const Record record = read_record(s);
    CHECK(record.series_count == 64);
    CHECK(record.bars == 3);
    CHECK(record.series.size() == 64);
    CHECK(record.open_ms.size() == 3 && record.close_ms.size() == 3);
    if (record.open_ms.size() == 3 && record.close_ms.size() == 3) {
        for (std::size_t row = 0; row < bars.size(); ++row) {
            CHECK(record.open_ms[row] == bars[row].timestamp);
            CHECK(record.close_ms[row] == bars[row].timestamp + kStep);
        }
    }
    if (record.series.size() == 64) {
        for (int slot = 0; slot < 64; ++slot) {
            CHECK(record.series[slot].size() == 3);
            if (record.series[slot].size() != 3) continue;
            for (std::size_t row = 0; row < bars.size(); ++row)
                CHECK(record.series[slot][row] == bars[row].close + slot);
        }
    }
    CHECK(record.events.size() == 3);
    if (record.events.size() == 3) {
        for (std::size_t row = 0; row < bars.size(); ++row) {
            CHECK(record.events[row].output_index == 1);
            CHECK(record.events[row].bar_index == static_cast<int>(row));
            CHECK(record.events[row].sequence == row + 1);
            CHECK(record.events[row].value == bars[row].close);
        }
    }
    CHECK(record.constants.size() == 1 && record.constants[0] == 17.0);
    return record;
}

void refuses_series(OutputsHost& host, int slots) {
    bool thrown = false;
    try {
        host.declare(slots, 7, 3);
    } catch (const std::runtime_error& error) {
        thrown = true;
        const RunFailureValue value = classify_run_failure(error);
        CHECK(value.code == RunFailureCode::outputs_limit);
        CHECK(value.args && *value.args == R"({"max":64,"reason":"too_many_series"})");
        CHECK(std::string(error.what()) == "outputs: the series count is at its limit");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected declaration exception: %s\n", error.what());
        CHECK(false);
    }
    CHECK(thrown);
}

void exactly_64_series() {
    OutputsHost host({64, 2, 1});
    CHECK(host.outputs_declared());
    record_64_series(host);
}

void oversized_declaration_then_valid_declaration() {
    OutputsHost host({0, 0, 0, false});
    const pf_strategy_t s = host.handle();
    for (int slots : {65, INT_MAX}) {
        refuses_series(host, slots);
        CHECK(!host.outputs_declared());
        CHECK(!host.recording());
        CHECK(strategy_outputs_series_count(s) == 0);
        CHECK(strategy_outputs_bars_len(s) == 0);
        CHECK(strategy_outputs_events_len(s) == 0);
        CHECK(host.output_constants_count() == 0);
        CHECK(strategy_last_run_status(s) == 0);
        CHECK(std::string(strategy_get_last_error(s)).empty());
        CHECK(std::string(strategy_get_last_error_code(s)).empty());
        CHECK(std::string(strategy_get_last_error_args(s)).empty());
    }
    // Different event/constant counts expose a refused declaration that
    // latched its shape; an identical valid redeclaration remains idempotent.
    host.declare(64, 2, 1);
    host.declare(64, 2, 1);
    CHECK(host.outputs_declared());
    record_64_series(host);
}

void oversized_redeclaration_keeps_record() {
    OutputsHost host({64, 2, 1});
    const Record before = record_64_series(host);
    refuses_series(host, 65);
    CHECK(host.outputs_declared());
    CHECK(host.recording());
    host.declare(64, 2, 1);
    const Record after = read_record(host.handle());
    CHECK(after.series_count == before.series_count);
    CHECK(after.bars == before.bars);
    CHECK(after.open_ms == before.open_ms);
    CHECK(after.close_ms == before.close_ms);
    CHECK(after.series == before.series);
    CHECK(after.constants == before.constants);
    CHECK(after.events.size() == before.events.size());
    if (after.events.size() == before.events.size()) {
        for (std::size_t i = 0; i < after.events.size(); ++i)
            CHECK(same_event(after.events[i], before.events[i], true));
    }
    CHECK(strategy_last_run_status(host.handle()) == 0);
    CHECK(std::string(strategy_get_last_error_code(host.handle())).empty());
}

}  // namespace

int main() {
    exactly_64_series();
    oversized_declaration_then_valid_declaration();
    oversized_redeclaration_keeps_record();
    return finish("test_outputs_series_limit");
}
