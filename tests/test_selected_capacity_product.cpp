// Strong product binding and all-five integration. No observer table is
// injected into any materialization call in this test.
#include "../src/selected_capacity_driver.hpp"
#include "selected_capacity_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace sc = pineforge::detail::selected_capacity;
namespace fixture = selected_capacity_fixture;

struct CheckFailure {};
int checks = 0;
const char* scenario = "not started";

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL selected_product [%s] %s:%d: %s\n",       \
                         scenario, __FILE__, __LINE__, #condition);              \
            throw CheckFailure{};                                               \
        }                                                                       \
    } while (false)

constexpr std::int64_t kBase = 1704067200000;
constexpr std::int64_t kMinute = 60000;
constexpr fixture::CsvRecord kPrimary[] = {
    {kBase, 51, 53, 49, 52, 201},
    {kBase + kMinute, 61, 63, 59, 62, 202},
    {kBase + 5 * kMinute, 71, 73, 69, 72, 203},
    {kBase + 6 * kMinute, 81, 83, 79, 82, 204},
    {kBase + 10 * kMinute, 91, 93, 89, 92, 205},
};
constexpr char kAuxiliaryLast[] = "1704067440000,101,103,99,102,304\n";
using Texts = std::array<std::string, 6>;

Texts input_texts() {
    // Interleave the roles. There are two Source streams (2+1), two Primary
    // streams (2+3), and two Auxiliary streams (1+2).
    return {{
        fixture::make_csv({
            {kBase - kMinute, 11, 13, 9, 12, 101},
            {kBase, 21, 23, 19, 22, 102},
        }),
        fixture::make_csv({kPrimary[0], kPrimary[1]}),
        fixture::make_csv({{kBase + 2 * kMinute, 31, 33, 29, 32, 301}}),
        fixture::make_csv({{kBase + 16 * kMinute, 41, 43, 39, 42, 103}}),
        fixture::make_csv({kPrimary[2], kPrimary[3], kPrimary[4]}),
        fixture::make_csv({
            {kBase + 3 * kMinute, 91, 93, 89, 92, 303},
            {kBase + 4 * kMinute, 101, 103, 99, 102, 304},
        }),
    }};
}

std::uint64_t total_bytes(const Texts& text) {
    std::uint64_t total = 0;
    for (const auto& csv : text) total += csv.size();
    return total;
}

pineforge::SelectedPlanRequest selected_request() {
    pineforge::SelectedPlanRequest request{};
    request.start_ms = kBase + 5 * kMinute;
    request.end_ms = kBase + 15 * kMinute;
    request.fed_start_ms = kBase;
    request.preroll_bars = 1;
    request.input_tf = "1";
    request.script_tf = "5";
    request.chart_timezone = "UTC";
    request.engine_timezone = "UTC";
    request.session = "24x7";
    return request;
}

sc::Limits exact_limits(const Texts& text) {
    sc::Limits limits;
    limits.source_rows = {true, 3};
    limits.primary_rows = {true, 5};
    // Keys at minutes 0, 5, 10. Minute 10 is an opened, unsealed tail.
    limits.script_bars = {true, 3};
    limits.auxiliary_rows = {true, 3};
    limits.bytes = {true, total_bytes(text)};
    return limits;
}

struct Run {
    std::array<std::istringstream, 6> streams;
    std::vector<pineforge::Bar> retained;

    explicit Run(const Texts& text) {
        for (std::size_t i = 0; i < text.size(); ++i) streams[i].str(text[i]);
    }

    static void sink(void* context, const pineforge::Bar& bar) {
        static_cast<Run*>(context)->retained.push_back(bar);
    }

    sc::DriverResult execute(const sc::Limits& limits,
                             const pineforge::SelectedPlanRequest& request = selected_request()) {
        const std::array<sc::StreamInput, 6> inputs{{
            {sc::Role::Source, streams[0], "Source A"},
            {sc::Role::Primary, streams[1], "Primary A"},
            {sc::Role::Auxiliary, streams[2], "Auxiliary A"},
            {sc::Role::Source, streams[3], "Source B"},
            {sc::Role::Primary, streams[4], "Primary B"},
            {sc::Role::Auxiliary, streams[5], "Auxiliary B"},
        }};
        return sc::materialize_selected_capacity(inputs.data(), inputs.size(), request,
                                                 limits, sink, this);
    }
};

void check_primary_prefix(const Run& run, std::size_t count) {
    CHECK(count <= 5);
    CHECK(run.retained.size() == count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& actual = run.retained[i];
        const auto& expected = kPrimary[i];
        CHECK(actual.timestamp == expected.timestamp);
        CHECK(actual.open == expected.open && actual.high == expected.high);
        CHECK(actual.low == expected.low && actual.close == expected.close);
        CHECK(actual.volume == expected.volume);
    }
}

void table_strongly_binds_every_real_owner() {
    static_assert(noexcept(sc::make_product_observer_table()));
    const auto table = sc::make_product_observer_table();
    CHECK(table.source_rows == &sc::observe_source_rows);
    CHECK(table.primary_rows == &sc::observe_primary_rows);
    CHECK(table.script_bars == &sc::observe_script_bars);
    CHECK(table.auxiliary_rows == &sc::observe_auxiliary_rows);
    CHECK(table.bytes == &sc::observe_bytes);
}

