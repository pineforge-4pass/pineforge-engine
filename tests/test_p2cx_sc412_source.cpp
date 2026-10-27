#include "p2cx_sc412_io.hpp"

// Reuse the existing partial-fill tape and leaf serializer. The fixture's
// own main remains callable but is not our row; its file stays unedited.
#define main sc412_original_idpair_main
#include "test_adapter_idpair_ram.cpp"
#undef main

namespace sc412_source {
// BASE GOLDENS ONLY: this constant block was recorded from the unchanged
// engine (frozen base 6b33c47fda4e), never from the cached path.
// No expected hash may come from the changed product. Missing is a failure.
struct SourceGolden {
    std::uint64_t trades, equity, digests, observations, final_digest, final_leaves;
    std::uint64_t trade_count, equity_count, digest_count, observation_count;
};
inline constexpr std::optional<SourceGolden> kFrozenBasePartialFillGolden = SourceGolden{0xfaef911c850b9799ULL, 0x23ee94bd5e8fd03aULL, 0xd7b6d2c373c1690fULL, 0xdeb6770c10307e67ULL, 0x26efe3569ea4d9d8ULL, 0xcb33ebf48b35ad82ULL, 36, 128, 128, 128};

using Adapter = pineforge::source::PineExecutionAdapter;
template<class Tag, auto Member> struct Access { friend auto access(Tag) { return Member; } };
struct Cohorts { friend auto access(Cohorts); };
struct Ranks { friend auto access(Ranks); };
struct Cycles { friend auto access(Cycles); };
struct Placements { friend auto access(Placements); };
struct Minima { friend auto access(Minima); };
struct Curve { friend auto access(Curve); };
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<Ranks, &Adapter::exit_rank_keys_>;
template struct Access<Cycles, &Adapter::consumed_partial_exit_cycles_>;
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Minima, &pineforge::source::PlacementTable::erased_minima_>;
template struct Access<Curve, &pineforge::BacktestEngine::equity_curve_>;

class CacheHost final : public pineforge::source::PineStrategyHost {
public:
    CacheHost() {
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.pyramiding = 2;
        config.process_orders_on_close = true;
        config.close_entries_rule_any = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC"); set_syminfo_session("24x7"); set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) observations.clear();
        // Same public source workload as MultiEntryHost in the reused file:
        // two simultaneous openings, repeated ids and partial bracket fills.
        const int cycle = index / 10;
        if (cycle < 12) {
            const auto suffix = std::to_string(cycle % 3);
            const std::string a = "A" + suffix, b = "B" + suffix;
            if (index % 10 == 0) strategy_entry(a, true, kNa, kNa, 4.0);
            if (index % 10 == 1) strategy_entry(b, true, kNa, kNa, 4.0);
            if (index % 10 == 2 || index % 10 == 4)
                strategy_exit("half-" + a, a, 100.5, 99.5, kNa, kNa, kNa, 50.0);
            if (index % 10 == 3 || index % 10 == 5)
                strategy_exit("half-" + b, b, 100.5, 99.5, kNa, kNa, kNa, 50.0);
            if (index % 10 == 6) strategy_close_all();
        }
        observations.push_back(leaves(adapter_));
    }
    const auto& curve() const { return this->*access(Curve{}); }
    const auto& hashes() const { return broker_state_hashes_; }
    Leaves final_leaves() const { return leaves(adapter_); }
    void assert_engaged() {
        // Every uncached successful lookup must pread at least once. Count
        // real calls for 256 queries on each actual adapter-owned table.
        // No capacity override, disabled adapter, product hook or counter.
        const auto fewer = [](sc412::Family family, auto lookup, const char* why) {
            sc412::Measure measure;
            for (int n = 0; n < 256; ++n) sc412::require(lookup(), "adapter lookup present");
            const auto io = measure.finish()[family];
            std::printf("sc412 adapter %s preads=%llu uncached_lower_bound=256 bytes=%llu\n",
                        why, static_cast<unsigned long long>(io.reads),
                        static_cast<unsigned long long>(io.read_bytes));
            sc412::require(io.reads < 256, why);
        };
        auto& cohorts = adapter_.*access(Cohorts{});
        std::string cohort_key;
        cohorts.for_each_id([&](const std::string& key) { if (cohort_key.empty()) cohort_key = key; });
        sc412::require(!cohort_key.empty(), "actual cohort table nonempty");
        fewer(sc412::ids, [&] { return cohorts.find(cohort_key) != cohorts.end(); },
              "cohort cache strictly fewer calls");
        auto& ranks = adapter_.*access(Ranks{});
        const auto rank_key = ranks.key(ranks.first_inserted());
        fewer(sc412::ids, [&] { return ranks.find(rank_key).has_value(); },
              "rank cache strictly fewer calls");
        auto& cycles = adapter_.*access(Cycles{});
        sc412::require(!cycles.empty(), "actual consumed table nonempty");
        const auto cycle_key = cycles.begin()->first;
        fewer(sc412::pairs, [&] { return cycles.lookup(cycle_key).has_value(); },
              "consumed cache strictly fewer calls");
        auto& minima = (adapter_.*access(Placements{})).*access(Minima{});
        sc412::require(!minima.empty(), "actual erased-minima table nonempty");
        const auto entry = *minima.begin();
        fewer(sc412::pairs, [&] {
            return entry.from_entry ? minima.lookup(entry.first, *entry.from_entry).has_value()
                                    : minima.lookup(entry.first).has_value();
        }, "erased-minima cache strictly fewer calls");
    }
    std::vector<Leaves> observations;
};

// Canonical bytes of every Trade field (never object padding or string
// pointers). The frozen-base hash covers the entire stream.
std::vector<unsigned char> trades(const CacheHost& host) {
    std::vector<unsigned char> bytes;
    const auto append = [&](const auto& value) {
        const auto* p = reinterpret_cast<const unsigned char*>(&value);
        bytes.insert(bytes.end(), p, p + sizeof value);
    };
    const auto text = [&](const std::string& value) {
        const std::uint64_t size = value.size();
        append(size);
        bytes.insert(bytes.end(), value.begin(), value.end());
    };
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto t = host.get_trade(i);
        append(t.entry_time); append(t.exit_time);
        append(t.entry_bar_index); append(t.exit_bar_index);
        append(t.entry_incarnation); append(t.is_long); append(t.open_at_end);
        append(t.exit_from_bracket); append(t.close_cause);
        append(t.entry_price); append(t.exit_price); append(t.qty); append(t.pnl);
        append(t.pnl_pct); append(t.commission); append(t.max_runup); append(t.max_drawdown);
        text(t.entry_id); text(t.exit_id); text(t.entry_comment); text(t.exit_comment);
    }
    return bytes;
}
std::uint64_t leaf_hash(std::uint64_t seed, const Leaves& leaf) {
    const std::array<std::uint64_t, 4> fields{
        leaf.consumed, leaf.erased, leaf.erased_rows, static_cast<std::uint64_t>(leaf.consumed_keys)};
    return sc412::hash(seed, fields.data(), sizeof fields);
}
void identity() {
    const auto bars = tape();
    CacheHost host;
    for (int repeat = 0; repeat < 2; ++repeat) {
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        sc412::require(host.last_error().empty(), "source run completes");
        sc412::require(host.trade_count() > 0 && !host.curve().empty() && !host.hashes().empty(),
                       "nonempty source trades, equity and digests");
        bool partial = false;
        for (int n = 0; n < host.trade_count(); ++n)
            partial = partial || (host.get_trade(n).exit_from_bracket && host.get_trade(n).qty < 4.0);
        sc412::require(partial && host.final_leaves().consumed_keys == 6
                       && host.final_leaves().erased_rows > 0, "partial fills and both pair stores exercised");
        const auto trade_bytes = trades(host);
        std::uint64_t observation_hash = sc412::basis;
        for (const auto& leaf : host.observations) observation_hash = leaf_hash(observation_hash, leaf);
        const SourceGolden actual{
            sc412::hash(sc412::basis, trade_bytes.data(), trade_bytes.size()),
            sc412::hash(sc412::basis, host.curve().data(), host.curve().size() * sizeof(pf_equity_point_t)),
            sc412::hash(sc412::basis, host.hashes().data(), host.hashes().size() * sizeof(std::uint64_t)),
            observation_hash, host.broker_state_hash(), leaf_hash(sc412::basis, host.final_leaves()),
            static_cast<std::uint64_t>(host.trade_count()), host.curve().size(), host.hashes().size(),
            host.observations.size()};
        std::printf("sc412 observed source trades=%016llx equity=%016llx digests=%016llx "
                    "observations=%016llx final_digest=%016llx final_leaves=%016llx "
                    "counts=%llu/%llu/%llu/%llu\n",
                    static_cast<unsigned long long>(actual.trades),
                    static_cast<unsigned long long>(actual.equity),
                    static_cast<unsigned long long>(actual.digests),
                    static_cast<unsigned long long>(actual.observations),
                    static_cast<unsigned long long>(actual.final_digest),
                    static_cast<unsigned long long>(actual.final_leaves),
                    static_cast<unsigned long long>(actual.trade_count),
                    static_cast<unsigned long long>(actual.equity_count),
                    static_cast<unsigned long long>(actual.digest_count),
                    static_cast<unsigned long long>(actual.observation_count));
        host.assert_engaged();
        sc412::require(kFrozenBasePartialFillGolden.has_value(),
                       "BASE GOLDENS MISSING: the frozen-base source hashes must be recorded");
        const auto& expected = *kFrozenBasePartialFillGolden;
        sc412::require(actual.trades == expected.trades && actual.equity == expected.equity
            && actual.digests == expected.digests && actual.observations == expected.observations
            && actual.final_digest == expected.final_digest && actual.final_leaves == expected.final_leaves
            && actual.trade_count == expected.trade_count && actual.equity_count == expected.equity_count
            && actual.digest_count == expected.digest_count && actual.observation_count == expected.observation_count,
            "complete trade/equity streams, every digest and leaves match BASE GOLDENS");
    }
}
} // namespace sc412_source

int main() {
    try {
        sc412_source::identity();
        std::puts("test_p2cx_sc412_source: trades/equity/digests match frozen-base goldens");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_p2cx_sc412_source: %s\n", error.what());
        return 1;
    }
}
