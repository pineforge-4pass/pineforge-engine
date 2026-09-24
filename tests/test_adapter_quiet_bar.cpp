// R5 lane PERF-L4: the Pine adapter's quiet-bar gates move no value.
//
// Every TradingView policy hook the adapter runs at a bar boundary is now
// first asked whether its body has anything to act on, and is not run when
// it has not (src/source/pine_quiet_bar.hpp). Each precondition is the hook's
// own no-op condition, so a gated run must be the ungated run, value for
// value, at every point either can be observed. This TU is the witness,
// built four ways:
//
//   * test_adapter_quiet_bar, on the shipped library: every run of the
//     battery below reproduces the values pinned from the base tree (the
//     receipt readers' fold into the host view and every gate at once), and
//     a bar with nothing to act on allocates nothing (the cost row; it FAILS
//     on the base tree, whose hooks copy and scan on such bars). With the
//     argument `transcript` it prints the battery bar by bar instead.
//   * test_adapter_quiet_bar_ungated, on the source layer compiled with
//     PINEFORGE_PINE_QUIET_BAR_GATES=0: the same transcript without a gate.
//     The differential row requires the two transcripts to be identical.
//   * test_adapter_quiet_bar_probe, on the source layer compiled with the
//     gate probe: a quiet bar runs no hook body at all, and every gate is
//     taken both ways somewhere in the battery (the witness covers each
//     hook's live and quiet cases).
//   * -DPINEFORGE_QUIET_BAR_HARVEST, against the base library: prints the
//     pinned values below instead of checking them.
//
// The battery: fixture scenarios aimed at each gated hook -- nothing live,
// held positions at full margin and leveraged, resting and re-issued
// brackets, trailing exits, parents pending with their legs staged, flat
// stop pairs, same-bar batches, pyramiding market adds behind a gapped
// bracket, declined reversals, margin calls, calc_on_order_fills,
// process_orders_on_close, an intraday loss rule, account FX, a stream --
// and seeded random strategies over random configurations and tapes. Each
// runs with the magnifier off and on and with broker-state hash recording
// on and off. A run's observations: the source layer's fold
// (hash_host_extension) and the physical book at every source callback, every
// recorded broker-state hash row, the final broker-state hash and every
// closed trade row.
//
// Portability of the pinned values: they fold one fixed execution hash
// instead of the consumer's continuation (which folds tzdata content, as
// test_adapter_recording_hash_witness.cpp notes), every price sits on the
// 0.25 tick, and the random strategies take each draw in a statement of its
// own. The order in which a call's arguments are evaluated is unspecified:
// GCC on x86-64 evaluates them right to left, Clang and GCC on aarch64 left
// to right, so two draws in one argument list would give each compiler a
// different battery. The transcript folds the real continuation: both of its
// runs are on the same machine.
//
// Provenance of the pinned data: this TU, compiled unchanged against the
// f71cd820 library with -DPINEFORGE_QUIET_BAR_HARVEST. Rebuild them the same
// way; never edit one by hand. The harvest is byte-identical with AppleClang
// on arm64 and with GCC 13 on aarch64 and x86-64.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>
#if defined(PINEFORGE_QUIET_BAR_WITNESS_PROBE)
#include "../src/source/pine_quiet_bar.hpp"
#endif
// Counts the quiet bars' heap allocations: every replaceable form, one
// allocator. `allocations` below reads its count; `count_allocations` says
// whether a bar records it.
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <new>
#include <string>
#include <vector>

namespace {
bool count_allocations = false;
std::size_t& allocations = global_allocation::allocations;
}  // namespace

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

constexpr std::uint64_t kProbeExecutionHash = 0x5eed1234abcd0014ull;
constexpr std::int64_t T = 1736121600000LL;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

// ── Tapes ───────────────────────────────────────────────────────────────
Bar mk(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, T + static_cast<std::int64_t>(index) * 60000};
}

// A triangular wave in quarter steps: every price on the 0.25 tick.
std::vector<Bar> wave(int count, double base = 100.0) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 8;
        const int triangle = phase < 4 ? phase : 8 - phase;
        const double p = base + 0.5 * triangle + 0.25 * (i % 3);
        bars.push_back(mk(i, p, p + 0.5, p - 0.5, p + 0.25));
    }
    return bars;
}

// A steady climb, then a fall: the trailing exits' tape.
std::vector<Bar> ramp(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double p = i < count / 2 ? 100.0 + 0.5 * i : 100.0 + 0.5 * (count - i);
        bars.push_back(mk(i, p, p + 0.75, p - 0.25, p + 0.5));
    }
    return bars;
}

// Splice explicit bars into a tape at their indices (timestamps restamped).
std::vector<Bar> with(std::vector<Bar> bars, const std::vector<Bar>& spliced) {
    for (const Bar& bar : spliced) {
        const auto index = static_cast<std::size_t>((bar.timestamp - T) / 60000);
        if (index < bars.size()) bars[index] = bar;
    }
    return bars;
}

class QuietHost;
using Script = std::function<void(QuietHost&, int, const Bar&)>;

struct Setup {
    source::PineStrategyConfig config{};
    double mintick = 0.25;
    double qty_step = 0.0;
    bool margin_calls = true;
    double intraday_loss = 0.0;
    bool fx_series = false;
    int intraday_cap = 0;             // max_intraday_filled_orders
    bool cap_defers_close = false;    // intraday_cap_defer_pooc_close
};

struct Scenario {
    std::string name;
    Setup setup;
    std::vector<Bar> bars;
    Script script;
    int stream_warmup = 0;  // > 0: stream_begin these bars, push the rest
};

