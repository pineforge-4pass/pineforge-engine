// Ordinary packed pages and single oversized extents share one position and
// ordinal index. Compare full event bytes, including external opening lists,
// across chunk boundaries and repeated eviction of the four reload slots.
#include <pineforge/native_order.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using Status = pineforge::detail::ScratchStatus;
constexpr std::size_t kPackingBytes = 32768;
constexpr std::size_t kResidentEvents = 2048;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

std::vector<std::byte> record(const no::CommandEvent& event) {
    const auto source = no::encode_journal_record(event);
    require(source.size() <= std::numeric_limits<std::size_t>::max(), "record size fits size_t");
    std::vector<std::byte> bytes(static_cast<std::size_t>(source.size()));
    require(source.read(0, {bytes.data(), bytes.size()}) == Status::ok,
            "encode complete event oracle");
    return bytes;
}

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& value) { return value.ordinal; }, event);
}

struct Openings final : no::OpeningObservationReadback {
    no::RunIdentity run;
    static constexpr std::uint64_t rows = 1613;
    explicit Openings(no::RunIdentity value) : run(std::move(value)) {}
    std::uint64_t count() const noexcept override { return rows; }
    Status observation_at(std::uint64_t index, no::OpeningObservation& out) const noexcept override {
        if (index >= rows) return Status::not_found;
        out = {{run, index + 1}, 7, no::PositionNonflat{7, no::Side::Long}, true};
        return Status::ok;
    }
    static Status read(void* context, std::uint64_t index, no::RequestHandle& out) {
        const auto& self = *static_cast<const Openings*>(context);
        if (index >= rows) return Status::not_found;
        out = {self.run, index + 1};
        return Status::ok;
    }
};

struct Listed final : no::OpeningReadback {
    std::vector<no::RequestHandle> rows;
    std::uint64_t count() const noexcept override { return rows.size(); }
    Status opening_at(std::uint64_t index, no::RequestHandle& out) const noexcept override {
        if (index >= rows.size()) return Status::not_found;
        out = rows[static_cast<std::size_t>(index)];
        return Status::ok;
    }
};

struct Core {
    no::RunIdentity run{"journal-mixed-extents", 1};
    no::WorkingRequestCore core{run};
    std::uint64_t incarnation = 100000;
    std::uint64_t ordinal = 1;
    bool direct;
    std::vector<std::vector<std::byte>> expected;
    std::vector<std::uint64_t> ordinals;
    std::vector<std::weak_ptr<const no::RequestDefinition>> owners;
    explicit Core(bool use_direct) : direct(use_direct) {}

    void advance() {
        if (incarnation <= core.last_incarnation()) incarnation = core.last_incarnation() + 1;
        ordinal = core.last_ordinal() + 3; // Real holes exercise the ordinal index too.
    }
    void capture() {
        const auto& event = core.history().back();
        expected.push_back(record(event));
        ordinals.push_back(ordinal_of(event));
    }
    no::SubmitResult submit(const no::Request& request, const no::CommandContext& context = {}) {
        advance();
        no::SubmitResult result;
        if (direct) {
            result = core.apply_submit(request, context, incarnation, ordinal).result;
        } else {
            auto prepared = core.prepare_submit(request, context, incarnation, ordinal);
            auto installed = core.install_submit(std::move(prepared));
            require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                    "prepared submit installs");
            result = std::get<no::CommandInstalled<no::SubmitResult>>(installed).result;
        }
        capture();
        return result;
    }
    no::RequestHandle replace(const no::RequestHandle& handle, const no::Request& request) {
        advance();
        const auto result = direct
            ? core.apply_replace(handle, request, {}, incarnation, ordinal).result
            : core.replace(handle, request, 0, incarnation, ordinal);
        require(result.status == no::ReplaceStatus::Replaced && result.successor,
                "mixed replacement is installed");
        capture();
        return *result.successor;
    }
    void cancel(const no::RequestHandle& handle) {
        advance();
        const auto result = direct
            ? core.apply_cancel(handle, ordinal).result : core.cancel(handle, ordinal);
        require(result.status == no::CancelStatus::Cancelled, "mixed cancellation is installed");
        capture();
    }
    void filler() {
        require(submit({no::Transact{0.0}, "small-refused", "kept verbatim"}).status
                    == no::SubmitStatus::Rejected,
                "mixed filler records exactly one refusal");
    }
};

