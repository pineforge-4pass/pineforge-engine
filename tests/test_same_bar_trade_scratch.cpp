// X07: exercise the production publication over an actual same-bar trade
// tail. The frozen stable-sort oracle checks every Trade field, phase and
// phase hash leaf. On 21ba62bd the resident-byte bound fails on the real tail,
// not on a missing declaration or a fixture-sized container.
#include <pineforge/source/pine_strategy_host.hpp>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using SourceHost = source::PineStrategyHost;

int failures = 0;
#define CHECK(condition) do {                                                  \
    if (!(condition)) {                                                        \
        ++failures;                                                            \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition);  \
    }                                                                          \
} while (false)

#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
struct ScratchFixture {
    using Status = detail::ScratchStatus;
    static constexpr std::size_t page_size = 512;
    std::vector<std::byte> storage = std::vector<std::byte>(64u * 1024u * 1024u);
    detail::ScratchKeyOrder order = SourceHost::trade_scratch_order();
    std::uint64_t comparisons = 0, reads = 0, writes = 0, syncs = 0;
    static Status compare(void* context, const detail::ScratchByteSource& a,
                          const detail::ScratchByteSource& b, int& result) noexcept {
        auto& self = *static_cast<ScratchFixture*>(context);
        ++self.comparisons;
        return self.order.compare(self.order.context, a, b, result);
    }
    static void charge(void* context, const detail::ScratchIoCharge& io) noexcept {
        auto& self = *static_cast<ScratchFixture*>(context);
        CHECK(io.kind == detail::ScratchBackingKind::test_memory);
        if (io.operation == detail::ScratchIoOperation::read_page) ++self.reads;
        if (io.operation == detail::ScratchIoOperation::write_page) ++self.writes;
        if (io.operation == detail::ScratchIoOperation::sync) ++self.syncs;
    }
    detail::FixedMemoryScratchBacking backing{
        {storage.data(), storage.size()}, page_size, {this, charge}};
    detail::CommandScratchStore store{
        {2, page_size, 1, 1, {order.id, this, compare}, detail::ScratchOpenMode::create_empty},
        &backing};
    std::uint64_t resident = store.stats().resident_bytes;