source::PineStrategyConfig base_config() {
    source::PineStrategyConfig config;
    config.initial_capital = 10000.0;
    config.default_qty_type = static_cast<int>(QtyType::FIXED);
    config.default_qty_value = 2.0;
    config.pyramiding = 1;
    config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
    config.commission_value = 0.0;
    return config;
}

// A generated-strategy-shaped source host whose body is the scenario's
// script. It observes the source layer at every callback it is given.
class QuietHost final : public source::PineStrategyHost {
public:
    QuietHost(const Scenario& scenario, bool portable, bool recording)
        : script_(scenario.script), portable_(portable) {
        attach_pine_execution_adapter();
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(scenario.setup.mintick);
        if (scenario.setup.qty_step > 0.0)
            set_syminfo_metadata("qty_step", scenario.setup.qty_step);
        configure_pine_strategy(scenario.setup.config);
        set_margin_call_enabled(scenario.setup.margin_calls);
        if (scenario.setup.intraday_loss > 0.0)
            set_pine_risk_max_intraday_loss(scenario.setup.intraday_loss, false);
        if (scenario.setup.intraday_cap > 0)
            set_pine_risk_max_intraday_filled_orders(scenario.setup.intraday_cap);
        if (scenario.setup.cap_defers_close)
            set_syminfo_metadata("intraday_cap_defer_pooc_close", 1.0);
        if (scenario.setup.fx_series) {
            const std::int64_t stamps[] = {T, T + 20 * 60000, T + 40 * 60000};
            const double rates[] = {1.0, 1.25, 0.75};
            set_account_currency_fx_series(stamps, rates, 3);
        }
        set_broker_state_hash_recording(recording);
    }

    std::uint64_t broker_state_hash_projection() const override {
        if (portable_) return broker_state_hash_from_execution_hash(kProbeExecutionHash);
        return source::PineStrategyHost::broker_state_hash_projection();
    }

    void on_source_bar(const Bar& bar) override {
        if (count_allocations) {
            quiet_allocations.push_back(allocations - allocation_mark_);
        }
        boundary.push_back(fold());
        const int index = pine_bar_index();
        repeat_ = index == last_index_ ? repeat_ + 1 : 0;
        last_index_ = index;
        script_(*this, index, bar);
        allocation_mark_ = allocations;
    }

    // How many callbacks of this bar came before this one: fill
    // recalculations re-enter the script on the bar it already evaluated.
    int repeat() const noexcept { return repeat_; }

    // The source layer (host extension, adapter, scheduler) and the book.
    std::uint64_t fold() const {
        BrokerStateHashSink f;
        hash_host_extension(f);
        const auto book = physical_position();
        f.d(book.signed_units);
        f.d(book.average_price);
        f.u(book.lot_count);
        f.i(pending_order_count());
        return f.h;
    }

    std::uint64_t continuation() {
        return execution_consumer().continuation_hash();
    }

    const std::vector<Trade>& rows() const { return trades_; }
    std::vector<std::uint64_t> boundary;
    std::vector<std::size_t> quiet_allocations;

private:
    Script script_;
    bool portable_;
    std::size_t allocation_mark_ = 0;
    int last_index_ = -1;
    int repeat_ = 0;
};

// ── Fixture scenarios, one or more per gated hook ───────────────────────
Scenario scenario(std::string name, Setup setup, std::vector<Bar> bars, Script script) {
    return Scenario{std::move(name), std::move(setup), std::move(bars), std::move(script)};
}

