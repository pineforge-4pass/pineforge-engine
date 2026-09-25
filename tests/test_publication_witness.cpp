// R5 lane D2-D: the host's per-bar source publication moves no value.
//
// PineStrategyHost::scheduler_publish_source_bar now asks two questions
// before two calls it made on every bar (src/source/pine_strategy_host.cpp):
//   * flush_pending_entries' own quiet gate -- nothing delayed, batched or
//     held -- in front of the call, doing what the flush does then (the
//     batch's close total reset); and
//   * sort_same_bar_exit_trades' first test -- the last two closed rows are
//     bracket exits of one entry at one exit time -- in front of that call,
//     which otherwise sorts a one-row group and answers nothing.
// The adapter's configuration stays published after every source evaluation,
// as it was: an exact comparison of what set_configuration copies costs more
// than the copy (measured), so that call is not gated.
//
// This TU is the witness. Every run below reproduces the values pinned from
// the base tree (6c081f5d): at every source callback the source layer's fold
// (hash_host_extension: the host's configuration and the adapter's copy of
// it, the pending entry, batch and delayed queues, the batch's close total,
// the day ledger, every adapter row) and the physical book, then every closed
// trade row in report order, every broker-state hash row the kernel records
// at a script bar's end -- its own reads between two publications, which see
// what a publication leaves behind -- and the final broker-state hash. The
// battery:
//   * configuration mutation: generated-strategy-shaped scripts that write
//     every field of the live configuration (all thirteen) at seeded bars,
//     some twice in one evaluation and some back to their old value, while
//     orders that each field sizes, prices, admits or margins are working;
//     and hosts reconfigured between runs by configure_pine_strategy and by
//     strategy overrides;
//   * pending entries: entries queued behind strategy.close_all, reversals,
//     same-bar market batches, partial closes of a same-bar batch, delayed
//     stops under calc_on_order_fills, market adds under
//     process_orders_on_close, and seeded command storms over random
//     configurations, each with the magnifier off and on;
//   * same-bar exits: entries whose bracket legs fill together on one bar in
//     an order other than their command order, and the near misses -- two
//     entries, pyramided lots of one id, a leg beside a strategy.close.
//
// Portability of the pinned values (as test_adapter_quiet_bar.cpp): the
// broker-state hash folds a fixed execution hash instead of the consumer's
// continuation, every price sits on the 0.25 tick, and every random draw is a
// statement of its own. Provenance: this TU compiled unchanged against the
// 6c081f5d library with -DPINEFORGE_D2D_PUBLICATION_HARVEST; the harvest is
// byte-identical with AppleClang on arm64 and GCC 13 on aarch64. Rebuild the
// table that way; never edit a row by hand. Re-harvested once on the INT23
// tree, which carries lane V19-D: its steps 6 and 7 change what the source
// layer's fold (pineforge-source-adapter/v4) folds -- a bracket family's
// erased members park behind a retained one, and K1 releases a leg the
// revival's superseded test answers for -- so 31 of the 156 runs pin a new
// digest. The same TU harvests the same table against V19-D's own library,
// and its digest without the three hash inputs (that fold, the recorded
// broker-state hash rows, the final hash) is the 6c081f5d harvest's in all
// 156 runs, as are every count. Re-harvested once more for R5 lane K-ULP3:
// three runs (ConfigFlags103, Storm18, Storm18M) close a pyramided book by
// exactly its own sum, whose last lot the kernel now closes whole, so their
// closing rows book one ulp more -- the lot's full size instead of the rest
// fl(U - C), whose dust lot the host swept unbooked -- and those rows' P&L,
// the adapter's day ledger that sums it, the recorded broker-state hash rows
// and the final hash move with them. Every count and the other 153 runs keep
// their digest, and the rule fires in exactly those three runs (its census).
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <string>
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

constexpr std::uint64_t kExecutionHash = 0x5eed1234abcd0d2dull;
constexpr std::int64_t T = 1736121600000LL;  // 2025-01-06 00:00 UTC
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();

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

// ── Tapes ───────────────────────────────────────────────────────────────
Bar mk(int index, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, T + static_cast<std::int64_t>(index) * 60000};
}

