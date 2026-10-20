// CR-150 L22C: every sink, finalizer, scratch and certified-arena failure is
// an engine fault. Exercise the compiled registry and exception classifier.

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <string>
#include <system_error>

namespace {

using pineforge::RunFailureArgs;
using pineforge::RunFailureClass;
using pineforge::RunFailureCode;
using pineforge::RunFailureValue;

std::string args_json(const RunFailureValue& value) {
    return value.args ? *value.args : std::string("{}");
}

void expect_sink_failure(const RunFailureValue& value, const std::string& expected_args) {
    assert(value.code == RunFailureCode::report_sink_failed);
    assert(std::string(pineforge::run_failure_code_name(value.code)) == "report_sink_failed");
    assert(pineforge::run_failure_code_class(value.code) == RunFailureClass::engine_fault);
    assert(!pineforge::run_failure_code_retryable(value.code));
    assert(args_json(value) == expected_args);
}

void expect_rejected(const RunFailureArgs& args) {
    const auto value = pineforge::make_run_failure(RunFailureCode::report_sink_failed, args);
    assert(value.code == RunFailureCode::engine_invariant);
    assert(args_json(value) == "{}");
}

// Keep arbitrary registry-shape coverage separate from real I/O raiser tests.
class SinkFailureWithArgs final : public std::system_error, public pineforge::RunFailureInfo {
public:
    explicit SinkFailureWithArgs(const RunFailureArgs& args)
        : std::system_error(EIO, std::generic_category(), "report sink write failed"),
          RunFailureInfo(RunFailureCode::report_sink_failed, args) {}
};

void reason_rows() {
    const char* const reasons[] = {
        "arena_exhausted",
        "arena_io",
        "arena_backing",
        "scratch_io",
        "sink_io",
        "sink_peer_closed",
    };
    for (const char* reason : reasons) {
        const RunFailureArgs args{{"reason", reason}};
        const std::string expected = std::string("{\"reason\":\"") + reason + "\"}";
        expect_sink_failure(
            pineforge::make_run_failure(RunFailureCode::report_sink_failed, args), expected);
        bool caught = false;
        try {
            throw SinkFailureWithArgs(args);
        } catch (const std::exception& error) {
            caught = true;
            expect_sink_failure(pineforge::classify_run_failure(error), expected);
        }
        assert(caught);
    }
    expect_rejected({{"reason", "unknown_reason"}});
    expect_rejected({{"reason", 1}});
    expect_rejected({{"reason", nullptr}});
    expect_rejected({{"reason", "sink_io"}, {"reason", "sink_peer_closed"}});
}

void arena_counters() {
    // Values beyond 32 bits must stay JSON integers, in canonical key order.
    constexpr std::int64_t bytes = INT64_C(4294967296);
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed,
        {{"reason", "arena_exhausted"}, {"high_water_bytes", bytes}, {"arena_bytes", bytes}}),
        "{\"arena_bytes\":4294967296,\"high_water_bytes\":4294967296,"
        "\"reason\":\"arena_exhausted\"}");

    // All eleven scratch users and the default identity retain the complete
    // exhaustion payload through validation, JSON and exception classification.
    constexpr std::int64_t needed = INT64_C(1048576);
    const char* const stores[] = {
        "curve",
        "position_tail",
        "cohort",
        "exit_rank",
        "sequence_index",
        "sequence_index_idx",
        "erased_minima",
        "erased_minima_idx",
        "partial_cycles",
        "partial_cycles_idx",
        "successor_roots",
        "unknown",
    };
    for (const char* store : stores) {
        const RunFailureArgs args{
            {"store", store},
            {"reason", "arena_exhausted"},
            {"needed_bytes_lower_bound", needed},
            {"high_water_bytes", bytes},
            {"claimed_bytes", bytes},
            {"arena_bytes", bytes},
        };
        const std::string expected =
            std::string("{\"arena_bytes\":4294967296,\"claimed_bytes\":4294967296,"
                        "\"high_water_bytes\":4294967296,\"needed_bytes_lower_bound\":1048576,"
                        "\"reason\":\"arena_exhausted\",\"store\":\"") + store + "\"}";
        expect_sink_failure(
            pineforge::make_run_failure(RunFailureCode::report_sink_failed, args), expected);
        bool caught = false;
        try {
            throw SinkFailureWithArgs(args);
        } catch (const std::exception& error) {
            caught = true;
            expect_sink_failure(pineforge::classify_run_failure(error), expected);
        }
        assert(caught);
    }

    // Each optional argument is independently usable; no reason is required
    // for older callers that only attach a measured byte count.
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed, {{"arena_bytes", bytes}}),
        "{\"arena_bytes\":4294967296}");
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed, {{"claimed_bytes", bytes}}),
        "{\"claimed_bytes\":4294967296}");
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed, {{"high_water_bytes", bytes}}),
        "{\"high_water_bytes\":4294967296}");
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed, {{"needed_bytes_lower_bound", needed}}),
        "{\"needed_bytes_lower_bound\":1048576}");
    expect_sink_failure(pineforge::make_run_failure(
        RunFailureCode::report_sink_failed, {{"store", "unknown"}}),
        "{\"store\":\"unknown\"}");

    expect_rejected({{"arena_bytes", "4294967296"}});
    expect_rejected({{"arena_bytes", 4294967296.0}});
    expect_rejected({{"arena_bytes", true}});
    expect_rejected({{"claimed_bytes", "4294967296"}});
    expect_rejected({{"claimed_bytes", 4294967296.0}});
    expect_rejected({{"claimed_bytes", true}});
    expect_rejected({{"high_water_bytes", "4294967296"}});
    expect_rejected({{"high_water_bytes", 4294967296.0}});
    expect_rejected({{"high_water_bytes", false}});
    expect_rejected({{"needed_bytes_lower_bound", "1048576"}});
    expect_rejected({{"needed_bytes_lower_bound", 1048576.0}});
    expect_rejected({{"needed_bytes_lower_bound", false}});
    expect_rejected({{"reason", "arena_exhausted"}, {"store", "not_a_scratch_store"}});
    expect_rejected({{"store", 1}});
    expect_rejected({{"store", nullptr}});
    expect_rejected({{"undeclared", bytes}});
}

void existing_scratch_error() {
    // File-mode errors default to a reason-only payload.
    bool caught = false;
    try {
        throw pineforge::detail::ScratchIoError(EIO, "scratch write");
    } catch (const std::system_error& error) {
        caught = true;
        assert(error.code().value() == EIO);
        expect_sink_failure(pineforge::classify_run_failure(error), "{\"reason\":\"scratch_io\"}");
    }
    assert(caught);
}

}  // namespace

int main() {
    assert(pineforge::run_failure_code_class(RunFailureCode::report_sink_failed)
           == RunFailureClass::engine_fault);
    assert(!pineforge::run_failure_code_retryable(RunFailureCode::report_sink_failed));
    expect_sink_failure(
        pineforge::make_run_failure(RunFailureCode::report_sink_failed, {}), "{}");
    reason_rows();
    arena_counters();
    existing_scratch_error();
    std::puts("report sink failure catalog: registry, arguments and classifier checked");
    return 0;
}
