#include <pineforge/detail/journal_codec.hpp>
#include "../src/source/admission_journal_codec.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using Status = pd::ScratchStatus;
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t seed = 1469598103934665603ULL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
double real(std::uint64_t bits) {
    double result = 0;
    std::memcpy(&result, &bits, sizeof result);
    return result;
}
void word(Bytes& out, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) out.push_back(static_cast<std::byte>(value >> (8 * i)));
}
void text(Bytes& out, const std::string& value) {
    word(out, value.size());
    for (unsigned char c : value) out.push_back(static_cast<std::byte>(c));
}
struct Model {
    std::vector<ad::Field> fields;
    explicit Model(const ad::Event& event) {
        ad::reflect(event, "event", [&](const ad::Field& field) { fields.push_back(field); });
    }
    Bytes bytes(bool storage) const {
        Bytes out;
        if (storage) word(out, fields.size());
        for (const auto& field : fields) {
            text(out, field.path); word(out, field.value.index());
            std::visit([&](const auto& value) {
                using T = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<T, std::string>) text(out, value);
                else if constexpr (std::is_same_v<T, double>) {
                    std::uint64_t bits = 0;
                    std::memcpy(&bits, &value, sizeof bits);
                    if (!storage) {
                        if (value == 0) bits = 0;
                        else if (std::isnan(value)) bits = 0x7ff8000000000000ULL;
                    }
                    word(out, bits);
                } else word(out, static_cast<std::uint64_t>(value));
            }, field.value);
        }
        return out;
    }
    std::uint64_t fold(std::uint64_t digest) const {
        for (const auto byte : bytes(false))
            digest = (digest ^ std::to_integer<unsigned>(byte)) * 1099511628211ULL;
        return digest;
    }
};
class Source final : public pd::ScratchByteSource {
    const pd::ScratchByteSource& source_;
public:
    std::uint64_t fault = std::numeric_limits<std::uint64_t>::max();
    mutable std::uint64_t reads = 0;
    mutable std::uint64_t faults = 0;
    mutable bool last_failed = false;
    bool fail_once = false;
    const pd::ReportScratchIoError* typed_error = nullptr;
    explicit Source(const pd::ScratchByteSource& source) : source_(source) {}
    std::uint64_t size() const noexcept override { return source_.size(); }
    const pd::ReportScratchIoError* carried_failure() const noexcept override {
        return last_failed ? typed_error : nullptr;
    }
    Status read(std::uint64_t offset, pd::ScratchBuffer out) const noexcept override {
        ++reads;
        if (fault >= offset && fault - offset < out.size && (!fail_once || faults == 0)) {
            ++faults; last_failed = true;
            return Status::io_error;
        }
        const auto status = source_.read(offset, out);
        last_failed = status == Status::io_error;
        return status;
    }
};
Status legacy_fold(const pd::ScratchByteSource& source, pd::ScratchBuffer page,
                   ad::JournalRecordSummary& summary, std::uint64_t& digest,
                   std::uint64_t after_sequence = 0) {
    ad::JournalRecordSummary candidate;
    auto status = ad::scan_journal_record(source, page, candidate);
    if (status != Status::ok) return status;
    auto next = digest;
    if (candidate.sequence > after_sequence) status = ad::fold_journal_record(source, page, next);
    if (status == Status::ok) { summary = candidate; digest = next; }
    return status;
}
bool same(const ad::JournalRecordSummary& a, const ad::JournalRecordSummary& b) {
    return a.sequence == b.sequence && a.rejected_bar == b.rejected_bar && a.rejected == b.rejected;
}
ad::Event command(std::size_t length) {
    auto input = std::make_shared<ad::CommandObservation>();
    input->command = 17; input->bar = -7;
    input->id.resize(length);
    for (std::size_t i = 0; i != length; ++i) input->id[i] = static_cast<char>((37 * i + 19) & 255);
    input->oca_name = std::string("a\0b", 3);
    input->requested_quantity = -0.0;
    input->prices = {real(0x7ff8000000000041ULL), real(0xfff0000000000083ULL)};
    input->configuration.slippage = std::numeric_limits<int>::min();
    input->configuration.default_quantity_value = real(0xfff0000000000000ULL);
    input->original_sizing = ad::SizingObservation{0.0, -0.0, 1.0,
        real(0x7ff0000000000001ULL), real(0xfff8000000000041ULL)};
    return ad::CommandEvent{std::move(input), ad::Outcome::RejectedPricedCap, 9, {}, {3, 2, 1}};
}
ad::Event review(std::size_t count) {
    ad::ReviewEvent result;
    result.receipt = {19, ad::Checkpoint::TerminalGross, 8, 0};
    const auto origin = std::get<ad::CommandEvent>(command(513)).observation;
    for (std::size_t i = 0; i != count; ++i) {
        ad::BookObservation row;
        row.id = std::string(i % 2 ? 513 : 0, 'b');
        row.incarnation = i + 1; row.priority = -static_cast<std::int64_t>(i);
        row.prices.limit = -0.0;
        if (i % 3) {
            row.draft.bind(origin);
            row.draft.reviewed({18, ad::Checkpoint::ExplicitPair, 8, origin->command});
            row.draft.sizing_revised({19, 1, 8, origin->command});
        }
        result.book.push_back(row);
        if (i % 2) result.reviewed.push_back(row);
        result.resolutions.push_back({i + 1, ad::ResolutionKind::Original, 0, 0, 0, -0.0});
        result.causes.push_back(i + 1);
    }
    return result;
}

