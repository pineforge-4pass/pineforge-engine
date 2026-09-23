// R5 lane PERF-L2: the fused settlement against the staged chain it short-cuts,
// bit for bit, and a count of which one ran.
//
// A settlement call on a book of at most one lot that keeps none of it -- an
// opening from flat, the whole lot closing on the Book or on its opening's
// scope, that close and the opposite opening, a ReverseTo of the one lot --
// takes one pass on the stack (BacktestEngine::NativeSettlementStage::OneLot,
// src/engine_execution.cpp); every other call stages through the chain.
// internal::set_fused_settlement(false) sends every call through the chain,
// which is the computation before the lane, so every part below runs twice,
// fused and staged, and requires every value to be equal:
//
//   1. The admission table. One book per admission condition on each side of
//      it -- the condition that admits, then the same call with that one
//      condition flipped -- asserting which path each entry took (the
//      process-wide probe, internal::count_settlement_paths) and that the
//      inspection, projection, precommit preview and settlement answer the
//      same fields, throw the same exception with the same text, consult an
//      excursion owner with the same facts the same number of times, and
//      leave the same book, rows, sums, counters, stream actions and broker
//      hash. The conditions no fused call can fail after admission (the
//      prepare, preview and preflight checks, the sinks' own throws) have
//      rows of their own.
//   2. Randomized direct books: 6,000 seeded books of zero, one or two lots
//      under every fee form, explicit tickets and rebates, account FX scalar
//      and table, a legacy unquoted entry fee, prior rows and counters near
//      exhaustion, stream observation, excursion owners that answer, count
//      or throw, and sequences of fills through every fused-capable entry,
//      with prices, quantities and tickets on both sides of every test.
//   3. Randomized runs through the real matcher (native_fused_settlement_fixture.hpp):
//      fee forms, account FX (unit, constant, a stepping curve), the margin
//      model under both liquidation sizings, the magnifier off / synthesized
//      / lower timeframe, calculate-on-fills, the price grid, host-owned
//      excursions and batch or stream driving; the continuation at every
//      bar and fill, every fill's book, equity, lots and rows, every
//      precommit view, every excursion consultation, the broker and stream
//      hashes, the trades and the events.
//   4. K3's randomized working books and L5's band-edge books, both ways.
//   5. The counter witness: the common case takes the fused path on every
//      entry (bracket and market books: every inspection, preview and
//      settlement), the chain still serves what it declines (a pyramid), and
//      the switch sends everything to the chain.
//
// Fail-before: at the lane's base there is no internal::set_fused_settlement,
// so this TU does not compile. Source-free, so it also runs in the kernel-only
// profile.
#include "../src/engine_internal.hpp"
#include "native_fused_settlement_fixture.hpp"
#include "native_match_band_fixture.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
namespace ex = pineforge::execution;
namespace no = pineforge::native_order;
using internal::SettlementEntry;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

// A transcript of words: every double as its bits, every string as its bytes.
struct Words {
    std::vector<std::uint64_t> w;
    void u(std::uint64_t v) { w.push_back(v); }
    void i(std::int64_t v) { w.push_back(static_cast<std::uint64_t>(v)); }
    void d(double v) { w.push_back(bits(v)); }
    void b(bool v) { w.push_back(v ? 1u : 0u); }
    void s(const std::string& v) {
        w.push_back(v.size());
        std::uint64_t word = 0;
        for (std::size_t k = 0; k < v.size(); ++k) {
            word = word * 131u + static_cast<unsigned char>(v[k]);
        }
        w.push_back(word);
    }
    void tag(std::uint64_t t) { w.push_back(0xABCD000000000000ull | t); }
};

// ---------------------------------------------------------------------------
// Direct books: an engine whose settlement state a case writes and reads.
// ---------------------------------------------------------------------------

struct LotSpec {
    double price = 100.0;
    std::int64_t time = 1000;
    double qty = 1.0;
    std::string id = "E";
    int bar = 0;
    std::string comment;
    double runup = 0.0;
    double drawdown = 0.0;
    bool skip_high = false;
    bool skip_low = false;
    double commission = 0.0;  // NaN: a legacy lot whose paid fee was never quoted
    std::uint64_t incarnation = 7;
};

enum class HookMode { None, Answer, Throw };

struct BookSpec {
    PositionSide side = PositionSide::FLAT;
    std::vector<LotSpec> lots;
    std::int64_t cycle = 0;
    std::int64_t next_cycle = 5;
    int entry_count = -1;  // < 0: the lot count
    int wins = 0;
    int losses = 0;
    int evens = 0;
    double net = 0.0;
    double gross_profit = 0.0;
    double gross_loss = 0.0;
    double initial_capital = 10000.0;
    CommissionType fee = CommissionType::PERCENT;
    double fee_value = 0.1;
    double pointvalue = 1.0;
    double fx = 1.0;
    std::vector<std::int64_t> fx_times;
    std::vector<double> fx_rates;
    std::int64_t clock = 60000;
    bool stream = false;
    std::uint64_t stream_sequence = 0;
    int prior_rows = 0;
    HookMode hook = HookMode::None;
    int hook_throw_at = -1;  // the consultation that throws, under HookMode::Throw
};

enum class ScopeKind { Book, Opening, Selected };
enum class SettleEntry { ScopedAt, At, WithContext, SelectedAt, ReversalAt };

struct Step {
    bool reversal = false;
    ex::Action action = ex::Flatten{};
    double reverse_units = 0.0;
    ScopeKind scope = ScopeKind::Book;
    ex::OpeningExposure opening{};
    ex::SelectedOpeningSet selected{};
    double price = 120.0;
    std::string id = "X";
    std::string comment = "c";
    std::uint64_t incarnation = 100;
    std::optional<double> commission;
    ex::CloseCause cause = ex::CloseCause::Unspecified;
    std::int64_t time = 120000;
    int index = 3;
    std::optional<double> trail_peak;
    bool lifecycle = false;         // a non-empty lifecycle batch (settle_with_context)
    bool pin_ticket = true;         // preview/settle carry the inspected ticket, as the consumer does
    std::optional<double> inspect_fx;  // inspect_native_settlement_scoped_at's explicit rate
    int inspect_form = 0;           // 0 scoped, 1 scoped_at, 2 unscoped (Book only)
    int project_form = 0;           // 0 scoped, 1 unscoped (Book only)
    SettleEntry settle = SettleEntry::ScopedAt;
};

