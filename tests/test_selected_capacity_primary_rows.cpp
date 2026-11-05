// Source preparation for the separated driver, before the primary-row guard.
// The test-local row callback counts validated events but enforces no limit.
// After an executed bound RED, bind the primary slot to the real owner; keep
// the inputs and assertions unchanged.
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
                         "CHECK FAIL test=selected_primary_rows scenario=%s "    \
                         "%s:%d %s actual=%s\n", scenario, __FILE__, __LINE__,   \
                         #condition, actual_outcome);                            \
        }                                                                       \
    } while (false)

// The values are deliberately varied so a fixture that copies a declared count
// or ignores the stream contents cannot satisfy the row or materialization
// assertions. Source and Auxiliary are observed but never sent to the sink.
constexpr const char* kSourceCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,100,101,99,100.5,10\n"
    "1704067260000,100.5,101.5,99.5,101,11\n"
    "1704067320000,101,102,100,101.5,12\n"
    "1704067380000,101.5,102.5,100.5,102,13\n"
    "1704067440000,102,103,101,102.5,14\n";

constexpr const char* kPrimaryOneCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,200,201,199,200.5,20\n";

constexpr const char* kPrimaryThreeCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,200,201,199,200.5,20\n"
    "1704067260000,200.5,201.5,199.5,201,21\n"
    "1704067320000,201,202,200,201.5,22\n";

constexpr const char* kPrimaryFourCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,200,201,199,200.5,20\n"
    "1704067260000,200.5,201.5,199.5,201,21\n"
    "1704067320000,201,202,200,201.5,22\n"
    "1704067380000,201.5,202.5,200.5,202,23\n";

constexpr const char* kAuxiliaryCsv =
    "timestamp,open,high,low,close,volume\n"
    "1704067200000,300,301,299,300.5,30\n"
    "1704067260000,300.5,301.5,299.5,301,31\n"
    "1704067320000,301,302,300,301.5,32\n"
    "1704067380000,301.5,302.5,300.5,302,33\n";

struct Observation {
    std::vector<pineforge::Bar> source;
    std::vector<pineforge::Bar> primary;
    std::vector<pineforge::Bar> auxiliary;
    std::vector<pineforge::Bar> delivered;
    std::uint64_t primary_used_at_event = 0;
};

Observation* active_observation = nullptr;

// This requested Source slot also observes the actual boundary immediately
// before the Primary slot. It does not impose a product Source-row guard.
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
        active_observation->primary_used_at_event = state.primary_rows;
        break;
    case Role::Auxiliary:
        active_observation->auxiliary.push_back(*event.bar);
        break;
    }
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

ObservedRun execute(const std::string& primary_csv, sc::Bound primary_bound,
                    const char* source_csv = kSourceCsv) {
    actual_outcome = "setup";
    ObservedRun result;
    // Separate objects and stream buffers, even when Source and Primary have
    // identical CSV bytes. Labels are diagnostic, never identity.
    std::istringstream source(source_csv);
    std::istringstream primary(primary_csv);
    std::istringstream auxiliary(kAuxiliaryCsv);
    const sc::StreamInput inputs[] = {
        {Role::Source, source, "primary-source.csv"},
        {Role::Primary, primary, "primary-output.csv"},
        {Role::Auxiliary, auxiliary, "primary-auxiliary.csv"},
    };
    sc::Limits limits{};
    limits.source_rows = {true, 5};
    limits.primary_rows = primary_bound;
    sc::ObserverTable observers{};
    observers.source_rows = &record_events;
    if (limits.primary_rows.present) {
        observers.primary_rows = &sc::observe_primary_rows;
    }
    ObservationScope scope(result.observation);
    try {
        result.driver = sc::run_driver(inputs, 3, request(), limits, observers,
                                       &retain_primary, &result.observation);
    } catch (const pineforge::coded<std::runtime_error>& error) {
        result.failure = error.run_failure();
        result.english = error.what();
        actual_outcome = run_failure_code_name(result.failure.code);
        std::fprintf(stderr,
                     "test=selected_primary_rows scenario=%s %s:%d "
                     "actual=coded code=%s args=%s english=%s\n",
                     scenario, __FILE__, __LINE__, actual_outcome,
                     result.failure.args ? result.failure.args->c_str() : "{}",
                     error.what());
        if (std::strcmp(actual_outcome, "selected_primary_rows_limit_exceeded") != 0) {
            throw std::runtime_error("unexpected coded failure is setup, not bound RED");
        }
        result.refused = true;
        return result;
    }
    std::fprintf(stderr,
                 "test=selected_primary_rows scenario=%s %s:%d actual=returned "
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
                      "selected_primary_rows_limit_exceeded") == 0);
    CHECK(run_failure_code_class(failure.code) == RunFailureClass::resource);
    CHECK(!run_failure_code_retryable(failure.code));
    CHECK(failure.args != nullptr);
    if (failure.args != nullptr) CHECK(*failure.args == args);
    CHECK(result.english == "Selected primary-row capacity exceeded.");
}