std::vector<Scenario> fixtures() {
    std::vector<Scenario> out;

    // Nothing live at any bar: every gate quiet (the cost row's run).
    out.push_back(scenario("Quiet", Setup{base_config()}, wave(40),
                           [](QuietHost&, int, const Bar&) {}));

    // A long held at full margin with a lot grid: the one-contract money
    // call and its trail twin are consulted every bar the long is held.
    {
        Setup setup{base_config()};
        setup.qty_step = 0.25;
        setup.config.default_qty_value = 90.0;
        out.push_back(scenario("HoldLongFull", setup, wave(40),
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) h.strategy_entry("L", true);
                if (i == 30) h.strategy_close("L");
            }));
    }

    // A leveraged short that a spike margin-calls, its bracket revived by
    // the call; a leveraged long that a slump margin-calls.
    {
        Setup setup{base_config()};
        setup.mintick = 0.01;
        setup.config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        setup.config.default_qty_value = 100.0;
        setup.config.margin_short = 20.0;
        setup.config.margin_long = 25.0;
        auto bars = wave(36, 90.0);
        bars = with(bars, {mk(0, 100, 100, 100, 100), mk(1, 100, 101, 90, 90),
                           mk(2, 91, 91, 91, 91), mk(3, 165, 170, 160, 168),
                           mk(4, 175, 185, 170, 180), mk(5, 170, 171, 169, 170),
                           mk(20, 100, 100, 99, 100), mk(21, 100, 100, 99, 99),
                           mk(22, 80, 81, 60, 62), mk(23, 62, 63, 61, 62)});
        out.push_back(scenario("MarginCalls", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 0) {
                    h.strategy_entry("S", false);
                    h.strategy_exit("XS", "S", kNa, 180.0, kNa, kNa, kNa, 100.0, "");
                }
                if (i == 10) h.strategy_close("S");
                if (i == 18) h.strategy_entry("L", true);
                if (i == 30) h.strategy_close_all();
            }));
    }

    // The corpus has no margin-call regime, so this is the witness's own:
    // over-sized fixed-quantity positions at 25 % (long) and 20 % (short)
    // margin, three cycles each, every one margin-called by a slump or a
    // spike -- one short under a far bracket that the call revives -- while
    // the calls' checkpoints consult the book bar after bar.
    {
        Setup setup{base_config()};
        setup.config.default_qty_value = 300.0;
        setup.config.pyramiding = 1;
        setup.config.margin_long = 25.0;
        setup.config.margin_short = 20.0;
        std::vector<Bar> shocks;
        for (int cycle = 0; cycle < 3; ++cycle) {
            const int at = 20 * cycle;
            shocks.push_back(mk(at + 5, 100.5, 118, 100, 116));
            shocks.push_back(mk(at + 6, 116, 117, 108, 110));
            shocks.push_back(mk(at + 15, 99.5, 100, 78, 80));
            shocks.push_back(mk(at + 16, 80, 90, 79, 88));
        }
        out.push_back(scenario("MarginCallCycles", setup, with(wave(64), shocks),
            [](QuietHost& h, int i, const Bar& bar) {
                const int phase = i % 20;
                if (phase == 2) {
                    h.strategy_entry("S", false);
                    if (i == 22) h.strategy_exit("sx", "S", kNa, bar.close + 30.0);
                }
                if (phase == 9 || phase == 19) h.strategy_close_all();
                if (phase == 12) h.strategy_entry("L", true);
            }));
    }

    // A bracket resting under a held long for many bars, re-issued every
    // bar, until a leg fills; then a second one that rests to the end.
    out.push_back(scenario("Bracket", Setup{base_config()}, wave(48),
        [](QuietHost& h, int i, const Bar& bar) {
            if (i == 1 || i == 30) h.strategy_entry("B", true);
            if (i >= 1 && i < 20)
                h.strategy_exit("tp", "B", bar.close + 6.0, bar.close - 6.0);
            if (i == 20) h.strategy_exit("tp", "B", bar.close + 0.5, bar.close - 0.5);
            if (i == 30) h.strategy_exit("tp2", "B", bar.close + 9.0, bar.close - 9.0);
        }));

    // A trailing exit that arms on the climb and fills on the fall.
    out.push_back(scenario("Trail", Setup{base_config()}, ramp(48),
        [](QuietHost& h, int i, const Bar&) {
            if (i == 1) {
                h.strategy_entry("T", true);
                h.strategy_exit("tr", "T", kNa, kNa, 8.0, 4.0);
            }
        }));

    // Flat opposite stop entries resting for many bars; a gap then makes
    // both marketable at the open (the deferred sell and its re-arm).
    {
        Setup setup{base_config()};
        setup.config.pyramiding = 2;
        auto bars = with(wave(40), {mk(24, 110, 111, 89, 100)});
        out.push_back(scenario("DualStops", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) {
                    h.strategy_entry("up", true, kNa, 105.0);
                    h.strategy_entry("dn", false, kNa, 95.0);
                }
                if (i == 34) h.strategy_close_all();
            }));
    }

    // Flat stop entries placed on the wrong side of the price -- a sell stop
    // above, a buy stop below, the sell first -- so both are marketable at
    // the next open: the sell is deferred behind the buy and admitted after
    // the bar's path.
    {
        Setup setup{base_config()};
        setup.config.pyramiding = 2;
        out.push_back(scenario("InvertedStops", setup, wave(40),
            [](QuietHost& h, int i, const Bar& bar) {
                const int phase = i % 10;
                if (phase == 2) {
                    h.strategy_entry("dn", false, kNa, bar.close + 1.0);
                    h.strategy_entry("up", true, kNa, bar.close - 1.0);
                }
                if (phase == 7) {
                    h.strategy_cancel_all();
                    h.strategy_close_all();
                }
            }));
    }

    // A parent limit pending below the price with its bracket staged
    // behind it, filled many bars later.
    out.push_back(scenario("PendingParent", Setup{base_config()},
        with(wave(40), {mk(20, 100, 100.5, 97, 99)}),
        [](QuietHost& h, int i, const Bar&) {
            if (i == 2) {
                h.strategy_entry("P", true, 97.5);
                h.strategy_exit("px", "P", 103.0, 94.0);
            }
        }));

    // Pyramided market adds behind a from_entry stop the next open gaps
    // through (the gapped bracket re-accepted behind the add).
    {
        Setup setup{base_config()};
        setup.config.pyramiding = 3;
        auto bars = with(wave(40), {mk(13, 97, 98, 96, 97)});
        out.push_back(scenario("GappedAdd", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) {
                    h.strategy_entry("L", true);
                    h.strategy_exit("X", "L", kNa, 98.5);
                }
                if (i == 12) h.strategy_entry("L", true);
                if (i == 30) h.strategy_close_all();
            }));
    }

    // A reversal that cannot be afforded at the next open while its
    // position's bracket is touched on the same bar; re-issued later.
    {
        Setup setup{base_config()};
        setup.mintick = 0.01;
        setup.config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        setup.config.default_qty_value = 100.0;
        setup.margin_calls = false;
        auto bars = with(wave(24, 95.0), {
            mk(0, 100, 100, 100, 100), mk(1, 100, 112, 99, 110),
            mk(2, 111, 112, 89, 111), mk(3, 111, 112, 88, 111),
            mk(4, 110, 110, 110, 110), mk(5, 110, 110, 110, 110),
            mk(6, 96, 97, 89, 95), mk(7, 95, 95, 95, 95)});
        out.push_back(scenario("DeclinedReversal", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 0) h.strategy_entry("L", true);
                if (i == 1) {
                    h.strategy_exit("X", "L", kNa, 90.0, kNa, kNa, kNa, 100.0, "");
                    h.strategy_entry("S", false);
                }
                if (i == 5) h.strategy_exit("X", "L", kNa, 95.0, kNa, kNa, kNa, 100.0, "");
            }));
    }

    // calc_on_order_fills: fills recalculate the script, which re-prices
    // exits and stages requests for the next open.
    {
        Setup setup{base_config()};
        setup.config.calc_on_order_fills = true;
        setup.config.pyramiding = 2;
        out.push_back(scenario("CalcOnFills", setup, wave(40),
            [](QuietHost& h, int i, const Bar& bar) {
                const int phase = i % 10;
                if (phase == 1) h.strategy_entry("C", true);
                if (phase >= 1 && phase < 8)
                    h.strategy_exit("cx", "C", bar.close + 1.0, bar.close - 1.25);
                if (phase == 4) h.strategy_entry("D", true, kNa, bar.close + 0.25);
                if (phase == 8) h.strategy_close_all();
            }));
    }

    // process_orders_on_close with explicit opposite entries at pyramiding
    // 0 (the terminal gross pair), limit entries marketable at the close,
    // same-bar exits and a carried short.
    {
        Setup setup{base_config()};
        setup.config.process_orders_on_close = true;
        setup.config.pyramiding = 0;
        setup.qty_step = 0.25;
        out.push_back(scenario("CloseOrders", setup, wave(48),
            [](QuietHost& h, int i, const Bar& bar) {
                const int phase = i % 12;
                if (phase == 1) {
                    h.strategy_entry("A", true, kNa, kNa, 2.0);
                    h.strategy_entry("B", false, kNa, kNa, 3.0);
                }
                if (phase == 3) h.strategy_entry("A", true, bar.close + 0.25, kNa, 2.0);
                if (phase >= 3 && phase < 7)
                    h.strategy_exit("ax", "A", bar.close + 0.75, bar.close - 0.25);
                if (phase == 8) h.strategy_entry("S", false, kNa, kNa, 2.0);
                if (phase == 11) h.strategy_close_all();
            }));
    }

    // process_orders_on_close with slippage and a lot grid: the slipped
    // opening money scope and the rounded short checkpoint consult their
    // books.
    {
        Setup setup{base_config()};
        setup.config.process_orders_on_close = true;
        setup.config.slippage = 1;
        setup.qty_step = 0.25;
        out.push_back(scenario("CloseOrdersSlipped", setup, wave(36),
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) h.strategy_entry("L", true, kNa, kNa, 3.0);
                if (i == 14) h.strategy_close("L");
                if (i == 18) h.strategy_entry("S", false, kNa, kNa, 3.0);
                if (i == 30) h.strategy_close("S");
            }));
    }

    // An intraday loss rule under a held long.
    {
        Setup setup{base_config()};
        setup.intraday_loss = 3.0;
        auto bars = with(wave(40), {mk(15, 100, 100, 95, 96)});
        out.push_back(scenario("IntradayLoss", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2 || i == 20) h.strategy_entry("L", true);
            }));
    }

    // A filled-orders cap reached by a close-order market entry: the cap's
    // close is deferred to the next opening, which executes it.
    {
        Setup setup{base_config()};
        setup.config.process_orders_on_close = true;
        setup.intraday_cap = 1;
        setup.cap_defers_close = true;
        out.push_back(scenario("IntradayCapDeferred", setup, wave(24),
            [](QuietHost& h, int i, const Bar&) {
                if (i == 3) h.strategy_entry("L", true);
            }));
    }

    // Default-percent stop entries under leverage: the pre-open slice of a
    // stop that is marketable at the open.
    {
        Setup setup{base_config()};
        setup.mintick = 0.01;
        setup.config.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        setup.config.default_qty_value = 100.0;
        setup.config.margin_long = 50.0;
        setup.config.margin_short = 50.0;
        auto bars = with(wave(36), {mk(12, 104, 104.5, 96, 97)});
        out.push_back(scenario("PreopenSlice", setup, bars,
            [](QuietHost& h, int i, const Bar&) {
                if (i == 4) h.strategy_entry("E", true, kNa, 103.5);
                if (i == 24) h.strategy_close_all();
            }));
    }

    // Same-bar market batches: two entries and a close issued together.
    {
        Setup setup{base_config()};
        setup.config.pyramiding = 2;
        out.push_back(scenario("SameBarBatch", setup, wave(36),
            [](QuietHost& h, int i, const Bar&) {
                const int phase = i % 9;
                if (phase == 1) {
                    h.strategy_entry("A", true);
                    h.strategy_entry("B", true);
                }
                if (phase == 4) {
                    h.strategy_close("A");
                    h.strategy_entry("S", false);
                }
                if (phase == 7) h.strategy_close_all();
            }));
    }

    // A commissioned full-margin short held past its entry bar (the
    // non-close-order script-close checkpoint).
    {
        Setup setup{base_config()};
        setup.config.commission_type = static_cast<int>(CommissionType::PERCENT);
        setup.config.commission_value = 0.1;
        setup.config.default_qty_value = 5.0;
        out.push_back(scenario("CommissionedShort", setup, wave(32),
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) h.strategy_entry("S", false);
                if (i == 24) h.strategy_close("S");
            }));
    }

    // Timestamped account FX under a held long.
    {
        Setup setup{base_config()};
        setup.fx_series = true;
        out.push_back(scenario("AccountFx", setup, wave(48),
            [](QuietHost& h, int i, const Bar&) {
                if (i == 2) h.strategy_entry("L", true);
                if (i == 42) h.strategy_close("L");
            }));
    }

    // A stream: a bracket strategy over the warmup, then pushed bars.
    {
        Scenario stream = scenario("Stream", Setup{base_config()}, wave(40),
            [](QuietHost& h, int i, const Bar& bar) {
                const int phase = i % 8;
                if (phase == 1) {
                    h.strategy_entry("W", true);
                    h.strategy_exit("wx", "W", bar.close + 2.0, bar.close - 2.0);
                }
                if (phase == 6) h.strategy_close_all();
            });
        stream.stream_warmup = 16;
        out.push_back(std::move(stream));
    }
    return out;
}

