// Physical CSV octets through the production entry. Expected sizes come only
// from the independent input strings, including headers and terminators.
#include "../src/selected_capacity_driver.hpp"

#include <pineforge/run_failure.hpp>

#include <array>
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
namespace sc = pineforge::detail::selected_capacity;

struct CheckFailure {};
int checks = 0;
const char* scenario = "not started";

#define CHECK(condition)                                                        \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            std::fprintf(stderr, "FAIL selected_bytes [%s] %s:%d: %s\n",         \
                         scenario, __FILE__, __LINE__, #condition);              \
            throw CheckFailure{};                                               \
        }                                                                       \
    } while (false)

constexpr char kHeaderLf[] = "timestamp,open,high,low,close,volume\n";
constexpr char kHeaderCrlf[] = "timestamp,open,high,low,close,volume\r\n";
constexpr char kSourceFirst[] = "-60000,11,13,9,12,101\r\n";
constexpr char kPrimaryFirst[] = "0,51,53,49,52,201\n";
constexpr char kPrimaryLast[] = "60000,61,63,59,62,202\r\n";
constexpr char kAuxiliaryLast[] = "180000,81,83,79,82,302,\"two,\r\nlines \xc3\xa9\"\n";

struct Inputs {
    std::string source;
    std::string primary;
    std::string auxiliary_a;
    std::string auxiliary_b;

    std::uint64_t total_bytes() const {
        return source.size() + primary.size() + auxiliary_a.size() + auxiliary_b.size();
    }
};

Inputs mixed_inputs() {
    return {
        std::string(kHeaderCrlf) + kSourceFirst + "900000,21,23,19,22,102",
        std::string(kHeaderLf) + kPrimaryFirst + kPrimaryLast,
        "timestamp,open,high,low,close,volume\r120000,71,73,69,72,301",
        std::string("timestamp,open,high,low,close,volume,note\r\n") + kAuxiliaryLast,
    };
}

Inputs lf_inputs() {
    // The same parsed values have different actual octets. Quoted commas,
    // embedded newlines and UTF-8 extras are physical bytes, too.
    return {
        "timestamp,open,high,low,close,volume\n"
        "-60000,11,13,9,12,101\n900000,21,23,19,22,102\n",
        "timestamp,open,high,low,close,volume\n"
        "0,51,53,49,52,201\n60000,61,63,59,62,202\n",
        "timestamp,open,high,low,close,volume\n120000,71,73,69,72,301\n",
        "timestamp,open,high,low,close,volume,note\n"
        "180000,81,83,79,82,302,\"two,\nlines \xc3\xa9\"\n",
    };
}

struct Run {
    std::istringstream source;
    std::istringstream primary;
    std::istringstream auxiliary_a;
    std::istringstream auxiliary_b;
    std::vector<pineforge::Bar> retained;

    explicit Run(const Inputs& text)
        : source(text.source), primary(text.primary),
          auxiliary_a(text.auxiliary_a), auxiliary_b(text.auxiliary_b) {}

    static void sink(void* context, const pineforge::Bar& bar) {
        static_cast<Run*>(context)->retained.push_back(bar);
    }

    sc::DriverResult execute(const sc::Limits& limits) {
        const std::array<sc::StreamInput, 4> inputs{{
            {sc::Role::Source, source, "Source"},
            {sc::Role::Primary, primary, "Primary"},
            {sc::Role::Auxiliary, auxiliary_a, "Auxiliary A"},
            {sc::Role::Auxiliary, auxiliary_b, "Auxiliary B"},
        }};
        // Calendar work must stay dormant when script_bars is absent.
        const pineforge::SelectedPlanRequest request{};
        return sc::materialize_selected_capacity(inputs.data(), inputs.size(), request,
                                                 limits, sink, this);
    }
};

sc::Limits byte_bound(std::uint64_t limit) {
    sc::Limits limits;
    limits.bytes = {true, limit};
    return limits;
}

void require_completed(const sc::DriverResult& result) {
    if (result.status != sc::DriverStatus::Completed
        || result.reason != sc::DriverReason::None) {
        throw std::runtime_error("Byte control did not complete: " + result.message);
    }
}

void check_primary_prefix(const Run& run, std::size_t count) {
    CHECK(run.retained.size() == count);
    if (count > 0) {
        const auto& bar = run.retained[0];
        CHECK(bar.timestamp == 0);
        CHECK(bar.open == 51 && bar.high == 53 && bar.low == 49);
        CHECK(bar.close == 52 && bar.volume == 201);
    }
    if (count > 1) {
        const auto& bar = run.retained[1];
        CHECK(bar.timestamp == 60000);
        CHECK(bar.open == 61 && bar.high == 63 && bar.low == 59);
        CHECK(bar.close == 62 && bar.volume == 202);
    }
}

