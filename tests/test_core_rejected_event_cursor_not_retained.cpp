// 1.7.0 review N-01 (verifier rev-read-verifyb1, lane b1 round 4): a refused
// request's external openings are never read through the caller's cursor
// after the call that refused it.
//
// The header makes a cursor call-local (native_order.hpp, OpeningInputCursor:
// "neither this object nor its callback/context is retained"). The core
// journals a refused request as it was given (RejectedEvent, and the attempted
// request of ReplaceRejectedEvent, NotWorkingEvent and InvalidHandleEvent), and
// since round 1 a journal page carries the request's external openings, read
// through the request's readback when the page is written. So the page spill
// that comes 2,048 events later called the reader of a refused request's
// cursor whose context had died with the call: a heap-use-after-free, from an
// unrelated command (BookSpill/JournalStore make_room). Round 4 copies the
// list when the refused event is built.
//
// The run (the core alone, its public C++ API): six refused commands, each
// over a close bound to two external openings through an OpeningInputCursor
// whose context is on the heap -- a submit and a replace of a live request
// (InvalidOwner: these plain forms carry no observations), and a replace of a
// handle that is not live, each in the prepared form (submit, replace) and in
// the direct form (apply_submit, apply_replace). Each call returns; then the
// default row frees that context (the caller's cursor destroyed, as a
// call-local one is) and `--poisoned` keeps it but makes every later read of
// it fail. Then more than JournalStore::kMaxResident (2,048) accepted submits
// page the six refused events.
//
// Expected: every later submit is accepted with no throw, and each paged
// refused event reads back its two handles in the order given. At the unfixed
// head the page spill reads the cursors: freed, AddressSanitizer reports a
// heap-use-after-free (the decisive control, the sanitizers profile; a Release
// build may read the freed bytes unnoticed); poisoned, the paging submit
// throws ScratchIoError(EIO, "native opening readback") on any build.
//
// Previous-epoch API only, so this TU compiles against the unfixed head.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
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

// The caller's own data behind its cursor, on the heap, so a read of it after
// it is freed is a heap-use-after-free AddressSanitizer reports.
struct Input {
    std::vector<no::RequestHandle> rows;
    bool poisoned = false;
};

Status read_input(void* context, std::uint64_t index, no::RequestHandle& out) {
    const auto& input = *static_cast<const Input*>(context);
    if (input.poisoned) return Status::io_error;
    if (index >= input.rows.size()) return Status::not_found;
    out = input.rows[static_cast<std::size_t>(index)];
    return Status::ok;
}

// The request a refused event recorded, or null.
const no::Request* refused_request(const no::CommandEvent& event) {
    if (const auto* rejected = std::get_if<no::RejectedEvent>(&event)) return &rejected->request;
    if (const auto* rejected = std::get_if<no::ReplaceRejectedEvent>(&event)) {
        return &rejected->attempted;
    }
    if (const auto* gone = std::get_if<no::NotWorkingEvent>(&event)) {
        return gone->attempted ? &*gone->attempted : nullptr;
    }
    if (const auto* invalid = std::get_if<no::InvalidHandleEvent>(&event)) {
        return invalid->attempted ? &*invalid->attempted : nullptr;
    }
    return nullptr;
}

struct Case {
    std::string label;
    std::size_t at = 0;    // the refused event's journal position
};