// ── Seeded random strategies ────────────────────────────────────────────
struct Random {
    std::uint64_t state;
    explicit Random(std::uint64_t seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
    std::uint64_t next() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool chance(int percent) { return below(100) < percent; }
};

Scenario random_scenario(std::uint64_t seed) {
    Random r(seed);
    Setup setup{base_config()};
    auto& c = setup.config;
    c.process_orders_on_close = r.chance(30);
    c.calc_on_order_fills = r.chance(20);
    c.pyramiding = r.below(4);
    const int qty_type = r.below(3);
    if (qty_type == 1) {
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = r.chance(50) ? 100.0 : 25.0;
    } else if (qty_type == 2) {
        c.default_qty_type = static_cast<int>(QtyType::CASH);
        c.default_qty_value = 400.0;
    }
    const double margins[] = {100.0, 100.0, 50.0, 25.0};
    c.margin_long = margins[r.below(4)];
    c.margin_short = margins[r.below(4)];
    c.slippage = r.chance(25) ? 1 : 0;
    const int commission = r.below(3);
    if (commission == 1) {
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.commission_value = 0.1;
    } else if (commission == 2) {
        c.commission_value = 1.0;
    }
    c.close_entries_rule_any = r.chance(20);
    setup.qty_step = r.chance(40) ? 0.25 : 0.0;
    setup.margin_calls = r.chance(80);
    if (r.chance(15)) setup.intraday_loss = 4.0;

    // A random walk on the tick with occasional gaps and wide bars.
    std::vector<Bar> bars;
    double price = 100.0;
    const int count = 40 + r.below(24);
    for (int i = 0; i < count; ++i) {
        double open = price + 0.25 * (r.below(5) - 2);
        if (r.chance(8)) open += 0.25 * (r.below(33) - 16);
        if (open < 20.0) open = 20.0;
        const double wide = r.chance(10) ? 4.0 : 1.0;
        const double high = open + 0.25 * r.below(static_cast<int>(8 * wide) + 1);
        const double low = open - 0.25 * r.below(static_cast<int>(8 * wide) + 1);
        const double close = low + 0.25 * r.below(static_cast<int>((high - low) / 0.25) + 1);
        bars.push_back(mk(i, open, high, low, close));
        price = close;
    }

    const std::uint64_t script_seed = r.next();
    Script script = [script_seed](QuietHost& h, int i, const Bar& bar) {
        // A fill recalculation re-enters the script on its bar: it commands
        // again once at most, so a cascade stays short of Pine's loop guard.
        if (h.repeat() > 1) return;
        Random s(script_seed ^ (static_cast<std::uint64_t>(i) * 0xD1B54A32D192ED03ull)
                 ^ static_cast<std::uint64_t>(h.repeat()));
        if (h.repeat() == 1 && !s.chance(20)) return;
        static const char* const kIds[] = {"L", "S", "A", "B"};
        static const char* const kExits[] = {"X", "Y"};
        const int commands = s.chance(55) ? 1 + s.below(3) : 0;
        // One draw per statement, never two in one call's arguments (see the
        // portability note at the top of this file).
        for (int k = 0; k < commands; ++k) {
            const char* id = kIds[s.below(4)];
            const bool is_long = s.chance(55);
            const double off = 0.25 * (1 + s.below(12));
            switch (s.below(12)) {
            case 0: case 1:
                h.strategy_entry(id, is_long);
                break;
            case 2:
                h.strategy_entry(id, is_long, is_long ? bar.close - off : bar.close + off);
                break;
            case 3:
                h.strategy_entry(id, is_long, kNa, is_long ? bar.close + off : bar.close - off);
                break;
            case 4: {
                const double qty = 1.0 + s.below(3);
                const char* const oca = s.chance(30) ? "g" : "";
                const int oca_type = s.chance(30) ? 1 + s.below(2) : 0;
                h.strategy_entry(id, is_long, kNa, kNa, qty, "", oca, oca_type);
                break;
            }
            case 5: case 6: {
                const double limit = s.chance(70) ? bar.close + off : kNa;
                const double stop = s.chance(70) ? bar.close - off : kNa;
                h.strategy_exit(kExits[s.below(2)], id, limit, stop);
                break;
            }
            case 7: {
                const char* const exit_id = kExits[s.below(2)];
                const double trail_points = 4.0 + s.below(8);
                const double trail_offset = 1.0 + s.below(4);
                h.strategy_exit(exit_id, id, kNa, kNa, trail_points, trail_offset);
                break;
            }
            case 8:
                if (s.chance(50)) h.strategy_close(id);
                else h.strategy_close(id, "", kNa, 50.0);
                break;
            case 9:
                if (s.chance(50)) h.strategy_close_all();
                else h.strategy_cancel(id);
                break;
            case 10:
                if (s.chance(50)) h.strategy_cancel_all();
                else h.strategy_exit(kExits[s.below(2)], "", bar.close + off, bar.close - off);
                break;
            default: {
                const double qty = 1.0 + s.below(2);
                const double limit =
                    s.chance(50) ? (is_long ? bar.close - off : bar.close + off) : kNa;
                h.strategy_order(id, is_long, qty, limit);
                break;
            }
            }
        }
    };
    char name[32];
    std::snprintf(name, sizeof name, "Random%02" PRIu64, seed);
    return Scenario{name, setup, std::move(bars), std::move(script)};
}

constexpr int kRandomScenarios = 48;

std::vector<Scenario> battery() {
    auto out = fixtures();
    for (int seed = 1; seed <= kRandomScenarios; ++seed)
        out.push_back(random_scenario(static_cast<std::uint64_t>(seed)));
    return out;
}

// ── One run and its observations ────────────────────────────────────────
struct Observed {
    std::vector<std::uint64_t> boundary;
    std::vector<std::uint64_t> rows;
    std::uint64_t final_hash = 0;
    std::uint64_t continuation = 0;
    std::vector<Trade> trades;
    std::string error;
};

bool runnable(const Scenario& s, bool magnifier) {
    // A stream has no magnifier; the FX curve refuses one.
    return !magnifier || (s.stream_warmup == 0 && !s.setup.fx_series);
}

Observed observe(const Scenario& s, bool magnifier, bool recording, bool portable) {
    QuietHost host(s, portable, recording);
    const int n = static_cast<int>(s.bars.size());
    if (s.stream_warmup > 0) {
        CHECK(host.stream_begin(s.bars.data(), s.stream_warmup, "1"));
        for (int i = s.stream_warmup; i < n; ++i) CHECK(host.stream_push_bar(s.bars[i]));
        CHECK(host.stream_end());
    } else if (magnifier) {
        host.run(s.bars.data(), n, "", "", true, 4, MagnifierDistribution::ENDPOINTS);
    } else {
        host.run(s.bars.data(), n);
    }
    Observed out;
    out.error = host.last_error();
    out.boundary = host.boundary;
    ReportC report{};
    host.fill_report(&report);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i)
        out.rows.push_back(report.broker_state_hash[i]);
    BacktestEngine::free_report(&report);
    out.final_hash = host.broker_state_hash();
    if (!portable) out.continuation = host.continuation();
    out.trades = host.rows();
    return out;
}

