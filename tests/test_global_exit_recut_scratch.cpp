// X06: real global-exit blocks, FIFO splits and fees against the frozen
// 21ba62bd algorithm. The byte bound fails on that base's staging vectors.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_policy_support.hpp>

#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <numeric>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using SourceHost = source::PineStrategyHost;
namespace no = native_order;

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
    detail::ScratchStore store{
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
        std::printf("X06 ordered queries=%zu max_rows_visited=%llu resident=%llu\n", population,
            static_cast<unsigned long long>(maximum), static_cast<unsigned long long>(resident));
    }
};
#endif

template<class Tag, typename Tag::type Member>
struct PrivateAccess {
    friend typename Tag::type access(Tag) { return Member; }
};
struct RecutTag {
    using type = void (SourceHost::*)(const no::ExecutionAppliedEvent&);
    friend type access(RecutTag);
};
template struct PrivateAccess<RecutTag, &SourceHost::order_global_exit_children>;
struct PlacementTag {
    using type = source::PlacementTable Adapter::*;
    friend type access(PlacementTag);
};
template struct PrivateAccess<PlacementTag, &Adapter::placement_>;
struct RankKeyTag {
    using type = void (Adapter::*)(const source::SourceId&);
    friend type access(RankKeyTag);
};
template struct PrivateAccess<RankKeyTag, &Adapter::note_exit_rank_key>;
struct PhasesTag {
    using type = std::vector<std::uint8_t> Adapter::*;
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
    Host(bool any, CommissionType fee, double rate) {
        source::PineStrategyConfig config;
        config.close_entries_rule_any = any;
        config.commission_type = static_cast<int>(fee);
        config.commission_value = rate;
        configure_pine_strategy(config);
        commission_type_ = fee;
        commission_value_ = rate;
        syminfo_.pointvalue = 2.5;
    }
    void on_source_bar(const Bar&) override {}
    TradeLedger& rows() { return trades_; }
    Adapter& adapter() { return adapter_; }
    std::vector<std::uint8_t>& phases() { return adapter_.*access(PhasesTag{}); }
    double fee(double price, double qty) const { return calc_commission_at(price, qty, 1.0); }
    double point_value() const { return syminfo_.pointvalue; }
    bool fees() const { return commission_value_ != 0.0; }
    void recut(const no::ExecutionAppliedEvent& event) { (this->*access(RecutTag{}))(event); }
    void tally(double pnl, int sign) {
        if (pnl > 0.0) { gross_profit_sum_ += sign * pnl; win_trades_count_ += sign; }
        else if (pnl < 0.0) { gross_loss_sum_ += sign * pnl; loss_trades_count_ += sign; }
        else eventrades_count_ += sign;
    }
    void initialize_tallies() {
        for (const auto& row : trades_) {
            tally(row.pnl, 1);
            net_profit_sum_ += row.pnl;
        }
    }
    std::array<double, 3> money() const {
        return {net_profit_sum_, gross_profit_sum_, gross_loss_sum_};
    }
    std::array<int, 3> counts() const {
        return {win_trades_count_, loss_trades_count_, eventrades_count_};
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

no::ExecutionAppliedEvent populate(Host& host, std::size_t population) {
    (host.adapter().*access(RankKeyTag{}))("Z");
    (host.adapter().*access(RankKeyTag{}))("A");
    source::PlacementSnapshot placement;
    placement.family = source::PineOrderFamily::ExitLimit;
    placement.source_id = "global";
    (host.adapter().*access(PlacementTag{})).try_emplace(1, std::move(placement));
    auto definition = std::make_shared<no::RequestDefinition>();
    definition->handle = no::RequestHandle{{"x06-scratch", 1}, 1};
    no::ExecutionAppliedEvent event;
    event.definition = std::move(definition);
    event.first_trade_index = 1;
    event.closed_trade_count = population;
    // Allocate the final ledger's own slots before measuring temporary
    // staging. Its retained strings are charged separately below.
    host.rows().reserve(2 * population + 1);
    Trade barrier{};
    barrier.entry_time = 1;
    barrier.exit_time = 2;
    host.rows().push_back(barrier);
    constexpr double quantities[] = {3.125, 1.5, 2.25, 5.75};
    for (std::size_t index = 0; index < population; ++index) {
        Trade row{};
        row.entry_time = 100 + static_cast<std::int64_t>(index);
        row.exit_time = 10000;
        row.entry_price = 31.125 + (index % 7) * 2.0;
        row.exit_price = 37.25;
        row.qty = quantities[index % 4];
        row.commission = host.fee(row.entry_price, row.qty) + host.fee(row.exit_price, row.qty);
        row.pnl = source::tv_money_round(
            (row.exit_price - row.entry_price) * row.qty * host.point_value()) - row.commission;
        row.pnl_pct = row.pnl / (row.entry_price * row.qty * host.point_value()) * 100.0;
        row.is_long = true;
        row.entry_bar_index = static_cast<int>(index);
        row.exit_bar_index = 1000;
        row.entry_id = index % 2 == 0 ? "Z" : "A";
        row.exit_id = "global";
        row.entry_comment = "entry payload wider than the inline string buffer " + std::to_string(index);
        row.exit_comment = "exit payload wider than the inline string buffer " + std::to_string(index);
        row.exit_from_bracket = true;
        row.max_runup = index * 0.125;
        row.max_drawdown = index * 0.0625;
        row.entry_incarnation = index + 100;
        row.close_cause = execution::CloseCause::Bracket;
        host.rows().push_back(std::move(row));
    }
    auto& phases = host.phases();
    phases.reserve(2 * population + 1);
    phases.push_back(static_cast<std::uint8_t>(NativePathPhase::None));
    for (std::size_t index = 0; index < population; ++index)
        phases.push_back(static_cast<std::uint8_t>(index % 3 == 0 ? NativePathPhase::Open
            : index % 3 == 1 ? NativePathPhase::High : NativePathPhase::Close));
    host.adapter().*access(PhaseFinalTag{}) = phases.size();
    auto& digest = host.adapter().*access(PhaseDigestTag{});
    digest = 1469598103934665603ULL;
    for (const auto phase : phases) digest = Adapter::fold_exit_phase(digest, phase);
    host.initialize_tallies();
    CHECK(host.closed_trade_count() == population + 1);
    CHECK(event.first_trade_index + event.closed_trade_count == host.closed_trade_count());
    const auto ranks = host.adapter().global_exit_child_ranks({"Z", "A"});
    CHECK(ranks && (*ranks)[0] > (*ranks)[1]);
    return event;
}

void reference_recut(Host& host, std::size_t population, bool any) {
    using source::tv_money_round;
    constexpr std::size_t first = 1;
    const auto ranks = host.adapter().global_exit_child_ranks({"Z", "A"});
    std::vector<std::size_t> children(population);
    std::iota(children.begin(), children.end(), std::size_t{0});
    const auto rank_of = [&](std::size_t lot) {
        return (*ranks)[host.rows()[first + lot].entry_id == "Z" ? 0 : 1];
    };
    std::stable_sort(children.begin(), children.end(), [&](std::size_t a, std::size_t b) {
        return rank_of(a) < rank_of(b);
    });
    std::vector<Trade> rows;
    std::vector<std::size_t> sources;
    if (any) {
        for (const auto lot : children) {
            rows.push_back(host.rows()[first + lot]);
            sources.push_back(first + lot);
        }
    } else {
        struct Slice { std::size_t lot; double qty; std::size_t child; };
        std::vector<Slice> slices;
        std::vector<double> left(population);
        for (std::size_t lot = 0; lot < population; ++lot)
            left[lot] = host.rows()[first + lot].qty;
        std::size_t lot = 0;
        for (std::size_t child = 0; child < children.size(); ++child) {
            double wanted = host.rows()[first + children[child]].qty;
            while (wanted > 1e-10 && lot < population) {
                const double taken = std::min(left[lot], wanted);
                slices.push_back({lot, taken, child});
                left[lot] = tv_money_round(left[lot] - taken);
                wanted = tv_money_round(wanted - taken);
                if (left[lot] <= 1e-10) ++lot;
            }
            CHECK(wanted <= 1e-10);
        }
        CHECK(lot == population);
        for (const auto& slice : slices) {
            const auto& whole = host.rows()[first + slice.lot];
            const double transaction = host.rows()[first + children[slice.child]].qty;
            const double share = slice.qty / whole.qty;
            double commission = 0.0;
            if (host.fees()) {
                const double exit_fee = host.fee(whole.exit_price, whole.qty);
                const double entry_part =
                    tv_money_round(std::max(0.0, whole.commission - exit_fee) * share);
                const double transaction_fee = tv_money_round(host.fee(whole.exit_price, transaction));
                const double exit_part = tv_money_round(transaction_fee * slice.qty / transaction);
                commission = tv_money_round(entry_part + exit_part);
            }
            Trade row = whole;
            row.qty = slice.qty;
            row.commission = commission;
            row.pnl = tv_money_round((whole.pnl + whole.commission) * share) - commission;
            const double cost = whole.entry_price * slice.qty * host.point_value();
            row.pnl_pct = cost > 0.0 ? row.pnl / cost * 100.0 : 0.0;
            row.max_runup = whole.max_runup * share;
            row.max_drawdown = whole.max_drawdown * share;
            rows.push_back(std::move(row));
            sources.push_back(first + slice.lot);
        }
    }
    for (std::size_t index = first; index < first + population; ++index)
        host.tally(host.rows()[index].pnl, -1);
    for (const auto& row : rows) host.tally(row.pnl, 1);
    host.rows().resize(first);
    for (auto& row : rows) host.rows().push_back(std::move(row));
    host.adapter().permute_exit_phases(first, sources);
}

void exercise(std::size_t population, bool any, CommissionType fee, double rate,
              bool wide_text = false) {
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    ScratchFixture scratch;
    CHECK(scratch.store.status() == detail::ScratchStatus::ok);
    scratch.check_queries(population);
    const auto scratch_digest = scratch.digest();
    const auto comparisons_before = scratch.comparisons;
    const auto writes_before = scratch.writes;
#endif
    Host actual(any, fee, rate);
    Host expected(any, fee, rate);
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    actual.set_trade_scratch(scratch.store);
#endif
    const auto event = populate(actual, population);
    populate(expected, population);
    if (wide_text) {
        for (auto* host : {&actual, &expected}) {
            for (std::size_t index = 1; index < host->rows().size(); ++index) {
                host->rows()[index].entry_comment.assign(3 * 512 + 17, 'a' + index % 26);
                host->rows()[index].entry_comment[511] = '\0';
                host->rows()[index].exit_comment.assign(2 * 512 + 31, 'z' - index % 26);
                host->rows()[index].exit_comment[1023] = '\0';
            }
        }
    }
    reference_recut(expected, population, any);

    peak_bytes = 0;
    global_allocation::live_bytes = 0;
    global_allocation::on_recorded = record_bytes;
    global_allocation::recording = true;
    actual.recut(event);
    const auto retained_output_bytes = global_allocation::live_bytes;
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;

#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
    CHECK(scratch.comparisons > comparisons_before);
    CHECK(scratch.writes > writes_before);
#endif
    CHECK(actual.closed_trade_count() == expected.closed_trade_count());
    CHECK(any ? actual.closed_trade_count() == population + 1
              : actual.closed_trade_count() > population + 1);
    std::size_t represented_lots = 0;
    for (std::size_t lot = 0; lot < population; ++lot) {
        bool represented = false;
        for (std::size_t index = 1; index < actual.rows().size(); ++index)
            if (actual.rows()[index].entry_incarnation == lot + 100) represented = true;
        if (represented) ++represented_lots;
    }
    CHECK(represented_lots == population);
    const auto common = std::min(actual.rows().size(), expected.rows().size());
    for (std::size_t index = 0; index < common; ++index)
        same_trade(actual.rows()[index], expected.rows()[index]);
    CHECK(actual.phases() == expected.phases());
    CHECK(actual.adapter().*access(PhaseFinalTag{})
          == expected.adapter().*access(PhaseFinalTag{}));
    CHECK(actual.adapter().*access(PhaseDigestTag{})
          == expected.adapter().*access(PhaseDigestTag{}));
    CHECK(actual.adapter_hash() == expected.adapter_hash());
    CHECK(actual.counts() == expected.counts());
    for (std::size_t index = 0; index < actual.money().size(); ++index)
        CHECK(same_double(actual.money()[index], expected.money()[index]));
    CHECK(peak_bytes >= retained_output_bytes);
    const auto scratch_bytes = peak_bytes - retained_output_bytes;
    constexpr std::size_t kRecutScratchBytes = 4096;
    CHECK(scratch_bytes <= kRecutScratchBytes);
    std::printf("X06 lots=%zu replacements=%zu any=%d fee=%d scratch=%zu output=%zu bound=%zu\n",
                population, actual.closed_trade_count() - 1, any, static_cast<int>(fee),
                scratch_bytes, retained_output_bytes, kRecutScratchBytes);
}

void validation_order(bool any, int mode) {
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    ScratchFixture scratch;
    const auto scratch_digest = scratch.digest();
#endif
    Host actual(any, CommissionType::PERCENT, 0.0137);
    Host expected(any, CommissionType::PERCENT, 0.0137);
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    actual.set_trade_scratch(scratch.store);
#endif
    const auto event = populate(actual, 64);
    populate(expected, 64);
    const auto valid_rows = actual.rows();
    if (mode == 0) {
        actual.rows().back().entry_incarnation = actual.rows()[1].entry_incarnation;
        actual.rows()[1].entry_price = std::numeric_limits<double>::quiet_NaN();
    } else if (mode == 1) {
        actual.rows().back().qty = std::nextafter(actual.rows().back().qty,
                                                 std::numeric_limits<double>::infinity());
        actual.rows()[1].pnl = std::numeric_limits<double>::quiet_NaN();
    } else {
        actual.rows().back().max_runup = std::numeric_limits<double>::quiet_NaN();
    }
    const auto originals = actual.rows();
    const auto phases = actual.phases();
    const auto money = actual.money();
    const auto counts = actual.counts();
    const auto hash = actual.adapter_hash();
    bool rejected = false;
    try {
        actual.recut(event);
    } catch (const std::logic_error& error) {
        rejected = true;
        CHECK(std::strcmp(error.what(), "non-finite amended trade row") == 0);
    }
    CHECK(rejected == (mode == 2));
    CHECK(actual.rows().size() == originals.size());
    for (std::size_t index = 0; index < originals.size(); ++index)
        same_trade(actual.rows()[index], originals[index]);
    CHECK(actual.phases() == phases);
    CHECK(actual.counts() == counts);
    for (std::size_t index = 0; index < money.size(); ++index)
        CHECK(same_double(actual.money()[index], money[index]));
    CHECK(actual.adapter_hash() == hash);
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
#endif
    // A rejected amendment must leave the same store usable for a full recut.
    actual.rows() = valid_rows;
    actual.recut(event);
    reference_recut(expected, 64, any);
    CHECK(actual.rows().size() == expected.rows().size());
    for (std::size_t index = 0; index < std::min(actual.rows().size(), expected.rows().size()); ++index)
        same_trade(actual.rows()[index], expected.rows()[index]);
    CHECK(actual.phases() == expected.phases());
    CHECK(actual.adapter_hash() == expected.adapter_hash());
    CHECK(actual.counts() == expected.counts());
    for (std::size_t index = 0; index < money.size(); ++index)
        CHECK(same_double(actual.money()[index], expected.money()[index]));
#ifdef PINEFORGE_HAS_TRADE_SCRATCH_V1
    scratch.check_empty();
    CHECK(scratch.digest() == scratch_digest);
#endif
}
}  // namespace

int main() {
    for (const bool any : {false, true}) {
        for (const std::size_t population : {64u, 512u}) {
            exercise(population, any, CommissionType::PERCENT, 0.0);
            exercise(population, any, CommissionType::PERCENT, 0.0137);
            exercise(population, any, CommissionType::CASH_PER_CONTRACT, 0.07);
        }
        exercise(64, any, CommissionType::PERCENT, 0.0137, true);
        validation_order(any, 0);
        validation_order(any, 2);
    }
    validation_order(false, 1);
    std::printf("%s X06 global-exit recut scratch (%d failures)\n",
                failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
