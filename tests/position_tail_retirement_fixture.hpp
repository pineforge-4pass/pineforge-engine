#pragma once

// Compile the real adapter/hash TUs with observation only, as the existing
// position-tail and reversal-storage witnesses do. No layout/access rewrite.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_placement_retention.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace o1w_tail {
using namespace pineforge;
namespace ps = pineforge::source;

inline void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Work {
    std::uint64_t pairs = 0, placements = 0, families = 0, settlements = 0;
    std::uint64_t oracle_pairs = 0, oracle_placements = 0, oracle_families = 0;
    std::uint64_t sweeps = 0, hashes = 0, witnesses = 0, filled_witnesses = 0;
    // A scenario selects this handle only after observing its terminal fill.
    // The all-pairs walk, rather than the production memo, supplies its answer.
    std::uint64_t filled_candidate = 0, filled_candidate_first = 0;
    std::uint64_t filled_candidate_witnesses = 0;
    std::size_t peak_placements = 0, peak_families = 0, peak_archived = 0;
};
inline Work* counting = nullptr;
inline bool check_archive = true;

inline void work(const char* site, std::uint64_t count) {
    if (!counting) return;
    if (std::strcmp(site, "k3_pairs") == 0) counting->pairs += count;
    else if (std::strcmp(site, "hash_placements") == 0) counting->placements += count;
    else if (std::strcmp(site, "hash_families") == 0) counting->families += count;
    else if (std::strcmp(site, "settle_families") == 0) counting->settlements += count;
    else check(false, "unknown production loop counter");
}

// Independent all-pairs oracle. No use of production matching helpers,
// high-water cursors, cached answers or an expected implementation count.
template<class Candidates>
void k3_oracle(const ps::PlacementTable& rows, const Candidates& candidates,
               const std::vector<std::uint64_t>& actual) {
    if (!counting) return;
    ++counting->sweeps;
    std::vector<std::uint64_t> expected;
    for (const auto& candidate : candidates) {
        const auto& q = *candidate.second;
        std::uint64_t first = 0;
        for (const auto& row : rows) {
            const auto& peer = row.second;
            const bool exit = peer.family == ps::PineOrderFamily::ExitLimit
                || peer.family == ps::PineOrderFamily::ExitStop
                || peer.family == ps::PineOrderFamily::ExitTrail;
            if (!exit || peer.placement_cycle != q.placement_cycle) continue;
            ++counting->oracle_pairs;
            if (row.first <= candidate.first || peer.family != q.family
                || peer.source_id != q.source_id || peer.from_entry != q.from_entry
                || peer.placement_script_open_ms <= q.placement_script_open_ms
                || std::memcmp(&peer.exit_levels.stop, &q.exit_levels.stop, sizeof(double)) == 0)
                continue;
            if (first == 0 || row.first < first) first = row.first;
        }
        if (first != 0) {
            expected.push_back(first);
            ++counting->witnesses;
            if (rows.at(first).filled_exit) ++counting->filled_witnesses;
            if (candidate.first == counting->filled_candidate) {
                counting->filled_candidate_first = first;
                ++counting->filled_candidate_witnesses;
            }
        }
    }
    std::sort(expected.begin(), expected.end());
    check(expected == actual, "incremental K3 differs from full candidate/row walk");
}

template<class Families, class Archive>
void hash_oracle(const ps::PlacementTable& rows, const Families& families,
                 const Archive& archived, std::uint64_t digest) {
    if (!counting) return;
    ++counting->hashes;
    for (const auto& row : rows) {
        (void)row;
        ++counting->oracle_placements;
    }
    for (const auto& row : families) {
        (void)row;
        ++counting->oracle_families;
    }
    counting->peak_placements = std::max(counting->peak_placements, rows.size());
    counting->peak_families = std::max(counting->peak_families, families.size());
    counting->peak_archived = std::max(counting->peak_archived, archived.size());
    if (check_archive) {
        std::uint64_t expected = 0;
        for (const auto& row : archived) {
            check(families.find(row.first) == families.end(), "family active and archived");
            BrokerStateHashSink f;
            f.u(row.first); f.u(row.second.last); f.u(row.second.count); f.u(row.second.digest);
            expected ^= f.h;
        }
        check(expected == digest, "settled-family digest is stale");
    }
}
} // namespace o1w_tail