void check_physical_coverage(const sc::DriverResult& result, const Inputs& inputs) {
    CHECK(result.coverage.source_bytes == inputs.source.size());
    CHECK(result.coverage.primary_bytes == inputs.primary.size());
    CHECK(result.coverage.auxiliary_bytes
          == inputs.auxiliary_a.size() + inputs.auxiliary_b.size());
    CHECK(result.coverage.source_rows == 2);
    CHECK(result.coverage.primary_rows == 2);
    CHECK(result.coverage.auxiliary_rows == 2);
    CHECK(result.coverage.source_streams == 1 && result.coverage.primary_streams == 1);
    CHECK(result.coverage.auxiliary_streams == 2);
    CHECK(result.coverage.source_complete && result.coverage.primary_complete);
    CHECK(result.coverage.auxiliary_complete);
    CHECK(!result.plan_attempted);
}

void equality_includes_headers_all_roles_and_actual_newlines() {
    const auto mixed = mixed_inputs();
    const auto lf = lf_inputs();
    CHECK(mixed.total_bytes() != lf.total_bytes());
    for (const auto& inputs : {mixed, lf}) {
        Run run(inputs);
        const auto result = run.execute(byte_bound(inputs.total_bytes()));
        require_completed(result);
        CHECK(result.state.bytes == inputs.total_bytes());
        CHECK(result.state.source_rows == 0 && result.state.primary_rows == 0);
        CHECK(result.state.auxiliary_rows == 0 && result.state.script_bars == 0);
        check_physical_coverage(result, inputs);
        check_primary_prefix(run, 2);
    }
}

void absent_byte_bound_does_not_charge_state() {
    const auto inputs = mixed_inputs();
    for (const std::uint64_t ignored_value : {0U, 1U}) {
        Run run(inputs);
        sc::Limits limits;
        limits.bytes = {false, ignored_value};
        limits.primary_rows = {true, 2};
        const auto result = run.execute(limits);
        require_completed(result);
        CHECK(result.state.bytes == 0);
        CHECK(result.state.primary_rows == 2);
        check_physical_coverage(result, inputs);
        check_primary_prefix(run, 2);
    }
}

void empty_source_and_auxiliary_headers_are_charged() {
    const Inputs inputs{
        "timestamp,open,high,low,close,volume",  // header ending at EOF
        std::string(kHeaderCrlf) + "0,51,53,49,52,201",  // record ending at EOF
        kHeaderLf,
        "timestamp,open,high,low,close,volume\r",
    };
    Run run(inputs);
    const auto result = run.execute(byte_bound(inputs.total_bytes()));
    require_completed(result);
    CHECK(result.state.bytes == inputs.total_bytes());
    CHECK(result.coverage.source_bytes == inputs.source.size());
    CHECK(result.coverage.primary_bytes == inputs.primary.size());
    CHECK(result.coverage.auxiliary_bytes
          == inputs.auxiliary_a.size() + inputs.auxiliary_b.size());
    CHECK(result.coverage.source_rows == 0 && result.coverage.primary_rows == 1);
    CHECK(result.coverage.auxiliary_rows == 0);
    CHECK(result.coverage.source_streams == 1 && result.coverage.auxiliary_streams == 2);
    CHECK(result.coverage.source_complete && result.coverage.primary_complete);
    CHECK(result.coverage.auxiliary_complete);
    check_primary_prefix(run, 1);
}

void expect_byte_refusal(Run& run, std::uint64_t limit, std::uint64_t used,
                         std::uint64_t requested) {
    bool refused = false;
    try {
        const auto result = run.execute(byte_bound(limit));
        require_completed(result);
    } catch (const pineforge::coded<std::runtime_error>& error) {
        if (error.run_failure_code() != pineforge::RunFailureCode::selected_bytes_limit_exceeded) {
            throw;
        }
        refused = true;
        const auto failure = error.run_failure();
        CHECK(std::strcmp(pineforge::run_failure_code_name(failure.code),
                          "selected_bytes_limit_exceeded") == 0);
        CHECK(pineforge::run_failure_code_class(failure.code)
              == pineforge::RunFailureClass::resource);
        CHECK(!pineforge::run_failure_code_retryable(failure.code));
        CHECK(failure.args != nullptr);
        const std::string expected = "{\"limit\":\"" + std::to_string(limit)
            + "\",\"requested\":\"" + std::to_string(requested)
            + "\",\"used\":\"" + std::to_string(used) + "\"}";
        CHECK(*failure.args == expected);
        CHECK(std::strcmp(error.what(),
                          "Selected source-and-retained CSV byte capacity exceeded.") == 0);
        std::fprintf(stderr, "selected_bytes [%s] code=%s args=%s english=%s\n",
                     scenario, pineforge::run_failure_code_name(failure.code),
                     failure.args->c_str(), error.what());
    }
    CHECK(refused);
}