class Book final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void seed(const BookSpec& spec) {
        spec_ = spec;
        position_side_ = spec.side;
        pyramid_entries_.clear();
        double qty = 0.0;
        double weighted = 0.0;
        for (const auto& l : spec.lots) {
            PyramidEntry lot{l.price, l.time, l.qty, l.id, l.bar};
            lot.entry_comment = l.comment;
            lot.max_runup = l.runup;
            lot.max_drawdown = l.drawdown;
            lot.skip_entry_bar_high = l.skip_high;
            lot.skip_entry_bar_low = l.skip_low;
            lot.entry_commission_account = l.commission;
            lot.entry_incarnation = l.incarnation;
            pyramid_entries_.push_back(lot);
            qty += l.qty;
            weighted += l.qty * l.price;
        }
        position_qty_ = qty;
        position_entry_price_ = qty > 0.0 ? weighted / qty : 0.0;
        position_entry_count_ = spec.entry_count >= 0 ? spec.entry_count
                                                      : static_cast<int>(spec.lots.size());
        position_cycle_seq_ = spec.cycle;
        next_position_cycle_seq_ = spec.next_cycle;
        position_open_bar_ = spec.lots.empty() ? -1 : spec.lots.front().bar;
        position_entry_time_ = spec.lots.empty() ? 0 : spec.lots.front().time;
        win_trades_count_ = spec.wins;
        loss_trades_count_ = spec.losses;
        eventrades_count_ = spec.evens;
        net_profit_sum_ = spec.net;
        net_profit_roundoff_value_ = spec.net;
        gross_profit_sum_ = spec.gross_profit;
        gross_loss_sum_ = spec.gross_loss;
        initial_capital_ = spec.initial_capital;
        commission_type_ = spec.fee;
        commission_value_ = spec.fee_value;
        syminfo_.pointvalue = spec.pointvalue;
        account_currency_fx_ = spec.fx;
        account_currency_fx_timestamps_ = spec.fx_times;
        account_currency_fx_rates_ = spec.fx_rates;
        current_bar_.timestamp = spec.clock;
        stream_observe_actions_ = spec.stream;
        stream_action_sequence_ = spec.stream_sequence;
        stream_order_actions_.clear();
        trades_.clear();
        for (int r = 0; r < spec.prior_rows; ++r) {
            Trade row{};
            row.entry_time = 10 + r;
            row.exit_time = 20 + r;
            row.entry_price = 90.0 + r;
            row.exit_price = 95.0 + r;
            row.qty = 1.0;
            row.pnl = r % 2 ? 5.0 : -2.0;
            row.pnl_pct = 1.0;
            row.is_long = true;
            row.entry_id = "P";
            row.exit_id = "Q";
            row.entry_incarnation = spec.lots.empty() ? 1 : spec.lots.front().incarnation;
            trades_.push_back(row);
        }
        consultations_ = 0;
        if (spec.hook == HookMode::None) {
            lot_excursion_hook_ = nullptr;
        } else {
            lot_excursion_hook_ = [this](const ClosedLotExcursionFacts& f) {
                log_.tag(1);
                log_.u(f.entry_incarnation);
                log_.i(f.entry_time_ms);
                log_.d(f.entry_price);
                log_.d(f.lot_qty);
                log_.d(f.closed_qty);
                log_.d(f.fill_price);
                log_.d(f.carried_favorable);
                log_.d(f.carried_adverse);
                log_.b(f.is_long);
                log_.i(f.entry_bar_index);
                log_.i(f.exit_bar_index);
                log_.b(f.entry_bar_high_masked);
                log_.b(f.entry_bar_low_masked);
                log_.d(f.entry_commission);
                const int asked = consultations_++;
                if (spec_.hook == HookMode::Throw && asked == spec_.hook_throw_at)
                    throw std::runtime_error("excursion owner failed");
                return ClosedLotExcursion{1.5 + asked, 0.25 * asked};
            };
        }
    }

    // Every field a settlement can write, as words.
    void state(Words& out) const {
        out.tag(2);
        out.i(static_cast<int>(position_side_));
        out.d(position_qty_);
        out.d(position_entry_price_);
        out.i(position_entry_count_);
        out.i(position_cycle_seq_);
        out.i(next_position_cycle_seq_);
        out.i(position_open_bar_);
        out.i(position_entry_time_);
        out.d(trail_best_price_);
        out.d(net_profit_sum_);
        out.d(net_profit_roundoff_bound_);
        out.d(net_profit_roundoff_value_);
        out.d(gross_profit_sum_);
        out.d(gross_loss_sum_);
        out.i(win_trades_count_);
        out.i(loss_trades_count_);
        out.i(eventrades_count_);
        out.u(stream_action_sequence_);
        out.u(pyramid_entries_.size());
        for (const auto& lot : pyramid_entries_) {
            out.d(lot.price);
            out.i(lot.time);
            out.d(lot.qty);
            out.s(lot.entry_id);
            out.i(lot.entry_bar_index);
            out.s(lot.entry_comment);
            out.d(lot.max_runup);
            out.d(lot.max_drawdown);
            out.b(lot.skip_entry_bar_high);
            out.b(lot.skip_entry_bar_low);
            out.d(lot.entry_commission_account);
            out.u(lot.entry_incarnation);
        }
        out.u(trades_.size());
        for (const auto& t : trades_) {
            out.i(t.entry_time);
            out.i(t.exit_time);
            out.d(t.entry_price);
            out.d(t.exit_price);
            out.d(t.qty);
            out.d(t.pnl);
            out.d(t.pnl_pct);
            out.b(t.is_long);
            out.i(t.entry_bar_index);
            out.i(t.exit_bar_index);
            out.s(t.entry_id);
            out.s(t.entry_comment);
            out.s(t.exit_comment);
            out.s(t.exit_id);
            out.b(t.exit_from_bracket);
            out.d(t.max_runup);
            out.d(t.max_drawdown);
            out.d(t.commission);
            out.u(t.entry_incarnation);
            out.b(t.open_at_end);
            out.u(static_cast<std::uint64_t>(t.close_cause));
        }
        out.u(stream_order_actions_.size());
        for (const auto& a : stream_order_actions_) {
            out.u(a.sequence);
            out.i(a.timestamp_ms);
            out.i(a.bar_index);
            out.b(a.is_entry);
            out.b(a.is_long);
            out.d(a.quantity);
            out.d(a.price);
            out.s(a.order_id);
            out.s(a.comment);
            out.u(a.entry_incarnation);
            out.u(a.closed_trade_index);
        }
        out.u(broker_state_hash());
    }

    ex::Fill fill_of(const Step& step, std::optional<double> ticket) const {
        ex::Fill fill{step.price, step.id, step.comment, step.incarnation, step.commission,
                      step.cause};
        if (ticket) fill.commission_account = ticket;
        return fill;
    }

    ex::PhysicalExecutionContext context_of(const Step& step) const {
        ex::PhysicalExecutionContext context;
        context.effective_time_ms = step.time;
        context.interval_index = step.index;
        context.preceding_exit_trail_peak = step.trail_peak;
        return context;
    }

    // One fill through every fused-capable entry, in the consumer's order:
    // inspect, project, preview, settle.
    void fill(const Step& step, Words& out) {
        auto guarded = [&](std::uint64_t tag, const std::function<void()>& call) {
            out.tag(tag);
            try {
                call();
            } catch (const std::overflow_error& e) {
                out.tag(90); out.s(e.what());
            } catch (const std::length_error& e) {
                out.tag(91); out.s(e.what());
            } catch (const std::runtime_error& e) {
                out.tag(92); out.s(e.what());
            } catch (const std::logic_error& e) {
                out.tag(93); out.s(e.what());
            } catch (const std::bad_alloc&) {
                out.tag(94);
            } catch (...) {
                out.tag(95);
            }
        };
        const ex::Fill fill = fill_of(step, std::nullopt);
        const ex::CloseScope scope = step.scope == ScopeKind::Opening
            ? ex::CloseScope{step.opening} : ex::CloseScope{ex::Book{}};
        const ex::ReverseTo reversal{step.reverse_units};
        std::optional<double> ticket;

        guarded(10, [&] {
            ex::SettlementInspection r;
            if (step.reversal) {
                r = inspect_native_reversal_v1(reversal, fill);
            } else if (step.scope == ScopeKind::Selected) {
                r = inspect_native_settlement_selected(step.action, fill, step.selected);
            } else if (step.inspect_form == 1) {
                r = inspect_native_settlement_scoped_at(step.action, fill, scope,
                    step.inspect_fx ? *step.inspect_fx : active_account_currency_fx());
            } else if (step.inspect_form == 2 && step.scope == ScopeKind::Book) {
                r = inspect_native_settlement(step.action, fill);
            } else {
                r = inspect_native_settlement_scoped(step.action, fill, scope);
            }
            out.u(static_cast<std::uint64_t>(r.status));
            out.d(r.closed_units);
            out.d(r.opened_units);
            out.d(r.resulting_abs_units);
            out.u(r.resulting_lot_count);
            out.d(r.resulting_abs_notional);
            out.d(r.current_ticket);
            out.b(r.would_open);
            out.b(r.incoming_short);
            if (step.pin_ticket) ticket = r.current_ticket;
        });
        guarded(11, [&] {
            ex::AccountEffectProjection a;
            if (step.reversal) {
                a = project_native_reversal_v1(reversal, fill);
            } else if (step.scope == ScopeKind::Selected) {
                a = project_native_settlement_selected_v1(step.action, fill, step.selected);
            } else if (step.project_form == 1 && step.scope == ScopeKind::Book) {
                a = project_native_settlement_v1(step.action, fill);
            } else {
                a = project_native_settlement_scoped_v1(step.action, fill, scope);
            }
            project_words(out, a);
        });
        const ex::Fill pinned = fill_of(step, ticket);
        const ex::PhysicalExecutionContext context = context_of(step);
        guarded(12, [&] {
            ex::AccountEffectProjection account;
            std::vector<double> row_pnl{-1.0, -2.0};  // stale content the preview clears
            ex::Status readiness;
            if (step.reversal) {
                readiness = preview_native_settlement_commit(reversal, pinned, context, account,
                                                             row_pnl);
            } else {
                readiness = preview_native_settlement_commit(
                    step.action, pinned, context, scope,
                    step.scope == ScopeKind::Selected ? &step.selected : nullptr, account,
                    row_pnl);
            }
            out.u(static_cast<std::uint64_t>(readiness));
            project_words(out, account);
            out.u(row_pnl.size());
            for (const double pnl : row_pnl) out.d(pnl);
        });
        guarded(13, [&] {
            ex::Result r;
            if (step.reversal) {
                r = settle_native_reversal_at_v1(reversal, pinned, context);
            } else if (step.scope == ScopeKind::Selected) {
                r = settle_native_execution_selected_at(step.action, pinned, context,
                                                        step.selected);
            } else if (step.settle == SettleEntry::WithContext && step.scope == ScopeKind::Book) {
                ex::LifecycleEffects lifecycle;
                if (step.lifecycle) lifecycle.removals.push_back(ex::PendingRemoval{9, 1, {}, 0});
                r = settle_with_context(step.action, pinned, lifecycle, context);
            } else if (step.settle == SettleEntry::At && step.scope == ScopeKind::Book) {
                r = settle_native_execution_at(step.action, pinned, context);
            } else {
                r = settle_native_execution_scoped_at(step.action, pinned, context, scope);
            }
            out.u(static_cast<std::uint64_t>(r.status));
            out.d(r.closed_units);
            out.d(r.opened_units);
            out.d(r.current_ticket);
            out.u(r.first_trade_index);
            out.u(r.closed_trade_count);
            out.u(r.opened_lot_incarnation);
        });
        state(out);
        out.tag(14);
        out.u(log_.w.size());
        out.w.insert(out.w.end(), log_.w.begin(), log_.w.end());
        log_.w.clear();
    }

    static void project_words(Words& out, const ex::AccountEffectProjection& a) {
        out.u(static_cast<std::uint64_t>(a.status));
        out.d(a.closed_units);
        out.d(a.opened_units);
        out.d(a.resulting_abs_units);
        out.u(a.resulting_lot_count);
        out.d(a.resulting_abs_notional);
        out.d(a.current_ticket);
        out.b(a.would_open);
        out.b(a.incoming_short);
        out.d(a.realized_balance);
        out.d(a.remaining_entry_cost);
        out.d(a.marked_equity);
        out.i(a.cycle_after);
        out.d(a.signed_units_after);
    }

    std::uint64_t open_incarnation() const {
        return pyramid_entries_.empty() ? 0 : pyramid_entries_.front().entry_incarnation;
    }
    std::int64_t cycle() const { return position_cycle_seq_; }

