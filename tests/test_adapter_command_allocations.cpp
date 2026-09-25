// R5 lane PERF-L5: the Pine adapter's command path copies and allocates less,
// and every value it feeds is the one it fed.
//
// A re-issued source command used to copy the predecessor's whole placement
// row (1.9 KB) to read a dozen of its fields, allocate the L4c priority's
// candidate list at every accepted command and every bar open whatever the
// run's configuration, and give back the replaced request's source-key node
// only to allocate a new one for its successor. The row is now read in place
// (its legs, the one field reset before the last read, move out at the
// reset), the candidate list is built only when OrderPriority::select could
// answer with it, and the key's node is carried over to the successor. The
// witness is data:
//
//   * three scripts that re-issue commands at every bar -- an exit bracket
//     re-priced every bar (the runtime replay's script), a limit entry
//     re-priced while flat with its bracket, and a trailing exit whose offset
//     alone changes (kept in place) or whose activation changes (replaced) --
//     each with process_orders_on_close off and on, and with the bar
//     magnifier off and on;
//   * every recorded broker-state hash row, the final broker-state hash and
//     every trade, folded into digests;
//
// observed on engine main f71cd820 (the tree before this lane) and pinned
// here. Portability follows test_adapter_receipts_in_place.cpp: the
// projection folds one fixed execution hash instead of the consumer's
// continuation, and every price sits on its tick.
//
// And the allocation count of a re-issued strategy.exit, taken inside the
// call after a warm-up: at most kAllocationsPerReissue. This half FAILS on
// f71cd820, where the call allocated the priority candidates for each of its
// two legs and a key node for each successor on top of what it still needs
// (a definition per replaced leg, the bracket roster's member and the leg
// lifecycle's prices, the reservation scan).
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// f71cd820 library with -DPINEFORGE_L5_HARVEST, which prints the observed
// values as the initializers below instead of checking them (the allocation
// half is skipped there). Rebuild them the same way; never edit one by hand.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

// Counts the re-issued exit's heap allocations: every replaceable form, one
// allocator.
#include "global_allocation_replacement.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
using namespace pineforge;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd00f5ull;
constexpr std::int64_t T = 1736121600000LL;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr int kBars = 60;
// A re-issued exit allocates at most this many times inside the call.
constexpr std::size_t kAllocationsPerReissue = 8;

enum class Script { ExitReissue, FarReissue, EntryReissue, TrailReissue };

const char* name_of(Script script) {
    switch (script) {
    case Script::ExitReissue: return "ExitReissue";
    case Script::FarReissue: return "FarReissue";
    case Script::EntryReissue: return "EntryReissue";
    case Script::TrailReissue: return "TrailReissue";
    }
    return "?";
}

// A slow wave in quarter ticks: prices rise and fall through the levels the
// scripts place, so brackets fill and re-open.
std::vector<Bar> tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i) {
        const int phase = i % 12;
        const int triangle = phase < 6 ? phase : 12 - phase;
        const double p = 100.0 + 0.75 * triangle + 0.25 * (i % 3);
        bars.push_back({p, p + 0.5, p - 0.5, p + 0.25, 1.0,
                        T + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

class ReissueHost final : public source::PineStrategyHost {
public:
    ReissueHost(Script script, bool pooc) : script_(script) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.0;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.process_orders_on_close = pooc;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kProbeExecutionHash);
    }

    // Allocations inside the re-issued exit calls of bars [10, kBars - 5).
    std::size_t counted = 0;
    int counted_calls = 0;

    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        switch (script_) {
        case Script::ExitReissue: exit_reissue(i, bar, 2.0); break;
        case Script::FarReissue: exit_reissue(i, bar, 50.0); break;
        case Script::EntryReissue: entry_reissue(i, bar); break;
        case Script::TrailReissue: trail_reissue(i, bar); break;
        }
    }

private:
    // The runtime replay's script: one entry, then its bracket re-priced at
    // every bar, `away` from the close; every 15th bar the book is closed
    // and re-entered.
    void exit_reissue(int i, const Bar& bar, double away) {
        if (i % 15 == 0) strategy_entry("L", true);
        if (i % 15 == 14) strategy_close_all();
        if (live_position_size() > 0.0) {
            const bool counting = i >= 10 && i < kBars - 5;
            const std::size_t before = global_allocation::allocations;
            strategy_exit("guard", "L", bar.close + away, bar.close - away);
            if (counting) {
                counted += global_allocation::allocations - before;
                ++counted_calls;
            }
        }
    }

    // A limit entry re-priced while flat, bracketed at once; the book is
    // closed every 9th bar.
    void entry_reissue(int i, const Bar& bar) {
        if (live_position_size() == 0.0) {
            strategy_entry("E", true, bar.close - 0.25);
            strategy_exit("x", "E", bar.close + 0.75, bar.close - 1.25);
        }
        if (i % 9 == 8) strategy_close_all();
    }

    // A trailing exit: its offset alone changes on most bars (the leg is
    // kept in place), its activation every 4th bar (the leg is replaced).
    void trail_reissue(int i, const Bar&) {
        if (i % 20 == 0) strategy_entry("T", true);
        if (live_position_size() > 0.0) {
            const double points = (i / 4) % 2 == 0 ? 4.0 : 6.0;
            const double offset = 2.0 + (i % 3);
            strategy_exit("t", "T", kNa, kNa, points, offset, kNa, 100.0, "");
        }
        if (i % 20 == 19) strategy_close_all();
    }

    Script script_;
};

