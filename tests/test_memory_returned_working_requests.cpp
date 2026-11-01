// working_request_snapshot: the owning working-request read is a per-call buffer.
// Expectations below come from the submitted commands, never another read
// of the production book. Expected populations are derived independently
// from the admission and fill schedule.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#define PINEFORGE_TEST_ALLOCATION_CENSUS
#include "global_allocation_replacement.hpp"

#include <pineforge/native_host.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;
namespace ga = global_allocation;

std::size_t checks = 0;
std::size_t failures = 0;
const char* row_id = "working_request_snapshot";
int input_bars = 0;
std::size_t pending_count = 0;
int script_bar = 0;

void check(bool ok, const char* file, int line, const char* expression) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s (bars=%d pending=%zu bar=%d)\n",
                     row_id, file, line, expression, input_bars, pending_count, script_bar);
    }
}
#define MEM_CHECK(expression) check(bool(expression), __FILE__, __LINE__, #expression)

constexpr std::size_t kLargestPopulation = 11;
constexpr int kBars = 32;

struct ExpectedRequest {
    no::RequestHandle handle{};
    bool live = false;
    bool fixed_units = false;
    double units = 0.0;
    bool resting_limit = false;
};

struct Snapshot {
    std::size_t count = 0;
    std::size_t capacity = 0;
};

class WorkingHost final : public NativeStrategyHost {
public:
    explicit WorkingHost(std::size_t population) : population_(population) {}

    int bars_seen = 0;
    std::size_t applied_callbacks = 0;

    Snapshot snapshot(std::size_t expected_count) const {
        // One process-wide recording window, with no counter resets and no
        // warm-up read: even the FIRST returned buffer must release its heap.
        const auto bytes_before = ga::live_bytes;
        const auto blocks_before = ga::live_blocks;
        const auto allocations_before = ga::recorded_allocations;
        const auto releases_before = ga::recorded_releases;
        Snapshot observed;
        {
            const auto working = native_working_requests();
            observed = {working.size(), working.capacity()};
            MEM_CHECK(working.size() == expected_count); // working_request_snapshot count

            // The array has capacity objects, each sizeof its actual element
            // type. Definitions are shared owners already created by submit;
            // these resident-book rows copy scalar variants, not nested arrays.
            // Do not assume a particular std::vector capacity growth factor.
            const auto array_bytes = working.capacity() * sizeof(NativeWorkingRequest);
            MEM_CHECK(ga::live_bytes == bytes_before + array_bytes);
            MEM_CHECK(ga::live_blocks == blocks_before + (working.capacity() != 0 ? 1u : 0u));
            MEM_CHECK(expected_count == 0 || working.capacity() != 0);

            // Independently retain the script's accepted identities and
            // commanded liveness in a fixed array. Check every live identity,
            // in insertion order, as well as each known remaining quantity.
            std::size_t at = 0;
            for (const auto& expected : reference_) {
                if (!expected.live) continue;
                if (at < working.size()) {
                    const auto& actual = working[at];
                    MEM_CHECK(bool(actual.definition));
                    if (actual.definition) {
                        MEM_CHECK(actual.definition->handle == expected.handle);
                    }
                    if (expected.fixed_units) {
                        const auto* units = std::get_if<no::RemainingProjectionUnits>(
                            &actual.remaining);
                        MEM_CHECK(units != nullptr && units->q == expected.units);
                    }
                    if (expected.resting_limit) {
                        MEM_CHECK(std::holds_alternative<no::LimitReady>(actual.trigger_state));
                    }
                }
                ++at;
            }
            MEM_CHECK(at == expected_count);
        }
        // working_request_snapshot observable release: no array or copy may survive the
        // caller's scope. These equalities are checked on every script bar.
        MEM_CHECK(ga::live_bytes == bytes_before);
        MEM_CHECK(ga::live_blocks == blocks_before);
        MEM_CHECK(ga::recorded_allocations - allocations_before
                 == ga::recorded_releases - releases_before);
        return observed;
    }

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        script_bar = ++bars_seen;
        if (bars_seen == 1) {
            snapshot(0);
            for (std::size_t i = 0; i < population_; ++i) {
                // All prints are 100. Buy limits 25..35 cannot match, so
                // every requested quantity i+1 remains outstanding.
                no::Request pending{no::Transact{double(i + 1)}, "read-pending", ""};
                pending.trigger = no::Limit{25.0 + double(i)};
                submit_expected(i, pending, true, double(i + 1), true);
            }
            snapshot(population_);
            entry_and_close();
        } else if (bars_seen == 2) {
            // Remove indices 0,2,...: ceil(population/2) cancellations,
            // leaving floor(population/2) never-filled orders.
            for (std::size_t i = 0; i < population_; i += 2) {
                const auto result = cancel(reference_[i].handle);
                MEM_CHECK(result.status == no::CancelStatus::Cancelled);
                reference_[i].live = false;
            }
            snapshot(population_ / 2);
        } else {
            snapshot(population_ / 2);
        }
    }

    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        ++applied_callbacks;
    }