private:
    BookSpec spec_;
    Words log_;
    int consultations_ = 0;
};

// Runs a book and its steps once, on a fresh engine, with the fused
// settlement on or off, and returns the transcript.
std::vector<std::uint64_t> transcript(const BookSpec& spec, const std::vector<Step>& steps,
                                      bool fused) {
    internal::set_fused_settlement(fused);
    Book book;
    book.seed(spec);
    Words out;
    book.state(out);
    for (const auto& step : steps) book.fill(step, out);
    internal::set_fused_settlement(true);
    return out.w;
}

bool first_difference(const std::vector<std::uint64_t>& a, const std::vector<std::uint64_t>& b,
                      const char* what) {
    if (a == b) return false;
    const std::size_t common = a.size() < b.size() ? a.size() : b.size();
    std::size_t at = 0;
    while (at < common && a[at] == b[at]) ++at;
    std::fprintf(stderr, "  %s: fused and staged transcripts differ at word %zu of %zu/%zu\n",
                 what, at, a.size(), b.size());
    return true;
}

LotSpec lot(double qty, double price = 100.0, std::uint64_t incarnation = 7) {
    LotSpec l;
    l.qty = qty;
    l.price = price;
    l.incarnation = incarnation;
    l.commission = 0.2 * qty;
    l.runup = 3.0;
    l.drawdown = 1.5;
    return l;
}

BookSpec long_book(double qty = 2.0) {
    BookSpec spec;
    spec.side = PositionSide::LONG;
    spec.lots = {lot(qty)};
    spec.cycle = 4;
    return spec;
}

BookSpec short_book(double qty = 2.0) {
    BookSpec spec = long_book(qty);
    spec.side = PositionSide::SHORT;
    return spec;
}

BookSpec flat_book() {
    BookSpec spec;
    spec.cycle = 0;
    return spec;
}

Step action(ex::Action value) {
    Step step;
    step.action = value;
    return step;
}

Step reverse_to(double units) {
    Step step;
    step.reversal = true;
    step.reverse_units = units;
    return step;
}

// ---------------------------------------------------------------------------
// 1. The admission table.
// ---------------------------------------------------------------------------

struct Row {
    const char* name;
    BookSpec book;
    Step step;
    bool fused;  // whether the inspection, preview and settlement take the pass
    bool settle_fused = true;  // the settlement's own answer, when it differs
};