std::uint64_t fold(std::uint64_t h, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        h ^= (value >> (8 * i)) & 0xffu;
        h *= 1099511628211ull;
    }
    return h;
}
std::uint64_t fold_double(std::uint64_t h, double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return fold(h, bits);
}

struct Observed {
    std::uint64_t rows_digest = 0;
    std::uint64_t rows = 0;
    std::uint64_t final_hash = 0;
    std::uint64_t trades_digest = 0;
    int trades = 0;
};

Observed run(ReissueHost& host, bool magnifier) {
    const auto bars = tape();
    if (magnifier) {
        host.run(bars.data(), static_cast<int>(bars.size()), "", "", true, 4,
                 MagnifierDistribution::ENDPOINTS);
    } else {
        host.run(bars.data(), static_cast<int>(bars.size()));
    }
    CHECK(host.last_error().empty());
    Observed out;
    ReportC report{};
    host.fill_report(&report);
    std::uint64_t rows = 1469598103934665603ull;
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        rows = fold(rows, report.broker_state_hash[i]);
    out.rows = static_cast<std::uint64_t>(report.broker_state_hash_len);
    out.rows_digest = rows;
    std::uint64_t trades = 1469598103934665603ull;
    for (int i = 0; i < report.trades_len; ++i) {
        const TradeC& t = report.trades[i];
        trades = fold(trades, static_cast<std::uint64_t>(t.entry_time));
        trades = fold(trades, static_cast<std::uint64_t>(t.exit_time));
        trades = fold_double(trades, t.entry_price);
        trades = fold_double(trades, t.exit_price);
        trades = fold_double(trades, t.qty);
        trades = fold(trades, static_cast<std::uint64_t>(t.open_at_end));
    }
    out.trades = report.trades_len;
    out.trades_digest = trades;
    BacktestEngine::free_report(&report);
    out.final_hash = host.broker_state_hash();
    return out;
}

Observed observe(Script script, bool pooc, bool magnifier) {
    ReissueHost host(script, pooc);
    return run(host, magnifier);
}

struct Pin {
    Script script;
    bool pooc;
    bool magnifier;
    std::uint64_t rows_digest;
    std::uint64_t rows;
    std::uint64_t final_hash;
    std::uint64_t trades_digest;
    int trades;
};

constexpr Script kScripts[] = {Script::ExitReissue, Script::FarReissue, Script::EntryReissue,
                               Script::TrailReissue};

