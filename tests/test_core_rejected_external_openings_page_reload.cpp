// 1.7.0 review F-04 (argon cross-read of lane b1): a refused request's
// external openings read back from a journal page as they were journaled.
//
// The core records a refused submit's request as it was given (RejectedEvent;
// prepare_submit journals it before canonicalize_owner freezes anything). A
// request bound to external openings (BindOpenings::external_openings) can be
// refused precisely because that list is bad: a handle named twice, a zero
// incarnation, another run's handle. Round 1 made a journal page carry the
// list and re-freeze it on every reload; the freeze refuses such a list, so
// the reload threw ScratchIoError(EIO, "native opening freeze") and every
// event of that page became unreadable, and a valid list in caller order
// came back sorted.
//
// The run (the core alone, its public C++ API): four refused submits, each a
// close bound to an external list -- a duplicate, a zero incarnation, a
// handle of another run, and two valid handles out of order -- then more
// than JournalStore::kMaxResident (2,048) further events, so the four
// RejectedEvents are read back from a page. Each must read back, without a
// throw, as the request it recorded: the same label and the same handles in
// the same order.
//
// Round 4 (verifier N-01): the list is the caller's, and the header makes it
// call-local (OpeningInputCursor: neither the object nor its context is
// retained). So once each refusing call has returned, the caller reuses its
// list (here: overwrites every handle), and the journal must still read back
// what was given at the call, resident and paged. Before the round-4 fix the
// event kept the caller's object: the resident read answered the overwritten
// handles, and the page was written from them.
//
// Previous-epoch API only, so this TU compiles against the unfixed head.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;

int failures = 0;
int checks = 0;
#define CHECK(x)                                                                    \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
        }                                                                           \
    } while (false)
#define REQUIRE(x)                                                                  \
    do {                                                                            \
        ++checks;                                                                   \
        if (!(x)) {                                                                 \
            ++failures;                                                             \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            throw std::runtime_error(#x);                                           \
        }                                                                           \
    } while (false)

constexpr std::size_t kResidentEvents = 2048;   // JournalStore::kMaxResident

// An external list held by the caller, read in the order given.
struct Listed final : no::OpeningReadback {
    std::vector<no::RequestHandle> rows;
    explicit Listed(std::vector<no::RequestHandle> value) : rows(std::move(value)) {}
    std::uint64_t count() const noexcept override { return rows.size(); }
    Status opening_at(std::uint64_t i, no::RequestHandle& out) const noexcept override {
        if (i >= rows.size()) return Status::not_found;
        out = rows[static_cast<std::size_t>(i)];
        return Status::ok;
    }
};

struct Refused {
    std::string label;
    std::vector<no::RequestHandle> handles;
    std::size_t at = 0;       // the RejectedEvent's journal position
    std::shared_ptr<Listed> caller;   // the caller's own list object
};

struct Core {
    no::RunIdentity run{"f-04-refused-openings", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1000;
    std::uint64_t ordinal = 1;

    // Every later command takes counters past everything committed.
    void advance() {
        if (incarnation <= core.last_incarnation()) incarnation = core.last_incarnation() + 1;
        if (ordinal <= core.last_ordinal()) ordinal = core.last_ordinal() + 1;
    }
    void submit_resting() {
        no::Request request{no::Transact{1.0}, "rest", ""};
        request.trigger = no::Limit{50.0};
        advance();
        REQUIRE(core.submit(request, 0, incarnation, ordinal).status
                == no::SubmitStatus::Accepted);
    }
    // A close bound to `refused.handles` as an external list; the core
    // refuses it and journals it as given.
    void submit_refused(Refused& refused) {
        no::Request close{no::Reduce{no::ExplicitUnits{1}}, refused.label, "external"};
        refused.caller = std::make_shared<Listed>(refused.handles);
        close.owner = no::BindOpenings{{}, 7, refused.caller};
        advance();
        const auto result = core.submit(close, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Rejected);
        refused.at = core.history_end() - 1;
        REQUIRE(std::get_if<no::RejectedEvent>(&core.history_at(refused.at)) != nullptr);
    }
};

// The refused event at `refused.at`, read back: the request it recorded, the
// same external handles in the same order.
void check_reads_back(const Core& c, const Refused& refused) {
    try {
        const no::CommandEvent& event = c.core.history_at(refused.at);
        const auto* rejected = std::get_if<no::RejectedEvent>(&event);
        CHECK(rejected != nullptr);
        if (!rejected) return;
        CHECK(rejected->request.label == refused.label);
        const auto* owner = std::get_if<no::BindOpenings>(&rejected->request.owner);
        CHECK(owner != nullptr && owner->external_openings != nullptr);
        if (!owner || !owner->external_openings) return;
        const auto& external = *owner->external_openings;
        CHECK(external.count() == refused.handles.size());
        for (std::uint64_t i = 0; i != external.count() && i != refused.handles.size(); ++i) {
            no::RequestHandle got;
            CHECK(external.opening_at(i, got) == Status::ok);
            CHECK(got == refused.handles[static_cast<std::size_t>(i)]);
        }
    } catch (const std::exception& error) {
        ++checks;
        ++failures;
        std::fprintf(stderr, "%s: the paged event did not read back: %s\n",
                     refused.label.c_str(), error.what());
    }
}

void refused_openings_read_back_as_journaled() {
    Core c;
    const no::RunIdentity other{"f-04-another-run", 2};
    std::vector<Refused> refused{
        {"duplicate", {{c.run, 1}, {c.run, 1}}},
        {"zero-incarnation", {{c.run, 0}, {c.run, 2}}},
        {"another-run", {{other, 1}}},
        {"out-of-order", {{c.run, 3}, {c.run, 1}}},
    };
    for (Refused& row : refused) c.submit_refused(row);
    // Resident, every refused event reads back as given (a precondition).
    for (const Refused& row : refused) check_reads_back(c, row);

    // Every refusing call has returned: the caller reuses its lists. The
    // journal holds what was given at the call, not the caller's object.
    for (Refused& row : refused) {
        for (auto& handle : row.caller->rows) handle = no::RequestHandle{other, 99};
    }
    for (const Refused& row : refused) check_reads_back(c, row);

    // More than the journal's resident bound of events after them: the
    // refused events are read back from a page.
    for (std::size_t i = 0; i != kResidentEvents + 64; ++i) c.submit_resting();
    for (const Refused& row : refused) {
        REQUIRE(c.core.history_end() - row.at > kResidentEvents);
    }
    for (const Refused& row : refused) check_reads_back(c, row);
}

}  // namespace

int main() {
    try {
        refused_openings_read_back_as_journaled();
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "refused external openings page reload: %s\n", error.what());
    }
    std::printf("test_core_rejected_external_openings_page_reload: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