std::vector<Bar> wave(int count, double base = 100.0, double reach = 0.5) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const int phase = i % 8;
        const int triangle = phase < 4 ? phase : 8 - phase;
        const double p = base + 0.5 * triangle + 0.25 * (i % 3);
        bars.push_back(mk(i, p, p + reach, p - reach, p + 0.25));
    }
    return bars;
}

std::vector<Bar> walk(Random& r, int count, double wide_percent) {
    std::vector<Bar> bars;
    double price = 100.0;
    for (int i = 0; i < count; ++i) {
        double open = price + 0.25 * (r.below(5) - 2);
        if (r.chance(8)) open += 0.25 * (r.below(33) - 16);
        if (open < 20.0) open = 20.0;
        const double wide = r.chance(static_cast<int>(wide_percent)) ? 4.0 : 1.0;
        const double high = open + 0.25 * r.below(static_cast<int>(8 * wide) + 1);
        const double low = open - 0.25 * r.below(static_cast<int>(8 * wide) + 1);
        const double close = low + 0.25 * r.below(static_cast<int>((high - low) / 0.25) + 1);
        bars.push_back(mk(i, open, high, low, close));
        price = close;
    }
    return bars;
}

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

class PubHost;
using Script = std::function<void(PubHost&, int, const Bar&)>;

struct Scenario {
    std::string name;
    source::PineStrategyConfig config;
    std::vector<Bar> bars;
    Script script;
    bool magnifier = false;
    double qty_step = 0.0;
    // > 0: after the first run, reconfigure the host (a second
    // configure_pine_strategy, then strategy overrides) and run again.
    int reruns = 0;
};

// A generated-strategy-shaped source host whose body is the scenario's
// script; it folds the source layer at every callback it is given.
class PubHost final : public source::PineStrategyHost {
public:
    explicit PubHost(const Scenario& s) : script_(s.script) {
        attach_pine_execution_adapter();
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        if (s.qty_step > 0.0) set_syminfo_metadata("qty_step", s.qty_step);
        configure_pine_strategy(s.config);
        set_broker_state_hash_recording(true);
    }

    std::uint64_t broker_state_hash_projection() const override {
        return broker_state_hash_from_execution_hash(kExecutionHash);
    }

    void on_source_bar(const Bar& bar) override {
        folds.push_back(fold());
        const int index = pine_bar_index();
        repeat_ = index == last_index_ ? repeat_ + 1 : 0;
        last_index_ = index;
        script_(*this, index, bar);
    }

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

    // The live configuration a generated body writes directly.
    source::PineStrategyConfig& live() noexcept { return fixture_configuration(); }
    int repeat() const noexcept { return repeat_; }
    void restart() {
        last_index_ = -1;
        repeat_ = 0;
    }

    std::vector<std::uint64_t> folds;

private:
    Script script_;
    int last_index_ = -1;
    int repeat_ = 0;
};

// ── Configuration mutation ───────────────────────────────────────────────
// Writes field `field` (0-12, PineStrategyConfig's declaration order) to a
// value drawn from its palette.
void mutate(PubHost& h, Random& r, int field) {
    auto& c = h.live();
    switch (field) {
    case 0: c.process_orders_on_close = !c.process_orders_on_close; break;
    case 1: c.calc_on_order_fills = !c.calc_on_order_fills; break;
    case 2: {
        const double capitals[] = {10000.0, 2500.0, 40000.0, 10000.25};
        c.initial_capital = capitals[r.below(4)];
        break;
    }
    case 3: c.default_qty_type = r.below(3); break;
    case 4: {
        const double values[] = {1.0, 2.0, 3.5, 25.0, 100.0};
        c.default_qty_value = values[r.below(5)];
        break;
    }
    case 5: c.pyramiding = r.below(4); break;
    case 6: {
        const double values[] = {0.0, 0.1, 1.0, 2.5};
        c.commission_value = values[r.below(4)];
        break;
    }
    case 7: c.commission_type = r.below(3); break;
    case 8: c.slippage = r.below(3); break;
    case 9: {
        const double margins[] = {100.0, 50.0, 25.0, 12.5};
        c.margin_long = margins[r.below(4)];
        break;
    }
    case 10: {
        const double margins[] = {100.0, 50.0, 25.0, 12.5};
        c.margin_short = margins[r.below(4)];
        break;
    }
    case 11: c.close_entries_rule_any = !c.close_entries_rule_any; break;
    default: c.src_series_active = !c.src_series_active; break;
    }
}

