// LAB-O1S3-P14: deterministic work counts, never elapsed time.
// Observe the linked library's fold/encoder through the detail codec seam.
#include <pineforge/compat/pine/market_admission.hpp>
#include <pineforge/detail/journal_codec.hpp>
#include <pineforge/run_failure.hpp>
#include "../src/source/admission_journal_codec.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace fold_cost {
struct Counts {
    std::uint64_t records = 0;
    std::uint64_t calls = 0;
    std::uint64_t bytes = 0;
    std::uint64_t words = 0;
    std::uint64_t checks = 0;
    Counts& operator+=(const Counts& other) {
        records += other.records; calls += other.calls;
        bytes += other.bytes; words += other.words; checks += other.checks;
        return *this;
    }
};
Counts work;

namespace jc = pineforge::detail::journal_codec;
class ObserveWork {
    static void observe(void* context, jc::WorkOperation operation,
                        std::uint64_t bytes) noexcept {
        auto& counts = *static_cast<Counts*>(context);
        switch (operation) {
        case jc::WorkOperation::payload_source: ++counts.records; break;
        case jc::WorkOperation::payload_read:
            ++counts.calls; counts.bytes += bytes; break;
        case jc::WorkOperation::encoder_word: ++counts.words; break;
        case jc::WorkOperation::writer_status: ++counts.checks; break;
        }
    }
    const jc::WorkObserver observer_{&work, &observe};
    const jc::WorkObserver* previous_;
public:
    ObserveWork() noexcept : previous_(jc::set_work_observer(&observer_)) {}
    ~ObserveWork() { jc::set_work_observer(previous_); }
    ObserveWork(const ObserveWork&) = delete;
    ObserveWork& operator=(const ObserveWork&) = delete;
};
}

namespace {
namespace ad = pineforge::admission;
namespace pd = pineforge::detail;
using fold_cost::Counts;
using Bytes = std::vector<std::byte>;
constexpr std::uint64_t seed = 1469598103934665603ULL;
constexpr std::size_t page_size = 512;
int failures = 0;

#define CHECK(condition) do {                                                   \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    }                                                                          \
} while (false)

void ok(pd::ScratchStatus status) {
    if (status != pd::ScratchStatus::ok)
        throw std::runtime_error("fold-cost fixture/storage failure");
}
pd::ScratchStatus numeric_order(void*, const pd::ScratchByteSource& a,
                               const pd::ScratchByteSource& b, int& order) noexcept {
    if (a.size() != 8 || b.size() != 8) return pd::ScratchStatus::programming_error;
    std::array<std::byte, 8> left{}, right{};
    auto status = a.read(0, {left.data(), left.size()});
    if (status != pd::ScratchStatus::ok) return status;
    status = b.read(0, {right.data(), right.size()});
    if (status != pd::ScratchStatus::ok) return status;
    order = 0;
    for (std::size_t i = 8; i-- != 0;)
        if (left[i] != right[i]) {
            order = std::to_integer<unsigned>(left[i]) < std::to_integer<unsigned>(right[i])
                ? -1 : 1;
            break;
        }
    return pd::ScratchStatus::ok;
}
struct Rig {
    // Test medium only. Resident store geometry is identical at both E sizes.
    Bytes storage = Bytes(32 * 1024 * 1024);
    pd::FixedMemoryScratchBacking backing{{storage.data(), storage.size()}, page_size};
    pd::ScratchStore store{{2, page_size, 1, 1, {0x703134666f6c6401ULL, nullptr, &numeric_order},
                           pd::ScratchOpenMode::create_empty}, &backing};
    Rig() { ok(backing.status()); ok(store.status()); }
};

