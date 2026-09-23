#pragma once

// R5 lane PERF-L4: the Pine adapter's quiet-bar gates.
//
// Every TradingView policy hook the adapter runs at a bar boundary is first
// asked whether its body has anything to act on -- a live handle of the
// family it handles, a queued command, a held position, an armed rule or
// configuration -- and is not run when it has not. The gate sits at the
// hook's per-bar call site (on_bar_open, on_bar_close, the host's source
// publication), or at the top of a hook that is also reached elsewhere. Each
// precondition is the hook's own logic restated as the condition under which
// its body changes nothing and calls no kernel mutator, so a skipped hook
// leaves the adapter, the kernel and every hash exactly as the body would
// have. A gate never fires on an unbound adapter: the hook then refuses
// through require_host() exactly as it always did.
//
// Two test-only switches build the witness's references
// (tests/test_adapter_quiet_bar.cpp); the shipped library sets neither:
//   PINEFORGE_PINE_QUIET_BAR_GATES=0  compiles every gate out: the ungated
//                                     reference the differential row runs;
//   PINEFORGE_PINE_QUIET_BAR_PROBE=1  counts every gate decision per hook:
//                                     the cost and coverage rows.
// Not part of the installed API.

#include <cstddef>
#include <cstdint>

#ifndef PINEFORGE_PINE_QUIET_BAR_GATES
#define PINEFORGE_PINE_QUIET_BAR_GATES 1
#endif
#ifndef PINEFORGE_PINE_QUIET_BAR_PROBE
#define PINEFORGE_PINE_QUIET_BAR_PROBE 0
#endif

namespace pineforge::source::detail {

// One entry per gated hook, in the order a bar reaches them.
enum class QuietHook : std::uint8_t {
    ReceiptRead,             // observe_terminal_receipts: no command above the cursor
    DelayedRelease,          // release_delayed_orders at the open: nothing delayed
    L4cPriority,             // update_l4c_priority at the open: no pair to rank
    OpenMarketAdmission,     // apply_open_market_admission: no entry of the prior bar
    OpenMarketableSells,     // defer_open_marketable_sells: no marketable stop-entry pair
    GappedBracketReaccept,   // reaccept_gapped_bracket_behind_same_id_add: no market add
    DualEntryPath,           // on_bar_open's flat two-stop arbitration: no touched pair
    CoofTail,                // flush_coof_tail at the open: nothing staged
    CoofDeclinedReversal,    // suspend_coof_declined_reversal_at_open: no reversal entry
    DueCapClose,             // execute_due_cap_close: no filled-orders cap close due
    SlippedPoocMoney,        // submit_slipped_pooc_opening_money_call: out of scope
    TvMoneyLongMargin,       // submit_tv_money_long_margin_call: out of scope
    TvMoneyTrailMargin,      // schedule_tv_money_long_margin_before_trail: out of scope
    OpenMarginCheckpoints,   // on_bar_open's open/path margin block: flat book
    DeclinedReversalAtOpen,  // declined_reversal_at_open: no reversal entry
    RoundedPoocShortMargin,  // defer_rounded_pooc_short_margin_until_close: out of scope
    IntradayLossClose,       // submit_intraday_loss_close at the open: no loss rule
    IntradayLossPath,        // schedule_intraday_loss_path at the open: no loss rule
    PreopenMarginSlice,      // schedule_preopen_margin_slice: no default-percent opening
    BracketLegs,             // flush_pending_bracket_legs at publication: nothing staged
    PendingCloses,           // flush_pending_closes at publication: no close batch
    PendingEntries,          // flush_pending_entries at publication: nothing queued
    RelativeExits,           // anchor_relative_exits at publication: none queued
    PoocLimitEntryFills,     // flush_pooc_marketable_limit_entry_fills: no entry of this bar
    PoocExitFills,           // flush_pooc_marketable_exit_fills: no exit leg of this bar
    DeferredSellAdmission,   // admit_deferred_open_marketable_sells: none deferred
    ThrottledReopens,        // rearm_throttled_reopens at the close: none throttled
    TerminalExplicitMarket,  // apply_terminal_explicit_market_policy: no entry pair
    CloseMarginCheckpoints,  // on_bar_close's short checkpoints: out of scope
    Count,
};

inline constexpr std::size_t kQuietHookCount = static_cast<std::size_t>(QuietHook::Count);

#if PINEFORGE_PINE_QUIET_BAR_PROBE
// Per hook: how often its gate was asked, and how often the answer was quiet.
struct QuietBarCounts {
    std::uint64_t asked[kQuietHookCount] = {};
    std::uint64_t quiet[kQuietHookCount] = {};
};
QuietBarCounts& quiet_bar_counts() noexcept;
#endif

// True when the hook is skipped: `quiet` is the hook's exact no-op
// precondition, evaluated where the hook would run.
inline bool skip_quiet(QuietHook hook, bool quiet) noexcept {
#if PINEFORGE_PINE_QUIET_BAR_PROBE
    auto& counts = quiet_bar_counts();
    ++counts.asked[static_cast<std::size_t>(hook)];
    if (quiet) ++counts.quiet[static_cast<std::size_t>(hook)];
#else
    (void)hook;
#endif
    return PINEFORGE_PINE_QUIET_BAR_GATES != 0 && quiet;
}

}  // namespace pineforge::source::detail
