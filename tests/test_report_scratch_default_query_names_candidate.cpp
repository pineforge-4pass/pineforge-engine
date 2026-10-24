#include "scratch_candidate_message_fixture.hpp"
#include "../src/report_scratch.hpp"

namespace {
void row() {
    using namespace s4e_tmp;
    PlainEnvironment environment;
    CandidateDirectory directory;
    Environment temporary("TMPDIR", directory.missing.c_str());
    expect_candidate_failure<pineforge::detail::ReportScratchIoError>(
        [] { pineforge::report_sink::ScratchFile file(""); },
        "report scratch create", directory.missing, pineforge::RunFailureCode::report_sink_failed);
    try {
        pineforge::report_sink::ScratchFile file("/dev/shm");
    } catch (const pineforge::detail::ReportScratchIoError& error) {
        const std::string text = error.what();
        require(error.code().value() == ENODEV && text.find("report scratch create") == 0
                    && text.find("scratch requires disk backing") != std::string::npos
                    && text.find("explicit directory '/dev/shm': tmpfs") != std::string::npos,
                "report create translator must also retain the explicit memory refusal");
        return;
    }
    throw std::runtime_error("explicit tmpfs report scratch unexpectedly succeeded");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_report_scratch_default_query_names_candidate", row);
}
