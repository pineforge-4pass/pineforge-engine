// A strategy.entry reversal sends a cancel only for the exit orders that are
// still working, whichever side it opens, and takes every one of them off the
// adapter's own book.
//
// When a reversal's fill is applied, the adapter takes the exit orders of the
// side that fill closed off its book in purge_brackets_after_applied_reversal
// (src/source/pine_adapter.cpp). Some of those orders are no longer working
// when it runs. Under the default FIFO close rule a strategy.exit bracket of
// a held lot is a close of the whole position. The kernel binds such a close
// to the position it is first evaluated against and ends it with that
// position, inside the reversal's own settlement, before the adapter is told
// of the fill (WorkingRequestCore::apply_bound_expiry, src/native_order.cpp).
// The adapter used to send a cancel for such an order all the same. A cancel
// of a request that is not working changes nothing on the book, but the
// kernel answers it NotWorking and records the answer: one NotWorkingEvent
// for each order, and each takes one ordinal of the run's journal
// (WorkingRequestCore::prepare_cancel). The adapter reads its receipts up to
// the journal's last ordinal and folds that cursor into the broker-state hash
// (src/source/pine_state_hash.cpp), so the hash of the run moved where no
// order, fill or trade did.
// The adapter now sends the cancel only for an order the kernel still lists
// as working, and drops every such order from its own book as before. This
// test holds a reversal to both halves of that rule, in both directions: what
// it writes to the journal, and what it leaves on the adapter's own book.
//
// One script, driven only through the public host commands:
//   Bar 0: strategy.entry "X" on the side the reversal will close.
//   Bar 2: strategy.exit "XB", from_entry "X": a profit limit and a stop.
//   Bar 4: strategy.entry "X" on the other side: the reversal.
// It runs under two close rules, which decide who ends the bracket:
//   kernel-ended    the default FIFO rule. The bracket is a close of the whole
//                   position. It is evaluated against the first lot's
//                   position on bars 3 and 4, so it is bound to it, and the
//                   kernel ends both legs when the reversal closes that
//                   position. The adapter has nothing left to cancel.
//   still-working   close_entries_rule = "ANY". The bracket is bound to the
//                   lots of its entry id (PineExecutionAdapter::
//                   owner_for_close), not to the position, and the reversal's
//                   new lot carries the same id. The kernel does not end it,
//                   so both legs still work when the fill is applied, and the
//                   adapter's cancel is what ends them. This is the control:
//                   it gave these results before the rule changed and must
//                   keep them.
// Each runs "into-long" and "into-short". The second is the sign image of the
// first: every side is flipped and every price is reflected about 100. The
// bars reflect onto themselves.
//
// What the test reads.
//   The journal, through the public event reader
//   NativeStrategyHost::native_events (include/pineforge/native_host.hpp:1371),
//   whose rows are NativeMarketEvent (the same file, :390). The host is asked
//   before the run to keep the whole journal readable after it, with
//   PineStrategyHost::fixture_retain_all_events
//   (include/pineforge/source/pine_strategy_host.hpp:742). The event types
//   are those of include/pineforge/native_order.hpp: ExecutionAppliedEvent,
//   CancelledEvent with its CancelReason, and NotWorkingEvent. The adapter's
//   receipt cursor has no public reader, so the test reads the journal the
//   cursor follows.
//   The adapter's own book, through the host's pending-order projection:
//   PineStrategyHost::pending_intent_view
//   (include/pineforge/source/pine_strategy_host.hpp:563) and its size and
//   copy_v1 (include/pineforge/source/pine_adapter.hpp:1684, :1690). The
//   projection is built from the adapter's list of live orders
//   (PineExecutionAdapter::projected_raw_pending_at), not from the kernel's
//   working requests. The two differ for an order the kernel has ended and
//   the adapter has not dropped: the adapter learns of such an end only when
//   it next reads its receipts, at a bar's open or before a calculation. So
//   the projection is read where that difference can still be seen: when the
//   reversal's own fill notification ends, in the host's applied-event
//   observer, PineStrategyHost::on_native_applied_event (the same header,
//   :752), which runs after the adapter has settled the fill.
//   The closed rows and the position, as a strategy reads them.
//
// Hand derivation. `s` is +1 in "into-long" and -1 in "into-short". Bars: O
// 100, H 102, L 98, C 100. A market order placed on bar i fills at the open
// of bar i + 1.
//   Bar 1: the first entry fills: position -s * 10.
//   Bar 2: the bracket is placed on that lot. Its limit is s * 3 below 100
//   and its stop s * 3 above, at 97 and 103: outside every bar, so neither
//   leg is reached on any bar, in either scenario.
//   Bar 4: the bracket has rested since bar 2, so the adapter's book holds it
//   when this bar is calculated.
//   Bar 5: the opposite strategy.entry reverses (docs/pages/pine-to-native.md,
//   the strategy.entry row): it trades 10 + 30 = 40, closes the first lot
//   whole and leaves s * 30. That is the run's only fill that closes units,
//   and its only closed row.
//   The bracket ends at that fill and never fills: no fill carries its id,
//   the position stays s * 30 to the last bar, and the closed rows stay 1.
//   Each of its two legs ends exactly once, so two CancelledEvents carry its
//   id. In "kernel-ended" the kernel ended both with the position: reason
//   OwnerGone. In "still-working" the adapter's cancel ended both: reason
//   User. No leg ends for any other reason.
//   No command of the run addresses a request that is not working: the script
//   cancels nothing, and the reversal cancels only what still works. So the
//   journal holds no NotWorkingEvent.
//   After the reversal's fill nothing rests and the script calls nothing, so
//   the journal's command events above that fill are the two ends of the
//   bracket and nothing else: 2.
//   The reversal takes the bracket off the adapter's book whoever ended it:
//   one notification is for a fill that closed units, and when it ends the
//   adapter's book holds no order with the bracket's id, in every run.
//
// How the old cancel shows, in the "kernel-ended" runs of both directions: two
// NotWorkingEvents, one for each leg the kernel had already ended, and four
// command events above the reversal's fill where two are expected. The
// "still-working" runs do not show it.
//
// How a reversal that drops only the orders it cancels shows, in the
// "kernel-ended" runs of both directions: the journal is as expected, but the
// adapter's book still holds the bracket when the reversal's fill
// notification ends. The "still-working" runs do not show it.
//
// tests/test_reversal_purge_direction.cpp holds the book, the position and the
// closed rows of the same two scenarios bar by bar; this test holds what the
// reversal writes to the journal and what it leaves on the adapter's book
// inside its own fill notification.
#include <pineforge/bar.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kBars = 10;
constexpr std::int64_t kStart = 1704067200000LL;  // 2024-01-01 00:00 UTC
constexpr std::int64_t kMinute = 60000LL;
constexpr double kMid = 100.0;    // every bar opens and closes here
constexpr double kRange = 2.0;    // bars: O 100, H 102, L 98, C 100
constexpr double kFirstQty = 10.0;     // bar 0: the lot the bracket is placed on
constexpr double kReversalQty = 30.0;  // bar 4: the reversal's own lot
// The bracket's limit and stop sit 3 away from 100, at 97 and 103: outside
// every bar, so neither is reached.
constexpr double kBracketDistance = 3.0;
const char* const kEntryId = "X";
const char* const kBracketId = "XB";

