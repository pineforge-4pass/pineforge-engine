// B1-07: count source callbacks, not elapsed time. The base scans every
// unsorted member per observation, then scans observations again for duplicates.
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;
const no::RunIdentity kRun{"p2-b1-07-admission", 1};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct Members final : no::OpeningReadback {
    const std::vector<no::RequestHandle>& rows;
    mutable std::uint64_t reads = 0;
    explicit Members(const std::vector<no::RequestHandle>& input) : rows(input) {}
    std::uint64_t count() const noexcept override { return rows.size(); }
    Status opening_at(std::uint64_t i, no::RequestHandle& out) const noexcept override {
        ++reads;
        if (i >= rows.size()) return Status::not_found;
        out = rows[static_cast<std::size_t>(i)];
        return Status::ok;
    }
};

struct Observations final : no::OpeningObservationReadback {
    const std::vector<no::OpeningObservation>& rows;
    mutable std::uint64_t reads = 0;
    explicit Observations(const std::vector<no::OpeningObservation>& input) : rows(input) {}
    std::uint64_t count() const noexcept override { return rows.size(); }
    Status observation_at(std::uint64_t i, no::OpeningObservation& out) const noexcept override {
        ++reads;
        if (i >= rows.size()) return Status::not_found;
        out = rows[static_cast<std::size_t>(i)];
        return Status::ok;
    }
};

struct Input {
    std::vector<no::RequestHandle> members;
    std::vector<no::OpeningObservation> observations;
    std::int64_t cycle = 7;
};

Input descending(std::size_t n) {
    Input input;
    for (std::size_t i = n; i != 0; --i) {
        const no::RequestHandle handle{kRun, i};
        input.members.push_back(handle);
        input.observations.push_back(
            {handle, 7, no::PositionNonflat{7, no::Side::Long}, true});
    }
    return input;
}

struct Verdict {
    no::SubmitStatus status = no::SubmitStatus::Rejected;
    std::optional<no::RequestRejectReason> reason;
    std::string error_text;
    std::uint64_t reads = 0;
};

Verdict submit(const Input& input, bool external_members, bool external_observations) {
    no::WorkingRequestCore core(kRun);
    std::uint64_t incarnation = 10000;
    std::uint64_t ordinal = 1;
    auto members = std::make_shared<Members>(input.members);
    Observations observations(input.observations);
    no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "close", ""};
    request.owner = external_members
        ? no::BindOpenings{{}, input.cycle, members}
        : no::BindOpenings{input.members, input.cycle, {}};
    no::CommandContext context;
    if (external_observations) context.external_openings = &observations;
    else context.openings = input.observations;
    Verdict verdict;
    try {
        auto prepared = core.prepare_submit(request, context, incarnation, ordinal);
        verdict.status = prepared.predicted().status;
        verdict.reason = prepared.predicted().reason;
        auto installed = core.install_submit(std::move(prepared));
        require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                "admission token did not install");
        if (verdict.status == no::SubmitStatus::Rejected) {
            const auto& row = std::get<no::RejectedEvent>(core.history().back());
            require(row.reason == verdict.reason, "rejected event reason changed");
            const auto& retained = std::get<no::BindOpenings>(row.request.owner);
            require(no::opening_count(retained.openings, retained.external_openings.get())
                        == input.members.size(), "rejected membership count changed");
            for (std::size_t i = 0; i != input.members.size(); ++i) {
                no::RequestHandle got;
                require(no::opening_at(retained.openings, retained.external_openings.get(), i, got)
                            == Status::ok && got == input.members[i],
                        "rejected attempted membership order changed");
            }
        }
    } catch (const std::exception& error) {
        verdict.error_text = error.what();
    }
    verdict.reads = members->reads + observations.reads;
    return verdict;
}