void admission_table() {
    using ex::Flatten;
    using order_action::Reduce;
    using order_action::Transact;
    std::vector<Row> rows;
    auto add = [&](const char* name, BookSpec book, Step step, bool fused) {
        rows.push_back(Row{name, std::move(book), std::move(step), fused, fused});
    };

    // Admitted: every one-lot shape, fee form, ticket, rate and sink.
    add("open long from flat", flat_book(), action(Transact{2.0}), true);
    add("open short from flat", flat_book(), action(Transact{-2.0}), true);
    add("flatten a long lot", long_book(), action(Flatten{}), true);
    add("flatten a short lot", short_book(), action(Flatten{}), true);
    add("reduce the whole lot", long_book(), action(Reduce{2.0}), true);
    add("reduce more than the lot", long_book(), action(Reduce{5.0}), true);
    add("transact to flat", long_book(), action(Transact{-2.0}), true);
    add("transact through flat", long_book(), action(Transact{-3.5}), true);
    add("transact through flat, short", short_book(), action(Transact{3.5}), true);
    add("reverse to a target", long_book(), reverse_to(-0.75), true);
    add("reverse a short to a target", short_book(), reverse_to(4.0), true);
    {
        Step step = action(Reduce{2.0});
        step.scope = ScopeKind::Opening;
        step.opening = ex::OpeningExposure{7, 4};
        add("opening scope reduces its lot", long_book(), step, true);
        step.action = Flatten{};
        add("opening scope flattens its lot", long_book(), step, true);
        step.action = Reduce{9.0};
        add("opening scope reduces more than its lot", short_book(), step, true);
    }
    {
        BookSpec book = long_book();
        book.fee = CommissionType::CASH_PER_ORDER;
        book.fee_value = 2.5;
        add("cash per order, close", book, action(Flatten{}), true);
        add("cash per order, reversal", book, action(Transact{-3.0}), true);
        book.fee = CommissionType::CASH_PER_CONTRACT;
        book.fee_value = 0.75;
        add("cash per contract, reversal", book, action(Transact{-3.0}), true);
        book.fee = CommissionType::PERCENT;
        book.fee_value = 0.0;
        add("no fee", book, action(Transact{-3.0}), true);
    }
    {
        Step step = action(Transact{-3.0});
        step.commission = -1.25;
        add("explicit rebate", long_book(), step, true);
        step.commission = 0.0;
        add("explicit waiver", long_book(), step, true);
        step.commission = 4.0;
        step.pin_ticket = false;
        add("explicit ticket, unpinned", long_book(), step, true);
    }
    {
        BookSpec book = long_book();
        book.fx = 1.37;
        book.pointvalue = 50.0;
        Step step = action(Transact{-3.0});
        step.inspect_form = 1;
        step.inspect_fx = 0.5;
        add("account fx, inspection at another rate", book, step, true);
        book.fx_times = {30000, 90000};
        book.fx_rates = {1.25, 3.0};
        add("account fx table", book, step, true);
    }
    {
        BookSpec book = long_book();
        book.lots[0].commission = kNaN;
        add("legacy unquoted entry fee", book, action(Flatten{}), true);
        book.hook = HookMode::Answer;
        add("excursion owner answers", book, action(Transact{-3.0}), true);
        // Consultations in call order: the projection's row (0), the
        // preview's prepared row (1) and projected row (2), the settlement's (3).
        book.hook = HookMode::Throw;
        book.hook_throw_at = 1;
        add("excursion owner throws in the preview", book, action(Transact{-3.0}), true);
        book.hook_throw_at = 3;
        add("excursion owner throws in the settlement", book, action(Flatten{}), true);
        book.hook = HookMode::None;
        book.stream = true;
        book.prior_rows = 3;
        add("stream observes", book, action(Transact{-3.0}), true);
        book.stream_sequence = std::numeric_limits<std::uint64_t>::max() - 1;
        add("stream sequence exhausted in preflight", book, action(Transact{-3.0}), true);
    }
    {
        Step step = action(Transact{2.0});
        step.price = 0.0;
        add("zero price opening", flat_book(), step, true);
        step.price = -5.0;
        add("negative price opening", flat_book(), step, true);
        step.trail_peak = 130.0;
        step.price = 120.0;
        step.action = Flatten{};
        add("trail peak carried into the row", long_book(), step, true);
    }
    {
        BookSpec book = flat_book();
        book.next_cycle = std::numeric_limits<std::int64_t>::max();
        add("cycle exhausted: the prepare throws", book, action(Transact{2.0}), true);
        book = long_book();
        book.next_cycle = 0;
        add("cycle exhausted under a reversal", book, action(Transact{-3.0}), true);
        book = long_book();
        book.wins = std::numeric_limits<int>::max();
        add("win counter exhausted: the preflight throws", book, action(Flatten{}), true);
        Step rise = action(Flatten{});
        rise.price = 1e295;
        book = long_book(1e10);
        book.net = 1.7976e308;
        add("realized sum overflows: prepare refuses", book, rise, true);
        // A percent fee multiplies the same price * quantity product the row's
        // pnl does, so only a fee that ignores the price lets the pnl alone
        // overflow.
        rise.price = 1e300;
        book.net = 0.0;
        book.fee = CommissionType::CASH_PER_ORDER;
        add("row pnl overflows", book, rise, true);
    }

    // Declined: each condition flipped once. The chain answers each.
    {
        Step step = action(Reduce{2.0});
        step.scope = ScopeKind::Selected;
        step.selected = ex::SelectedOpeningSet{4, {7}};
        add("a selection", long_book(), step, false);
    }
    {
        BookSpec book = long_book();
        book.lots.push_back(lot(1.0, 110.0, 8));
        add("two lots", book, action(Flatten{}), false);
        add("two lots reversed", book, reverse_to(-1.0), false);
    }
    {
        Step step = action(Flatten{});
        step.price = kNaN;
        add("price not finite", long_book(), step, false);
        step.price = kInf;
        add("price infinite", long_book(), step, false);
        step.price = 120.0;
        step.commission = kNaN;
        step.pin_ticket = false;
        add("ticket not finite", long_book(), step, false);
    }
    add("quantity not finite", long_book(), action(Transact{kNaN}), false);
    add("negative reduction", long_book(), action(Reduce{-1.0}), false);
    {
        BookSpec book = long_book();
        book.side = PositionSide::FLAT;
        add("book invalid: flat with a lot", book, action(Flatten{}), false);
        book = long_book();
        book.lots[0].qty = 0.0;
        add("book invalid: an empty lot", book, action(Flatten{}), false);
        book = flat_book();
        book.side = PositionSide::LONG;
        add("book invalid: long with no lot", book, action(Transact{1.0}), false);
    }
    {
        Row row{"lifecycle not empty", long_book(), action(Flatten{}), true, false};
        row.step.settle = SettleEntry::WithContext;
        row.step.lifecycle = true;
        rows.push_back(row);
        Row empty{"lifecycle empty", long_book(), action(Flatten{}), true, true};
        empty.step.settle = SettleEntry::WithContext;
        rows.push_back(empty);
    }
    {
        Step step = action(Reduce{2.0});
        step.scope = ScopeKind::Opening;
        step.opening = ex::OpeningExposure{8, 4};
        add("opening scope names another lot", long_book(), step, false);
        step.opening = ex::OpeningExposure{7, 3};
        add("opening scope from another cycle", long_book(), step, false);
        step.opening = ex::OpeningExposure{0, 4};
        add("opening scope of no opening", long_book(), step, false);
        step.opening = ex::OpeningExposure{7, 4};
        step.action = Transact{-2.0};
        add("opening scope under a transaction", long_book(), step, false);
    }
    add("no effect: zero transaction", long_book(), action(Transact{0.0}), false);
    add("no effect: zero reduction", long_book(), action(Reduce{0.0}), false);
    add("no effect: flatten a flat book", flat_book(), action(Flatten{}), false);
    add("no effect: reduce a flat book", flat_book(), action(Reduce{1.0}), false);
    {
        BookSpec book = long_book(1e17);
        add("plan: an absorbed reduction", book, action(Reduce{1.0}), false);
        add("plan: an absorbed transaction", book, action(Transact{1.0}), false);
    }
    add("an addition", long_book(), action(Transact{1.0}), false);
    add("a partial reduction", long_book(), action(Reduce{0.5}), false);
    add("a partial transaction", long_book(), action(Transact{-0.5}), false);
    add("allocation: the remainder absorbed", long_book(1.0), action(Transact{-1e17}), false);
    {
        BookSpec book = long_book(10.0);
        book.fee = CommissionType::CASH_PER_CONTRACT;
        book.fee_value = 1e308;
        Step step = action(Flatten{});
        step.pin_ticket = false;
        add("quote not finite", book, step, false);
        book = long_book(1.0);
        book.fee = CommissionType::CASH_PER_CONTRACT;
        book.fee_value = 1.5e308;
        step.action = Transact{-2.0};
        add("ticket sum not finite", book, step, false);
    }
    add("reverse to the same side", long_book(), reverse_to(2.0), false);
    add("reverse to zero", long_book(), reverse_to(0.0), false);
    add("reverse to a non-finite target", long_book(), reverse_to(kNaN), false);
    add("reverse a flat book", flat_book(), reverse_to(-1.0), false);
    add("reverse: closed and opening overflow", long_book(1e308), reverse_to(-1e308), false);

    int admitted = 0;
    int declined = 0;
    for (const auto& row : rows) {
        internal::count_settlement_paths(true);
        const auto fused = transcript(row.book, {row.step}, true);
        const auto counts = internal::settlement_path_counts();
        const auto staged = transcript(row.book, {row.step}, false);
        const auto forced = internal::settlement_path_counts();
        internal::count_settlement_paths(false);
        const bool differ = first_difference(fused, staged, row.name);
        CHECK(!differ);
        const auto entry = [](SettlementEntry e) { return static_cast<int>(e); };
        const bool inspect_fused = counts.fused[entry(SettlementEntry::Inspect)] == 1
            && counts.staged[entry(SettlementEntry::Inspect)] == 0;
        const bool preview_fused = counts.fused[entry(SettlementEntry::Preview)] == 1
            && counts.staged[entry(SettlementEntry::Preview)] == 0;
        const bool project_fused = counts.fused[entry(SettlementEntry::Project)] == 1
            && counts.staged[entry(SettlementEntry::Project)] == 0;
        const std::uint64_t settle_fused = counts.fused[entry(SettlementEntry::Settle)];
        const std::uint64_t settle_staged = counts.staged[entry(SettlementEntry::Settle)];
        const bool expect_settle = row.settle_fused && row.fused;
        // A settlement a throw in the preview never reaches is counted by
        // neither path.
        const bool settled_at_all = settle_fused + settle_staged == 1;
        const bool paths_ok = inspect_fused == row.fused && project_fused == row.fused
            && preview_fused == row.fused
            && (!settled_at_all || (settle_fused == 1) == expect_settle);
        if (!paths_ok) {
            std::fprintf(stderr, "  %s: inspect %d project %d preview %d settle %llu/%llu, "
                         "expected %s\n", row.name, inspect_fused ? 1 : 0, project_fused ? 1 : 0,
                         preview_fused ? 1 : 0, static_cast<unsigned long long>(settle_fused),
                         static_cast<unsigned long long>(settle_staged),
                         row.fused ? "fused" : "staged");
        }
        CHECK(paths_ok);
        std::uint64_t forced_fused = 0;
        for (int e = 0; e < internal::kSettlementEntries; ++e) forced_fused += forced.fused[e];
        // The second run's counts continue the first's: the switch added no fused call.
        CHECK(forced_fused == counts.fused[0] + counts.fused[1] + counts.fused[2]
                                  + counts.fused[3]);
        (row.fused ? admitted : declined) += 1;
    }
    std::printf("admission table: %zu rows, %d admitted, %d declined, fused == staged\n",
                rows.size(), admitted, declined);
}