struct Fnv {
    std::uint64_t h = 1469598103934665603ull;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ull; }
    }
    template <class T> void v(T x) { bytes(&x, sizeof x); }
    void s(const std::string& x) { v(x.size()); bytes(x.data(), x.size()); }
};

void fold_trade(Fnv& f, const Trade& t) {
    f.v(t.entry_time); f.v(t.exit_time); f.v(t.entry_price); f.v(t.exit_price);
    f.v(t.qty); f.v(t.pnl); f.v(t.pnl_pct); f.v(t.is_long); f.v(t.entry_bar_index);
    f.v(t.exit_bar_index); f.s(t.entry_id); f.s(t.entry_comment); f.s(t.exit_comment);
    f.s(t.exit_id); f.v(t.exit_from_bracket); f.v(t.max_runup); f.v(t.max_drawdown);
    f.v(t.commission); f.v(t.entry_incarnation); f.v(t.open_at_end);
    f.v(static_cast<int>(t.close_cause));
}

// One run's pinned digest: every observation, in order.
struct Digest {
    std::uint64_t boundary = 0;
    std::uint64_t rows = 0;
    std::uint64_t trades = 0;
    std::uint64_t final_hash = 0;
    std::size_t boundary_count = 0;
    std::size_t row_count = 0;
    std::size_t trade_count = 0;
};