void mixed_pages(bool direct) {
    Core c(direct);
    constexpr std::size_t sizes[]{7, 503, kPackingBytes - 256, kPackingBytes + 1,
                                 2 * kPackingBytes + 5, 7 * kPackingBytes + 3};
    for (std::size_t i = 0; i < 24; ++i) {
        const auto size = sizes[i % 6];
        no::Request request{no::Transact{1.0 + i}, "mixed-" + std::to_string(i),
                            std::string(size, static_cast<char>('a' + i % 26))};
        request.trigger = no::Limit{45.0 + i};
        const auto accepted = c.submit(request);
        require(accepted.status == no::SubmitStatus::Accepted && accepted.handle,
                "mixed acceptance");
        c.owners.push_back(std::get<no::AcceptedEvent>(c.core.history().back()).definition);
        if (size > kPackingBytes)
            require(c.expected.back().size() > kPackingBytes, "real oversized encoded acceptance");
        request.label += "-successor";
        request.comment = std::string(sizes[(i + 3) % 6], static_cast<char>('A' + i % 26));
        request.trigger = no::StopLimit{90.0 + i, 91.0 + i};
        const auto successor = c.replace(*accepted.handle, request);
        c.owners.push_back(std::get<no::ReplacedEvent>(
            c.core.history().back()).successor_definition);
        c.cancel(successor);
        // Vary where a large event lands inside a 32-event resident chunk.
        for (std::size_t j = 0; j <= i % 5; ++j) c.filler();
    }

    // Large membership with short text: this must use the same extent path
    // as large labels, with every handle and the cancellation authority intact.
    Openings observations(c.run);
    no::Request close{no::Reduce{no::ExplicitUnits{1.0}}, "external-close", "short text"};
    close.owner = no::BindOpenings{{}, 7,
        std::make_shared<no::OpeningInputCursor>(Openings::rows, &observations, Openings::read)};
    no::CommandContext context;
    context.external_openings = &observations;
    const auto external_at = c.expected.size();
    const auto accepted_close = c.submit(close, context);
    require(accepted_close.status == no::SubmitStatus::Accepted && accepted_close.handle,
            "large external list is accepted");
    require(Openings::rows * sizeof(no::RequestHandle) > kPackingBytes
                && c.expected.back().size() > kPackingBytes,
            "actual encoded opening handles exceed the packing target");
    c.owners.push_back(std::get<no::AcceptedEvent>(c.core.history().back()).definition);
    c.cancel(*accepted_close.handle);

    // A refused external list is preserved in caller order even when that
    // order contains invalid handles; the caller can reuse it after submit.
    auto caller = std::make_shared<Listed>();
    const no::RunIdentity foreign{"journal-mixed-foreign", 2};
    for (std::uint64_t i = 0; i < Openings::rows + 19; ++i)
        caller->rows.push_back({c.run, Openings::rows + 19 - i});
    caller->rows[0] = {c.run, 0};
    caller->rows[1] = {foreign, 3};
    caller->rows[2] = caller->rows[3];
    close.label = "external-refused";
    close.owner = no::BindOpenings{{}, 7, caller};
    const auto refused_at = c.expected.size();
    require(c.submit(close).status == no::SubmitStatus::Rejected, "bad opening list is refused");
    require(c.expected.back().size() > kPackingBytes, "refused event is really oversized");
    caller->rows.assign(1, {foreign, 999});

    const auto mixed_end = c.expected.size();
    for (std::size_t i = 0; i < kResidentEvents + 64; ++i) c.filler();
    require(c.core.history_end() == c.expected.size() && c.core.history_base() == 0
                && c.core.live().empty(), "all mixed events remain retained in absolute order");
    for (const auto& owner : c.owners)
        require(owner.expired(), "mixed definitions left resident storage before reload");

    std::size_t position = 0;
    for (const auto& event : c.core.history()) {
        require(position < c.expected.size() && record(event) == c.expected[position],
                "forward view preserves every field and event order");
        ++position;
    }
    require(position == c.expected.size(), "forward reader visits the complete journal");
    // Reverse the spilled prefix to evict ordinary and oversized reloads from
    // all four cache slots, then resolve those same events by ordinal.
    for (std::size_t i = mixed_end; i-- > 0;) {
        require(record(c.core.history_at(i)) == c.expected[i], "mixed cache eviction reload");
        const auto* found = c.core.event_at({c.run, c.ordinals[i]});
        require(found && record(*found) == c.expected[i], "mixed ordinal lookup");
        require(c.core.event_at({c.run, c.ordinals[i] - 1}) == nullptr, "ordinal holes stay empty");
    }
    require(record(c.core.history_at(external_at)) == c.expected[external_at],
            "accepted external membership reloads after cache churn");
    require(record(c.core.history_at(refused_at)) == c.expected[refused_at],
            "refused external membership reloads without sorting or truncation");
}
} // namespace

int main() {
    try {
        mixed_pages(false);
        mixed_pages(true);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_journal_oversized_event_mixed: %s\n", error.what());
        return 1;
    }
    return 0;
}