// ---------------------------------------------------------------------------
// 2. Randomized direct books.
// ---------------------------------------------------------------------------

double pick(l2_fused::Rng& rng, const std::vector<double>& values) {
    return values[static_cast<std::size_t>(rng.below(static_cast<int>(values.size())))];
}

BookSpec random_book(l2_fused::Rng& rng) {
    BookSpec book;
    const int lots = rng.percent(35) ? 0 : rng.percent(80) ? 1 : 2;
    const bool is_short = rng.percent(45);
    book.side = lots == 0 ? PositionSide::FLAT : is_short ? PositionSide::SHORT : PositionSide::LONG;
    if (rng.percent(2)) book.side = static_cast<PositionSide>(rng.below(3));  // maybe inconsistent
    book.cycle = lots == 0 ? 0 : rng.between(1, 9);
    book.next_cycle = book.cycle + 1;
    if (rng.percent(4)) book.next_cycle = rng.percent(50) ? 0 : std::numeric_limits<std::int64_t>::max();
    for (int k = 0; k < lots; ++k) {
        LotSpec l;
        l.price = pick(rng, {100.0, 97.25, 120.5, 0.5, 1000.0, 3.0e5});
        l.qty = pick(rng, {1.0, 2.0, 0.5, 0.1, 3.0, 1e-9, 12.75, 1e12});
        l.time = 1000 + k * 60000;
        l.id = rng.percent(50) ? "E" : "entry-long-id";
        l.bar = rng.between(0, 5);
        l.comment = rng.percent(30) ? "entry comment" : "";
        l.runup = pick(rng, {0.0, 2.5, 40.0});
        l.drawdown = pick(rng, {0.0, 1.25, 17.0});
        l.skip_high = rng.percent(20);
        l.skip_low = rng.percent(20);
        l.commission = rng.percent(6) ? kNaN : pick(rng, {0.0, 0.1, 2.0, -0.5});
        l.incarnation = rng.percent(3) ? 0 : static_cast<std::uint64_t>(10 + k);
        book.lots.push_back(l);
    }
    if (rng.percent(3) && !book.lots.empty()) book.lots[0].qty = pick(rng, {0.0, -1.0, kNaN});
    const int fee = rng.below(3);
    book.fee = fee == 0 ? CommissionType::PERCENT
        : fee == 1 ? CommissionType::CASH_PER_ORDER : CommissionType::CASH_PER_CONTRACT;
    book.fee_value = pick(rng, {0.0, 0.05, 0.1, 1.0, 2.5, -0.2});
    if (rng.percent(2)) book.fee_value = pick(rng, {1e308, kInf});
    book.pointvalue = pick(rng, {1.0, 1.0, 50.0, 0.1});
    book.fx = pick(rng, {1.0, 1.0, 1.37, 0.8});
    if (rng.percent(20)) {
        book.fx_times = {30000, 90000};
        book.fx_rates = {pick(rng, {1.25, 0.5}), pick(rng, {3.0, 0.9})};
    }
    book.clock = 10000 + 50000 * static_cast<std::int64_t>(rng.below(3));
    book.net = rng.percent(3) ? pick(rng, {1.7e308, -1.7e308}) : pick(rng, {0.0, 125.5, -40.0});
    book.gross_profit = book.net > 0.0 ? book.net : 0.0;
    book.gross_loss = book.net < 0.0 ? book.net : 0.0;
    if (rng.percent(4)) {
        const int max = std::numeric_limits<int>::max();
        book.wins = rng.percent(50) ? max : max - 1;
        book.losses = rng.percent(50) ? max : 3;
        book.evens = rng.percent(50) ? max : 0;
    }
    if (rng.percent(3)) book.entry_count = std::numeric_limits<int>::max();
    book.stream = rng.percent(30);
    book.stream_sequence = rng.percent(5) ? std::numeric_limits<std::uint64_t>::max() - rng.below(2)
                                          : static_cast<std::uint64_t>(rng.below(50));
    book.prior_rows = rng.below(3);
    const int hook = rng.below(10);
    book.hook = hook < 5 ? HookMode::None : hook < 9 ? HookMode::Answer : HookMode::Throw;
    book.hook_throw_at = rng.below(4);
    return book;
}