Digest digest(const Observed& o) {
    Digest d;
    Fnv b, r, t;
    for (const auto value : o.boundary) b.v(value);
    for (const auto value : o.rows) r.v(value);
    for (const auto& trade : o.trades) fold_trade(t, trade);
    t.s(o.error);
    d.boundary = b.h; d.rows = r.h; d.trades = t.h; d.final_hash = o.final_hash;
    d.boundary_count = o.boundary.size(); d.row_count = o.rows.size();
    d.trade_count = o.trades.size();
    return d;
}

struct RunKey {
    const Scenario* scenario;
    bool magnifier;
    bool recording;
};

std::vector<RunKey> runs(const std::vector<Scenario>& all) {
    std::vector<RunKey> out;
    for (const auto& s : all) {
        for (const bool magnifier : {false, true}) {
            if (!runnable(s, magnifier)) continue;
            for (const bool recording : {true, false}) out.push_back({&s, magnifier, recording});
        }
    }
    return out;
}

// ── The transcript the differential row compares ────────────────────────
void print_transcript() {
    const auto all = battery();
    for (const auto& key : runs(all)) {
        const Observed o = observe(*key.scenario, key.magnifier, key.recording, false);
        std::printf("run %s mag=%d rec=%d error=[%s]\n", key.scenario->name.c_str(),
                    key.magnifier ? 1 : 0, key.recording ? 1 : 0, o.error.c_str());
        for (std::size_t i = 0; i < o.boundary.size(); ++i)
            std::printf("  source %zu %016" PRIx64 "\n", i, o.boundary[i]);
        for (std::size_t i = 0; i < o.rows.size(); ++i)
            std::printf("  row %zu %016" PRIx64 "\n", i, o.rows[i]);
        std::printf("  final %016" PRIx64 " continuation %016" PRIx64 "\n",
                    o.final_hash, o.continuation);
        for (const auto& t : o.trades) {
            Fnv f;
            fold_trade(f, t);
            std::printf("  trade %s->%s %" PRId64 " %" PRId64 " %.17g %.17g %.17g %.17g %016" PRIx64 "\n",
                        t.entry_id.c_str(), t.exit_id.c_str(), t.entry_time, t.exit_time,
                        t.entry_price, t.exit_price, t.qty, t.pnl, f.h);
        }
    }
}