void admission_reads() {
    for (const std::size_t n : {std::size_t{1000}, std::size_t{4000}}) {
        const auto input = descending(n);
        const auto verdict = submit(input, true, true);
        require(verdict.error_text.empty(), "valid external input raised an exception");
        require(verdict.status == no::SubmitStatus::Accepted && !verdict.reason,
                "valid descending external input was rejected");
        std::uint64_t levels = 0;
        for (std::size_t width = 1; width < n; width *= 2) ++levels;
        // Four linear source passes plus up to four reads per member per
        // binary-search/merge level, and eight fixed lookahead reads. This
        // follows the algorithm's work, not a measured callback count.
        const std::uint64_t bound = 4 * n * (levels + 1) + 8;
        std::printf("n=%zu callbacks=%llu bound=%llu\n", n,
                    static_cast<unsigned long long>(verdict.reads),
                    static_cast<unsigned long long>(bound));
        require(verdict.reads <= bound, "external admission exceeded O(n log n) reads");
    }
}

void differential_verdicts() {
    struct Case { Input input; bool accepted; };
    std::vector<Case> cases;
    cases.push_back({descending(4), true});
    auto ordered = descending(4);
    std::reverse(ordered.observations.begin(), ordered.observations.end());
    cases.push_back({ordered, true});
    std::reverse(ordered.members.begin(), ordered.members.end());
    cases.push_back({ordered, true});
    auto mixed = descending(4);
    std::swap(mixed.observations[0], mixed.observations[2]);
    cases.push_back({mixed, true});
    cases.push_back({descending(0), false});
    auto duplicate = descending(4);
    duplicate.members[1] = duplicate.members[0];
    cases.push_back({duplicate, false});
    duplicate = descending(4);
    duplicate.observations[1] = duplicate.observations[0];
    cases.push_back({duplicate, false});
    duplicate.members[1] = duplicate.members[0];
    cases.push_back({duplicate, false});
    auto missing = descending(4);
    missing.observations.pop_back();
    cases.push_back({missing, false});
    auto unknown = descending(4);
    unknown.observations[1].queried_opening.incarnation = 99;
    cases.push_back({unknown, false});
    auto foreign = descending(4);
    foreign.members[1].run.run_number = 2;
    cases.push_back({foreign, false});
    auto zero = descending(4);
    zero.members[1].incarnation = 0;
    cases.push_back({zero, false});
    auto stale = descending(4);
    stale.observations[1].queried_cycle = 8;
    cases.push_back({stale, false});
    auto dead = descending(4);
    dead.observations[1].has_live_matching_lot = false;
    cases.push_back({dead, false});
    auto side = descending(4);
    side.observations[1].current_position = no::PositionNonflat{7, no::Side::Short};
    cases.push_back({side, false});
    auto cycle = descending(4);
    cycle.cycle = 0;
    cases.push_back({cycle, false});

    for (const auto& row : cases) {
        const auto reference = submit(row.input, false, false);
        require(reference.error_text.empty(), "vector admission unexpectedly threw");
        require(reference.status == (row.accepted ? no::SubmitStatus::Accepted
                                                 : no::SubmitStatus::Rejected),
                "vector admission differs from the independent case verdict");
        require(row.accepted ? !reference.reason
                             : reference.reason == no::RequestRejectReason::InvalidOwner,
                "vector refusal reason changed");
        for (const bool borrowed_observations : {false, true}) {
            const auto external = submit(row.input, true, borrowed_observations);
            require(external.status == reference.status && external.reason == reference.reason,
                    "external admission verdict differs from vector admission");
            // The core's refusal surface is a typed reason, with no error
            // string. Compare exception text too: rejection must stay a
            // receipt rather than turn into a scratch-freeze exception.
            require(external.error_text == reference.error_text,
                    "external admission changed error text or threw instead of rejecting");
        }
    }
}
} // namespace

int main() {
    try {
        differential_verdicts();
        admission_reads();
        std::puts("test_external_openings_admission_reads: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_external_openings_admission_reads: FAIL: %s\n", error.what());
        return 1;
    }
}