// The existing reflection is the value oracle. Wire retains exact float bits;
// the independent v4 model normalizes zeros/NaNs and omits the outer row count.
void word(Bytes& bytes, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i)
        bytes.push_back(static_cast<std::byte>(value >> (8 * i)));
}
void text(Bytes& bytes, const std::string& value) {
    word(bytes, value.size());
    for (unsigned char byte : value) bytes.push_back(static_cast<std::byte>(byte));
}
std::uint64_t real_bits(double value, bool normalize) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    if (normalize) {
        if ((bits & 0x7fffffffffffffffULL) == 0) bits = 0;
        else if ((bits & 0x7ff0000000000000ULL) == 0x7ff0000000000000ULL
                 && (bits & 0x000fffffffffffffULL)) bits = 0x7ff8000000000000ULL;
    }
    return bits;
}
Bytes reflected(const ad::Event& event, bool storage) {
    std::vector<ad::Field> fields;
    ad::reflect(event, "event", [&](const auto& field) { fields.push_back(field); });
    Bytes result;
    if (storage) word(result, fields.size());
    for (const auto& field : fields) {
        text(result, field.path); word(result, field.value.index());
        std::visit([&](const auto& value) {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, std::string>) text(result, value);
            else if constexpr (std::is_same_v<T, double>) word(result, real_bits(value, !storage));
            else word(result, static_cast<std::uint64_t>(value));
        }, field.value);
    }
    return result;
}
std::uint64_t reference_fold(std::uint64_t digest, const ad::Event& event) {
    for (const auto byte : reflected(event, false))
        digest = (digest ^ std::to_integer<unsigned>(byte)) * 1099511628211ULL;
    return digest;
}
ad::Event review(std::uint64_t sequence, std::size_t causes) {
    ad::ReviewEvent event;
    event.receipt = {sequence, ad::Checkpoint::TerminalGross, 7, 0};
    for (std::size_t i = 0; i != causes; ++i) event.causes.push_back(100 + i);
    return event;
}
ad::Event command(std::uint64_t sequence, int bar, ad::Outcome outcome) {
    auto observation = std::make_shared<ad::CommandObservation>();
    observation->command = sequence; observation->bar = bar; observation->id = "fold-cost";
    return ad::CommandEvent{std::move(observation), outcome, 0, {}, {}};
}
int gate(const ad::Journal& journal) {
    return pineforge::compat::pine::last_rejected_command_bar(journal);
}
void print(const char* phase, bool codec, std::size_t events, std::uint64_t bytes,
           const Counts& counts) {
    std::printf("%s codec=%d E=%zu R=%llu records=%llu reads=%llu bytes=%llu words=%llu checks=%llu\n",
        phase, codec, events, static_cast<unsigned long long>(bytes),
        static_cast<unsigned long long>(counts.records),
        static_cast<unsigned long long>(counts.calls),
        static_cast<unsigned long long>(counts.bytes),
        static_cast<unsigned long long>(counts.words),
        static_cast<unsigned long long>(counts.checks));
}
struct Sample {
    std::uint64_t record_bytes = 0;
    Counts folds, repeated, gates;
};
Sample event_axis(bool codec, std::size_t events) {
    Rig rig;
    ad::Journal journal;
    if (codec) ad::bind_journal_codec(journal, rig.store);
    Sample sample;
    std::uint64_t expected = seed;
    for (std::size_t i = 0; i != events; ++i) {
        const auto event = review(journal.next_sequence(), 0);
        expected = reference_fold(expected, event);
        const auto source = ad::encode_journal_record(event);
        if (i == 0) sample.record_bytes = source.size();
        CHECK(source.size() == sample.record_bytes);
        journal.append(event);
        fold_cost::work = {};
        const auto folded = journal.folded_window();
        sample.folds += fold_cost::work;
        CHECK(folded.count == i + 1);
        CHECK(folded.last_sequence == i + 1);
        CHECK(folded.digest == expected);
    }
    fold_cost::work = {};
    for (unsigned i = 0; i != 8; ++i) CHECK(journal.folded_window().digest == expected);
    sample.repeated = fold_cost::work;
    fold_cost::work = {};
    for (unsigned i = 0; i != 8; ++i) CHECK(gate(journal) == -1);
    sample.gates = fold_cost::work;
    print("incremental folds", codec, events, sample.record_bytes, sample.folds);
    std::printf("per event codec=%d E=%zu records=%.3f reads=%.3f words=%.3f checks=%.3f\n",
        codec, events, double(sample.folds.records) / events, double(sample.folds.calls) / events,
        double(sample.folds.words) / events, double(sample.folds.checks) / events);
    print("already folded (8)", codec, events, sample.record_bytes, sample.repeated);
    print("cached gate (8)", codec, events, sample.record_bytes, sample.gates);
    // One seek/frontier check per call is allowed, but no old payload scan.
    CHECK(sample.repeated.records <= 8);
    CHECK(sample.repeated.calls == 0 && sample.repeated.bytes == 0);
    CHECK(sample.repeated.words == 0 && sample.repeated.checks == 0);
    CHECK(sample.gates.records <= 8);
    CHECK(sample.gates.calls == 0 && sample.gates.bytes == 0 && sample.gates.words == 0);
    CHECK(sample.gates.checks == 0);
    return sample;
}
void event_growth() {
    for (bool codec : {false, true}) {
        const auto small = event_axis(codec, 256);
        const auto large = event_axis(codec, 1024);
        CHECK(small.record_bytes == large.record_bytes);
        // Four times as many events of identical shape permits four times the
        // work. Measuring every successive fold exposes a repeated-prefix scan.
        std::puts("E bound: 1024/256 = 4 for records, source reads/bytes and encoder words/checks");
        if (codec) {
            CHECK(small.folds.records > 0 && small.folds.calls > 0 && small.folds.bytes > 0);
            CHECK(large.folds.records <= 4 * small.folds.records);
            CHECK(large.folds.calls <= 4 * small.folds.calls);
            CHECK(large.folds.bytes <= 4 * small.folds.bytes);
        } else {
            CHECK(small.folds.words > 0 && small.folds.checks > 0);
            CHECK(large.folds.words <= 4 * small.folds.words);
            CHECK(large.folds.checks <= 4 * small.folds.checks);
        }
    }
}
Sample record_axis(std::size_t causes) {
    ad::Journal journal; // The production non-codec route.
    const auto event = review(journal.next_sequence(), causes);
    const auto expected = reference_fold(seed, event);
    const auto wire = reflected(event, true);
    journal.append(event);
    fold_cost::work = {};
    const auto folded = journal.folded_window();
    Sample sample;
    sample.record_bytes = wire.size(); sample.folds = fold_cost::work;
    CHECK(folded.count == 1 && folded.last_sequence == 1 && folded.digest == expected);
    CHECK(sample.folds.words > 0 && sample.folds.checks > 0);
    print("record-size fold", false, 1, sample.record_bytes, sample.folds);
    std::printf("array elements=%zu\n", causes);
    return sample;
}
void record_growth() {
    const auto small = record_axis(256);
    const auto large = record_axis(1024);
    CHECK(large.record_bytes > small.record_bytes);
    // R is measured from independently reflected wire bytes, not assumed to
    // quadruple: decimal subscripts and the fixed header affect its ratio.
    // 25% density slack permits page-edge retries; repeated whole-schema
    // walks instead grow with fields * pages and exceed this linear bound.
    std::puts("R bound: work_large/work_small <= (5/4) * R_large/R_small (words AND checks)");
    CHECK(large.folds.words * small.record_bytes * 4
          <= small.folds.words * large.record_bytes * 5);
    CHECK(large.folds.checks * small.record_bytes * 4
          <= small.folds.checks * large.record_bytes * 5);
}
void cached_gate_updates() {
    for (bool codec : {false, true}) {
        Rig rig;
        ad::Journal journal;
        if (codec) ad::bind_journal_codec(journal, rig.store);
        const auto early = journal.next_sequence();
        const auto later = journal.next_sequence();
        journal.append(command(later, 7, ad::Outcome::RejectedPricedCap));
        fold_cost::work = {};
        CHECK(gate(journal) == 7); // Before ANY explicit fold.
        const auto cold = fold_cost::work;
        print("gate before fold", codec, 1, 0, cold);
        CHECK(cold.records == 0 && cold.calls == 0 && cold.words == 0 && cold.checks == 0);
        journal.append(command(early, 30, ad::Outcome::RejectedAffordability));
        CHECK(gate(journal) == 7); // Highest sequence, not append order or max(bar).
        auto prefix = journal.folded_window();
        journal.retire_prefix(prefix.count, prefix.digest, prefix.last_sequence);
        const auto delayed = journal.next_sequence();
        journal.append(command(journal.next_sequence(), 80, ad::Outcome::Admitted));
        journal.folded_window();
        journal.append(command(delayed, 9, ad::Outcome::OpeningRejectedReductionAdmitted));
        CHECK(gate(journal) == 9); // Late insert below an already folded non-rejection.
        prefix = journal.folded_window();
        journal.retire_prefix(prefix.count, prefix.digest, prefix.last_sequence);
        journal.append(command(journal.next_sequence(), -3, ad::Outcome::RejectedIntradayCap));
        CHECK(gate(journal) == -3);
        ad::Journal copied(journal);
        CHECK(gate(copied) == -3);
        ad::Journal moved(std::move(copied));
        CHECK(gate(moved) == -3 && gate(copied) == -1);
        ad::Journal assigned;
        assigned = journal;
        CHECK(gate(assigned) == -3);
        assigned = std::move(moved);
        CHECK(gate(assigned) == -3 && gate(moved) == -1);
        journal.reset();
        CHECK(gate(journal) == -1);

        // Filtering before any fold must invalidate an append-maintained gate.
        const auto kept = journal.next_sequence();
        journal.append(command(kept, 4, ad::Outcome::RejectedFrozenMarketCap));
        journal.append(command(journal.next_sequence(), 5, ad::Outcome::RejectedPricedCap));
        journal.retain({kept});
        CHECK(gate(journal) == 4);
        prefix = journal.folded_window();
        journal.retire_prefix(prefix.count, prefix.digest, prefix.last_sequence);
        const auto removed = journal.next_sequence();
        const auto tail = journal.next_sequence();
        journal.append(command(removed, 6, ad::Outcome::Admitted));
        journal.append(command(tail, 8, ad::Outcome::RejectedAffordability));
        journal.folded_window();
        journal.retain({tail}); // Legal refold from nonzero prefix; gate unchanged.
        CHECK(gate(journal) == 8);
        CHECK(journal.folded_window().last_rejected_command_bar == 8);
        journal.append(command(journal.next_sequence(), 2, ad::Outcome::RejectedPricedCap));
        CHECK(gate(journal) == 2);
    }
}
void cursor_preserves_bytes() {
    auto value = std::get<ad::ReviewEvent>(review(1000, 41));
    for (std::size_t i = 0; i != 17; ++i) {
        ad::BookObservation row;
        row.id = std::string(31 + i * 97, 'b');
        row.oca_name = std::string("a\0b", 3);
        if (i % 3 != 0) {
            auto origin = std::make_shared<ad::CommandObservation>();
            origin->command = i + 1; origin->bar = 1;
            origin->id = std::string(513 + i, 'd');
            if (i % 2) origin->original_sizing = ad::SizingObservation{};
            row.draft.bind(origin);
            if (i % 2) row.draft.reviewed({100 + i, ad::Checkpoint::DefaultGross, 2, i + 1});
            if (i % 4) row.draft.sizing_revised({200 + i, 1, 3, i + 1});
        }
        value.book.push_back(row);
        if (i % 2) value.reviewed.push_back(row);
        value.resolutions.push_back({i + 1, ad::ResolutionKind::Original, 0, 0, 0, -0.0});
    }
    const ad::Event event = std::move(value);
    const auto expected = reflected(event, true);
    for (std::size_t chunk : {97U, 512U, 1024U}) {
        const auto source = ad::encode_journal_record(event);
        CHECK(source.size() == expected.size());
        std::array<std::byte, 1024> bytes{};
        for (std::uint64_t at = 0; at < source.size();) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(chunk, source.size() - at));
            ok(source.read(at, {bytes.data(), count}));
            CHECK(std::memcmp(bytes.data(), expected.data() + at, count) == 0);
            at += count;
        }
        for (const auto at : {source.size() / 2, std::uint64_t{0},
                              source.size() - 1, source.size() / 3, std::uint64_t{1}}) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(bytes.size(), source.size() - at));
            ok(source.read(at, {bytes.data(), count}));
            CHECK(std::memcmp(bytes.data(), expected.data() + at, count) == 0);
        }
        ok(source.read(source.size(), {nullptr, 0}));
    }
    ad::Journal journal;
    auto actual = std::get<ad::ReviewEvent>(event);
    actual.receipt.sequence = journal.next_sequence();
    const ad::Event input = std::move(actual);
    journal.append(input);
    CHECK(journal.folded_window().digest == reference_fold(seed, input));
}
}

int main() {
    const struct Case { const char* name; void (*run)(); } cases[] = {
        {"event axis", event_growth}, {"record axis", record_growth},
        {"gate updates", cached_gate_updates}, {"cursor bytes", cursor_preserves_bytes},
    };
    for (const auto& test : cases) {
        std::printf("CASE %s\n", test.name);
        try {
            fold_cost::ObserveWork observe;
            test.run();
        }
        catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", test.name, error.what());
        }
    }
    std::printf("o1s3 p14 fold cost: %d failures\n", failures);
    return failures ? 1 : 0;
}