// ── The cost row: what a bar with nothing to act on allocates ───────────
// Four books that rest untouched through a stretch of bars: nothing live; a
// held long with no order; a held long under a bracket that rests unmet; a
// flat pair of stop entries that rests unmet. Every stretch bar is counted
// from the end of one script evaluation to the start of the next -- the
// publication's drains, the close hooks, the next bar's input, open hooks,
// path matching and receipt reads. On such a bar the kernel does the same
// work whatever the book (no event is recorded; its logs are sized for the
// run at begin), so the three live books must allocate exactly what the
// book with nothing live allocates: its own amortized report growth and,
// under the magnifier, its samples. Before the gates the adapter's hooks
// copied the live roster, collected candidates and built the book's key
// set on every one of those bars.
struct CostBook {
    const char* name;
    Script script;
};

std::vector<CostBook> cost_books() {
    return {
        {"nothing live", [](QuietHost&, int, const Bar&) {}},
        {"held long", [](QuietHost& h, int i, const Bar&) {
             if (i == 1) h.strategy_entry("H", true);
         }},
        {"held long under a bracket", [](QuietHost& h, int i, const Bar& bar) {
             if (i == 1) {
                 h.strategy_entry("H", true);
                 h.strategy_exit("hx", "H", bar.close + 20.0, bar.close - 20.0);
             }
         }},
        {"flat stop pair", [](QuietHost& h, int i, const Bar& bar) {
             if (i == 1) {
                 h.strategy_entry("up", true, kNa, bar.close + 20.0);
                 h.strategy_entry("dn", false, kNa, bar.close - 20.0);
             }
         }},
    };
}

void quiet_bars_allocate_nothing() {
    constexpr std::size_t kFirstQuietBar = 4;
    for (const bool magnifier : {false, true}) {
        std::size_t nothing_live = 0;
        for (const auto& book : cost_books()) {
            const Scenario s = scenario(book.name, Setup{base_config()}, wave(48), book.script);
            QuietHost host(s, true, false);
            allocations = 0;
            count_allocations = true;
            if (magnifier) {
                host.run(s.bars.data(), static_cast<int>(s.bars.size()), "", "", true, 4,
                         MagnifierDistribution::ENDPOINTS);
            } else {
                host.run(s.bars.data(), static_cast<int>(s.bars.size()));
            }
            count_allocations = false;
            CHECK(host.last_error().empty());
            CHECK(host.quiet_allocations.size() == s.bars.size());
            std::size_t stretch = 0;
            for (std::size_t k = kFirstQuietBar; k < host.quiet_allocations.size(); ++k)
                stretch += host.quiet_allocations[k];
            if (std::string(book.name) == "nothing live") nothing_live = stretch;
            std::printf("  cost %-26s magnifier %-3s: %zu quiet bars allocate %zu "
                        "(nothing live: %zu)\n", book.name, magnifier ? "on" : "off",
                        host.quiet_allocations.size() - kFirstQuietBar, stretch, nothing_live);
            CHECK(stretch == nothing_live);
        }
    }
}

struct Pinned {
    const char* name;
    int magnifier;
    int recording;
    std::uint64_t boundary;
    std::uint64_t rows;
    std::uint64_t trades;
    std::uint64_t final_hash;
    std::size_t boundary_count;
    std::size_t row_count;
    std::size_t trade_count;
};

#if !defined(PINEFORGE_QUIET_BAR_HARVEST)
// ── Pinned data (see the provenance note at the top of this file) ───────
#include "test_adapter_quiet_bar_pinned.inc"

void every_run_matches_the_base_tree() {
    const auto all = battery();
    const auto keys = runs(all);
    constexpr std::size_t pinned = sizeof kPinned / sizeof kPinned[0];
    CHECK(keys.size() == pinned);
    std::size_t matched = 0;
    std::size_t margin_calls = 0;
    for (std::size_t i = 0; i < keys.size() && i < pinned; ++i) {
        const auto& key = keys[i];
        const Pinned& want = kPinned[i];
        const Observed observed = observe(*key.scenario, key.magnifier, key.recording, true);
        for (const auto& trade : observed.trades)
            if (trade.exit_id == source::kMarginCallLabel) ++margin_calls;
        const Digest got = digest(observed);
        const bool same = key.scenario->name == want.name
            && (key.magnifier ? 1 : 0) == want.magnifier
            && (key.recording ? 1 : 0) == want.recording
            && got.boundary == want.boundary && got.rows == want.rows
            && got.trades == want.trades && got.final_hash == want.final_hash
            && got.boundary_count == want.boundary_count && got.row_count == want.row_count
            && got.trade_count == want.trade_count;
        CHECK(same);
        if (same) {
            ++matched;
        } else {
            std::fprintf(stderr, "  run %s magnifier %d recording %d differs from the base tree\n",
                         key.scenario->name.c_str(), key.magnifier ? 1 : 0, key.recording ? 1 : 0);
        }
    }
    std::printf("  pinned: %zu of %zu runs match the base tree; the battery books %zu "
                "margin-call rows\n", matched, pinned, margin_calls);
    // The witness's own margin-call regime (the corpus has none).
    CHECK(margin_calls >= 24);
}
#endif

#if defined(PINEFORGE_QUIET_BAR_WITNESS_PROBE)
// ── The probe rows (built on the probed source layer) ──────────────────
namespace quiet = pineforge::source::detail;

