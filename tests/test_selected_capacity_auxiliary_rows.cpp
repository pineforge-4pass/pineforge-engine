// Source preparation for the separated driver, before the auxiliary-row guard.
// The test-local row callback counts validated events but enforces no limit.
// After an executed bound RED, bind the auxiliary slot to the real owner; keep
// the inputs and assertions unchanged. Auxiliary never reaches the sink.
#include "../src/selected_capacity_driver.hpp"
#include "selected_capacity_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using pineforge::RunFailureClass;
using pineforge::RunFailureValue;
using pineforge::run_failure_code_class;
using pineforge::run_failure_code_name;
using pineforge::run_failure_code_retryable;
namespace sc = pineforge::detail::selected_capacity;
using sc::Role;

int checks = 0;
int failures = 0;
const char* scenario = "not started";
const char* actual_outcome = "not run";

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                          \
            std::fprintf(stderr,                                                \
                         "CHECK FAIL test=selected_auxiliary_rows scenario=%s "  \
                         "%s:%d %s actual=%s\n", scenario, __FILE__, __LINE__,   \
                         #condition, actual_outcome);                            \
        }                                                                       \
    } while (false)

// Each stream has different values and timestamps.  Auxiliary rows are split
// across two streams so the expected five-row total is independently derived
// from both actual files, rather than read from a receipt or a declared total.
constexpr const char* kSourceCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,100,101,99,100.5,10\n"
    "1704067260000,100.5,101.5,99.5,101,11\n"
    "1704067320000,101,102,100,101.5,12\n"
    "1704067380000,101.5,102.5,100.5,102,13\n";

constexpr const char* kPrimaryCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,200,201,199,200.5,20\n"
    "1704067260000,200.5,201.5,199.5,201,21\n";

constexpr const char* kAuxiliaryACsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,300,301,299,300.5,30\n"
    "1704067260000,300.5,301.5,299.5,301,31\n";

constexpr const char* kAuxiliaryBCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067320000,400,401,399,400.5,40\n"
    "1704067380000,400.5,401.5,399.5,401,41\n"
    "1704067440000,401,402,400,401.5,42\n";

struct Observation {
    std::vector<pineforge::Bar> source;
    std::vector<pineforge::Bar> primary;
    std::vector<pineforge::Bar> auxiliary;
    std::vector<pineforge::Bar> delivered;
    std::uint64_t auxiliary_used_at_event = 0;
};

Observation* active_observation = nullptr;

// Source is a requested test-local control. This callback records every
// validated role before the Auxiliary callback and only charges Source State.
void record_events(sc::State& state, const sc::Limits&, const sc::RecordEvent& event) {
    if (active_observation == nullptr || event.bar == nullptr
        || event.timestamp_ms != event.bar->timestamp) {
        throw std::logic_error("invalid validated-event observation");
    }
    switch (event.role) {
    case Role::Source:
        active_observation->source.push_back(*event.bar);
        ++state.source_rows;
        break;
    case Role::Primary:
        active_observation->primary.push_back(*event.bar);
        break;
    case Role::Auxiliary:
        active_observation->auxiliary.push_back(*event.bar);
        active_observation->auxiliary_used_at_event = state.auxiliary_rows;
        break;
    }
}

void count_auxiliary_without_limit(sc::State& state, const sc::Limits&,
                                   const sc::RecordEvent& event) {
    if (event.role == Role::Auxiliary) ++state.auxiliary_rows;
}

void retain_primary(void* context, const pineforge::Bar& bar) {
    static_cast<Observation*>(context)->delivered.push_back(bar);
}

struct ObservationScope {
    explicit ObservationScope(Observation& observation) {
        active_observation = &observation;
    }
    ~ObservationScope() { active_observation = nullptr; }
};

pineforge::SelectedPlanRequest request() {
    pineforge::SelectedPlanRequest value{};
    value.start_ms = 1704067500000;      // 00:05 UTC
    value.end_ms = 1704067800000;        // 00:10 UTC
    value.fed_start_ms = 1704067200000;  // 00:00 UTC
    value.preroll_bars = 0;
    value.input_tf = "1";
    value.script_tf = "5";
    value.chart_timezone = "UTC";
    value.engine_timezone = "UTC";
    value.session = "24x7";
    return value;
}