#ifndef PINEFORGE_L5_HARVEST
// expectation corrected (32 values), because v19's broker-state hash folds the closed rows as their count and a running digest, each row once when it is final (pineforge-broker-state/v19); this host projects one fixed execution hash, so the continuation does not enter; the recorded row count, the trades and the trade digest did not move; re-harvested the same way on INT19's tree; V19-A's tip (6211dc94) gives the same rows:
//   Script::ExitReissue/false/false: rows_digest 0x45d5a3f28f256742ull -> 0xc5e49674997eb2ffull, final_hash 3451175041936365899ull -> 2994629124978087589ull
//   Script::ExitReissue/false/true: rows_digest 0x80eab7cde808d29dull -> 0x50af34b38baba1a4ull, final_hash 3349836046584918293ull -> 240956321056682147ull
//   Script::ExitReissue/true/false: rows_digest 0xa407c51973a95e40ull -> 0xfdd67afcaf5a1874ull, final_hash 15593133390039186596ull -> 4256117574465486300ull
//   Script::ExitReissue/true/true: rows_digest 0x5dc24488611c94f6ull -> 0x463341731946cf56ull, final_hash 13389315872770548534ull -> 1698320762378244830ull
//   Script::FarReissue/false/false: rows_digest 0x90c55410be880c61ull -> 0x20b8d643a3410913ull, final_hash 4795263033029944303ull -> 16681204851028006305ull
//   Script::FarReissue/false/true: rows_digest 0x53de86c211400e2bull -> 0x768203bfe6d5db96ull, final_hash 7085062931073367593ull -> 18345791997150650615ull
//   Script::FarReissue/true/false: rows_digest 0x41a848f356565f04ull -> 0xb81f553a69cfde7aull, final_hash 4162186342665973035ull -> 13089464396311421050ull
//   Script::FarReissue/true/true: rows_digest 0xd19316cdd18ba053ull -> 0x92fd0e65d9dada81ull, final_hash 10470574206823754130ull -> 11491809416163993779ull
//   Script::EntryReissue/false/false: rows_digest 0x753c27da77a1c5c2ull -> 0x7eeb1bf4f670ff49ull, final_hash 14321596246514279969ull -> 2762439606879927543ull
//   Script::EntryReissue/false/true: rows_digest 0x695b8c3c1a8244cdull -> 0x5e6b74b25add1d67ull, final_hash 10240597316547809238ull -> 11432021388847824369ull
//   Script::EntryReissue/true/false: rows_digest 0xf584c31ffd881384ull -> 0xfb14c04ecd2e82f5ull, final_hash 934878913592489708ull -> 15037004286527014866ull
//   Script::EntryReissue/true/true: rows_digest 0x3ea867ac99f2c805ull -> 0x45797c58e517d01eull, final_hash 17516905965457890816ull -> 9006290945422360539ull
//   Script::TrailReissue/false/false: rows_digest 0xbd823480afcc5235ull -> 0x10c60c48a36cd05full, final_hash 14116140155614560946ull -> 18306373629755674648ull
//   Script::TrailReissue/false/true: rows_digest 0xb2c05957b38959a0ull -> 0x88cc0cc66ad1062eull, final_hash 18289793877175804168ull -> 12308026923819682198ull
//   Script::TrailReissue/true/false: rows_digest 0xf40cdfc297ff459full -> 0x44d64237cf2066ecull, final_hash 9568522095434219343ull -> 2164334703724393590ull
//   Script::TrailReissue/true/true: rows_digest 0xc87c34af2c4b274bull -> 0x5397e85d70d98a2aull, final_hash 1972430874561998796ull -> 1382406362180372336ull
// expectation corrected (v19-E, 32 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); this host projects one fixed execution hash, so the continuation does not enter; the recorded row count, the trades and the trade digest did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   Script::ExitReissue/false/false: rows_digest 0xc5e49674997eb2ffull -> 0x378e8175f07c1d5dull, final_hash 2994629124978087589ull -> 5455427604675993531ull
//   Script::ExitReissue/false/true: rows_digest 0x50af34b38baba1a4ull -> 0x9d581c7efdf21626ull, final_hash 240956321056682147ull -> 5124001802418948517ull
//   Script::ExitReissue/true/false: rows_digest 0xfdd67afcaf5a1874ull -> 0x123a4d26c1440e48ull, final_hash 4256117574465486300ull -> 10475037478956129116ull
//   Script::ExitReissue/true/true: rows_digest 0x463341731946cf56ull -> 0xb8720a7926815bb3ull, final_hash 1698320762378244830ull -> 12173222249000782328ull
//   Script::FarReissue/false/false: rows_digest 0x20b8d643a3410913ull -> 0xdece58a579c5d8e0ull, final_hash 16681204851028006305ull -> 7214211385822448325ull
//   Script::FarReissue/false/true: rows_digest 0x768203bfe6d5db96ull -> 0x0bb13210fd7346c1ull, final_hash 18345791997150650615ull -> 10717416948830322455ull
//   Script::FarReissue/true/false: rows_digest 0xb81f553a69cfde7aull -> 0xfa15c45dc0196e57ull, final_hash 13089464396311421050ull -> 14180767476349608391ull
//   Script::FarReissue/true/true: rows_digest 0x92fd0e65d9dada81ull -> 0xdd4e85623e945fdaull, final_hash 11491809416163993779ull -> 12801755778883560301ull
//   Script::EntryReissue/false/false: rows_digest 0x7eeb1bf4f670ff49ull -> 0xf3e42100cdc76867ull, final_hash 2762439606879927543ull -> 18427522613823696029ull
//   Script::EntryReissue/false/true: rows_digest 0x5e6b74b25add1d67ull -> 0x1781a3542d87a7c9ull, final_hash 11432021388847824369ull -> 5868802478351644387ull
//   Script::EntryReissue/true/false: rows_digest 0xfb14c04ecd2e82f5ull -> 0x592d4b5617d2b6fcull, final_hash 15037004286527014866ull -> 11220719029914513450ull
//   Script::EntryReissue/true/true: rows_digest 0x45797c58e517d01eull -> 0x4afd65bc6d977d03ull, final_hash 9006290945422360539ull -> 12913321619721074841ull
//   Script::TrailReissue/false/false: rows_digest 0x10c60c48a36cd05full -> 0x08a2398c615ddd5bull, final_hash 18306373629755674648ull -> 8598862747086083069ull
//   Script::TrailReissue/false/true: rows_digest 0x88cc0cc66ad1062eull -> 0x56954a0db7786b71ull, final_hash 12308026923819682198ull -> 5779925080749936105ull
//   Script::TrailReissue/true/false: rows_digest 0x44d64237cf2066ecull -> 0x09315718e551137full, final_hash 2164334703724393590ull -> 269689715775399305ull
//   Script::TrailReissue/true/true: rows_digest 0x5397e85d70d98a2aull -> 0x5ef417ff409d6fadull, final_hash 1382406362180372336ull -> 3401687048604417982ull
// expectation corrected (v19-D, 8 values), because v19-D keeps fewer retired rows and folds a bracket family's erased members behind a retained one as runs (pineforge-source-adapter/v4: K1 releases a current-cycle leg the revival's superseded test answers for; BracketRoster parks); this host projects one fixed execution hash, so the continuation does not enter; the recorded row count, the trades and the trade digest did not move; harvested with the TU's own switch on this tree (r5/v19-d):
//   Script::EntryReissue/false/false: rows_digest 0xf3e42100cdc76867ull -> 0x4530d69ae4e1a298ull, final_hash 18427522613823696029ull -> 17005496595750190628ull
//   Script::EntryReissue/false/true: rows_digest 0x1781a3542d87a7c9ull -> 0xf4677e5c1916337full, final_hash 5868802478351644387ull -> 9541687758077282854ull
//   Script::EntryReissue/true/false: rows_digest 0x592d4b5617d2b6fcull -> 0x379d985d7c93b652ull, final_hash 11220719029914513450ull -> 7904694474988279479ull
//   Script::EntryReissue/true/true: rows_digest 0x4afd65bc6d977d03ull -> 0x2fd97b436aac3c5dull, final_hash 12913321619721074841ull -> 8582209322861755784ull
// expectation corrected (V19-FIX, 32 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; this host projects one fixed execution hash, so the continuation does not enter; the recorded row count, the trades and the trade digest did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   Script::ExitReissue/false/false: rows_digest 0x378e8175f07c1d5dull -> 0xab588834fadd3803ull, final_hash 5455427604675993531ull -> 5699976773346796219ull
//   Script::ExitReissue/false/true: rows_digest 0x9d581c7efdf21626ull -> 0x31d20a68825b4041ull, final_hash 5124001802418948517ull -> 5368550971089751205ull
//   Script::ExitReissue/true/false: rows_digest 0x123a4d26c1440e48ull -> 0xcecfebbbe86fc767ull, final_hash 10475037478956129116ull -> 6863695800300943516ull
//   Script::ExitReissue/true/true: rows_digest 0xb8720a7926815bb3ull -> 0x6209f50bb7d9d6acull, final_hash 12173222249000782328ull -> 1107562895774304952ull
//   Script::FarReissue/false/false: rows_digest 0xdece58a579c5d8e0ull -> 0xdf6b23adbf03e66eull, final_hash 7214211385822448325ull -> 873772099948613669ull
//   Script::FarReissue/false/true: rows_digest 0x0bb13210fd7346c1ull -> 0xa3863ebe522e6dbeull, final_hash 10717416948830322455ull -> 4285418729649473335ull
//   Script::FarReissue/true/false: rows_digest 0xfa15c45dc0196e57ull -> 0x688ef29ae4ccdebeull, final_hash 14180767476349608391ull -> 1841254732555988423ull
//   Script::FarReissue/true/true: rows_digest 0xdd4e85623e945fdaull -> 0xf4c8662f18c74e7bull, final_hash 12801755778883560301ull -> 462243035089940333ull
//   Script::EntryReissue/false/false: rows_digest 0x4530d69ae4e1a298ull -> 0x80ea582d7a285f56ull, final_hash 17005496595750190628ull -> 2970596418120183524ull
//   Script::EntryReissue/false/true: rows_digest 0xf4677e5c1916337full -> 0xa4e881bd0166e308ull, final_hash 9541687758077282854ull -> 10388992876317270182ull
//   Script::EntryReissue/true/false: rows_digest 0x379d985d7c93b652ull -> 0xc01d27d0c8fd1cadull, final_hash 7904694474988279479ull -> 8910455723513195319ull
//   Script::EntryReissue/true/true: rows_digest 0x2fd97b436aac3c5dull -> 0x2bf51a0396f73773ull, final_hash 8582209322861755784ull -> 5882765811046345928ull
//   Script::TrailReissue/false/false: rows_digest 0x08a2398c615ddd5bull -> 0x27b9526d5e960dafull, final_hash 8598862747086083069ull -> 1551066713552723293ull
//   Script::TrailReissue/false/true: rows_digest 0x56954a0db7786b71ull -> 0x12bab345b4c21bdeull, final_hash 5779925080749936105ull -> 5117488907256490505ull
//   Script::TrailReissue/true/false: rows_digest 0x09315718e551137full -> 0xbae98e9f6da67e3dull, final_hash 269689715775399305ull -> 1320551696866588489ull
//   Script::TrailReissue/true/true: rows_digest 0x5ef417ff409d6fadull -> 0x7e3565d7ef6b9c34ull, final_hash 3401687048604417982ull -> 12226981095802833438ull
const Pin kPins[] = {
    {Script::ExitReissue, false, false, 0xab588834fadd3803ull, 60ull, 5699976773346796219ull, 0x584fc4b9ebbba609ull, 4},
    {Script::ExitReissue, false, true, 0x31d20a68825b4041ull, 60ull, 5368550971089751205ull, 0x584fc4b9ebbba609ull, 4},
    {Script::ExitReissue, true, false, 0xcecfebbbe86fc767ull, 60ull, 6863695800300943516ull, 0x41e6c3b1e0481c20ull, 4},
    {Script::ExitReissue, true, true, 0x6209f50bb7d9d6acull, 60ull, 1107562895774304952ull, 0x41e6c3b1e0481c20ull, 4},
    {Script::FarReissue, false, false, 0xdf6b23adbf03e66eull, 60ull, 873772099948613669ull, 0x7aac154ea6a8629aull, 4},
    {Script::FarReissue, false, true, 0xa3863ebe522e6dbeull, 60ull, 4285418729649473335ull, 0x7aac154ea6a8629aull, 4},
    {Script::FarReissue, true, false, 0x688ef29ae4ccdebeull, 60ull, 1841254732555988423ull, 0xfdb435b76f2571b6ull, 4},
    {Script::FarReissue, true, true, 0xf4c8662f18c74e7bull, 60ull, 462243035089940333ull, 0xfdb435b76f2571b6ull, 4},
    {Script::EntryReissue, false, false, 0x80ea582d7a285f56ull, 60ull, 2970596418120183524ull, 0xfa79a63a9a8de06aull, 29},
    {Script::EntryReissue, false, true, 0xa4e881bd0166e308ull, 60ull, 10388992876317270182ull, 0xdc986aa5f5cc536aull, 29},
    {Script::EntryReissue, true, false, 0xc01d27d0c8fd1cadull, 60ull, 8910455723513195319ull, 0xfa79a63a9a8de06aull, 29},
    {Script::EntryReissue, true, true, 0x2bf51a0396f73773ull, 60ull, 5882765811046345928ull, 0xdc986aa5f5cc536aull, 29},
    {Script::TrailReissue, false, false, 0x27b9526d5e960dafull, 60ull, 1551066713552723293ull, 0xd64b560956c46b33ull, 3},
    {Script::TrailReissue, false, true, 0x12bab345b4c21bdeull, 60ull, 5117488907256490505ull, 0x5241bc8c55a4a4a3ull, 3},
    {Script::TrailReissue, true, false, 0xbae98e9f6da67e3dull, 60ull, 1320551696866588489ull, 0xcdcafa54f49f508aull, 3},
    {Script::TrailReissue, true, true, 0x7e3565d7ef6b9c34ull, 60ull, 12226981095802833438ull, 0x45d4c2aceeb466faull, 3},
};

