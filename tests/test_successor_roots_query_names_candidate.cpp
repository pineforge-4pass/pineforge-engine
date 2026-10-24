#include "scratch_candidate_message_fixture.hpp"
#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/native_order.hpp>

namespace {
namespace no = pineforge::native_order;
using namespace s4e_tmp;

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

void row() {
    PlainEnvironment environment;
    CandidateDirectory directory;
    Environment temporary("TMPDIR", directory.path.c_str());
    no::WorkingRequestCore core{{"candidate-roots", 1}};
    std::uint64_t incarnation = 1, ordinal = 1;
    const auto submitted = core.submit(request(), 0, incarnation, ordinal);
    require(submitted.status == no::SubmitStatus::Accepted && submitted.handle,
            "successor candidate fixture submit must be accepted");
    auto tail = *submitted.handle;
    core.retire_history(core.last_ordinal());
    const no::CommandContext context{};
    const auto replace = [&] {
        auto installed = core.apply_replace(tail, request(), context, incarnation, ordinal);
        require(installed.result.status == no::ReplaceStatus::Replaced
                    && installed.result.successor,
                "successor candidate fixture replacement must be accepted");
        tail = *installed.result.successor;
        ++incarnation;
        ++ordinal;
        core.retire_history(core.last_ordinal());
    };
    // Keep other scratch users healthy through warmup. The first roots
    // mkstemp is at replacement 65, after filling its 64-row resident tail.
    for (int i = 0; i < 64; ++i) replace();
    const auto before = core.last_ordinal();
    Environment missing("TMPDIR", directory.missing.c_str());
    expect_candidate_failure<pineforge::detail::AuxiliaryScratchIoError>(
        replace, "native successor roots create", directory.missing,
        pineforge::RunFailureCode::scratch_io_failed);
    require(core.last_ordinal() == before && core.find_live(tail),
            "failed roots create must precede logical mutation");
}
} // namespace

int main() {
    return s4e_tmp::test_main("test_successor_roots_query_names_candidate", row);
}