// Orders every field acts on: market and priced entries (sizing, pyramiding,
// admission, margin), exits (slippage, commission), partial and whole closes
// (close_entries_rule), sized orders.
void command(PubHost& h, Random& s, const Bar& bar) {
    static const char* const kIds[] = {"L", "S", "A"};
    const char* id = kIds[s.below(3)];
    const bool is_long = s.chance(55);
    const double off = 0.25 * (1 + s.below(10));
    switch (s.below(8)) {
    case 0: case 1:
        h.strategy_entry(id, is_long);
        break;
    case 2:
        h.strategy_entry(id, is_long, is_long ? bar.close - off : bar.close + off);
        break;
    case 3: {
        const double limit = bar.close + off;
        const double stop = bar.close - off;
        h.strategy_exit("X", id, limit, stop);
        break;
    }
    case 4:
        h.strategy_close(id, "", kNa, 50.0);
        break;
    case 5:
        h.strategy_close(id);
        break;
    case 6: {
        const double qty = 1.0 + s.below(3);
        h.strategy_order(id, is_long, qty);
        break;
    }
    default:
        h.strategy_close_all();
        break;
    }
}

Scenario config_scenario(std::uint64_t seed, bool flags, bool magnifier) {
    Random r(seed);
    Scenario s;
    char name[48];
    std::snprintf(name, sizeof name, "Config%s%02" PRIu64 "%s", flags ? "Flags" : "",
                  seed, magnifier ? "M" : "");
    s.name = name;
    s.config = base_config();
    s.config.pyramiding = 2;
    s.bars = walk(r, 48, 12.0);
    s.magnifier = magnifier;
    s.qty_step = r.chance(50) ? 0.25 : 0.0;
    s.reruns = r.chance(50) ? 1 : 0;
    const std::uint64_t script_seed = r.next();
    s.script = [script_seed, flags](PubHost& h, int i, const Bar& bar) {
        if (h.repeat() > 1) return;
        Random w(script_seed ^ (static_cast<std::uint64_t>(i) * 0xD1B54A32D192ED03ull)
                 ^ static_cast<std::uint64_t>(h.repeat()));
        // Every field over the run: a sweep of one field per bar, then
        // seeded writes, some twice and some restoring the old value.
        if (i < 13) {
            if (flags || i > 1) mutate(h, w, i);
        } else if (w.chance(35)) {
            const int field = flags ? w.below(13) : 2 + w.below(11);
            mutate(h, w, field);
            if (w.chance(25)) {
                const auto before = h.live();
                mutate(h, w, field);
                if (w.chance(50)) h.live() = before;
            }
        }
        const int commands = w.chance(60) ? 1 + w.below(2) : 0;
        for (int k = 0; k < commands; ++k) command(h, w, bar);
    };
    return s;
}