struct Core {
    no::RunIdentity run{"n-01-cursor-not-retained", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 1000;
    std::uint64_t ordinal = 1;
    std::vector<no::RequestHandle> given{{run, 2}, {run, 1}};
    std::vector<std::unique_ptr<Input>> inputs;
    bool poison = false;

    // Every later command takes counters past everything committed.
    void advance() {
        if (incarnation <= core.last_incarnation()) incarnation = core.last_incarnation() + 1;
        if (ordinal <= core.last_ordinal()) ordinal = core.last_ordinal() + 1;
    }
    no::RequestHandle submit_resting(const char* label) {
        no::Request request{no::Transact{1.0}, label, ""};
        request.trigger = no::Limit{50.0};
        advance();
        const auto result = core.submit(request, 0, incarnation, ordinal);
        REQUIRE(result.status == no::SubmitStatus::Accepted);
        REQUIRE(result.handle.has_value());
        return *result.handle;
    }
    // A close bound to `given` through a fresh call-local cursor.
    no::Request close(const std::string& label) {
        inputs.push_back(std::make_unique<Input>());
        inputs.back()->rows = given;
        no::Request request{no::Reduce{no::ExplicitUnits{1}}, label, "external"};
        request.owner = no::BindOpenings{
                {}, 7, std::make_shared<no::OpeningInputCursor>(
                               static_cast<std::uint64_t>(given.size()), inputs.back().get(),
                               &read_input)};
        return request;
    }
    // The call is over: its cursor is destroyed (or, poisoned, unreadable).
    Case after_call(const std::string& label) {
        Case done{label, core.history_end() - 1};
        REQUIRE(refused_request(core.history_at(done.at)) != nullptr);
        if (poison) {
            inputs.back()->poisoned = true;
        } else {
            inputs.back().reset();
        }
        return done;
    }
};

void check_reads_back(const Core& c, const Case& refused) {
    try {
        const no::Request* request = refused_request(c.core.history_at(refused.at));
        CHECK(request != nullptr);
        if (!request) return;
        CHECK(request->label == refused.label);
        const auto* owner = std::get_if<no::BindOpenings>(&request->owner);
        CHECK(owner != nullptr && owner->external_openings != nullptr);
        if (!owner || !owner->external_openings) return;
        const auto& external = *owner->external_openings;
        CHECK(external.count() == c.given.size());
        for (std::uint64_t i = 0; i != external.count() && i != c.given.size(); ++i) {
            no::RequestHandle got;
            CHECK(external.opening_at(i, got) == Status::ok);
            CHECK(got == c.given[static_cast<std::size_t>(i)]);
        }
    } catch (const std::exception& error) {
        ++checks;
        ++failures;
        std::fprintf(stderr, "%s: the paged event did not read back: %s\n",
                     refused.label.c_str(), error.what());
    }
}

void refused_cursors_are_not_read_after_the_call(bool poison) {
    Core c;
    c.poison = poison;
    const no::RequestHandle live = c.submit_resting("target");
    const no::RequestHandle gone{c.run, 1};    // never issued: not a live request
    const no::CommandContext context{};
    std::vector<Case> cases;

    // Prepared forms: submit, replace of a live request, replace of a handle
    // that is not live.
    {
        const no::Request request = c.close("submit");
        c.advance();
        REQUIRE(c.core.submit(request, 0, c.incarnation, c.ordinal).status
                == no::SubmitStatus::Rejected);
        cases.push_back(c.after_call("submit"));
    }
    {
        const no::Request request = c.close("replace-live");
        c.advance();
        REQUIRE(c.core.replace(live, request, 0, c.incarnation, c.ordinal).status
                == no::ReplaceStatus::ReplaceRejected);
        cases.push_back(c.after_call("replace-live"));
    }
    {
        const no::Request request = c.close("replace-gone");
        c.advance();
        const auto status = c.core.replace(gone, request, 0, c.incarnation, c.ordinal).status;
        REQUIRE(status == no::ReplaceStatus::InvalidHandle
                || status == no::ReplaceStatus::NotWorking);
        cases.push_back(c.after_call("replace-gone"));
    }
    // Direct forms of the same three.
    {
        const no::Request request = c.close("apply-submit");
        c.advance();
        REQUIRE(c.core.apply_submit(request, context, c.incarnation, c.ordinal).result.status
                == no::SubmitStatus::Rejected);
        cases.push_back(c.after_call("apply-submit"));
    }
    {
        const no::Request request = c.close("apply-replace-live");
        c.advance();
        REQUIRE(c.core.apply_replace(live, request, context, c.incarnation, c.ordinal)
                        .result.status
                == no::ReplaceStatus::ReplaceRejected);
        cases.push_back(c.after_call("apply-replace-live"));
    }
    {
        const no::Request request = c.close("apply-replace-gone");
        c.advance();
        const auto status =
                c.core.apply_replace(gone, request, context, c.incarnation, c.ordinal)
                        .result.status;
        REQUIRE(status == no::ReplaceStatus::InvalidHandle
                || status == no::ReplaceStatus::NotWorking);
        cases.push_back(c.after_call("apply-replace-gone"));
    }

    // More than the journal's resident bound of accepted events: the six
    // refused events are written to pages, and read back from them.
    std::size_t accepted = 0;
    for (std::size_t i = 0; i != kResidentEvents + 64; ++i) {
        no::Request rest{no::Transact{1.0}, "rest", ""};
        rest.trigger = no::Limit{50.0};
        c.advance();
        if (c.core.submit(rest, 0, c.incarnation, c.ordinal).status
            == no::SubmitStatus::Accepted) {
            ++accepted;
        }
    }
    CHECK(accepted == kResidentEvents + 64);
    for (const Case& refused : cases) {
        REQUIRE(c.core.history_end() - refused.at > kResidentEvents);
    }
    for (const Case& refused : cases) check_reads_back(c, refused);
}

}  // namespace

int main(int argc, char** argv) {
    // The twin row _poisoned is its own executable, built from this body with
    // PINEFORGE_B1_TWIN_FLAG (the dedicated scratch matching fixture).
#ifdef PINEFORGE_B1_TWIN_FLAG
    const char* const flag = PINEFORGE_B1_TWIN_FLAG;
    (void) argc;
    (void) argv;
#else
    const char* const flag = argc > 1 ? argv[1] : "";
#endif
    const bool poisoned = std::strcmp(flag, "--poisoned") == 0;
    try {
        refused_cursors_are_not_read_after_the_call(poisoned);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "refused event cursor not retained: %s\n", error.what());
    }
    std::printf("test_core_rejected_event_cursor_not_retained%s: %d checks, %d failures\n",
                poisoned ? " --poisoned" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