void valid_rows() {
    std::vector<ad::Event> events;
    for (std::size_t length : {0U, 1U, 511U, 512U, 513U, 4095U, 8193U, 131073U})
        events.push_back(command(length));
    for (std::size_t count : {0U, 1U, 17U, 257U}) events.push_back(review(count));
    ad::SizingEvent sizing;
    sizing.receipt = {23, 5, 9, 17};
    sizing.before = {-0.0, 0.0, real(0x7ff0000000000001ULL), -1.0, 1.0};
    sizing.after = {0.0, -0.0, real(0xfff8000000000011ULL), 1.0, -1.0};
    events.push_back(sizing);
    std::uint64_t expected = seed;
    for (const auto& event : events) {
        const Model model(event);
        const auto wire = model.bytes(true);
        const pd::ScratchSpanSource span({wire.data(), wire.size()});
        for (std::size_t page_size : {97U, 512U, 1024U}) {
            std::array<std::byte, 1024> page{};
            const pd::ScratchBuffer buffer{page.data(), page_size};
            ad::JournalRecordSummary old_summary, summary;
            auto old_digest = expected, digest = expected;
            require(legacy_fold(span, buffer, old_summary, old_digest) == Status::ok,
                    "legacy composition accepts the fixture");
            const auto encoded = ad::encode_journal_record(event);
            Source source(encoded);
            require(encoded.size() == wire.size(), "independent wire size");
            require(ad::fold_validated_journal_record(source, buffer, summary, digest) == Status::ok,
                    "streaming validation accepts the same fixture");
            require(same(summary, old_summary) && summary.sequence == ad::sequence(event),
                    "summary and sequence equal the separate scan");
            require(digest == old_digest && digest == model.fold(expected),
                    "canonical digest equals both independent reflection and legacy fold");
            require(source.reads == (wire.size() + page_size - 1) / page_size,
                    "exactly one forward payload pass, including long text and arrays");
            Source skipped(span);
            summary = {}; digest = expected;
            require(ad::fold_validated_journal_record(skipped, buffer, summary, digest,
                                                       ad::sequence(event)) == Status::ok,
                    "a frontier record is still validated");
            require(same(summary, old_summary) && digest == expected,
                    "a frontier record never extends the digest");
        }
        expected = model.fold(expected);
    }
}