    std::uint64_t digest() {
        std::array<std::byte, page_size> page{};
        std::uint64_t result = 0;
        CHECK(store.state_digest({page.data(), page.size()}, result) == Status::ok);
        return result;
    }
    void check_empty() {
        CHECK(store.status() == Status::ok);
        CHECK(!store.transaction_active());
        CHECK(store.record_count() == 0);
        const auto stats = store.stats();
        CHECK(stats.resident_bytes == resident);
        CHECK(resident > 0);
        static std::uint64_t fixed_resident = 0;
        if (!fixed_resident) fixed_resident = resident;
        CHECK(resident == fixed_resident);
        CHECK(stats.page_reads == reads && stats.page_writes == writes && stats.syncs == syncs);
    }
    void check_queries(std::size_t population) {
        CHECK(store.begin() == Status::ok);
        using Key = std::array<std::uint64_t, 4>;
        detail::ScratchSpanSource empty(detail::ScratchBytes{});
        const auto refused_before = global_allocation::refused;
        global_allocation::refusing = true;
        for (std::size_t index = 0; index < population; ++index) {
            const Key key{0, index * 2, 0, 0};
            detail::ScratchSpanSource source(
                {reinterpret_cast<const std::byte*>(key.data()), sizeof key});
            detail::ScratchHandle handle = detail::scratch_no_handle;
            CHECK(store.put(source, empty, handle) == Status::ok);
        }
        CHECK(store.record_count() == population);
        CHECK(store.stats().backing_pages > 2);
        std::size_t levels = 0;
        for (auto count = population; count; count >>= 1) ++levels;
        std::uint64_t maximum = 0;
        for (std::size_t index = 0; index < population; ++index) {
            const auto probe_index = (index * 73) % population;
            const Key key{0, probe_index * 2 + 1, 0, 0};
            detail::ScratchSpanSource source(
                {reinterpret_cast<const std::byte*>(key.data()), sizeof key});
            detail::ScratchHandle handle = detail::scratch_no_handle;
            const auto before = comparisons;
            const auto status = store.lower_bound(source, handle);
            CHECK(status == (probe_index + 1 == population ? Status::end : Status::ok));
            maximum = std::max(maximum, comparisons - before);
            if (status == Status::ok) {
                Key actual{};
                CHECK(store.read(handle, detail::ScratchField::key, 0,
                    {reinterpret_cast<std::byte*>(actual.data()), sizeof actual}) == Status::ok);
                CHECK((actual == Key{0, (probe_index + 1) * 2, 0, 0}));
            } else CHECK(handle == detail::scratch_no_handle);
        }
        CHECK(maximum > 0 && maximum <= 8 * (levels + 1));
        CHECK(store.rollback() == Status::ok);
        global_allocation::refusing = false;
        CHECK(global_allocation::refused == refused_before);
        check_empty();
        std::printf("X07 ordered queries=%zu max_rows_visited=%llu resident=%llu\n", population,
            static_cast<unsigned long long>(maximum), static_cast<unsigned long long>(resident));
    }
};
#endif

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct PublishTag {
    using type = void (SourceHost::*)(const Bar&, bool, bool);
    friend type access(PublishTag);
};
template struct PrivateAccess<PublishTag, &SourceHost::scheduler_publish_source_bar>;
struct PlacementTag {
    using type = source::PlacementTable Adapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &Adapter::placement_>;
struct PhasesTag {
    using type = detail::ScratchSequence<std::uint8_t> Adapter::*;
    friend type access(PhasesTag);
};
template struct PrivateAccess<PhasesTag, &Adapter::trade_exit_phase_>;
struct PhaseFinalTag {
    using type = std::size_t Adapter::*;
    friend type access(PhaseFinalTag);
};
template struct PrivateAccess<PhaseFinalTag, &Adapter::exit_phase_final_>;
struct PhaseDigestTag {
    using type = std::uint64_t Adapter::*;
    friend type access(PhaseDigestTag);
};
template struct PrivateAccess<PhaseDigestTag, &Adapter::exit_phase_digest_>;

struct Host final : SourceHost {
    void on_source_bar(const Bar&) override {}
    TradeLedger& rows() { return trades_; }
    Adapter& adapter() { return adapter_; }
    detail::ScratchSequence<std::uint8_t>& phases() { return adapter_.*access(PhasesTag{}); }
    std::vector<std::uint8_t> phase_values() const {
        const auto& phases = adapter_.*access(PhasesTag{});
        return std::vector<std::uint8_t>(phases.begin(), phases.end());
    }
    void publish() {
        const Bar bar{100.0, 101.0, 99.0, 100.0, 1.0, 1704067200000LL};
        (this->*access(PublishTag{}))(bar, false, false);
    }
    std::uint64_t adapter_hash() {
        BrokerStateHashSink sink;
        adapter_.hash_state(sink);
        return sink.h;
    }
};

std::size_t peak_bytes = 0;
void record_bytes(std::size_t) {
    peak_bytes = std::max(peak_bytes, global_allocation::live_bytes);
}

bool same_double(double a, double b) {
    return std::memcmp(&a, &b, sizeof a) == 0;
}

void same_trade(const Trade& a, const Trade& b) {
    CHECK(a.entry_time == b.entry_time && a.exit_time == b.exit_time);
    CHECK(same_double(a.entry_price, b.entry_price));
    CHECK(same_double(a.exit_price, b.exit_price));
    CHECK(same_double(a.qty, b.qty));
    CHECK(same_double(a.pnl, b.pnl));
    CHECK(same_double(a.pnl_pct, b.pnl_pct));
    CHECK(a.is_long == b.is_long);
    CHECK(a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index);
    CHECK(a.entry_id == b.entry_id && a.exit_id == b.exit_id);
    CHECK(a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment);
    CHECK(a.exit_from_bracket == b.exit_from_bracket);
    CHECK(same_double(a.max_runup, b.max_runup));
    CHECK(same_double(a.max_drawdown, b.max_drawdown));
    CHECK(same_double(a.commission, b.commission));
    CHECK(a.entry_incarnation == b.entry_incarnation);
    CHECK(a.open_at_end == b.open_at_end && a.close_cause == b.close_cause);
}

void populate(Host& host, std::size_t population, std::size_t phase_rows, bool unchanged_phases) {
    // Warm the publication itself before the population and counting window.
    host.publish();
    auto& placement = host.adapter().*access(PlacementTag{});
    for (std::uint64_t index = 0; index < 3; ++index) {
        source::PlacementSnapshot row;
        row.source_id = "s" + std::to_string(index);
        row.from_entry = "L";
        row.command_sequence = 10 * (index + 1);
        placement.try_emplace(index + 1, std::move(row));
    }
    Trade barrier{};
    barrier.entry_time = 1;
    barrier.exit_time = 2;
    host.rows().push_back(barrier);
    for (std::size_t index = 0; index < population; ++index) {
        Trade row{};
        row.entry_time = 100;
        row.exit_time = 200;
        row.entry_price = 10.0 + index * 0.25;
        row.exit_price = 20.0 + index * 0.5;
        row.qty = 1.0 + index * 0.125;
        row.pnl = static_cast<double>(index) - 7.0;
        row.pnl_pct = index * 0.03125;
        row.is_long = index % 2 == 0;
        row.entry_bar_index = static_cast<int>(index);
        row.exit_bar_index = static_cast<int>(index + 1);
        row.entry_id = "L";
        row.exit_id = unchanged_phases ? "s" + std::to_string(2 - index)
                                      : index % 4 == 3 ? "unknown" : "s" + std::to_string(2 - index % 3);
        row.entry_comment = "entry payload wider than a string inline buffer " + std::to_string(index);
        row.exit_comment = "exit " + std::to_string(index);
        row.exit_from_bracket = true;
        row.max_runup = index * 0.0625;
        row.max_drawdown = index * 0.125;
        row.commission = index * 0.015625;
        row.entry_incarnation = index + 1;
        row.close_cause = execution::CloseCause::Bracket;
        host.rows().push_back(std::move(row));
    }
    auto& phases = host.phases();
    phases.reserve(population + 1);  // retained phase output, outside the census
    phases.push_back(static_cast<std::uint8_t>(NativePathPhase::None));
    for (std::size_t index = 0; index < phase_rows; ++index) {
        const auto phase = unchanged_phases
            ? index == 1 ? NativePathPhase::Low : NativePathPhase::High
            : index % 5 == 0 ? NativePathPhase::Open
            : index % 5 == 1 ? NativePathPhase::High
            : index % 5 == 2 ? NativePathPhase::Low
            : index % 5 == 3 ? NativePathPhase::Close : NativePathPhase::None;
        phases.push_back(static_cast<std::uint8_t>(phase));
    }
    host.adapter().*access(PhaseFinalTag{}) = phases.size();
    auto& digest = host.adapter().*access(PhaseDigestTag{});
    digest = 1469598103934665603ULL;
    for (const auto phase : phases) digest = Adapter::fold_exit_phase(digest, phase);
    CHECK(host.closed_trade_count() == population + 1);
    std::size_t tail = 0;
    for (std::size_t index = host.rows().size(); index > 1; --index) {
        const auto& row = host.rows()[index - 1];
        if (!row.exit_from_bracket || row.entry_id != "L"
            || row.entry_time != 100 || row.exit_time != 200) break;
        ++tail;
    }
    CHECK(tail == population);
}

void reference_sort(Host& host) {
    const auto original = host.rows();
    std::vector<std::size_t> indices(original.size() - 1);
    std::iota(indices.begin(), indices.end(), std::size_t{1});
    std::stable_sort(indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
        const bool open_a = host.adapter().is_open_phase_exit(a);
        const bool open_b = host.adapter().is_open_phase_exit(b);
        if (open_a != open_b) return open_a;
        return host.adapter().command_sequence_for_exit(original[a].exit_id, original[a].entry_id)
            < host.adapter().command_sequence_for_exit(original[b].exit_id, original[b].entry_id);
    });
    for (std::size_t index = 0; index < indices.size(); ++index)
        host.rows().replace(index + 1, original[indices[index]]);
    host.adapter().permute_exit_phases(1, indices);
}