struct Checks {
    std::string context;
    int failures = 0;

    void require(bool value, const std::string& label) {
        if (!value) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s\n", context.c_str(), label.c_str());
        }
    }
    void units(double actual, double expected, const std::string& label) {
        if (actual != expected) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s expected %.17g, got %.17g\n",
                         context.c_str(), label.c_str(), expected, actual);
        }
    }
    void count(long long actual, long long expected, const std::string& label) {
        if (actual != expected) {
            ++failures;
            std::fprintf(stderr, "FAIL %s: %s expected %lld, got %lld\n",
                         context.c_str(), label.c_str(), expected, actual);
        }
    }
};

// Who ends the bracket's legs, by the close rule.
enum class Legs { KernelEnded, StillWorking };

const char* name_of(Legs legs) {
    switch (legs) {
    case Legs::KernelEnded: return "kernel-ended";
    case Legs::StillWorking: return "still-working";
    }
    return "unknown";
}

class ReversalHost final : public source::PineStrategyHost {
public:
    ReversalHost(bool into_long, Legs legs) : into_long(into_long) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.commission_value = 0.0;
        config.slippage = 0;
        config.close_entries_rule_any = legs == Legs::StillWorking;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        config.pyramiding = 1;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        set_syminfo_metadata("qty_step", 1.0);
        // The checks read the run's whole journal after the run has ended.
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        const int i = pine_bar_index();
        if (i == last_bar_) return;
        last_bar_ = i;
        last_position = live_position_size();
        // Read before this bar's own commands: the bracket of bar 2 rests.
        if (i == 4) bracket_rested = bracket_on_adapter_book();
        const double s = into_long ? 1.0 : -1.0;
        if (i == 0) strategy_entry(kEntryId, !into_long, kNaN, kNaN, kFirstQty);
        // The first lot is on the side the reversal closes: its profit limit
        // is s * 3 below 100 and its protective stop s * 3 above.
        if (i == 2)
            strategy_exit(kBracketId, kEntryId, kMid - s * kBracketDistance,
                          kMid + s * kBracketDistance);
        if (i == 4) strategy_entry(kEntryId, into_long, kNaN, kNaN, kReversalQty);
    }

    int bars_calculated() const { return last_bar_ + 1; }

    const bool into_long;
    double last_position = 0.0;  // the position the last bar's calculation found
    bool bracket_rested = false;  // the adapter's book held the bracket at bar 4
    int closing_notifications = 0;  // notifications of a fill that closed units
    bool bracket_left_on_book = false;  // the adapter's book at the end of the last one

