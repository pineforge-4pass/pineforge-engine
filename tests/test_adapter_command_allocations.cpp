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

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

namespace {
bool count_allocations = false;
std::size_t allocations = 0;
}  // namespace

void* operator new(std::size_t size) {
    if (count_allocations) ++allocations;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

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
            if (counting) {
                allocations = 0;
                count_allocations = true;
            }
            strategy_exit("guard", "L", bar.close + away, bar.close - away);
            if (counting) {
                count_allocations = false;
                counted += allocations;
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
const Pin kPins[] = {
    {Script::ExitReissue, false, false, 0x45d5a3f28f256742ull, 60ull, 3451175041936365899ull, 0x584fc4b9ebbba609ull, 4},
    {Script::ExitReissue, false, true, 0x80eab7cde808d29dull, 60ull, 3349836046584918293ull, 0x584fc4b9ebbba609ull, 4},
    {Script::ExitReissue, true, false, 0xa407c51973a95e40ull, 60ull, 15593133390039186596ull, 0x41e6c3b1e0481c20ull, 4},
    {Script::ExitReissue, true, true, 0x5dc24488611c94f6ull, 60ull, 13389315872770548534ull, 0x41e6c3b1e0481c20ull, 4},
    {Script::FarReissue, false, false, 0x90c55410be880c61ull, 60ull, 4795263033029944303ull, 0x7aac154ea6a8629aull, 4},
    {Script::FarReissue, false, true, 0x53de86c211400e2bull, 60ull, 7085062931073367593ull, 0x7aac154ea6a8629aull, 4},
    {Script::FarReissue, true, false, 0x41a848f356565f04ull, 60ull, 4162186342665973035ull, 0xfdb435b76f2571b6ull, 4},
    {Script::FarReissue, true, true, 0xd19316cdd18ba053ull, 60ull, 10470574206823754130ull, 0xfdb435b76f2571b6ull, 4},
    {Script::EntryReissue, false, false, 0x753c27da77a1c5c2ull, 60ull, 14321596246514279969ull, 0xfa79a63a9a8de06aull, 29},
    {Script::EntryReissue, false, true, 0x695b8c3c1a8244cdull, 60ull, 10240597316547809238ull, 0xdc986aa5f5cc536aull, 29},
    {Script::EntryReissue, true, false, 0xf584c31ffd881384ull, 60ull, 934878913592489708ull, 0xfa79a63a9a8de06aull, 29},
    {Script::EntryReissue, true, true, 0x3ea867ac99f2c805ull, 60ull, 17516905965457890816ull, 0xdc986aa5f5cc536aull, 29},
    {Script::TrailReissue, false, false, 0xbd823480afcc5235ull, 60ull, 14116140155614560946ull, 0xd64b560956c46b33ull, 3},
    {Script::TrailReissue, false, true, 0xb2c05957b38959a0ull, 60ull, 18289793877175804168ull, 0x5241bc8c55a4a4a3ull, 3},
    {Script::TrailReissue, true, false, 0xf40cdfc297ff459full, 60ull, 9568522095434219343ull, 0xcdcafa54f49f508aull, 3},
    {Script::TrailReissue, true, true, 0xc87c34af2c4b274bull, 60ull, 1972430874561998796ull, 0x45d4c2aceeb466faull, 3},
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