void zero_refuses_the_actual_source_header() {
    Run run(mixed_inputs());
    expect_byte_refusal(run, 0, 0, sizeof(kHeaderCrlf) - 1);
    CHECK(run.source.tellg() == std::streampos(sizeof(kHeaderCrlf) - 1));
    CHECK(run.primary.tellg() == std::streampos(0));
    CHECK(run.auxiliary_a.tellg() == std::streampos(0));
    CHECK(run.auxiliary_b.tellg() == std::streampos(0));
    check_primary_prefix(run, 0);
}

void header_equality_then_source_record_refusal() {
    Run run(mixed_inputs());
    const std::uint64_t header_bytes = sizeof(kHeaderCrlf) - 1;
    expect_byte_refusal(run, header_bytes, header_bytes, sizeof(kSourceFirst) - 1);
    CHECK(run.source.tellg()
          == std::streampos(header_bytes + sizeof(kSourceFirst) - 1));
    CHECK(run.primary.tellg() == std::streampos(0));
    check_primary_prefix(run, 0);
}

void primary_crossing_record_is_not_published() {
    const auto inputs = mixed_inputs();
    Run run(inputs);
    const std::uint64_t used = inputs.source.size()
        + sizeof(kHeaderLf) - 1 + sizeof(kPrimaryFirst) - 1;
    const std::uint64_t requested = sizeof(kPrimaryLast) - 1;
    expect_byte_refusal(run, used + requested - 1, used, requested);
    CHECK(run.primary.tellg() == std::streampos(inputs.primary.size()));
    CHECK(run.auxiliary_a.tellg() == std::streampos(0));
    CHECK(run.auxiliary_b.tellg() == std::streampos(0));
    check_primary_prefix(run, 1);
}

void one_byte_over_in_last_auxiliary_stream() {
    const auto inputs = mixed_inputs();
    Run run(inputs);
    const std::uint64_t requested = sizeof(kAuxiliaryLast) - 1;
    expect_byte_refusal(run, inputs.total_bytes() - 1,
                        inputs.total_bytes() - requested, requested);
    CHECK(run.auxiliary_b.tellg() == std::streampos(inputs.auxiliary_b.size()));
    check_primary_prefix(run, 2);
}

void malformed_record_does_not_reserve_unvalidated_bytes() {
    const std::string prefix = std::string(kHeaderCrlf) + kSourceFirst;
    Inputs inputs = mixed_inputs();
    inputs.source = prefix + "900000,21,23,19,22,invalid-volume\n";
    Run run(inputs);
    const auto result = run.execute(byte_bound(prefix.size()));
    CHECK(result.status == sc::DriverStatus::FormatError);
    CHECK(result.reason == sc::DriverReason::CsvValue);
    CHECK(result.role == sc::Role::Source && result.input_index == 0);
    CHECK(result.row_index == 2);
    CHECK(result.state.bytes == prefix.size());
    CHECK(result.coverage.source_bytes == prefix.size());
    CHECK(result.coverage.source_rows == 1 && !result.coverage.source_complete);
    CHECK(result.coverage.primary_bytes == 0 && result.coverage.auxiliary_bytes == 0);
    CHECK(run.primary.tellg() == std::streampos(0));
    check_primary_prefix(run, 0);
}

}  // namespace

int main() {
    const struct {
        const char* name;
        void (*run)();
    } cases[] = {
        {"equality/all roles/newlines", equality_includes_headers_all_roles_and_actual_newlines},
        {"absent byte bound", absent_byte_bound_does_not_charge_state},
        {"empty headers/EOF", empty_source_and_auxiliary_headers_are_charged},
        {"validated bytes only", malformed_record_does_not_reserve_unvalidated_bytes},
        {"explicit zero/header", zero_refuses_the_actual_source_header},
        {"header equality/Source record", header_equality_then_source_record_refusal},
        {"Primary prepublication refusal", primary_crossing_record_is_not_published},
        {"one byte over/Auxiliary aggregate", one_byte_over_in_last_auxiliary_stream},
    };
    try {
        for (const auto& test : cases) {
            scenario = test.name;
            test.run();
            std::printf("selected_bytes [%s]: completed\n", scenario);
        }
    } catch (const CheckFailure&) {
        std::fprintf(stderr, "selected_bytes: %d checks, 1 failure\n", checks);
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "NEEDS selected_bytes [%s]: %s\n", scenario, error.what());
        return 2;
    } catch (...) {
        std::fprintf(stderr, "NEEDS selected_bytes [%s]: nonstandard exception\n", scenario);
        return 2;
    }
    std::printf("selected_bytes: %d checks, 0 failures\n", checks);
    return 0;
}