Step random_step(l2_fused::Rng& rng, double held, bool long_book, std::uint64_t incarnation,
                 std::int64_t cycle) {
    Step step;
    const int kind = rng.below(100);
    const double size = pick(rng, {1.0, 2.0, 0.5, 3.0});
    const double through = held > 0.0 ? held + size : size;
    const double sign = long_book ? -1.0 : 1.0;
    if (kind < 12) {
        step.action = ex::Flatten{};
    } else if (kind < 30) {
        step.action = order_action::Reduce{
            pick(rng, {held, held * 2.0, held * 0.5, 0.0, 1e17, -1.0, kNaN, held + 1e-12})};
    } else if (kind < 62) {
        const double units = pick(rng, {held, through, held * 0.5, 0.0, 1e17, kNaN, kInf});
        step.action = order_action::Transact{
            rng.percent(80) ? sign * units : -sign * pick(rng, {size, 1e-30})};
    } else if (kind < 70 && held == 0.0) {
        step.action = order_action::Transact{rng.percent(50) ? size : -size};
    } else if (kind < 85) {
        step.reversal = true;
        step.reverse_units = rng.percent(85) ? sign * size : pick(rng, {0.0, -sign * size, kNaN, 1e308});
    } else {
        step.action = order_action::Transact{rng.percent(50) ? size : -size};
    }
    const int scope = rng.below(100);
    if (!step.reversal && scope < 22) {
        step.scope = ScopeKind::Opening;
        step.opening = ex::OpeningExposure{rng.percent(85) ? incarnation : incarnation + 1,
                                           rng.percent(90) ? cycle : cycle + 1};
    } else if (!step.reversal && scope < 30) {
        step.scope = ScopeKind::Selected;
        step.selected.cycle = cycle;
        step.selected.incarnations = {incarnation};
        if (rng.percent(20)) step.selected.incarnations.push_back(incarnation + 1);
    }
    step.price = rng.percent(4) ? pick(rng, {kNaN, kInf, 0.0, -3.0})
        : pick(rng, {120.0, 99.75, 100.0, 0.5, 2.0e5});
    step.id = rng.percent(50) ? "X" : "exit-with-a-long-identifier";
    step.comment = rng.percent(50) ? "" : "close comment";
    step.incarnation = static_cast<std::uint64_t>(100 + rng.below(5));
    if (rng.percent(25)) step.commission = pick(rng, {0.0, 1.5, -0.75, kNaN, 1e308});
    step.cause = static_cast<ex::CloseCause>(rng.below(7));
    step.time = 120000 + rng.below(4) * 60000;
    step.index = rng.between(1, 9);
    if (rng.percent(20)) step.trail_peak = pick(rng, {130.0, 90.0});
    step.pin_ticket = rng.percent(85);
    step.inspect_form = rng.below(3);
    if (step.inspect_form == 1) step.inspect_fx = pick(rng, {1.0, 0.5, 2.25});
    step.project_form = rng.below(2);
    const int settle = rng.below(3);
    step.settle = settle == 0 ? SettleEntry::ScopedAt : settle == 1 ? SettleEntry::At
                                                                    : SettleEntry::WithContext;
    step.lifecycle = step.settle == SettleEntry::WithContext && rng.percent(20);
    return step;
}

void randomized_books() {
    constexpr int kBooks = 6000;
    internal::count_settlement_paths(true);
    long steps = 0;
    for (int seed = 1; seed <= kBooks; ++seed) {
        l2_fused::Rng rng(static_cast<std::uint64_t>(seed) * 7919u);
        const BookSpec book = random_book(rng);
        double held = 0.0;
        for (const auto& l : book.lots) held += l.qty;
        const bool long_book = book.side != PositionSide::SHORT;
        const std::uint64_t incarnation = book.lots.empty() ? 7 : book.lots.front().incarnation;
        std::vector<Step> fills;
        const int count = rng.between(1, 3);
        for (int k = 0; k < count; ++k) {
            fills.push_back(random_step(rng, held, long_book, incarnation, book.cycle));
            // The next fill of a sequence sees the book the last one left; a
            // generator that guesses it only moves the shapes around.
            held = rng.percent(50) ? 0.0 : held;
        }
        steps += count;
        const auto fused = transcript(book, fills, true);
        const auto staged = transcript(book, fills, false);
        if (first_difference(fused, staged, "randomized book")) {
            std::fprintf(stderr, "  seed %d\n", seed);
            ++failures;
        }
        ++checks;
    }
    const auto counts = internal::settlement_path_counts();
    internal::count_settlement_paths(false);
    // The staged transcript sends every call to the chain, so of the fused
    // transcript's calls counts.fused took the pass and the declined rest is
    // half of what the staged count holds beyond it.
    std::uint64_t fused = 0;
    std::uint64_t declined = 0;
    for (int e = 0; e < internal::kSettlementEntries; ++e) {
        fused += counts.fused[e];
        declined += (counts.staged[e] - counts.fused[e]) / 2;
        // Both paths ran often, on every entry.
        CHECK(counts.fused[e] > 500);
        CHECK(counts.staged[e] > counts.fused[e] + 500);
    }
    std::printf("randomized direct books: %d books, %ld fills; the fused transcripts took %llu "
                "calls in one pass and declined %llu to the chain; fused == staged\n", kBooks,
                steps, static_cast<unsigned long long>(fused),
                static_cast<unsigned long long>(declined));
}

// ---------------------------------------------------------------------------
// 3. Randomized runs through the matcher.
// ---------------------------------------------------------------------------