private:
    void submit_expected(std::size_t slot, const no::Request& request,
                         bool fixed_units, double units, bool resting_limit) {
        const auto result = submit(request);
        MEM_CHECK(result.status == no::SubmitStatus::Accepted && result.handle.has_value());
        auto& expected = reference_[slot];
        if (result.handle) expected.handle = *result.handle;
        // The model follows the SCRIPT, even when a command fails; the check
        // above records the failure and later size/identity checks continue.
        expected.live = true;
        expected.fixed_units = fixed_units;
        expected.units = units;
        expected.resting_limit = resting_limit;
    }

    void entry_and_close() {
        // Two extra slots, one for a two-unit entry and one for its Flatten.
        // Observe both before execution and after their terminal receipts.
        const std::size_t entry = population_;
        const std::size_t close = population_ + 1;
        submit_expected(entry, no::Request{no::Transact{2.0}, "read-entry", ""},
                        true, 2.0, false);
        snapshot(population_ + 1);
        {
            const auto result = execute_current(NativeCurrentExecution{reference_[entry].handle});
            const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result);
            MEM_CHECK(applied != nullptr);
            if (applied) {
                MEM_CHECK(applied->terminal && applied->opened_units == 2.0);
                MEM_CHECK(applied->closed_units == 0.0);
            }
        }
        reference_[entry].live = false;
        MEM_CHECK(physical_position().signed_units == 2.0);
        snapshot(population_);

        submit_expected(close, no::Request{no::Flatten{}, "read-close", ""},
                        false, 0.0, false);
        snapshot(population_ + 1);
        {
            const auto result = execute_current(NativeCurrentExecution{reference_[close].handle});
            const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&result);
            MEM_CHECK(applied != nullptr);
            if (applied) {
                MEM_CHECK(applied->terminal && applied->closed_units == 2.0);
                MEM_CHECK(applied->opened_units == 0.0);
            }
        }
        reference_[close].live = false;
        MEM_CHECK(physical_position().signed_units == 0.0);
        snapshot(population_);
    }

    const std::size_t population_;
    std::array<ExpectedRequest, kLargestPopulation + 2> reference_{};
};

NativeRunSpec make_spec() {
    NativeRunSpec spec;
    spec.identity = {"requests", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:REQUESTS";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 1000000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.25;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    return spec;
}

struct Outcome {
    bool completed = false;
    Snapshot final{};
};

Outcome drive(int count, std::size_t population) {
    input_bars = count;
    pending_count = population;
    script_bar = 0;
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        bars.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                        1736121600000LL + std::int64_t(i) * 60000});
    }
    WorkingHost host(population);
    const auto setup = host.configure_native(make_spec());
    MEM_CHECK(setup.status == NativeSetupStatus::Applied);
    if (setup.status != NativeSetupStatus::Applied) return {};
    host.run(bars.data(), count);
    MEM_CHECK(host.last_error().empty());
    const bool completed = host.native_state().kind == NativeLifecycleKind::Completed;
    MEM_CHECK(completed);
    MEM_CHECK(host.bars_seen == count);
    MEM_CHECK(host.applied_callbacks == 2);
    MEM_CHECK(host.physical_position().signed_units == 0.0);
    return {completed, host.snapshot(population / 2)};
}

void working_requests_row() {
    for (const std::size_t population : std::array<std::size_t, 2>{3, 11}) {
        std::array<Outcome, 2> outcomes{};
        const std::array<int, 2> lengths{kBars, 4 * kBars};
        for (std::size_t i = 0; i < lengths.size(); ++i) {
            try {
                outcomes[i] = drive(lengths[i], population);
            } catch (const std::exception& error) {
                MEM_CHECK(false && "scenario threw; continue to next parameter");
                std::fprintf(stderr, "DETAIL [%s] %s:%d: exception: %s\n",
                             row_id, __FILE__, __LINE__, error.what());
            } catch (...) {
                MEM_CHECK(false && "scenario threw an unknown exception");
            }
        }
        MEM_CHECK(outcomes[0].completed && outcomes[1].completed);
        // working_request_snapshot fixed-as-bars-grow: the only survivors are the odd
        // indices, i.e. floor(population/2) = 1 or 5, at both 32 and 128 bars.
        MEM_CHECK(outcomes[0].final.count == population / 2);
        MEM_CHECK(outcomes[1].final.count == population / 2);
        MEM_CHECK(outcomes[0].final.count == outcomes[1].final.count);
        MEM_CHECK(outcomes[0].final.capacity == outcomes[1].final.capacity);
    }
}

struct Row {
    const char* id;
    void (*run)();
};
} // namespace

int main() {
    const Row rows[] = {{"working_request_snapshot", working_requests_row}};
    ga::recording = true;
    for (const auto& row : rows) {
        row_id = row.id;
        try {
            row.run();
        } catch (const std::exception& error) {
            MEM_CHECK(false && "row threw; continue to next row");
            std::fprintf(stderr, "DETAIL [%s] %s:%d: exception: %s\n",
                         row_id, __FILE__, __LINE__, error.what());
        } catch (...) {
            MEM_CHECK(false && "row threw an unknown exception");
        }
    }
    ga::recording = false;
    std::printf("memory proof returned_working_requests: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