protected:
    // Runs once for each fill, when its notification ends: the adapter has
    // settled the fill, and has not read a receipt since.
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (!(event.closed_units > 0.0)) return;
        ++closing_notifications;
        bracket_left_on_book = bracket_on_adapter_book();
    }

private:
    // Whether the host's pending-order projection holds an order with the
    // bracket's id: the adapter's own book, whatever the kernel still lists.
    bool bracket_on_adapter_book() const {
        const auto& view = pending_intent_view();
        for (int i = 0; i < view.size(); ++i) {
            pf_pending_order_v1_t row{};
            if (view.copy_v1(i, &row) == 0 && std::string(row.id) == kBracketId) return true;
        }
        return false;
    }

    int last_bar_ = -1;
};

// What the run wrote to the kernel's journal.
struct Journal {
    std::size_t closing_fills = 0;       // fills that closed units
    std::uint64_t reversal_ordinal = 0;  // the first of them: the reversal
    std::size_t bracket_fills = 0;       // fills that carry the bracket's id
    std::size_t ended_by_kernel = 0;     // the bracket's legs, by how they ended
    std::size_t ended_by_command = 0;
    std::size_t ended_otherwise = 0;
    std::size_t not_working = 0;         // cancels of a request that was not working
    std::string not_working_at;          // their ordinals, for the FAIL line
    std::size_t after_reversal = 0;      // command events above the reversal's fill
};

Journal read_journal(const ReversalHost& host) {
    Journal journal;
    const auto events = host.native_events(0);
    for (const auto& row : events) {
        if (!row.command) continue;
        if (const auto* fill =
                std::get_if<native_order::ExecutionAppliedEvent>(&*row.command)) {
            if (fill->request().label == kBracketId) ++journal.bracket_fills;
            if (fill->closed_units > 0.0) {
                if (journal.closing_fills == 0) journal.reversal_ordinal = row.ordinal;
                ++journal.closing_fills;
            }
        } else if (const auto* ended =
                       std::get_if<native_order::CancelledEvent>(&*row.command)) {
            if (ended->request().label != kBracketId) continue;
            if (ended->reason == native_order::CancelReason::OwnerGone)
                ++journal.ended_by_kernel;
            else if (ended->reason == native_order::CancelReason::User)
                ++journal.ended_by_command;
            else
                ++journal.ended_otherwise;
        } else if (std::holds_alternative<native_order::NotWorkingEvent>(*row.command)) {
            ++journal.not_working;
            if (!journal.not_working_at.empty()) journal.not_working_at += ", ";
            journal.not_working_at += std::to_string(row.ordinal);
        }
    }
    if (journal.reversal_ordinal != 0) {
        for (const auto& row : events)
            if (row.command && row.ordinal > journal.reversal_ordinal) ++journal.after_reversal;
    }
    return journal;
}