// ── Pending entries ──────────────────────────────────────────────────────
std::vector<Scenario> pending_fixtures() {
    std::vector<Scenario> out;
    const auto fixture = [&](const char* name, source::PineStrategyConfig config,
                             std::vector<Bar> bars, Script script) {
        for (bool magnifier : {false, true}) {
            Scenario s;
            s.name = std::string(name) + (magnifier ? "M" : "");
            s.config = config;
            s.bars = bars;
            s.script = script;
            s.magnifier = magnifier;
            out.push_back(std::move(s));
        }
    };
    // An entry issued after strategy.close_all waits behind it.
    {
        auto config = base_config();
        config.pyramiding = 2;
        fixture("CloseAllThenEntry", config, wave(40),
            [](PubHost& h, int i, const Bar&) {
                const int phase = i % 6;
                if (phase == 1) h.strategy_entry("L", true);
                if (phase == 3) {
                    h.strategy_close_all();
                    h.strategy_entry("S", false);
                }
                if (phase == 5) {
                    h.strategy_close_all();
                    h.strategy_entry("L", true);
                    h.strategy_entry("A", true);
                }
            });
    }
    // Reversals: an opposite entry while held, with and without its own
    // close in the same evaluation.
    {
        auto config = base_config();
        fixture("Reversals", config, wave(40),
            [](PubHost& h, int i, const Bar&) {
                const int phase = i % 5;
                if (phase == 0) h.strategy_entry("L", true);
                if (phase == 2) h.strategy_entry("S", false);
                if (phase == 3) {
                    h.strategy_close("S");
                    h.strategy_entry("L", true);
                }
            });
    }
    // Same-bar market batches and a partial close of the batch.
    {
        auto config = base_config();
        config.pyramiding = 3;
        fixture("SameBarBatch", config, wave(36),
            [](PubHost& h, int i, const Bar&) {
                const int phase = i % 7;
                if (phase == 1) {
                    h.strategy_entry("A", true);
                    h.strategy_entry("B", true);
                }
                if (phase == 3) {
                    h.strategy_close("A", "", 1.0);
                    h.strategy_entry("S", false);
                }
                if (phase == 4) h.strategy_close("B", "", kNa, 50.0);
                if (phase == 6) h.strategy_close_all();
            });
    }
    // Market adds under process_orders_on_close.
    {
        auto config = base_config();
        config.process_orders_on_close = true;
        config.pyramiding = 2;
        fixture("CloseOrderAdds", config, wave(36),
            [](PubHost& h, int i, const Bar&) {
                const int phase = i % 8;
                if (phase == 1) {
                    h.strategy_entry("A", true, kNa, kNa, 2.0);
                    h.strategy_entry("B", false, kNa, kNa, 3.0);
                }
                if (phase == 3) h.strategy_entry("A", true);
                if (phase == 5) {
                    h.strategy_close("A", "", 1.0);
                    h.strategy_entry("A", true);
                }
                if (phase == 7) h.strategy_close_all();
            });
    }
    // Stops that a fill recalculation finds marketable: delayed to the next
    // open under calc_on_order_fills.
    {
        auto config = base_config();
        config.calc_on_order_fills = true;
        config.pyramiding = 2;
        fixture("DelayedStops", config, wave(40, 100.0, 1.0),
            [](PubHost& h, int i, const Bar& bar) {
                if (h.repeat() > 1) return;
                const int phase = i % 9;
                if (phase == 1 && h.repeat() == 0) h.strategy_entry("C", true);
                if (phase >= 1 && phase < 6)
                    h.strategy_entry("D", true, kNa, bar.close - 0.25);
                if (phase == 3) h.strategy_entry("E", false, kNa, bar.close + 0.25);
                if (phase == 7) h.strategy_close_all();
            });
    }
    return out;
}

Scenario storm(std::uint64_t seed, bool magnifier) {
    Random r(seed);
    Scenario s;
    char name[48];
    std::snprintf(name, sizeof name, "Storm%02" PRIu64 "%s", seed, magnifier ? "M" : "");
    s.name = name;
    auto& c = s.config;
    c = base_config();
    c.process_orders_on_close = r.chance(35);
    c.calc_on_order_fills = r.chance(25);
    c.pyramiding = r.below(4);
    const int qty_type = r.below(3);
    if (qty_type == 1) {
        c.default_qty_type = static_cast<int>(QtyType::PERCENT_OF_EQUITY);
        c.default_qty_value = r.chance(50) ? 100.0 : 25.0;
    } else if (qty_type == 2) {
        c.default_qty_type = static_cast<int>(QtyType::CASH);
        c.default_qty_value = 400.0;
    }
    c.close_entries_rule_any = r.chance(20);
    s.bars = walk(r, 40 + r.below(20), 10.0);
    s.magnifier = magnifier;
    const std::uint64_t script_seed = r.next();
    s.script = [script_seed](PubHost& h, int i, const Bar& bar) {
        if (h.repeat() > 1) return;
        Random w(script_seed ^ (static_cast<std::uint64_t>(i) * 0xD1B54A32D192ED03ull)
                 ^ static_cast<std::uint64_t>(h.repeat()));
        if (h.repeat() == 1 && !w.chance(20)) return;
        static const char* const kIds[] = {"L", "S", "A", "B"};
        const int commands = w.chance(65) ? 1 + w.below(3) : 0;
        for (int k = 0; k < commands; ++k) {
            const char* id = kIds[w.below(4)];
            const bool is_long = w.chance(55);
            const double off = 0.25 * (1 + w.below(12));
            switch (w.below(13)) {
            case 0: case 1: case 2:
                h.strategy_entry(id, is_long);
                break;
            case 3:
                h.strategy_entry(id, is_long, kNa, is_long ? bar.close + off : bar.close - off);
                break;
            case 4: {
                const double qty = 1.0 + w.below(3);
                h.strategy_close(id, "", qty);
                break;
            }
            case 5:
                h.strategy_close(id, "", kNa, 50.0);
                break;
            case 6:
                h.strategy_close_all();
                break;
            case 7: {
                const double limit = bar.close + off;
                const double stop = bar.close - off;
                h.strategy_exit("X", id, limit, stop);
                break;
            }
            case 8:
                h.strategy_cancel(id);
                break;
            case 9: {
                const double qty = 1.0 + w.below(2);
                h.strategy_order(id, is_long, qty);
                break;
            }
            case 10: {
                const double trail_points = 4.0 + w.below(8);
                const double trail_offset = 1.0 + w.below(4);
                h.strategy_exit("T", id, kNa, kNa, trail_points, trail_offset);
                break;
            }
            case 11: {
                const double stop = is_long ? bar.close + off : bar.close - off;
                h.strategy_exit("W", id, kNa, stop);
                break;
            }
            default:
                h.strategy_close(id);
                break;
            }
        }
    };
    return s;
}