void exercise(std::size_t population, std::size_t phase_rows, bool unchanged_phases = false) {
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    ScratchFixture scratch;
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    scratch.check_queries(population);
    const auto scratch_digest = scratch.digest();
#endif
    Host actual;
    Host expected;
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    actual.set_trade_scratch(scratch.store);
#endif
    populate(actual, population, phase_rows, unchanged_phases);
    populate(expected, population, phase_rows, unchanged_phases);
    reference_sort(expected);
    expected.publish();

    for (int publication = 0; publication < 2; ++publication) {
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
        const auto comparisons_before = scratch.comparisons;
        const auto writes_before = scratch.writes;
#endif
        peak_bytes = 0;
        global_allocation::live_bytes = 0;
        global_allocation::on_recorded = record_bytes;
        global_allocation::recording = true;
        actual.publish();
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
        scratch.check_empty();
        CHECK(scratch.digest() == scratch_digest);
        CHECK(publication == 0 ? scratch.comparisons > comparisons_before
                               : scratch.comparisons == comparisons_before);
        CHECK(publication == 0 ? scratch.writes > writes_before : scratch.writes == writes_before);
#endif
        CHECK(actual.closed_trade_count() == population + 1);
        for (std::size_t index = 0; index < actual.rows().size(); ++index)
            same_trade(actual.rows()[index], expected.rows()[index]);
        CHECK(actual.phase_values() == expected.phase_values());
        CHECK(actual.adapter().*access(PhaseFinalTag{})
              == expected.adapter().*access(PhaseFinalTag{}));
        CHECK(actual.adapter().*access(PhaseDigestTag{})
              == expected.adapter().*access(PhaseDigestTag{}));
        CHECK(actual.adapter_hash() == expected.adapter_hash());
        constexpr std::size_t kSortBytes = 2048;
        CHECK(peak_bytes <= kSortBytes);
        std::printf("X07 population=%zu phase_rows=%zu publication=%d peak=%zu bound=%zu\n",
                    population, phase_rows, publication, peak_bytes, kSortBytes);
        expected.publish();
    }
}
}  // namespace

int main() {
    exercise(128, 128);
    exercise(2048, 2048);
    exercise(97, 10);
    exercise(3, 3, true);  // row order changes; phase bytes and folded prefix do not
    std::printf("%s X07 same-bar trade scratch (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
