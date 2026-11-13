// Source accounting through the real product entry, using independent CSV
// populations adapted from the retained Source-row input.
#include "../src/selected_capacity_driver.hpp"
#include "selected_capacity_fixture.hpp"

#include <pineforge/run_failure.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <iterator>
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
            std::fprintf(stderr, "FAIL selected_source_rows [%s] %s:%d: %s\n",   \
                         scenario, __FILE__, __LINE__, #condition);              \
            throw CheckFailure{};                                               \
        }                                                                       \
    } while (false)

std::string source_csv() {
    // Source precedes, overlaps and follows the selected Primary population.
    return fixture::make_csv({
        {-60000, 11, 13, 9, 12, 101},
        {0, 21, 23, 19, 22, 102},
        {60000, 31, 33, 29, 32, 103},
        {900000, 41, 43, 39, 42, 104},
    });
}

std::string primary_csv() {
    return fixture::make_csv({
        {0, 51, 53, 49, 52, 201},
        {60000, 61, 63, 59, 62, 202},
    });
}

std::string auxiliary_csv() {
    return fixture::make_csv({
        {120000, 71, 73, 69, 72, 301},
        {180000, 81, 83, 79, 82, 302},
        {240000, 91, 93, 89, 92, 303},
    });
}

struct Run {
    std::istringstream source;
    std::istringstream primary{primary_csv()};
    std::istringstream auxiliary{auxiliary_csv()};
    std::vector<pineforge::Bar> retained;

    explicit Run(const std::string& source_text = source_csv()) : source(source_text) {}

    static void sink(void* context, const pineforge::Bar& bar) {
        static_cast<Run*>(context)->retained.push_back(bar);
    }

    sc::DriverResult execute(const sc::Limits& limits) {
        const std::array<sc::StreamInput, 3> inputs{{
            {sc::Role::Source, source, "original Source"},
            {sc::Role::Primary, primary, "retained Primary"},
            {sc::Role::Auxiliary, auxiliary, "retained Auxiliary"},
        }};
        pineforge::SelectedPlanRequest request{};
        request.start_ms = 0;
        request.fed_start_ms = 0;
        request.end_ms = 600000;
        request.input_tf = "1";
        request.script_tf = "5";
        request.chart_timezone = "UTC";
        request.engine_timezone = "UTC";
        request.session = "24x7";
        return sc::materialize_selected_capacity(inputs.data(), inputs.size(), request,
                                                 limits, sink, this);
    }
};

sc::Limits source_bound(std::uint64_t limit) {
    sc::Limits limits;
    limits.source_rows = {true, limit};
    return limits;
}

void check_retained(const Run& run) {
    CHECK(run.retained.size() == 2);
    CHECK(run.retained[0].timestamp == 0);
    CHECK(run.retained[0].open == 51 && run.retained[0].high == 53);
    CHECK(run.retained[0].low == 49 && run.retained[0].close == 52);
    CHECK(run.retained[0].volume == 201);
    CHECK(run.retained[1].timestamp == 60000);
    CHECK(run.retained[1].open == 61 && run.retained[1].high == 63);
    CHECK(run.retained[1].low == 59 && run.retained[1].close == 62);
    CHECK(run.retained[1].volume == 202);
}

void check_completed(const Run& run, const sc::DriverResult& result,
                     std::uint64_t expected_source_rows) {
    if (result.status != sc::DriverStatus::Completed
        || result.reason != sc::DriverReason::None) {
        throw std::runtime_error("Source-row control did not complete: " + result.message);
    }
    CHECK(result.state.source_rows == expected_source_rows);
    CHECK(result.coverage.source_rows == expected_source_rows);
    CHECK(result.coverage.primary_rows == 2);
    CHECK(result.coverage.auxiliary_rows == 3);
    CHECK(result.coverage.source_streams == 1);
    CHECK(result.coverage.primary_streams == 1);
    CHECK(result.coverage.auxiliary_streams == 1);
    CHECK(result.coverage.source_complete && result.coverage.primary_complete);
    CHECK(result.coverage.auxiliary_complete);
    CHECK(result.state.primary_rows == 0 && result.state.auxiliary_rows == 0);
    CHECK(result.state.script_bars == 0 && result.state.bytes == 0);
    CHECK(!result.state.has_last_primary_bucket);
    CHECK(!result.plan_attempted);
    check_retained(run);
}

void ordinary_equality_and_changed_population() {
    for (const std::uint64_t limit : {5U, 4U}) {
        Run run;
        check_completed(run, run.execute(source_bound(limit)), 4);
    }
    // Three actual records, with no terminator on the final record.
    const std::string three =
        "timestamp,open,high,low,close,volume\n"
        "0,1,3,0,2,7\n"
        "60000,4,6,3,5,8\n"
        "120000,7,9,6,8,9";
    Run run(three);
    check_completed(run, run.execute(source_bound(3)), 3);
}

void zero_accepts_header_and_ignores_other_roles() {
    Run run(fixture::kCsvHeader);
    check_completed(run, run.execute(source_bound(0)), 0);
}