void check_delivered_prefix(const ObservedRun& result, std::size_t count) {
    CHECK(result.observation.delivered.size() == count);
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

void exact_limit_passes_and_unrelated_roles_do_not_charge_primary() {
    const ObservedRun result = execute(kPrimaryThreeCsv, {true, 3});
    CHECK(!result.refused);
    CHECK(result.driver.state.primary_rows == 3);
    CHECK(result.driver.state.source_rows == 5);
    CHECK(result.driver.state.auxiliary_rows == 0);  // no requested Auxiliary slot
    CHECK(result.driver.coverage.source_rows == 5);
    CHECK(result.driver.coverage.primary_rows == 3);
    CHECK(result.driver.coverage.auxiliary_rows == 4);
    CHECK(result.driver.coverage.source_complete);
    CHECK(result.driver.coverage.primary_complete);
    CHECK(result.driver.coverage.auxiliary_complete);
    CHECK(result.observation.source.size() == 5);
    CHECK(result.observation.primary.size() == 3);
    CHECK(result.observation.auxiliary.size() == 4);
    check_delivered_prefix(result, 3);
}

void one_more_primary_row_refuses_before_crossing_materialization() {
    const ObservedRun result = execute(kPrimaryFourCsv, {true, 3});
    check_limit_failure(result, "{\"limit\":\"3\",\"requested\":\"1\",\"used\":\"3\"}");
    if (!result.refused) return;
    CHECK(result.observation.source.size() == 5);
    CHECK(result.observation.primary.size() == 4);  // actual attempted fourth row
    CHECK(result.observation.primary_used_at_event == 3);
    CHECK(result.observation.auxiliary.empty());  // later stream was not consumed
    check_delivered_prefix(result, 3);
}

void absent_primary_limit_is_no_limit() {
    for (const std::uint64_t ignored_value : {std::uint64_t{0}, std::uint64_t{1}}) {
        const ObservedRun result = execute(kPrimaryFourCsv, {false, ignored_value});
        CHECK(!result.refused);
        CHECK(result.driver.state.primary_rows == 0);  // absent slots do not update State
        CHECK(result.driver.state.source_rows == 5);   // requested control keeps driver active
        CHECK(result.driver.coverage.primary_rows == 4);
        CHECK(result.driver.coverage.auxiliary_rows == 4);
        CHECK(result.observation.primary.size() == 4);
        CHECK(result.observation.auxiliary.size() == 4);
        check_delivered_prefix(result, 4);
        if (failures != 0) return;
    }
}

void identical_csv_in_distinct_source_and_primary_streams() {
    const ObservedRun result = execute(kPrimaryFourCsv, {true, 4}, kPrimaryFourCsv);
    CHECK(!result.refused);
    CHECK(result.driver.state.source_rows == 4);
    CHECK(result.driver.state.primary_rows == 4);
    CHECK(result.driver.coverage.source_streams == 1);
    CHECK(result.driver.coverage.primary_streams == 1);
    CHECK(result.observation.source.size() == 4);
    CHECK(result.observation.primary.size() == 4);
    check_delivered_prefix(result, 4);
}

void supplied_pretrim_and_partial_primary_rows_are_counted() {
    using selected_capacity_fixture::make_csv;
    using selected_capacity_fixture::make_record;
    const std::string csv = make_csv({
        make_record(1704067200000, 210, 211, 209, 210.5, 30),
        make_record(1704067260000, 211, 212, 210, 211.5, 31),
        make_record(1704067560000, 212, 213, 211, 212.5, 32),
    });
    const ObservedRun result = execute(csv, {true, 3});
    CHECK(!result.refused);
    CHECK(result.driver.state.primary_rows == 3);
    CHECK(result.driver.coverage.primary_rows == 3);
    CHECK(result.observation.delivered.size() == 3);
    if (result.observation.delivered.size() == 3) {
        CHECK(result.observation.delivered[0].timestamp < request().start_ms);
        CHECK(result.observation.delivered[1].timestamp < request().start_ms);
        CHECK(result.observation.delivered[2].timestamp == 1704067560000);
        CHECK(result.observation.delivered[2].close == 212.5);
        CHECK(result.observation.delivered[2].volume == 32);
    }
}

void explicit_zero_primary_limit_refuses_first_record() {
    const ObservedRun result = execute(kPrimaryOneCsv, {true, 0});
    check_limit_failure(result, "{\"limit\":\"0\",\"requested\":\"1\",\"used\":\"0\"}");
    if (!result.refused) return;
    CHECK(result.observation.primary.size() == 1);
    CHECK(result.observation.primary_used_at_event == 0);
    CHECK(result.observation.delivered.empty());
    CHECK(result.observation.auxiliary.empty());
}

}  // namespace

int main() {
    struct Scenario {
        const char* name;
        void (*run)();
        bool control;
    };
    const Scenario scenarios[] = {
        {"exact and unrelated roles",
         &exact_limit_passes_and_unrelated_roles_do_not_charge_primary, true},
        {"absent bound", &absent_primary_limit_is_no_limit, true},
        {"distinct streams with identical CSV",
         &identical_csv_in_distinct_source_and_primary_streams, true},
        {"pretrim and partial", &supplied_pretrim_and_partial_primary_rows_are_counted, true},
        {"one over bound", &one_more_primary_row_refuses_before_crossing_materialization, false},
        {"explicit zero", &explicit_zero_primary_limit_refuses_first_record, false},
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
        std::fprintf(stderr, "NEEDS setup test=selected_primary_rows scenario=%s %s:%d "
                             "actual=%s exception=%s\n",
                     scenario, __FILE__, __LINE__, actual_outcome, error.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "NEEDS setup test=selected_primary_rows scenario=%s %s:%d "
                             "actual=non-standard exception\n", scenario, __FILE__, __LINE__);
        return 2;
    }
    std::printf("selected primary rows: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