struct ObservedRun {
    sc::DriverResult driver;
    Observation observation;
    bool refused = false;
    RunFailureValue failure;
    std::string english;
};

ObservedRun execute(sc::Bound auxiliary_bound,
                    const std::string& auxiliary_a_csv = kAuxiliaryACsv,
                    const std::string& auxiliary_b_csv = kAuxiliaryBCsv) {
    actual_outcome = "setup";
    ObservedRun result;
    std::istringstream source(kSourceCsv);
    std::istringstream primary(kPrimaryCsv);
    std::istringstream auxiliary_a(auxiliary_a_csv);
    std::istringstream auxiliary_b(auxiliary_b_csv);
    const sc::StreamInput inputs[] = {
        {Role::Source, source, "auxiliary-source.csv"},
        {Role::Primary, primary, "auxiliary-primary.csv"},
        {Role::Auxiliary, auxiliary_a, "auxiliary-a.csv"},
        {Role::Auxiliary, auxiliary_b, "auxiliary-b.csv"},
    };
    sc::Limits limits{};
    limits.source_rows = {true, 4};
    limits.auxiliary_rows = auxiliary_bound;
    sc::ObserverTable observers{};
    observers.source_rows = &record_events;
    if (limits.auxiliary_rows.present) {
        observers.auxiliary_rows = &count_auxiliary_without_limit;
    }
    ObservationScope scope(result.observation);
    try {
        result.driver = sc::run_driver(inputs, 4, request(), limits, observers,
                                       &retain_primary, &result.observation);
    } catch (const pineforge::coded<std::runtime_error>& error) {
        result.failure = error.run_failure();
        result.english = error.what();
        actual_outcome = run_failure_code_name(result.failure.code);
        std::fprintf(stderr,
                     "test=selected_auxiliary_rows scenario=%s %s:%d "
                     "actual=coded code=%s args=%s english=%s\n",
                     scenario, __FILE__, __LINE__, actual_outcome,
                     result.failure.args ? result.failure.args->c_str() : "{}",
                     error.what());
        if (std::strcmp(actual_outcome, "selected_auxiliary_rows_limit_exceeded") != 0) {
            throw std::runtime_error("unexpected coded failure is setup, not bound RED");
        }
        result.refused = true;
        return result;
    }
    std::fprintf(stderr,
                 "test=selected_auxiliary_rows scenario=%s %s:%d actual=returned "
                 "status=%d reason=%d source=%llu primary=%llu auxiliary=%llu message=%s\n",
                 scenario, __FILE__, __LINE__, static_cast<int>(result.driver.status),
                 static_cast<int>(result.driver.reason),
                 static_cast<unsigned long long>(result.driver.coverage.source_rows),
                 static_cast<unsigned long long>(result.driver.coverage.primary_rows),
                 static_cast<unsigned long long>(result.driver.coverage.auxiliary_rows),
                 result.driver.message.c_str());
    if (result.driver.status != sc::DriverStatus::Completed
        || result.driver.reason != sc::DriverReason::None) {
        throw std::runtime_error("driver admission/CSV/identity failure is setup, not bound RED");
    }
    actual_outcome = "Completed";
    return result;
}

void check_limit_failure(const ObservedRun& result, const char* args) {
    CHECK(result.refused);
    if (!result.refused) return;
    const RunFailureValue& failure = result.failure;
    CHECK(std::strcmp(run_failure_code_name(failure.code),
                      "selected_auxiliary_rows_limit_exceeded") == 0);
    CHECK(run_failure_code_class(failure.code) == RunFailureClass::resource);
    CHECK(!run_failure_code_retryable(failure.code));
    CHECK(failure.args != nullptr);
    if (failure.args != nullptr) CHECK(*failure.args == args);
    CHECK(result.english == "Selected auxiliary-row capacity exceeded.");
}

