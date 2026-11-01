// Spilled-book reads preserve the complete sequence across reader-ring reuse.
// Include the implementation verbatim to complete the
// private BookSpill type. No product accessors, macro substitutions or seams.
#include "../src/native_order.cpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace memory_book_reader_ring {
namespace no = pineforge::native_order;
using Handle = no::RequestHandle;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Spill { friend auto access(Spill); };
template struct Access<Spill, &no::WorkingRequestCore::book_spill_>;
// WorkingRequestCore::clear_unbound is private. Use the same member-function pointer pattern as
// tests/test_id_adapter_storage.cpp's CohortFor, without a product change.
struct ClearUnbound { friend auto access(ClearUnbound); };
template struct Access<ClearUnbound, &no::WorkingRequestCore::clear_unbound>;

std::size_t checks = 0, failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [reader-ring] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define MEM_CHECK(expression) \
    ::memory_book_reader_ring::check(bool(expression), #expression, __LINE__)
#define MEM_NEED(expression) do { \
    if (!MEM_CHECK(expression)) throw ::memory_book_reader_ring::PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // Unsafe fixture prerequisites leave this trial; dispatch continues.
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

// Independently read from native_order.cpp's kBookMax and
// native_order_book.hpp's kSide declaration.
// Independent expected values ensure that replacing the ring with a
// fixed-size buffer disagrees with this model.
constexpr std::size_t kResident = 4096;
constexpr std::size_t kSlots = 8;

struct Core {
    no::RunIdentity identity{"reader-ring", 1};
    no::WorkingRequestCore core{identity};
    std::uint64_t incarnation = 10000, ordinal = 1;

    Handle submit(const no::Request& request, const no::CommandContext& context = {}) {
        incarnation = std::max(incarnation, core.last_incarnation() + 1);
        ordinal = std::max(ordinal, core.last_ordinal() + 1);
        const auto installed = core.apply_submit(request, context, incarnation, ordinal);
        MEM_NEED(installed.result.status == no::SubmitStatus::Accepted
                 && installed.result.handle);
        return *installed.result.handle;
    }
};

no::Request resting() {
    no::Request request{no::Transact{1.0}, "resident-prefix", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

struct Payload {
    Handle handle;
    std::string label, comment;
    std::vector<Handle> openings;

    std::uint64_t bytes() const {
        // Logical owned payload, not allocator capacity or RSS. make_live()
        // copies BindOpenings into OpeningsClose: two handle arrays plus
        // the definition's two strings. Handles contain an interned run key.
        return label.size() + comment.size()
            + 2 * openings.size() * sizeof(Handle);
    }
};

bool matches(const no::LiveRequest& row, const Payload& expected) {
    const auto* owner = std::get_if<no::BindOpenings>(&row.request().owner);
    const auto* authority = std::get_if<no::OpeningsClose>(&row.authority);
    return row.handle() == expected.handle
        && row.request().label == expected.label
        && row.request().comment == expected.comment
        && owner && authority
        && owner->cycle == 7 && authority->cycle == 7
        && authority->side == no::Side::Long
        && !owner->external_openings && !authority->external_openings
        && owner->openings == expected.openings
        && authority->openings == expected.openings;
}

std::uint64_t payload_bytes(const no::LiveRequest& row) {
    const auto* owner = std::get_if<no::BindOpenings>(&row.request().owner);
    const auto* authority = std::get_if<no::OpeningsClose>(&row.authority);
    // A wrong variant also fails matches(); never dereference it after failure.
    return row.request().label.size() + row.request().comment.size()
        + ((owner ? owner->openings.size() : 0)
           + (authority ? authority->openings.size() : 0)) * sizeof(Handle);
}

struct Result { std::size_t slots; std::uint64_t bytes; };

Result exercise(std::size_t requests, std::size_t scale) {
    Core c;
    // make_book_room() runs BEFORE the next push, and spills live_.back().
    // Keep C-1 prefix rows resident. Each selected close becomes the newest
    // resident, then the following submit spills it. One final push spills
    // the last selected close, so all model handles below are actually cold.
    for (std::size_t i = 0; i < kResident - 1; ++i) c.submit(resting());
    std::vector<Payload> submitted;
    submitted.reserve(requests);
    for (std::size_t i = 0; i < requests; ++i) {
        Payload expected;
        expected.label = std::string((32 + i % 7) * scale * 8,
                                     static_cast<char>('A' + i % 26));
        expected.comment = std::string((19 + i % 3) * scale,
                                       static_cast<char>('a' + i % 26));
        for (std::size_t j = 0; j < (3 + i % 5) * scale; ++j)
            expected.openings.push_back({c.identity, j + 1});
        no::Request request{no::Reduce{no::ExplicitUnits{1}},
                            expected.label, expected.comment};
        request.trigger = no::Limit{50.0};
        auto reversed = expected.openings;
        std::reverse(reversed.begin(), reversed.end());
        request.owner = no::BindOpenings{std::move(reversed), 7, {}};
        no::CommandContext context;
        // Physical opening observations are the C++ host's input contract;
        // this unit drives the real core admission and readback paths.
        for (const auto& opening : expected.openings)
            context.openings.push_back(
                {opening, 7, no::PositionNonflat{7, no::Side::Long}, true});
        expected.handle = c.submit(request, context);
        submitted.push_back(std::move(expected));
    }
    c.submit(resting());
    const auto& spill = c.core.*access(Spill{});
    MEM_NEED(spill != nullptr);
    MEM_CHECK(c.core.live().size() == kResident + requests);
    MEM_CHECK(spill->alive == requests && spill->rows.size() == requests);
    MEM_CHECK(spill->blobs.size() > 0); // Real spill insert evidence, not the bound.
    MEM_CHECK(spill->side.size() == kSlots && spill->side_next == 0);
    MEM_CHECK(std::all_of(spill->side.begin(), spill->side.end(),
                         [](const auto& entry) { return !entry; }));

    // Drop the acceptance journal through its public retire site. A decode
    // now owns its definition rather than reusing a journal-resident one.
    // visit_book() used by retirement does not call keep().
    const auto end = c.core.history_end();
    MEM_NEED(c.core.retire_history(c.core.last_ordinal()) == end);
    MEM_NEED(c.core.history_base() == c.core.history_end());

    std::array<const Payload*, kSlots> expected_ring{};
    std::array<std::weak_ptr<const no::LiveRequest>, kSlots> lifetime{};
    std::size_t reads = 0, actual_count = 0;
    std::uint64_t actual_bytes = 0;
    // 17 and 68 are deliberately not multiples of eight. Three full passes
    // exercise repeated handles and leave different nonzero write cursors.
    for (std::size_t pass = 0; pass < 3; ++pass) {
        for (const auto& expected : submitted) {
            const std::size_t slot = reads % kSlots;
            const auto overwritten = lifetime[slot]; // Weak: cannot retain it.
            const bool replacing = expected_ring[slot] != nullptr;
            const auto before_reads = c.core.book_lookup_reads().rows;
            auto held = c.core.hold_live(expected.handle); // spilled_held -> keep.
            MEM_NEED(held != nullptr);
            MEM_CHECK(c.core.book_lookup_reads().rows == before_reads + 1);
            MEM_CHECK(matches(*held, expected));
            MEM_CHECK(slot < spill->side.size() && spill->side[slot].get() == held.get());
            expected_ring[slot] = &expected;
            lifetime[slot] = held;
            ++reads;
            held.reset(); // The ring alone must retain the decoded row now.
            MEM_CHECK(!lifetime[slot].expired());
            if (replacing) MEM_CHECK(overwritten.expired());

            bool whole_ring = spill->side.size() == expected_ring.size();
            actual_count = 0;
            actual_bytes = 0;
            std::uint64_t expected_bytes = 0;
            for (const auto* entry : expected_ring)
                if (entry) expected_bytes += entry->bytes();
            for (std::size_t s = 0; s < spill->side.size(); ++s) {
                const auto& entry = spill->side[s];
                if (entry) {
                    ++actual_count;
                    actual_bytes += payload_bytes(*entry);
                }
                if (s >= expected_ring.size()) {
                    whole_ring = false;
                } else if (expected_ring[s]) {
                    whole_ring = whole_ring && entry && matches(*entry, *expected_ring[s]);
                } else {
                    whole_ring = whole_ring && !entry;
                }
            }
            MEM_CHECK(actual_count == std::min(reads, kSlots));
            MEM_CHECK(whole_ring);
            MEM_CHECK(spill->side_next == reads % kSlots);
            MEM_CHECK(actual_bytes == expected_bytes);
            MEM_CHECK(spill->alive == requests); // Reads do not promote the book.
        }
    }
    const Result result{actual_count, actual_bytes};
    std::vector<std::weak_ptr<const no::LiveRequest>> actual_survivors;
    for (const auto& entry : spill->side)
        if (entry) actual_survivors.emplace_back(entry);
    // WorkingRequestCore::clear_unbound resets book_spill_; BookSpill's
    // reader ring owns the remaining decoded rows.
    // Only weak observers remain here, so both lifetime assertions still apply.
    (c.core.*access(ClearUnbound{}))();
    MEM_CHECK(std::all_of(actual_survivors.begin(), actual_survivors.end(),
                         [](const auto& entry) { return entry.expired(); }));
    MEM_CHECK(std::all_of(lifetime.begin(), lifetime.end(),
                         [](const auto& entry) { return entry.expired(); }));
    return result;
}
} // namespace memory_book_reader_ring

int main() {
    using namespace memory_book_reader_ring;
    std::optional<Result> small, longer, wider;
    run("17 spilled closes, payload scale 1", [&] { small = exercise(17, 1); });
    run("68 spilled closes, payload scale 1", [&] { longer = exercise(68, 1); });
    run("17 spilled closes, payload scale 4", [&] { wider = exercise(17, 4); });
    run("independent read-count and input-payload comparisons", [&] {
        MEM_NEED(small && longer && wider);
        MEM_CHECK(small->slots == kSlots && longer->slots == small->slots
                  && wider->slots == small->slots);
        // Identical input identities/read order at scales 1 and 4; every
        // string length and both opening counts were multiplied by four.
        MEM_CHECK(small->bytes > 0 && wider->bytes == 4 * small->bytes);
    });
    std::printf("memory proof book_reader_ring: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