void verify(const ReversalHost& host, Legs legs, Checks& checks) {
    const double s = host.into_long ? 1.0 : -1.0;
    checks.require(host.last_error().empty(), "run error: " + host.last_error());
    checks.count(host.bars_calculated(), kBars, "bars calculated");

    // The reversal trades 10 + 30 against -s * 10: one closed row, s * 30
    // left. A bracket that filled would add a row and take the new lot.
    checks.count(static_cast<long long>(host.closed_trade_count()), 1,
                 "closed rows after the run");
    checks.units(host.last_position, s * kReversalQty, "position at the last bar");

    const Journal journal = read_journal(host);
    checks.count(static_cast<long long>(journal.closing_fills), 1, "fills that closed units");
    checks.count(static_cast<long long>(journal.bracket_fills), 0, "fills of the bracket");

    // Each of the bracket's two legs ends once: by the kernel, with the
    // position it was bound to, or by the adapter's cancel where it still
    // worked.
    const bool kernel_ended = legs == Legs::KernelEnded;
    checks.count(static_cast<long long>(journal.ended_by_kernel), kernel_ended ? 2 : 0,
                 "bracket legs the kernel ended with the position");
    checks.count(static_cast<long long>(journal.ended_by_command), kernel_ended ? 0 : 2,
                 "bracket legs a cancel command ended");
    checks.count(static_cast<long long>(journal.ended_otherwise), 0,
                 "bracket legs ended for another reason");

    // No command of the run addresses a request that is not working.
    checks.count(static_cast<long long>(journal.not_working), 0,
                 "not-working events (the reversal's fill has ordinal "
                     + std::to_string(journal.reversal_ordinal) + ", they have "
                     + (journal.not_working_at.empty() ? std::string("none")
                                                       : journal.not_working_at)
                     + ")");
    // The journal above the reversal's fill holds the two ends of the bracket
    // and nothing else.
    checks.count(static_cast<long long>(journal.after_reversal), 2,
                 "command events after the reversal's fill");

    // The adapter's own book. The bracket rests when bar 4 is calculated, and
    // the reversal takes it off: when that fill's notification ends, no
    // order with the bracket's id is left there, whoever ended its legs.
    checks.require(host.bracket_rested, "the bracket is on the adapter's book at bar 4");
    checks.count(host.closing_notifications, 1, "notifications of a fill that closed units");
    checks.require(!host.bracket_left_on_book,
                   "the bracket is off the adapter's book when the reversal's fill"
                   " notification ends");
}

std::vector<Bar> bars_of(double range) {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i)
        bars.push_back({kMid, kMid + range, kMid - range, kMid, 1000.0, kStart + i * kMinute});
    return bars;
}
} // namespace

int main() {
    Checks checks;
    for (const Legs legs : {Legs::KernelEnded, Legs::StillWorking}) {
        for (const bool into_long : {true, false}) {
            checks.context = std::string(name_of(legs))
                + (into_long ? "/into-long" : "/into-short");
            try {
                const std::vector<Bar> bars = bars_of(kRange);
                ReversalHost host(into_long, legs);
                host.run(bars.data(), static_cast<int>(bars.size()));
                verify(host, legs, checks);
            } catch (const std::exception& error) {
                checks.require(false, std::string("exception: ") + error.what());
            }
        }
    }
    if (checks.failures == 0) std::puts("reversal purge working only: OK");
    return checks.failures == 0 ? 0 : 1;
}