void check_primary_sink(const ObservedRun& result) {
    // This checks only Primary delivery. It is not an Auxiliary allocation count.
    CHECK(result.observation.delivered.size() == 2);
    for (std::size_t i = 0; i < result.observation.delivered.size(); ++i) {
        const pineforge::Bar& bar = result.observation.delivered[i];
        CHECK(bar.timestamp == 1704067200000 + static_cast<std::int64_t>(i) * 60000);
        CHECK(bar.open == 200.0 + 0.5 * i);
        CHECK(bar.high == 201.0 + 0.5 * i);
        CHECK(bar.low == 199.0 + 0.5 * i);
        CHECK(bar.close == 200.5 + 0.5 * i);
        CHECK(bar.volume == 20.0 + i);
    }
}

void check_auxiliary_inputs(const ObservedRun& result) {
    CHECK(result.observation.auxiliary.size() == 5);
    if (result.observation.auxiliary.size() != 5) return;
    // Independent witnesses from both streams; totals are never a receipt input.
    CHECK(result.observation.auxiliary[0].timestamp == 1704067200000);
    CHECK(result.observation.auxiliary[0].close == 300.5);
    CHECK(result.observation.auxiliary[1].timestamp == 1704067260000);
    CHECK(result.observation.auxiliary[1].volume == 31);
    CHECK(result.observation.auxiliary[2].timestamp == 1704067320000);
    CHECK(result.observation.auxiliary[2].close == 400.5);
    CHECK(result.observation.auxiliary[4].timestamp == 1704067440000);
    CHECK(result.observation.auxiliary[4].volume == 42);
}

void exact_limit_passes_and_aggregates_all_auxiliary_streams() {
    const ObservedRun result = execute({true, 5});
    CHECK(!result.refused);
    CHECK(result.driver.state.auxiliary_rows == 5);
    CHECK(result.driver.state.source_rows == 4);
    CHECK(result.driver.state.primary_rows == 0);  // no requested Primary slot
    CHECK(result.driver.coverage.source_rows == 4);
    CHECK(result.driver.coverage.primary_rows == 2);
    CHECK(result.driver.coverage.auxiliary_rows == 5);
    CHECK(result.driver.coverage.auxiliary_streams == 2);
    CHECK(result.driver.coverage.source_complete);
    CHECK(result.driver.coverage.primary_complete);
    CHECK(result.driver.coverage.auxiliary_complete);
    CHECK(result.observation.source.size() == 4);
    CHECK(result.observation.primary.size() == 2);
    check_auxiliary_inputs(result);
    check_primary_sink(result);
}

void aggregate_limit_plus_one_refuses_at_auxiliary_charge() {
    const ObservedRun result = execute({true, 4});
    check_limit_failure(result, "{\"limit\":\"4\",\"requested\":\"1\",\"used\":\"4\"}");
    if (!result.refused) return;
    check_auxiliary_inputs(result);  // attempted fifth row is the third in stream B
    CHECK(result.observation.auxiliary_used_at_event == 4);
    CHECK(result.observation.source.size() == 4);
    CHECK(result.observation.primary.size() == 2);
    check_primary_sink(result);
}

void absent_auxiliary_limit_is_no_limit() {
    for (const std::uint64_t ignored_value : {std::uint64_t{0}, std::uint64_t{1}}) {
        const ObservedRun result = execute({false, ignored_value});
        CHECK(!result.refused);
        CHECK(result.driver.state.auxiliary_rows == 0);  // absent slot never runs
        CHECK(result.driver.state.source_rows == 4);     // requested control is active
        CHECK(result.driver.coverage.auxiliary_rows == 5);
        CHECK(result.driver.coverage.auxiliary_streams == 2);
        CHECK(result.driver.coverage.auxiliary_complete);
        check_auxiliary_inputs(result);
        check_primary_sink(result);
        if (failures != 0) return;
    }
}

void unrelated_roles_do_not_charge_auxiliary() {
    const std::string one_row = selected_capacity_fixture::make_csv({
        selected_capacity_fixture::make_record(1704067200000, 310, 311, 309, 310.5, 35),
    });
    const ObservedRun result = execute({true, 1}, one_row,
                                      selected_capacity_fixture::kCsvHeader);
    CHECK(!result.refused);
    CHECK(result.driver.state.auxiliary_rows == 1);
    CHECK(result.driver.coverage.source_rows == 4);
    CHECK(result.driver.coverage.primary_rows == 2);
    CHECK(result.driver.coverage.auxiliary_rows == 1);
    CHECK(result.driver.coverage.auxiliary_streams == 2);
    CHECK(result.observation.auxiliary.size() == 1);
    check_primary_sink(result);
}