void all_five_owners_materialize_actual_inputs() {
    const auto text = input_texts();
    Run run(text);
    const auto result = run.execute(exact_limits(text));
    if (result.status != sc::DriverStatus::Completed
        || result.reason != sc::DriverReason::None) {
        throw std::runtime_error("All-five production control did not complete: " + result.message);
    }
    CHECK(result.plan_attempted && result.plan_status == pineforge::SelectedPlanStatus::Ok);
    CHECK(result.state.source_rows == 3);
    CHECK(result.state.primary_rows == 5);
    CHECK(result.state.script_bars == 3);
    CHECK(result.state.auxiliary_rows == 3);
    CHECK(result.state.bytes == total_bytes(text));
    CHECK(result.state.has_last_primary_bucket);
    CHECK(result.state.last_primary_bucket == kBase + 10 * kMinute);
    CHECK(result.coverage.source_rows == 3 && result.coverage.primary_rows == 5);
    CHECK(result.coverage.auxiliary_rows == 3 && result.coverage.primary_script_events == 5);
    CHECK(result.coverage.source_bytes == text[0].size() + text[3].size());
    CHECK(result.coverage.primary_bytes == text[1].size() + text[4].size());
    CHECK(result.coverage.auxiliary_bytes == text[2].size() + text[5].size());
    CHECK(result.coverage.source_streams == 2 && result.coverage.primary_streams == 2);
    CHECK(result.coverage.auxiliary_streams == 2);
    CHECK(result.coverage.source_complete && result.coverage.primary_complete);
    CHECK(result.coverage.auxiliary_complete);
    check_primary_prefix(run, 5);
}

void all_absent_preserves_no_requested_fields_without_consumption() {
    Run run(input_texts());
    // This request is deliberately inadmissible if a calendar is consulted.
    const pineforge::SelectedPlanRequest request{};
    const auto result = run.execute(sc::Limits{}, request);
    CHECK(result.status == sc::DriverStatus::NotAdmitted);
    CHECK(result.reason == sc::DriverReason::NoRequestedFields);
    CHECK(!result.plan_attempted && !result.has_field);
    CHECK(result.state.source_rows == 0 && result.state.primary_rows == 0);
    CHECK(result.state.script_bars == 0 && result.state.auxiliary_rows == 0);
    CHECK(result.state.bytes == 0 && !result.state.has_last_primary_bucket);
    CHECK(result.coverage.source_rows == 0 && result.coverage.primary_rows == 0);
    CHECK(result.coverage.auxiliary_rows == 0 && result.coverage.primary_script_events == 0);
    CHECK(result.coverage.source_bytes == 0 && result.coverage.primary_bytes == 0);
    CHECK(result.coverage.auxiliary_bytes == 0);
    CHECK(result.coverage.source_streams == 0 && result.coverage.primary_streams == 0);
    CHECK(result.coverage.auxiliary_streams == 0);
    for (auto& stream : run.streams) CHECK(stream.tellg() == std::streampos(0));
    check_primary_prefix(run, 0);

    const auto empty = sc::materialize_selected_capacity(nullptr, 0, request, sc::Limits{},
                                                        nullptr, nullptr);
    CHECK(empty.status == sc::DriverStatus::NotAdmitted);
    CHECK(empty.reason == sc::DriverReason::NoRequestedFields && !empty.plan_attempted);
}

void unrequested_roles_are_optional_in_the_forwarded_span() {
    Run run(input_texts());
    const sc::StreamInput input{sc::Role::Primary, run.streams[1], "only supplied input"};
    sc::Limits limits;
    limits.primary_rows = {true, 2};
    const pineforge::SelectedPlanRequest request{};
    const auto result = sc::materialize_selected_capacity(&input, 1, request, limits,
                                                         Run::sink, &run);
    if (result.status != sc::DriverStatus::Completed
        || result.reason != sc::DriverReason::None) {
        throw std::runtime_error("Primary-only production control did not complete: "
                                 + result.message);
    }
    CHECK(result.state.primary_rows == 2 && result.coverage.primary_rows == 2);
    CHECK(result.state.source_rows == 0 && result.state.bytes == 0);
    CHECK(result.state.auxiliary_rows == 0 && result.state.script_bars == 0);
    CHECK(result.coverage.source_streams == 0 && result.coverage.auxiliary_streams == 0);
    CHECK(result.coverage.primary_streams == 1 && result.coverage.primary_complete);
    CHECK(!result.plan_attempted);
    CHECK(run.streams[0].tellg() == std::streampos(0));
    for (std::size_t i = 2; i < run.streams.size(); ++i) {
        CHECK(run.streams[i].tellg() == std::streampos(0));
    }
    check_primary_prefix(run, 2);
}