const char* const kHookNames[] = {
    "ReceiptRead", "DelayedRelease", "L4cPriority", "OpenMarketAdmission",
    "OpenMarketableSells", "GappedBracketReaccept", "DualEntryPath", "CoofTail",
    "CoofDeclinedReversal", "DueCapClose", "SlippedPoocMoney", "TvMoneyLongMargin",
    "TvMoneyTrailMargin", "OpenMarginCheckpoints", "DeclinedReversalAtOpen",
    "RoundedPoocShortMargin", "IntradayLossClose", "IntradayLossPath", "PreopenMarginSlice",
    "BracketLegs", "PendingCloses", "PendingEntries", "RelativeExits", "PoocLimitEntryFills",
    "PoocExitFills", "DeferredSellAdmission", "ThrottledReopens", "TerminalExplicitMarket",
    "CloseMarginCheckpoints",
};
static_assert(sizeof kHookNames / sizeof kHookNames[0] == quiet::kQuietHookCount,
              "one name per gated hook");

// A bar with nothing to act on runs no hook body: every gate its hooks
// reach answers quiet, on a run of nothing but such bars.
void a_quiet_bar_runs_no_hook_body() {
    for (const bool magnifier : {false, true}) {
        const auto all = fixtures();
        const Scenario& quiet_run = all.front();
        CHECK(quiet_run.name == "Quiet");
        quiet::quiet_bar_counts() = {};
        const Observed o = observe(quiet_run, magnifier, false, true);
        CHECK(o.error.empty());
        const auto& counts = quiet::quiet_bar_counts();
        std::uint64_t asked = 0;
        std::uint64_t ran = 0;
        for (std::size_t hook = 0; hook < quiet::kQuietHookCount; ++hook) {
            asked += counts.asked[hook];
            ran += counts.asked[hook] - counts.quiet[hook];
            if (counts.asked[hook] != counts.quiet[hook]) {
                std::fprintf(stderr, "  %s ran its body on a quiet bar\n", kHookNames[hook]);
            }
            CHECK(counts.asked[hook] == counts.quiet[hook]);
        }
        const double bars = static_cast<double>(o.boundary.size());
        std::printf("  quiet bars, magnifier %-3s: %.2f gates asked per bar, %.2f hook bodies "
                    "run per bar (the ungated build runs every one asked)\n",
                    magnifier ? "on" : "off", static_cast<double>(asked) / bars,
                    static_cast<double>(ran) / bars);
        CHECK(o.boundary.size() == quiet_run.bars.size());
        CHECK(asked >= 20 * o.boundary.size());
        CHECK(ran == 0);
    }
}

// Every gate is taken both ways somewhere in the battery: each hook's quiet
// case and its live case are exercised by the differential transcript.
void every_gate_is_taken_both_ways() {
    quiet::quiet_bar_counts() = {};
    const auto all = battery();
    for (const auto& key : runs(all)) (void)observe(*key.scenario, key.magnifier, key.recording, true);
    const auto& counts = quiet::quiet_bar_counts();
    for (std::size_t hook = 0; hook < quiet::kQuietHookCount; ++hook) {
        const std::uint64_t live = counts.asked[hook] - counts.quiet[hook];
        std::printf("  gate %-24s asked %8" PRIu64 "  quiet %8" PRIu64 "  live %7" PRIu64 "\n",
                    kHookNames[hook], counts.asked[hook], counts.quiet[hook], live);
        CHECK(counts.quiet[hook] > 0);
        CHECK(live > 0);
    }
}
#endif

#if defined(PINEFORGE_QUIET_BAR_HARVEST)
void harvest() {
    const auto all = battery();
    std::printf("// R5 lane PERF-L4: the values tests/test_adapter_quiet_bar.cpp pins, one\n"
                "// row per run of its battery (scenario, magnifier, recording; the digests\n"
                "// of the source folds, the recorded rows and the trades; the final\n"
                "// broker-state hash; the three counts). Harvested on the base tree: see\n"
                "// the provenance note in the test. Generated -- never edit a row by hand.\n");
    std::printf("constexpr Pinned kPinned[] = {\n");
    for (const auto& key : runs(all)) {
        const Digest d = digest(observe(*key.scenario, key.magnifier, key.recording, true));
        std::printf("    {\"%s\", %d, %d, 0x%016" PRIx64 "ull, 0x%016" PRIx64 "ull, 0x%016" PRIx64
                    "ull, 0x%016" PRIx64 "ull, %zu, %zu, %zu},\n",
                    key.scenario->name.c_str(), key.magnifier ? 1 : 0, key.recording ? 1 : 0,
                    d.boundary, d.rows, d.trades, d.final_hash, d.boundary_count,
                    d.row_count, d.trade_count);
    }
    std::printf("};\n");
}
#endif

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    if (mode == "transcript") {
        print_transcript();
        return failures == 0 ? 0 : 1;
    }
    if (mode == "cost") {
        quiet_bars_allocate_nothing();
    } else {
#if defined(PINEFORGE_QUIET_BAR_HARVEST)
        harvest();
        return 0;
#elif defined(PINEFORGE_QUIET_BAR_WITNESS_PROBE)
        a_quiet_bar_runs_no_hook_body();
        every_gate_is_taken_both_ways();
#else
        every_run_matches_the_base_tree();
        quiet_bars_allocate_nothing();
#endif
    }
    if (failures == 0) {
        std::printf("test_adapter_quiet_bar: ok (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_adapter_quiet_bar: %d of %d checks failed\n", failures, checks);
    return 1;
}