bool same(const l2_fused::Outcome& a, const l2_fused::Outcome& b) {
    const int before = failures;
    CHECK(a.completed == b.completed);
    CHECK(a.error == b.error);
    CHECK(a.trace.size() == b.trace.size());
    const std::size_t common = a.trace.size() < b.trace.size() ? a.trace.size() : b.trace.size();
    for (std::size_t index = 0; index < common; ++index) {
        if (a.trace[index] != b.trace[index]) {
            std::fprintf(stderr, "  first continuation divergence at observation %zu of %zu\n",
                         index, common);
            ++failures;
            break;
        }
    }
    CHECK(a.fills.size() == b.fills.size());
    const std::size_t fills = a.fills.size() < b.fills.size() ? a.fills.size() : b.fills.size();
    for (std::size_t index = 0; index < fills; ++index) {
        if (a.fills[index].digest != b.fills[index].digest
            || a.fills[index].continuation != b.fills[index].continuation
            || a.fills[index].broker != b.fills[index].broker
            || a.fills[index].ordinal != b.fills[index].ordinal) {
            std::fprintf(stderr, "  first fill divergence at fill %zu (ordinal %llu)\n", index,
                         static_cast<unsigned long long>(a.fills[index].ordinal));
            ++failures;
            break;
        }
    }
    CHECK(a.precommits == b.precommits);
    CHECK(a.excursions == b.excursions);
    CHECK(a.continuation == b.continuation);
    CHECK(a.broker == b.broker);
    CHECK(a.stream_hash == b.stream_hash);
    CHECK(a.stream_actions == b.stream_actions);
    CHECK(a.trades == b.trades);
    CHECK(a.trades_digest == b.trades_digest);
    CHECK(a.lots_digest == b.lots_digest);
    CHECK(a.events == b.events);
    CHECK(a.events_digest == b.events_digest);
    CHECK(bits(a.position) == bits(b.position));
    CHECK(bits(a.equity) == bits(b.equity));
    CHECK(a.accepted == b.accepted);
    CHECK(a.rejected == b.rejected);
    CHECK(a.replaced == b.replaced);
    CHECK(a.cancelled == b.cancelled);
    CHECK(a.applied == b.applied);
    CHECK(a.refusals == b.refusals);
    return failures == before;
}

l2_fused::Outcome run_fixture(const l2_fused::Config& config, bool fused) {
    internal::set_fused_settlement(fused);
    l2_fused::FusedHost host(config);
    auto outcome = l2_fused::run(host, config);
    internal::set_fused_settlement(true);
    return outcome;
}

struct Totals {
    long runs = 0;
    long fills = 0;
    long trades = 0;
    long precommits = 0;
    long excursions = 0;
    std::size_t events = 0;
    void add(const l2_fused::Outcome& o) {
        ++runs;
        fills += o.applied;
        trades += o.trades;
        precommits += static_cast<long>(o.precommits.size());
        excursions += static_cast<long>(o.excursions.size());
        events += o.events;
    }
};

void randomized_runs() {
    using l2_fused::Config;
    using l2_fused::Drive;
    using l2_fused::Fx;
    using l2_fused::Path;
    Totals totals;
    internal::count_settlement_paths(true);
    std::uint64_t seed = 1;
    const NativeFeeKind fees[] = {NativeFeeKind::Percent, NativeFeeKind::CashPerUnit,
                                  NativeFeeKind::CashPerExecution};
    const Path paths[] = {Path::None, Path::Synthesized, Path::Lower};
    for (const auto fee : fees) {
        for (const auto path : paths) {
            for (int variant = 0; variant < 8; ++variant) {
                Config config;
                config.seed = seed++;
                config.path = path;
                config.fee_kind = fee;
                config.fee_value = fee == NativeFeeKind::Percent ? 0.05
                    : fee == NativeFeeKind::CashPerUnit ? 0.4 : 1.5;
                config.fx = variant % 3 == 0 ? Fx::Unit : variant % 3 == 1 ? Fx::Constant : Fx::Curve;
                config.margin = variant == 3 || variant == 6;
                config.flatten_liquidations = variant == 6;
                config.owns_excursions = variant % 2 == 1;
                config.calc_on_fills = variant == 2 || variant == 5;
                config.quantize = variant == 4;
                config.drive = variant == 7 ? Drive::Stream : Drive::Batch;
                config.single_lot_percent = variant == 5 ? 50 : 80;
                const auto a = run_fixture(config, true);
                const auto b = run_fixture(config, false);
                if (!same(a, b)) {
                    std::fprintf(stderr, "  config seed=%llu fee=%s path=%s fx=%s margin=%d "
                                 "flatten=%d excursions=%d calc_on_fills=%d quantize=%d stream=%d\n",
                                 static_cast<unsigned long long>(config.seed),
                                 l2_fused::fee_name(fee), l2_fused::path_name(path),
                                 l2_fused::fx_name(config.fx), config.margin ? 1 : 0,
                                 config.flatten_liquidations ? 1 : 0,
                                 config.owns_excursions ? 1 : 0, config.calc_on_fills ? 1 : 0,
                                 config.quantize ? 1 : 0, config.drive == Drive::Stream ? 1 : 0);
                }
                CHECK(a.completed || !a.error.empty());
                totals.add(a);
            }
        }
    }
    const auto counts = internal::settlement_path_counts();
    internal::count_settlement_paths(false);
    // Every run twice, one of them staged: the fused half is what the fused
    // run took.
    const auto entry = [](SettlementEntry e) { return static_cast<int>(e); };
    const std::uint64_t fused_settle = counts.fused[entry(SettlementEntry::Settle)];
    const std::uint64_t staged_settle = counts.staged[entry(SettlementEntry::Settle)];
    // The staged run settles every fill through the chain: what the fused
    // run declined is half of what remains.
    const std::uint64_t fused_run_staged = (staged_settle - fused_settle) / 2;
    CHECK(fused_settle > 0);
    CHECK(fused_run_staged > 0);
    std::printf("randomized runs: %ld configurations, %ld fills, %ld trades, %ld precommit views, "
                "%ld excursion consultations, %zu events; the fused run settled %llu fills in one "
                "pass and %llu through the chain; fused == staged\n",
                totals.runs, totals.fills, totals.trades, totals.precommits, totals.excursions,
                totals.events, static_cast<unsigned long long>(fused_settle),
                static_cast<unsigned long long>(fused_run_staged));
}

// ---------------------------------------------------------------------------
// 4. K3's randomized working books and L5's band-edge books, both ways.
// ---------------------------------------------------------------------------

bool same_book(const k3_book::Outcome& a, const k3_book::Outcome& b) {
    const int before = failures;
    CHECK(a.completed == b.completed);
    CHECK(a.error == b.error);
    CHECK(a.trace == b.trace);
    CHECK(a.continuation == b.continuation);
    CHECK(a.broker == b.broker);
    CHECK(a.trades == b.trades);
    CHECK(a.trades_digest == b.trades_digest);
    CHECK(a.events == b.events);
    CHECK(a.events_digest == b.events_digest);
    CHECK(bits(a.position) == bits(b.position));
    CHECK(a.accepted == b.accepted);
    CHECK(a.rejected == b.rejected);
    CHECK(a.replaced == b.replaced);
    CHECK(a.cancelled == b.cancelled);
    CHECK(a.applied == b.applied);
    return failures == before;
}