void each_slot_enforces_its_bound_with_the_other_four_active() {
    const auto text = input_texts();
    struct Expected {
        const char* name;
        sc::Field field;
        pineforge::RunFailureCode code;
        const char* code_name;
        const char* english;
        std::size_t retained;
        std::size_t stream;
        std::uint64_t limit, used, requested;
    };
    const std::uint64_t last_bytes = sizeof(kAuxiliaryLast) - 1;
    const Expected cases[] = {
        {"Source with all five active", sc::Field::SourceRows,
         pineforge::RunFailureCode::selected_source_rows_limit_exceeded,
         "selected_source_rows_limit_exceeded", "Selected source-row capacity exceeded.",
         2, 3, 2, 2, 1},
        {"Primary with all five active", sc::Field::PrimaryRows,
         pineforge::RunFailureCode::selected_primary_rows_limit_exceeded,
         "selected_primary_rows_limit_exceeded", "Selected primary-row capacity exceeded.",
         4, 4, 4, 4, 1},
        {"ScriptBars with all five active", sc::Field::ScriptBars,
         pineforge::RunFailureCode::selected_script_bars_limit_exceeded,
         "selected_script_bars_limit_exceeded", "Selected script-bar capacity exceeded.",
         4, 4, 2, 2, 1},
        {"Auxiliary with all five active", sc::Field::AuxiliaryRows,
         pineforge::RunFailureCode::selected_auxiliary_rows_limit_exceeded,
         "selected_auxiliary_rows_limit_exceeded", "Selected auxiliary-row capacity exceeded.",
         5, 5, 2, 2, 1},
        {"Bytes with all five active", sc::Field::Bytes,
         pineforge::RunFailureCode::selected_bytes_limit_exceeded,
         "selected_bytes_limit_exceeded",
         "Selected source-and-retained CSV byte capacity exceeded.",
         5, 5, total_bytes(text) - 1, total_bytes(text) - last_bytes, last_bytes},
    };
    for (const auto& expected : cases) {
        scenario = expected.name;
        Run run(text);
        auto limits = exact_limits(text);
        switch (expected.field) {
        case sc::Field::SourceRows: limits.source_rows.value = expected.limit; break;
        case sc::Field::PrimaryRows: limits.primary_rows.value = expected.limit; break;
        case sc::Field::ScriptBars: limits.script_bars.value = expected.limit; break;
        case sc::Field::AuxiliaryRows: limits.auxiliary_rows.value = expected.limit; break;
        case sc::Field::Bytes: limits.bytes.value = expected.limit; break;
        }
        bool refused = false;
        try {
            const auto result = run.execute(limits);
            if (result.status != sc::DriverStatus::Completed
                || result.reason != sc::DriverReason::None) {
                throw std::runtime_error("Unexpected production refusal setup result: "
                                         + result.message);
            }
        } catch (const pineforge::coded<std::runtime_error>& error) {
            if (error.run_failure_code() != expected.code) throw;
            refused = true;
            const auto failure = error.run_failure();
            CHECK(std::strcmp(pineforge::run_failure_code_name(failure.code),
                              expected.code_name) == 0);
            CHECK(pineforge::run_failure_code_class(failure.code)
                  == pineforge::RunFailureClass::resource);
            CHECK(!pineforge::run_failure_code_retryable(failure.code));
            CHECK(failure.args != nullptr);
            const std::string args = "{\"limit\":\"" + std::to_string(expected.limit)
                + "\",\"requested\":\"" + std::to_string(expected.requested)
                + "\",\"used\":\"" + std::to_string(expected.used) + "\"}";
            CHECK(*failure.args == args);
            CHECK(std::strcmp(error.what(), expected.english) == 0);
            std::fprintf(stderr, "selected_product [%s] code=%s args=%s english=%s\n",
                         scenario, pineforge::run_failure_code_name(failure.code),
                         failure.args->c_str(), error.what());
        }
        CHECK(refused);
        check_primary_prefix(run, expected.retained);
        CHECK(run.streams[expected.stream].tellg() == std::streampos(text[expected.stream].size()));
        for (std::size_t i = expected.stream + 1; i < run.streams.size(); ++i) {
            CHECK(run.streams[i].tellg() == std::streampos(0));
        }
        std::printf("selected_product [%s]: completed\n", scenario);
    }
}

}  // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } cases[] = {
        {"strong table identity", table_strongly_binds_every_real_owner},
        {"all-five actual input equality", all_five_owners_materialize_actual_inputs},
        {"all absent", all_absent_preserves_no_requested_fields_without_consumption},
        {"borrowed Primary-only span", unrequested_roles_are_optional_in_the_forwarded_span},
        {"all-five bound integration", each_slot_enforces_its_bound_with_the_other_four_active},
    };
    try {
        for (const auto& test : cases) {
            scenario = test.name;
            test.run();
        }
    } catch (const CheckFailure&) {
        std::fprintf(stderr, "selected_product: %d checks, 1 failure\n", checks);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NEEDS selected_product [%s]: %s\n", scenario, error.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "NEEDS selected_product [%s]: nonstandard exception\n", scenario);
        return 2;
    }
    std::printf("selected_product: %d checks, 0 failures\n", checks);
    return 0;
}