#define PINEFORGE_TAIL_WORK(site, count) ::o1w_tail::work(site, count)
#define PINEFORGE_RETIREMENT_OBSERVE(rows, candidates, first) \
    ::o1w_tail::k3_oracle(rows, candidates, first)
#define PINEFORGE_BROKER_HASH_OBSERVE(rows, families, archived, digest) \
    ::o1w_tail::hash_oracle(rows, families, archived, digest)
#include "../src/source/pine_adapter.cpp"
#include "../src/source/pine_state_hash.cpp"
#undef PINEFORGE_BROKER_HASH_OBSERVE
#undef PINEFORGE_RETIREMENT_OBSERVE
#undef PINEFORGE_TAIL_WORK

// Reuse exactly the original feed, commands and report owner. Its main is
// not called; no existing fixture, scale selector or assertion is changed.
#define main o1w_unused_scale_out_main
#include "test_position_tail_scale_out.cpp"
#undef main

namespace o1w_tail {
struct CountScope {
    Work* previous = counting;
    explicit CountScope(Work& value) { counting = &value; }
    ~CountScope() { counting = previous; }
};
struct Retention {
    bool previous = ps::detail::retain_retired_rows();
    explicit Retention(bool value) { ps::detail::set_retain_retired_rows(value); }
    ~Retention() { ps::detail::set_retain_retired_rows(previous); }
};

inline void check_work(const Work& value) {
    check(value.hashes > 0 && value.sweeps > 0, "work witness was not exercised");
    check(value.placements == value.oracle_placements, "hash placement count differs from walk");
    check(value.families == value.oracle_families, "hash family count differs from walk");
    check(value.pairs <= value.oracle_pairs, "K3 revisited more pairs than a full walk");
}

inline void equal_report(const ReportC& a, const ReportC& b) {
#define O1W_REPORT_FIELD(field) check(a.field == b.field, "report field differs: " #field)
    O1W_REPORT_FIELD(total_trades); O1W_REPORT_FIELD(trades_len); O1W_REPORT_FIELD(net_profit);
    O1W_REPORT_FIELD(input_bars_processed); O1W_REPORT_FIELD(script_bars_processed);
    O1W_REPORT_FIELD(security_feeds_total); O1W_REPORT_FIELD(security_eval_complete_total);
    O1W_REPORT_FIELD(security_eval_partial_total); O1W_REPORT_FIELD(magnifier_sub_bars_total);
    O1W_REPORT_FIELD(magnifier_sample_ticks_total); O1W_REPORT_FIELD(input_tf_seconds);
    O1W_REPORT_FIELD(script_tf_seconds); O1W_REPORT_FIELD(script_tf_ratio);
    O1W_REPORT_FIELD(needs_aggregation); O1W_REPORT_FIELD(bar_magnifier_enabled);
    O1W_REPORT_FIELD(security_diag_len); O1W_REPORT_FIELD(trace_len);
    O1W_REPORT_FIELD(trace_names_len); O1W_REPORT_FIELD(equity_curve_len);
    O1W_REPORT_FIELD(broker_state_hash_len);
#undef O1W_REPORT_FIELD
    check(std::memcmp(&a.metrics, &b.metrics, sizeof a.metrics) == 0, "report metrics differ");
    for (int i = 0; i < a.trades_len; ++i) {
        const auto& x = a.trades[i];
        const auto& y = b.trades[i];
#define O1W_TRADE_FIELD(field) check(x.field == y.field, "trade field differs: " #field)
        O1W_TRADE_FIELD(entry_time); O1W_TRADE_FIELD(exit_time);
        O1W_TRADE_FIELD(entry_price); O1W_TRADE_FIELD(exit_price);
        O1W_TRADE_FIELD(pnl); O1W_TRADE_FIELD(pnl_pct); O1W_TRADE_FIELD(is_long);
        O1W_TRADE_FIELD(max_runup); O1W_TRADE_FIELD(max_drawdown); O1W_TRADE_FIELD(qty);
        O1W_TRADE_FIELD(commission); O1W_TRADE_FIELD(entry_bar_index);
        O1W_TRADE_FIELD(exit_bar_index); O1W_TRADE_FIELD(open_at_end);
#undef O1W_TRADE_FIELD
    }
    for (int i = 0; i < a.security_diag_len; ++i) {
        const auto& x = a.security_diag[i]; const auto& y = b.security_diag[i];
        check(x.sec_id == y.sec_id && x.feed_count == y.feed_count
              && x.eval_complete_count == y.eval_complete_count
              && x.eval_partial_count == y.eval_partial_count, "security report differs");
    }
    for (int i = 0; i < a.trace_len; ++i) {
        const auto& x = a.trace[i]; const auto& y = b.trace[i];
        check(x.timestamp == y.timestamp && x.bar_index == y.bar_index
              && x.name_id == y.name_id && (x.value == y.value
                  || (std::isnan(x.value) && std::isnan(y.value))), "trace report differs");
    }
    for (int i = 0; i < a.trace_names_len; ++i)
        check(std::strcmp(a.trace_names[i], b.trace_names[i]) == 0, "trace name differs");
    for (std::int64_t i = 0; i < a.equity_curve_len; ++i) {
        const auto& x = a.equity_curve[i]; const auto& y = b.equity_curve[i];
        check(x.time_ms == y.time_ms && x.equity == y.equity
              && x.open_profit == y.open_profit, "equity report differs");
    }
    // Broker hash values intentionally describe different retirement
    // representations. Their population is still required to agree.
}

inline std::uint64_t trade_identity(const Trade& t) {
    BrokerStateHashSink f;
    f.i(t.entry_time); f.i(t.exit_time); f.d(t.entry_price); f.d(t.exit_price);
    f.d(t.qty); f.d(t.pnl); f.d(t.pnl_pct); f.b(t.is_long);
    f.i(t.entry_bar_index); f.i(t.exit_bar_index);
    f.s(t.entry_id); f.s(t.entry_comment); f.s(t.exit_comment); f.s(t.exit_id);
    f.b(t.exit_from_bracket); f.d(t.max_runup); f.d(t.max_drawdown); f.d(t.commission);
    f.u(t.entry_incarnation); f.b(t.open_at_end); f.u(static_cast<std::uint64_t>(t.close_cause));
    return f.h;
}

template<class Host>
void equal_outcome(Host& a, Host& b) {
    check(a.last_error().empty() && b.last_error().empty(), "host run failed");
    Report ar, br;
    a.fill_report(&ar.value); b.fill_report(&br.value);
    equal_report(ar.value, br.value);
    check(a.trade_count() == b.trade_count(), "trade population differs");
    for (int i = 0; i < a.trade_count(); ++i)
        check(trade_identity(a.get_trade(i)) == trade_identity(b.get_trade(i)),
              "full trade identity differs");
}

inline Work scale_run(int cycles, bool side, bool retain) {
    const auto bars = feed(cycles);
    ScaleOut host(side);
    Retention reference(retain);
    Work result;
    CountScope scope(result);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check(host.last_error().empty(), "scale-out run failed");
    check(host.scaled_while_open >= cycles && host.trade_count() >= cycles,
          "scale-out work fixture lost its original population");
    check_work(result);
    return result;
}
} // namespace o1w_tail