void compare_failure(const Bytes& wire, std::uint64_t fault,
                     std::uint64_t after_sequence = 0) {
    const pd::ScratchSpanSource span({wire.data(), wire.size()});
    Source old_source(span), source(span);
    old_source.fault = source.fault = fault;
    std::array<std::byte, 512> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    const ad::JournalRecordSummary sentinel{991, -31, true};
    auto old_summary = sentinel, summary = sentinel;
    auto old_digest = seed, digest = seed;
    const auto old_status = legacy_fold(old_source, buffer, old_summary, old_digest, after_sequence);
    const auto status = ad::fold_validated_journal_record(source, buffer, summary, digest, after_sequence);
    require(status == old_status && same(summary, old_summary) && digest == old_digest,
            "failure status and publication equal validation followed by folding");
    require(status != Status::ok, "the mutation or fault must actually reject");
    require(same(summary, sentinel) && digest == seed, "failure publishes neither output");
}
void invalid_rows() {
    const Model model(command(8193));
    const auto valid = model.bytes(true);
    const auto no_fault = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t offset = 0; offset < valid.size(); offset += 512)
        compare_failure(valid, offset);
    compare_failure(valid, valid.size() - 1);
    std::vector<Bytes> invalid;
    for (const auto& path : {"event.kind", "event.observation.birth.cause",
                            "event.observation.buy", "event.observation.quantity_type",
                            "event.removed.size"}) {
        auto changed = model;
        bool found = false;
        for (auto& field : changed.fields) {
            if (field.path != path) continue;
            if (field.value.index() == 0) field.value = std::uint64_t{99};
            else field.value = std::int64_t{std::numeric_limits<std::int64_t>::max()};
            found = true;
        }
        require(found, "mutation names a real field");
        invalid.push_back(changed.bytes(true));
    }
    auto path = model;
    path.fields.back().path.back() = 'X'; invalid.push_back(path.bytes(true));
    auto tag = model;
    tag.fields.back().value = std::string("invalid tag"); invalid.push_back(tag.bytes(true));
    auto count = valid;
    std::fill_n(count.begin(), 8, std::byte{0xff}); invalid.push_back(std::move(count));
    auto truncated = valid;
    truncated.pop_back(); invalid.push_back(std::move(truncated));
    auto trailing = valid;
    trailing.push_back(std::byte{0}); invalid.push_back(std::move(trailing));
    for (const auto& wire : invalid) {
        compare_failure(wire, no_fault);
        // A hash-only read inside the long id must not outrank a subsequent
        // schema error. Earlier schema errors still win without reading it.
        compare_failure(wire, 4096);
    }
    // The old scan skips an interior text page on a frontier record. Both
    // operations must still refuse a later malformed field, without hashing it.
    compare_failure(invalid.back(), 4096, ad::sequence(command(8193)));
    std::array<std::byte, 512> page{};
    const pd::ScratchSpanSource span({valid.data(), valid.size()});
    Source skipped(span);
    skipped.fault = 4096;
    ad::JournalRecordSummary summary;
    auto digest = seed;
    require(ad::fold_validated_journal_record(skipped, {page.data(), page.size()}, summary,
                                               digest, ad::sequence(command(8193))) == Status::ok
            && digest == seed, "frontier validation does not introduce hash-only text reads");
}
void carried_failures() {
    const auto wire = Model(command(8193)).bytes(true);
    const pd::ScratchSpanSource span({wire.data(), wire.size()});
    const pd::ReportScratchIoError original(EIO, "admission fold read");
    std::array<std::byte, 512> page{};
    const pd::ScratchBuffer buffer{page.data(), page.size()};
    for (bool once : {false, true}) {
        Source old_source(span), source(span);
        old_source.fault = source.fault = 4096;
        old_source.fail_once = source.fail_once = once;
        old_source.typed_error = source.typed_error = &original;
        const ad::JournalRecordSummary sentinel{991, -31, true};
        auto old_summary = sentinel, summary = sentinel;
        auto old_digest = seed, digest = seed;
        const auto old_status = legacy_fold(old_source, buffer, old_summary, old_digest);
        require(old_status == Status::io_error && old_source.carried_failure(),
                "the legacy hash reads the injected text-page fault");
        std::optional<pd::ReportScratchIoError> failure;
        const auto status = ad::fold_validated_journal_record(source, buffer, summary, digest, 0, &failure);
        require(status == old_status && failure && same(summary, old_summary) && digest == old_digest,
                "read failure remains a failure after validation-only replay");
        const auto& expected = *old_source.carried_failure();
        require(failure->code() == expected.code() && std::strcmp(failure->what(), expected.what()) == 0
                && failure->run_failure_code() == expected.run_failure_code()
                && failure->run_failure().args && expected.run_failure().args
                && *failure->run_failure().args == *expected.run_failure().args,
                "errno, English and typed failure arguments are preserved");
        require(source.carried_failure() == nullptr && source.faults == 1,
                "the saved diagnostic survives successful reads that clear the borrowed failure");
        require(same(summary, sentinel) && digest == seed, "typed failure publishes no value output");
    }
}
} // namespace

int main() {
    try { valid_rows(); invalid_rows(); carried_failures(); }
    catch (const std::exception& error) {
        std::fprintf(stderr, "admission streaming fold: %s\n", error.what());
        return 1;
    }
    std::puts("admission streaming fold: all comparisons completed");
    return 0;
}