void the_scripts_produce_the_values_pinned_before_the_lane() {
    for (const Pin& pin : kPins) {
        const Observed got = observe(pin.script, pin.pooc, pin.magnifier);
        const int before = failures;
        CHECK(got.rows_digest == pin.rows_digest);
        CHECK(got.rows == pin.rows);
        CHECK(got.final_hash == pin.final_hash);
        CHECK(got.trades_digest == pin.trades_digest);
        CHECK(got.trades == pin.trades);
        CHECK(got.trades > 0);
        if (failures != before) {
            std::fprintf(stderr, "  script %s, pooc %d, magnifier %d moved\n",
                         name_of(pin.script), pin.pooc ? 1 : 0, pin.magnifier ? 1 : 0);
        }
    }
}

void a_reissued_exit_allocates_at_most_its_bound() {
    for (const bool magnifier : {false, true}) {
        ReissueHost host(Script::FarReissue, false);
        run(host, magnifier);
        CHECK(host.counted_calls > 20);
        const double per_call = host.counted_calls > 0
            ? static_cast<double>(host.counted) / host.counted_calls : 0.0;
        std::printf("  re-issued strategy.exit (magnifier %s): %zu allocations over %d calls, "
                    "%.2f per call (bound %zu)\n",
                    magnifier ? "on" : "off", host.counted, host.counted_calls, per_call,
                    kAllocationsPerReissue);
        CHECK(host.counted <= kAllocationsPerReissue * static_cast<std::size_t>(host.counted_calls));
    }
}
#endif

}  // namespace

int main() {
#ifdef PINEFORGE_L5_HARVEST
    for (const Script script : kScripts) {
        for (const bool pooc : {false, true}) {
            for (const bool magnifier : {false, true}) {
                const Observed got = observe(script, pooc, magnifier);
                std::printf("    {Script::%s, %s, %s, 0x%016llxull, %lluull, %lluull, "
                            "0x%016llxull, %d},\n",
                            name_of(script), pooc ? "true" : "false",
                            magnifier ? "true" : "false",
                            static_cast<unsigned long long>(got.rows_digest),
                            static_cast<unsigned long long>(got.rows),
                            static_cast<unsigned long long>(got.final_hash),
                            static_cast<unsigned long long>(got.trades_digest), got.trades);
            }
        }
    }
    return failures == 0 ? 0 : 1;
#else
    the_scripts_produce_the_values_pinned_before_the_lane();
    a_reissued_exit_allocates_at_most_its_bound();
    if (failures == 0) {
        std::printf("test_adapter_command_allocations: ok (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_adapter_command_allocations: %d of %d checks failed\n",
                 failures, checks);
    return 1;
#endif
}
