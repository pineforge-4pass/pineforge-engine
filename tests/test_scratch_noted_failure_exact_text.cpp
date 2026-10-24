// Pin the complete translated default-directory failure, including exactly
// one outer system_error suffix. Expected text is independent of the helpers
// that strip and translate the inner scratch-create diagnostic.
#include "scratch_candidate_message_fixture.hpp"
#include "../src/report_scratch.hpp"
#include <pineforge/detail/id_pair_scratch_file.hpp>
#include <pineforge/native_order.hpp>

namespace {
namespace pd = pineforge::detail;
namespace no = pineforge::native_order;
using namespace s4e_tmp;

template<class Error, class Operation>
void expect_noted_exact(Operation operation, const char* prefix,
                        const std::string& missing, pineforge::RunFailureCode code) {
    // The resolver supplies this candidate note for ENOENT. The inner
    // "scratch create; " and its system_error suffix must be stripped before
    // the facade adds its own operation and one final system_error suffix.
    const std::string note = "TMPDIR '" + missing
        + "': directory check failed (errno 2: No such file or directory)";
    const std::string expected = std::string(prefix) + "; " + note
        + ": No such file or directory";
    try {
        operation();
    } catch (const Error& error) {
        require(error.code() == std::error_code(ENOENT, std::generic_category()),
                std::string(prefix) + " must retain ENOENT and its generic category");
        require(std::string(error.what()) == expected,
                "expected '" + expected + "', got '" + error.what() + "'");
        const auto failure = pineforge::classify_run_failure(error);
        require(failure.code == code, std::string(prefix) + " changed its failure code");
        require(code == pineforge::RunFailureCode::report_sink_failed
                    ? (failure.args && *failure.args == "{\"reason\":\"scratch_io\"}")
                    : !failure.args,
                std::string(prefix) + " changed its failure arguments");
        return;
    }
    throw std::runtime_error(std::string(prefix) + " unexpectedly succeeded");
}

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

void successor_noted_failure(const CandidateDirectory& directory) {
    Environment temporary("TMPDIR", directory.path.c_str());
    no::WorkingRequestCore core{{"noted-text-roots", 1}};
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto submitted = core.submit(request(), 0, incarnation, ordinal);
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
            "noted-text successor submit must be accepted");
    auto tail = *submitted.handle;
    core.retire_history(core.last_ordinal());
    const no::CommandContext context{};
    const auto replace = [&] {
        const auto installed = core.apply_replace(tail, request(), context, incarnation, ordinal);
        require(installed.result.status == no::ReplaceStatus::Replaced
                    && installed.result.successor,
                "noted-text successor replacement must be accepted");
        tail = *installed.result.successor;
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
    };
    // Match the existing candidate-roots witness: allow its 64 resident
    // replacements, then fail the first roots open without an explicit path.
    for (int i = 0; i < 64; ++i) replace();
    const auto before = core.last_ordinal();
    Environment missing("TMPDIR", directory.missing.c_str());
    expect_noted_exact<pd::AuxiliaryScratchIoError>(
        replace, "native successor roots create", directory.missing,
        pineforge::RunFailureCode::scratch_io_failed);
    require(core.last_ordinal() == before && core.find_live(tail),
            "failed noted-text roots create must precede logical mutation");
}

void row() {
    PlainEnvironment environment;
    CandidateDirectory directory;
    {
        Environment temporary("TMPDIR", directory.missing.c_str());
        expect_noted_exact<pd::ReportScratchIoError>(
            [] { pineforge::report_sink::ScratchFile file(""); },
            "report scratch create", directory.missing,
            pineforge::RunFailureCode::report_sink_failed);
        pd::IdPairScratchFile pairs;
        expect_noted_exact<pd::IdPairStoreError>(
            [&] { pairs.open(""); }, "id table create", directory.missing,
            pineforge::RunFailureCode::scratch_io_failed);
        require(!pairs.opened(), "failed noted-text id table create must publish no file");
    }
    successor_noted_failure(directory);
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_scratch_noted_failure_exact_text", row);
}
