// Operator series limits through a real host and the C output readers.
// Source-free: this TU also runs in the kernel-only profile.
#include "outputs_test_support.hpp"

#include <pineforge/run_failure.hpp>

#include <climits>
#include <dlfcn.h>
#include <stdexcept>

using namespace outputs_test;

namespace {

constexpr std::int64_t kStep = 5 * kMinute;

Record record_series(OutputsHost& host, int slots) {
    const pf_strategy_t s = host.handle();
    CHECK(host.configure_native(make_spec("outputs-series-limit", 1)).status
          == NativeSetupStatus::Applied);
    CHECK(strategy_outputs_set_enabled(s, 1) == 0);
    CHECK(strategy_outputs_series_count(s) == slots);
    host.script = [slots](OutputsHost& h, const Bar& bar, const NativeDecisionContext&) {
        for (int slot = 0; slot < slots; ++slot) h.value(slot, bar.close + slot);
        h.event(1, bar.close);
        h.constant(0, 17.0);
    };
    const auto bars = make_bars(3, kStep);
    host.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(strategy_last_run_status(s) == 0);
    CHECK(std::string(strategy_get_last_error(s)).empty());
    CHECK(std::string(strategy_get_last_error_code(s)).empty());
    const Record record = read_record(s);
    CHECK(record.series_count == slots);
    CHECK(record.bars == 3);
    CHECK(record.series.size() == static_cast<std::size_t>(slots));
    CHECK(record.open_ms.size() == 3 && record.close_ms.size() == 3);
    if (record.open_ms.size() == 3 && record.close_ms.size() == 3) {
        for (std::size_t row = 0; row < bars.size(); ++row) {
            CHECK(record.open_ms[row] == bars[row].timestamp);
            CHECK(record.close_ms[row] == bars[row].timestamp + kStep);
        }
    }
    if (record.series.size() == static_cast<std::size_t>(slots)) {
        for (int slot = 0; slot < slots; ++slot) {
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

void refuses_series(OutputsHost& host, int slots, const char* args) {
    bool thrown = false;
    try {
        host.declare(slots, 7, 3);
    } catch (const std::runtime_error& error) {
        thrown = true;
        const RunFailureValue value = classify_run_failure(error);
        CHECK(value.code == RunFailureCode::outputs_limit);
        CHECK(value.args && *value.args == args);
        CHECK(std::string(error.what()) == "outputs: the series count is at its limit");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "unexpected declaration exception: %s\n", error.what());
        CHECK(false);
    }
    CHECK(thrown);
}

void unlimited_records_are_readable() {
    for (int slots : {65, 1000}) {
        try {
            OutputsHost host({slots, 2, 1});
            CHECK(host.outputs_declared());
            record_series(host, slots);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "default declaration of %d series failed: %s\n",
                         slots, error.what());
            CHECK(false);
        }
    }
}

using SetLimit = int (*)(const std::int64_t*, char*, std::size_t);
using ReadFailure = int (*)(std::int64_t*);

void operator_limits_are_checked_and_atomic() {
    // Resolve additive runtime exports after the default declaration proof:
    // the previous engine still compiles this test and fails on its actual
    // 65-series refusal, rather than on a missing C++ declaration.
    const auto set_limit = reinterpret_cast<SetLimit>(
        dlsym(RTLD_DEFAULT, "pf_output_series_limit_set_checked"));
    const auto read_failure = reinterpret_cast<ReadFailure>(
        dlsym(RTLD_DEFAULT, "pf_output_series_limit_failure"));
    CHECK(set_limit && read_failure);
    if (!set_limit || !read_failure) return;
    char error[256]{};
    std::int64_t limit = 64;
    CHECK(set_limit(&limit, error, sizeof(error)) == PF_SETTINGS_OK);
    CHECK(std::string(error).empty());
    OutputsHost host({0, 0, 0, false});
    const pf_strategy_t s = host.handle();
    for (int slots : {65, INT_MAX}) {
        refuses_series(host, slots, R"({"max":64,"reason":"too_many_series"})");
        std::int64_t refused_max = -1;
        CHECK(read_failure(&refused_max) == 1 && refused_max == 64);
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
    const Record before = record_series(host, 64);
    refuses_series(host, 65, R"({"max":64,"reason":"too_many_series"})");
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

    // A refused setter preserves the accepted default. A newly constructed
    // host must still accept 64 and refuse 65 with that previous value.
    limit = -1;
    CHECK(set_limit(&limit, error, sizeof(error)) == PF_SETTINGS_INVALID_ARGUMENT);
    CHECK(std::string(error) == "max output series must not be negative");
    OutputsHost unchanged({64, 2, 1});
    refuses_series(unchanged, 65, R"({"max":64,"reason":"too_many_series"})");

    limit = 10;
    CHECK(set_limit(&limit, error, sizeof(error)) == PF_SETTINGS_OK);
    std::int64_t refused_max = -1;
    CHECK(read_failure(&refused_max) == 0 && refused_max == -1);
    OutputsHost ten({0, 0, 0, false});
    refuses_series(ten, 11, R"({"max":10,"reason":"too_many_series"})");
    CHECK(!ten.outputs_declared());
    ten.declare(10, 2, 1);
    record_series(ten, 10);

    // The run option is snapshotted before the constructor's declaration.
    // Changing it for the next host cannot change an existing host's limit.
    refuses_series(host, 65, R"({"max":64,"reason":"too_many_series"})");
    limit = 1;
    CHECK(set_limit(&limit, error, sizeof(error)) == PF_SETTINGS_OK);
    OutputsHost one({0, 0, 0, false});
    refuses_series(one, 2, R"({"max":1,"reason":"too_many_series"})");
    refused_max = -1;
    CHECK(read_failure(&refused_max) == 1 && refused_max == 1);

    limit = 0;
    CHECK(set_limit(&limit, error, sizeof(error)) == PF_SETTINGS_OK);
    refused_max = -1;
    CHECK(read_failure(&refused_max) == 0 && refused_max == -1);
    OutputsHost zero({0, 0, 0, false});
    zero.declare(65, 2, 1);
    CHECK(zero.outputs_declared());
    record_series(zero, 65);

    CHECK(set_limit(nullptr, error, sizeof(error)) == PF_SETTINGS_OK);
    CHECK(read_failure(&refused_max) == 0);
    unlimited_records_are_readable();
}

}  // namespace

int main() {
    unlimited_records_are_readable();
    operator_limits_are_checked_and_atomic();
    return finish("test_outputs_series_limit");
}