void warmup_and_partial_auxiliary_rows_are_counted() {
    const std::string partial_b = selected_capacity_fixture::make_csv({
        selected_capacity_fixture::make_record(1704067320000, 410, 411, 409, 410.5, 45),
        selected_capacity_fixture::make_record(1704067380000, 411, 412, 410, 411.5, 46),
        selected_capacity_fixture::make_record(1704067560000, 412, 413, 411, 412.5, 47),
    });
    const ObservedRun result = execute({true, 5}, kAuxiliaryACsv, partial_b);
    CHECK(!result.refused);
    CHECK(result.driver.state.auxiliary_rows == 5);
    CHECK(result.driver.coverage.auxiliary_rows == 5);
    CHECK(result.driver.coverage.auxiliary_streams == 2);
    CHECK(result.observation.auxiliary.size() == 5);
    if (result.observation.auxiliary.size() == 5) {
        CHECK(result.observation.auxiliary[0].timestamp < request().start_ms);
        CHECK(result.observation.auxiliary[3].timestamp < request().start_ms);
        CHECK(result.observation.auxiliary[4].timestamp == 1704067560000);
        CHECK(result.observation.auxiliary[4].close == 412.5);
        CHECK(result.observation.auxiliary[4].volume == 47);
    }
    check_primary_sink(result);
}

void explicit_zero_accepts_empty_auxiliary_streams() {
    const ObservedRun result = execute({true, 0}, selected_capacity_fixture::kCsvHeader,
                                      selected_capacity_fixture::kCsvHeader);
    CHECK(!result.refused);
    CHECK(result.driver.state.auxiliary_rows == 0);
    CHECK(result.driver.coverage.auxiliary_rows == 0);
    CHECK(result.driver.coverage.auxiliary_streams == 2);
    CHECK(result.driver.coverage.auxiliary_complete);
    CHECK(result.observation.auxiliary.empty());
    check_primary_sink(result);
}

void explicit_zero_auxiliary_limit_refuses_first_record() {
    const ObservedRun result = execute({true, 0});
    check_limit_failure(result, "{\"limit\":\"0\",\"requested\":\"1\",\"used\":\"0\"}");
    if (!result.refused) return;
    CHECK(result.observation.auxiliary.size() == 1);
    CHECK(result.observation.auxiliary_used_at_event == 0);
    if (result.observation.auxiliary.size() == 1) {
        CHECK(result.observation.auxiliary[0].timestamp == 1704067200000);
        CHECK(result.observation.auxiliary[0].close == 300.5);
    }
    check_primary_sink(result);
}

}  // namespace

int main() {
    struct Scenario {
        const char* name;
        void (*run)();
        bool control;
    };
    const Scenario scenarios[] = {
        {"exact two streams 2+3",
         &exact_limit_passes_and_aggregates_all_auxiliary_streams, true},
        {"absent bound", &absent_auxiliary_limit_is_no_limit, true},
        {"unrelated roles", &unrelated_roles_do_not_charge_auxiliary, true},
        {"warmup and partial", &warmup_and_partial_auxiliary_rows_are_counted, true},
        {"zero with empty auxiliary streams", &explicit_zero_accepts_empty_auxiliary_streams, true},
        {"aggregate one over bound", &aggregate_limit_plus_one_refuses_at_auxiliary_charge, false},
        {"explicit zero", &explicit_zero_auxiliary_limit_refuses_first_record, false},
    };
    try {
        for (const Scenario& item : scenarios) {
            scenario = item.name;
            item.run();
            // A failed control prevents all subsequent runs. The two negative
            // cases independently report their expected-refusal assertions.
            if (item.control && failures != 0) break;
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NEEDS setup test=selected_auxiliary_rows scenario=%s %s:%d "
                             "actual=%s exception=%s\n",
                     scenario, __FILE__, __LINE__, actual_outcome, error.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "NEEDS setup test=selected_auxiliary_rows scenario=%s %s:%d "
                             "actual=non-standard exception\n", scenario, __FILE__, __LINE__);
        return 2;
    }
    std::printf("selected auxiliary rows: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