// ── Same-bar exits ───────────────────────────────────────────────────────
// Entry `L` of three contracts, then three bracket legs of a third each at
// three limits (or stops), issued in a seeded order, on a bar that reaches
// all of them: the legs fill in price order, the report lists them in
// command order.
Scenario legs(std::uint64_t seed, bool stops, bool magnifier) {
    Random r(seed);
    Scenario s;
    char name[48];
    std::snprintf(name, sizeof name, "Legs%s%02" PRIu64 "%s", stops ? "Stops" : "", seed,
                  magnifier ? "M" : "");
    s.name = name;
    s.config = base_config();
    s.config.default_qty_value = 3.0;
    s.magnifier = magnifier;
    auto bars = wave(30);
    // Bars 8 and 20 reach far on both sides.
    bars[8] = mk(8, 100.0, 104.0, 96.0, 100.5);
    bars[20] = mk(20, 100.0, 104.5, 95.5, 99.5);
    s.bars = bars;
    int order[3] = {0, 1, 2};
    for (int i = 2; i > 0; --i) {
        const int j = r.below(i + 1);
        const int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    const int second_entry = 12 + r.below(4);
    s.script = [order, stops, second_entry](PubHost& h, int i, const Bar& bar) {
        static const char* const kLegs[] = {"X1", "X2", "X3"};
        if (i == 5 || i == second_entry) h.strategy_entry("L", true);
        if (i == 6 || i == second_entry + 1) {
            for (int k = 0; k < 3; ++k) {
                const int leg = order[k];
                const double reach = 1.0 + 0.75 * leg;
                const double limit = stops ? kNa : bar.close + reach;
                const double stop = stops ? bar.close - reach : bar.close - 6.0;
                h.strategy_exit(kLegs[leg], "L", limit, stop, kNa, kNa, kNa, 100.0 / 3.0);
            }
        }
    };
    return s;
}

std::vector<Scenario> exit_near_misses() {
    std::vector<Scenario> out;
    const auto fixture = [&](const char* name, source::PineStrategyConfig config, Script script) {
        auto bars = wave(30);
        bars[9] = mk(9, 100.0, 104.0, 96.0, 100.5);
        bars[21] = mk(21, 100.0, 104.5, 95.5, 99.5);
        for (bool magnifier : {false, true}) {
            Scenario s;
            s.name = std::string(name) + (magnifier ? "M" : "");
            s.config = config;
            s.bars = bars;
            s.script = script;
            s.magnifier = magnifier;
            out.push_back(std::move(s));
        }
    };
    // Two entries, one leg each, both filled on bar 9: one exit time, two
    // entry ids.
    {
        auto config = base_config();
        config.pyramiding = 2;
        fixture("TwoEntries", config, [](PubHost& h, int i, const Bar& bar) {
            if (i == 5) {
                h.strategy_entry("A", true, kNa, kNa, 1.0);
                h.strategy_entry("B", true, kNa, kNa, 1.0);
            }
            if (i == 6) {
                h.strategy_exit("XB", "B", bar.close + 1.0, bar.close - 6.0);
                h.strategy_exit("XA", "A", bar.close + 2.0, bar.close - 6.0);
            }
        });
    }
    // Two lots of one id at two entry times, one leg over both.
    {
        auto config = base_config();
        config.pyramiding = 2;
        config.default_qty_value = 1.0;
        fixture("PyramidLots", config, [](PubHost& h, int i, const Bar& bar) {
            if (i == 3 || i == 5) h.strategy_entry("L", true);
            if (i == 6) {
                h.strategy_exit("X2", "L", bar.close + 2.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
                h.strategy_exit("X1", "L", bar.close + 1.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
            }
        });
    }
    // A bracket leg and a strategy.close of the same entry on bar 9.
    {
        auto config = base_config();
        config.default_qty_value = 2.0;
        fixture("LegBesideClose", config, [](PubHost& h, int i, const Bar& bar) {
            if (i == 5) h.strategy_entry("L", true);
            if (i == 6) h.strategy_exit("X", "L", bar.close + 1.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
            if (i == 9) h.strategy_close("L", "", 1.0);
        });
    }
    // The run's only two rows: two legs of one entry filled together on bar
    // 9, the lower limit first, the higher one commanded first.
    fixture("TwoLegsOnly", base_config(), [](PubHost& h, int i, const Bar& bar) {
        if (i == 5) h.strategy_entry("L", true);
        if (i == 6) {
            h.strategy_exit("X2", "L", bar.close + 1.75, bar.close - 6.0, kNa, kNa, kNa, 50.0);
            h.strategy_exit("X1", "L", bar.close + 1.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
        }
    });
    // Legs of one entry on two different bars.
    fixture("LegsApart", base_config(), [](PubHost& h, int i, const Bar& bar) {
        if (i == 5) h.strategy_entry("L", true);
        if (i == 6) {
            h.strategy_exit("X2", "L", bar.close + 2.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
            h.strategy_exit("X1", "L", bar.close + 3.0, bar.close - 6.0, kNa, kNa, kNa, 50.0);
        }
    });
    return out;
}

std::vector<Scenario> battery() {
    std::vector<Scenario> out;
    for (std::uint64_t seed = 1; seed <= 16; ++seed) {
        for (bool magnifier : {false, true}) {
            out.push_back(config_scenario(seed, false, magnifier));
            out.push_back(config_scenario(seed + 100, true, magnifier));
        }
    }
    for (auto& s : pending_fixtures()) out.push_back(std::move(s));
    for (std::uint64_t seed = 1; seed <= 24; ++seed) {
        for (bool magnifier : {false, true}) out.push_back(storm(seed, magnifier));
    }
    for (std::uint64_t seed = 1; seed <= 6; ++seed) {
        for (bool stops : {false, true}) {
            for (bool magnifier : {false, true}) out.push_back(legs(seed, stops, magnifier));
        }
    }
    for (auto& s : exit_near_misses()) out.push_back(std::move(s));
    return out;
}

// ── One run: its digest ──────────────────────────────────────────────────
struct Digest {
    std::uint64_t value = 1469598103934665603ull;
    std::size_t folds = 0;
    std::size_t trades = 0;
    std::size_t rows = 0;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            value ^= c[i];
            value *= 1099511628211ull;
        }
    }
    template <class V> void v(V x) { bytes(&x, sizeof x); }
    void s(const std::string& text) {
        v(text.size());
        bytes(text.data(), text.size());
    }
};

void fold_run(Digest& d, PubHost& h, const Scenario& s) {
    h.folds.clear();
    h.restart();
    if (s.magnifier) {
        h.run(s.bars.data(), static_cast<int>(s.bars.size()), "1", "1", true, 4,
              MagnifierDistribution::ENDPOINTS);
    } else {
        h.run(s.bars.data(), static_cast<int>(s.bars.size()));
    }
    d.s(h.last_error());
    for (std::uint64_t f : h.folds) d.v(f);
    d.folds += h.folds.size();
    const int trades = h.trade_count();
    for (int i = 0; i < trades; ++i) {
        const Trade& t = h.get_trade(i);
        d.v(t.entry_time); d.v(t.exit_time); d.v(t.entry_price); d.v(t.exit_price);
        d.v(t.qty); d.v(t.pnl); d.v(t.commission); d.v(t.is_long);
        d.v(t.entry_bar_index); d.v(t.exit_bar_index); d.v(t.exit_from_bracket);
        d.s(t.entry_id); d.s(t.exit_id); d.s(t.exit_comment); d.s(t.entry_comment);
    }
    d.trades += static_cast<std::size_t>(trades);
    // The kernel's own reads between publications: the broker-state hash row
    // it records at every script bar.
    ReportC report{};
    h.fill_report(&report);
    for (std::int64_t i = 0; i < report.broker_state_hash_len; ++i) d.v(report.broker_state_hash[i]);
    d.rows += static_cast<std::size_t>(report.broker_state_hash_len);
    BacktestEngine::free_report(&report);
    d.v(h.broker_state_hash());
}

Digest observe(const Scenario& s) {
    Digest d;
    PubHost h(s);
    fold_run(d, h, s);
    if (s.reruns > 0) {
        // A host reconfigured between runs: a new configuration, then
        // overrides over it.
        auto next = s.config;
        next.default_qty_value = s.config.default_qty_value + 1.0;
        next.slippage = 1;
        next.margin_short = 50.0;
        h.configure_pine_strategy(next);
        fold_run(d, h, s);
        source::StrategyOverrides overrides;
        overrides.commission_value = 1.5;
        overrides.pyramiding = 3;
        overrides.close_entries_rule = 1;
        h.set_strategy_override(overrides);
        fold_run(d, h, s);
    }
    return d;
}

struct Pinned {
    const char* name;
    std::uint64_t digest;
    std::size_t folds;
    std::size_t trades;
    std::size_t rows;
};

#if defined(PINEFORGE_D2D_PUBLICATION_HARVEST)
void harvest() {
    std::printf("// R5 lane D2-D: the values tests/test_publication_witness.cpp pins, one\n"
                "// row per run of its battery (name, digest, source folds, closed trades,\n"
                "// recorded broker-state hash rows).\n"
                "// Harvested on 6c081f5d, re-harvested once for V19-D's v4 fold (INT23) and\n"
                "// once for K-ULP3's whole-lot close: see the provenance note in the test.\n"
                "// Generated -- never edit a row by hand.\n");
    std::printf("constexpr Pinned kPinned[] = {\n");
    for (const Scenario& s : battery()) {
        const Digest d = observe(s);
        std::printf("    {\"%s\", 0x%016" PRIx64 "ull, %zu, %zu, %zu},\n", s.name.c_str(),
                    d.value, d.folds, d.trades, d.rows);
    }
    std::printf("};\n");
}
#else
#include "test_publication_witness_pinned.inc"

void every_run_matches_the_base_tree() {
    const auto all = battery();
    constexpr std::size_t pinned = sizeof kPinned / sizeof kPinned[0];
    CHECK(all.size() == pinned);
    std::size_t matched = 0;
    std::size_t folds = 0;
    std::size_t trades = 0;
    std::size_t rows = 0;
    for (std::size_t i = 0; i < all.size() && i < pinned; ++i) {
        const Digest d = observe(all[i]);
        const Pinned& want = kPinned[i];
        const bool same = all[i].name == want.name && d.value == want.digest
            && d.folds == want.folds && d.trades == want.trades && d.rows == want.rows;
        CHECK(same);
        if (!same) {
            std::fprintf(stderr, "  %s: digest %016" PRIx64 " folds %zu trades %zu rows %zu; "
                         "pinned %s %016" PRIx64 " %zu %zu %zu\n", all[i].name.c_str(), d.value,
                         d.folds, d.trades, d.rows, want.name, want.digest, want.folds,
                         want.trades, want.rows);
        } else {
            ++matched;
        }
        folds += d.folds;
        trades += d.trades;
        rows += d.rows;
    }
    std::printf("  %zu of %zu runs match the base tree (%zu source folds, %zu closed trades, "
                "%zu recorded hash rows)\n", matched, pinned, folds, trades, rows);
    CHECK(trades > 1500);
}
#endif

}  // namespace

int main() {
#if defined(PINEFORGE_D2D_PUBLICATION_HARVEST)
    harvest();
    return 0;
#else
    every_run_matches_the_base_tree();
    if (failures == 0) {
        std::printf("test_publication_witness: ok (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_publication_witness: %d of %d checks failed\n", failures, checks);
    return 1;
#endif
}