void neighbour_books() {
    using k3_book::BookConfig;
    using k3_book::Path;
    int configurations = 0;
    long fills = 0;
    std::uint64_t seed = 101;
    for (const Path path : {Path::None, Path::Synthesized, Path::Lower}) {
        for (const int live : {1, 3, 10}) {
            BookConfig config;
            config.seed = seed++;
            config.live = live;
            config.bars = 90;
            config.path = path;
            config.calc_on_fills = live == 3;
            config.quantize = live == 10;
            internal::set_fused_settlement(true);
            k3_book::BookHost fused_host(config);
            const auto a = k3_book::run_book(fused_host, config);
            internal::set_fused_settlement(false);
            k3_book::BookHost staged_host(config);
            const auto b = k3_book::run_book(staged_host, config);
            internal::set_fused_settlement(true);
            if (!same_book(a, b)) std::fprintf(stderr, "  K3 book seed=%llu live=%d\n",
                                                static_cast<unsigned long long>(config.seed), live);
            fills += a.applied;
            ++configurations;

            const auto tape = k3_book::make_tape(config);
            l5_band::EdgeHost fused_edges(config, tape);
            const auto c = l5_band::run_edges(fused_edges, config, tape);
            internal::set_fused_settlement(false);
            l5_band::EdgeHost staged_edges(config, tape);
            const auto d = l5_band::run_edges(staged_edges, config, tape);
            internal::set_fused_settlement(true);
            if (!same_book(c, d)) std::fprintf(stderr, "  L5 band book seed=%llu live=%d\n",
                                                static_cast<unsigned long long>(config.seed), live);
            fills += c.applied;
            ++configurations;
        }
    }
    std::printf("K3 and L5 books: %d configurations, %ld fills, fused == staged\n",
                configurations, fills);
}

// ---------------------------------------------------------------------------
// 5. The counter witness.
// ---------------------------------------------------------------------------

// PERF0-K's two common-case books: a market entry and flatten every tenth bar,
// and a market entry with a take-profit and a stop-loss its fill arms on the
// owner's lot, one OCA group.
class CommonHost final : public NativeStrategyHost {
public:
    explicit CommonHost(bool bracket) : bracket_(bracket) {}
    long fills = 0;
    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        ++bars_;
        const double position = physical_position().signed_units;
        if (!bracket_) {
            if (bars_ % 5 == 0) {
                no::Request request;
                if (position == 0.0) request.intent = no::Transact{1.0};
                else request.intent = no::Flatten{};
                request.label = "m";
                (void)submit_market(request);
            }
            return;
        }
        if (position != 0.0 || bars_ % 5 != 0) return;
        no::Request entry{no::Transact{bars_ % 2 ? 1.0 : -1.0}, "E", ""};
        const auto parent = submit(entry);
        if (!parent.handle) return;
        no::WaitForApplied wait;
        wait.parent = *parent.handle;
        no::Member member;
        member.group = static_cast<std::uint64_t>(bars_);
        const bool buy = bars_ % 2 == 1;
        no::Request take{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        take.trigger = no::Limit{bar.close * (buy ? 1.004 : 0.996)};
        take.owner = wait;
        take.group = member;
        no::Request stop{no::Reduce{no::ExplicitUnits{1.0}}, "x", ""};
        stop.trigger = no::Stop{bar.close * (buy ? 0.996 : 1.004)};
        stop.owner = wait;
        stop.group = member;
        (void)submit(take);
        (void)submit(stop);
    }
    void on_native_applied(const no::ExecutionAppliedEvent&, const NativeDecisionContext&) override {
        ++fills;
    }

private:
    bool bracket_;
    long bars_ = 0;
};

class PyramidHost final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bars_;
        no::Request request;
        request.label = "p";
        if (bars_ % 6 == 0) request.intent = no::Flatten{};
        else request.intent = no::Transact{1.0};
        (void)submit_market(request);
    }

private:
    long bars_ = 0;
};

template <class Host>
internal::SettlementPathCounts count_run(Host& host, const l2_fused::Config& config) {
    const auto tape = l2_fused::make_tape(config);
    CHECK(host.configure_native(l2_fused::make_spec(config, tape)).status
          == NativeSetupStatus::Applied);
    internal::count_settlement_paths(true);
    host.run(tape.bars.data(), static_cast<int>(tape.bars.size()));
    const auto counts = internal::settlement_path_counts();
    internal::count_settlement_paths(false);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    return counts;
}

void counter_witness() {
    l2_fused::Config config;
    config.bars = 400;
    const auto entry = [](SettlementEntry e) { return static_cast<int>(e); };
    const int inspect = entry(SettlementEntry::Inspect);
    const int preview = entry(SettlementEntry::Preview);
    const int settle = entry(SettlementEntry::Settle);
    for (const bool bracket : {false, true}) {
        CommonHost host(bracket);
        const auto counts = count_run(host, config);
        const auto fused = counts.fused[settle];
        std::printf("counter witness, %s book: %ld fills; inspect %llu fused / %llu staged, "
                    "preview %llu / %llu, settle %llu / %llu\n", bracket ? "bracket" : "market",
                    host.fills, static_cast<unsigned long long>(counts.fused[inspect]),
                    static_cast<unsigned long long>(counts.staged[inspect]),
                    static_cast<unsigned long long>(counts.fused[preview]),
                    static_cast<unsigned long long>(counts.staged[preview]),
                    static_cast<unsigned long long>(fused),
                    static_cast<unsigned long long>(counts.staged[settle]));
        // Every fill is inspected, previewed and settled in one pass.
        CHECK(host.fills > 40);
        CHECK(fused == static_cast<std::uint64_t>(host.fills));
        CHECK(counts.staged[settle] == 0);
        CHECK(counts.fused[preview] == static_cast<std::uint64_t>(host.fills));
        CHECK(counts.staged[preview] == 0);
        CHECK(counts.fused[inspect] >= static_cast<std::uint64_t>(host.fills));
        CHECK(counts.staged[inspect] == 0);
    }
    {
        PyramidHost host;
        const auto counts = count_run(host, config);
        std::printf("counter witness, pyramid book: settle %llu fused / %llu staged\n",
                    static_cast<unsigned long long>(counts.fused[settle]),
                    static_cast<unsigned long long>(counts.staged[settle]));
        CHECK(counts.fused[settle] > 0);   // each opening from flat
        CHECK(counts.staged[settle] > 0);  // each addition and each two-lot flatten
    }
    {
        internal::set_fused_settlement(false);
        CommonHost host(true);
        const auto counts = count_run(host, config);
        internal::set_fused_settlement(true);
        std::uint64_t fused = 0;
        for (int e = 0; e < internal::kSettlementEntries; ++e) fused += counts.fused[e];
        std::printf("counter witness, bracket book with the chain forced: %llu fused, "
                    "settle %llu staged\n", static_cast<unsigned long long>(fused),
                    static_cast<unsigned long long>(counts.staged[settle]));
        CHECK(fused == 0);
        CHECK(counts.staged[settle] == static_cast<std::uint64_t>(host.fills));
    }
}

}  // namespace

int main() {
    admission_table();
    randomized_books();
    randomized_runs();
    neighbour_books();
    counter_witness();
    std::printf("%d checks\n", checks);
    if (failures != 0) {
        std::printf("test_native_fused_settlement: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    std::printf("test_native_fused_settlement: ok\n");
    return 0;
}