void absent_source_bound_with_real_primary_control() {
    for (const std::uint64_t ignored_value : {0U, 1U}) {
        Run run;
        sc::Limits limits;
        limits.source_rows = {false, ignored_value};
        limits.primary_rows = {true, 2};
        const auto result = run.execute(limits);
        if (result.status != sc::DriverStatus::Completed
            || result.reason != sc::DriverReason::None) {
            throw std::runtime_error("Absent Source-bound control did not complete: "
                                     + result.message);
        }
        CHECK(result.state.source_rows == 0);
        CHECK(result.coverage.source_rows == 4);
        CHECK(result.state.primary_rows == 2);
        CHECK(result.coverage.primary_rows == 2 && result.coverage.auxiliary_rows == 3);
        CHECK(result.state.auxiliary_rows == 0 && result.state.bytes == 0);
        CHECK(result.coverage.source_complete && result.coverage.primary_complete);
        CHECK(result.coverage.auxiliary_complete);
        check_retained(run);
    }
}

void malformed_record_is_not_a_source_reservation() {
    const std::string valid = fixture::make_csv({
        {0, 11, 13, 9, 12, 101},
        {60000, 21, 23, 19, 22, 102},
    });
    Run run(valid + "120000,31,33,29,32,invalid-volume\n");
    // The malformed third row must produce a format result, not a capacity
    // exception, even though the two validated records exhaust the bound.
    const auto result = run.execute(source_bound(2));
    CHECK(result.status == sc::DriverStatus::FormatError);
    CHECK(result.reason == sc::DriverReason::CsvValue);
    CHECK(result.role == sc::Role::Source && result.input_index == 0);
    CHECK(result.row_index == 3);
    CHECK(result.state.source_rows == 2 && result.coverage.source_rows == 2);
    CHECK(!result.coverage.source_complete && result.coverage.source_streams == 0);
    CHECK(result.coverage.source_bytes == valid.size());
    CHECK(result.coverage.primary_rows == 0 && result.coverage.auxiliary_rows == 0);
    CHECK(run.retained.empty());
    CHECK(run.primary.tellg() == std::streampos(0));
    CHECK(run.auxiliary.tellg() == std::streampos(0));
}

void expect_source_refusal(std::uint64_t limit, std::uint64_t used,
                           const std::string& unread_tail) {
    Run run;
    bool refused = false;
    try {
        const auto result = run.execute(source_bound(limit));
        if (result.status != sc::DriverStatus::Completed
            || result.reason != sc::DriverReason::None) {
            throw std::runtime_error("Unexpected Source refusal setup result: " + result.message);
        }
    } catch (const pineforge::coded<std::runtime_error>& error) {
        if (error.run_failure_code()
            != pineforge::RunFailureCode::selected_source_rows_limit_exceeded) {
            throw;
        }
        refused = true;
        CHECK(std::strcmp(pineforge::run_failure_code_name(error.run_failure_code()),
                          "selected_source_rows_limit_exceeded") == 0);
        CHECK(pineforge::run_failure_code_class(error.run_failure_code())
              == pineforge::RunFailureClass::resource);
        CHECK(!pineforge::run_failure_code_retryable(error.run_failure_code()));
        const auto failure = pineforge::classify_run_failure(error);
        CHECK(failure.args != nullptr);
        const std::string expected = "{\"limit\":\"" + std::to_string(limit)
            + "\",\"requested\":\"1\",\"used\":\"" + std::to_string(used) + "\"}";
        CHECK(*failure.args == expected);
        CHECK(std::strcmp(error.what(), "Selected source-row capacity exceeded.") == 0);
        std::fprintf(stderr, "selected_source_rows [%s] code=%s args=%s english=%s\n",
                     scenario, pineforge::run_failure_code_name(failure.code),
                     failure.args->c_str(), error.what());
    }
    CHECK(refused);
    // The escaping exception carries no DriverResult. Inspect the real stream
    // tail and sink instead of manufacturing a partial result.
    const std::string tail{std::istreambuf_iterator<char>(run.source),
                           std::istreambuf_iterator<char>()};
    CHECK(tail == unread_tail);
    CHECK(run.primary.tellg() == std::streampos(0));
    CHECK(run.auxiliary.tellg() == std::streampos(0));
    CHECK(run.retained.empty());
}

void one_over_and_early_stop() {
    expect_source_refusal(3, 3, "");
    expect_source_refusal(2, 2, "900000,41,43,39,42,104\n");
}

void explicit_zero_refuses_first_source_record() {
    expect_source_refusal(0, 0,
        "0,21,23,19,22,102\n60000,31,33,29,32,103\n900000,41,43,39,42,104\n");
}

}  // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } cases[] = {
        {"ordinary/equality/changed population", ordinary_equality_and_changed_population},
        {"zero/header/wrong roles", zero_accepts_header_and_ignores_other_roles},
        {"absent Source bound", absent_source_bound_with_real_primary_control},
        {"validated prefix only", malformed_record_is_not_a_source_reservation},
        {"one over/early stop", one_over_and_early_stop},
        {"explicit zero", explicit_zero_refuses_first_source_record},
    };
    try {
        for (const auto& test : cases) {
            scenario = test.name;
            test.run();
            std::printf("selected_source_rows [%s]: completed\n", scenario);
        }
    } catch (const CheckFailure&) {
        std::fprintf(stderr, "selected_source_rows: %d checks, 1 failure\n", checks);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NEEDS selected_source_rows [%s]: %s\n", scenario, error.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "NEEDS selected_source_rows [%s]: nonstandard exception\n", scenario);
        return 2;
    }
    std::printf("selected_source_rows: %d checks, 0 failures\n", checks);
    return 0;
}
