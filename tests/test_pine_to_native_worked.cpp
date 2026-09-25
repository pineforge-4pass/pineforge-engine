// The worked migration of docs/pages/pine-to-native.md, executed.
//
// The page ports one Pine strategy in six C++ blocks and says everything the
// port needs is on the page. tests/extract_pine_to_native_worked.py copies
// those blocks out of the page verbatim at build time (steps 1-2 into
// pine_to_native_worked_spec.inc, steps 3 and 6 -- with 4 and 5 in step 6's
// placeholder -- into pine_to_native_worked_members.inc), and this file
// compiles them against PineForge::kernel and runs them on a 16-bar tape.
//
// What is NOT from the page is only the glue the page does not print: the
// class skeleton around the member blocks, the function around the spec
// blocks, the tape, and the checks below. The checks are the page's own
// claims, with their expected values computed from the page's Pine block:
//
//   * configure_native answers Applied for the spec steps 1-2 build;
//   * no request is refused, at submission or at a match;
//   * the entry is Sized by cash with the fee reserved, so it buys
//     default_qty_value / (price * (1 + commission_value / 100)) units;
//   * three tapes close by take-profit, trail and stop respectively; the
//     take-profit's siblings are withdrawn as CancelReason::OwnerGone;
//   * armed levels use the Pine block's profit, loss, trail_points and
//     trail_offset values as tick counts on the declared price grid;
//   * the row's commission is commission_value percent of both notionals.
//
// The binary re-reads the page and refuses to run when the page changed
// after it was built (the stale-test-binary trap: a page edit without a
// rebuild would otherwise test the old page).

#include <pineforge/native_host.hpp>

#include "pine_to_native_worked_page.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace {

class WorkedMigration final : public pineforge::NativeStrategyHost {
public:
#include "pine_to_native_worked_members.inc"
};

pineforge::NativeRunSpec page_spec() {
#include "pine_to_native_worked_spec.inc"
    // The row reads the run's whole event record once it has ended; the
    // page's host needs no readback and keeps the default Window (V19-B).
    spec.event_retention = pineforge::NativeEventRetention::Full;
    return spec;
}

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

bool near(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::fmax(1.0, std::fmax(std::fabs(a), std::fabs(b)));
}

bool page_is_current() {
    std::ifstream in(PF_P2N_PAGE_PATH, std::ios::binary);
    if (!in) return false;
    const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    std::uint64_t digest = 0xcbf29ce484222325ULL;   // FNV-1a 64, as the extractor computes it
    for (const char c : bytes) {
        digest ^= static_cast<unsigned char>(c);
        digest *= 0x100000001b3ULL;
    }
    return bytes.size() == PF_P2N_PAGE_BYTES && digest == PF_P2N_PAGE_FNV1A64;
}

// Hour 1 flat at 100, so the first completed hourly close is 100. Bar 4 is the
// first to close above it, at 101, and every later bar opens at the previous
// close, so the entry decided at bar 4's close fills at bar 5's open, 101 --
// the price it was sized at. Then a steady climb with quarter-tick wicks: the
// trail arms but never catches up, the stop-loss is never reached, and the
// take-profit is.
constexpr double kSignalClose = 101.0;

std::vector<pineforge::Bar> tape() {
    std::vector<pineforge::Bar> bars;
    const std::int64_t t0 = 1704067200000LL;   // 2024-01-01 00:00 UTC
    const std::int64_t step = 15LL * 60 * 1000;
    for (int i = 0; i < 4; ++i) bars.push_back({100.0, 100.0, 100.0, 100.0, 1000.0, t0 + i * step});
    double close = 100.0;
    for (int i = 4; i < 16; ++i) {
        const double open = close;
        close = open + 1.0;
        bars.push_back({open, close + 0.25, open - 0.25, close, 1000.0, t0 + i * step});
    }
    return bars;
}

std::vector<pineforge::Bar> variant_tape(bool trailing) {
    std::vector<pineforge::Bar> bars = tape();
    bars.resize(5);  // Keep the same first hourly close and entry signal.
    const auto add = [&bars](double open, double high, double low, double close) {
        bars.push_back({open, high, low, close, 1000.0,
                        bars.back().timestamp + 15LL * 60 * 1000});
    };
    if (trailing) {
        add(101.0, 105.0, 100.75, 105.0);
        add(105.0, 109.5, 105.0, 109.5);
        add(109.5, 110.5, 109.5, 110.5);
        add(110.5, 110.5, 107.0, 107.0);
        add(107.0, 107.0, 107.0, 107.0);
    } else {
        add(101.0, 101.5, 95.5, 96.0);
        add(96.0, 96.0, 96.0, 96.0);
    }
    return bars;
}

void check_variant(const pineforge::NativeRunSpec& spec, bool trailing) {
    WorkedMigration host;
    const auto setup = host.configure_native(spec);
    check(setup.status == pineforge::NativeSetupStatus::Applied,
          "variant configures the page's extracted spec");
    if (setup.status != pineforge::NativeSetupStatus::Applied) return;
    const auto bars = variant_tape(trailing);
    host.run(bars.data(), static_cast<int>(bars.size()));
    check(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
          "variant run completes");

    std::optional<double> take_profit, stop_loss, trail_arm, trail_offset;
    int rejected = 0;
    namespace no = pineforge::native_order;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        rejected += std::holds_alternative<no::RejectedEvent>(*event.command)
                 || std::holds_alternative<no::MatchRejectedEvent>(*event.command);
        if (const auto* armed = std::get_if<no::ArmedEvent>(&*event.command)) {
            if (!armed->definition) continue;
            const auto& trigger = armed->definition->request.trigger;
            if (const auto* limit = std::get_if<no::Limit>(&trigger))
                take_profit = limit->price;
            if (const auto* stop = std::get_if<no::Stop>(&trigger))
                stop_loss = stop->price;
            if (const auto* trail = std::get_if<no::Trail>(&trigger)) {
                trail_arm = trail->arm_price;
                trail_offset = trail->offset;
            }
        }
    }
    const double fill = kSignalClose;
    check(rejected == 0, "variant has no refused request");
    check(take_profit && near(*take_profit,
          fill + PF_P2N_PINE_PROFIT_TICKS * spec.price_tick),
          "take-profit arms at fill plus Pine profit ticks");
    check(stop_loss && near(*stop_loss,
          fill - PF_P2N_PINE_LOSS_TICKS * spec.price_tick),
          "stop arms at fill minus Pine loss ticks");
    check(trail_arm && near(*trail_arm,
          fill + PF_P2N_PINE_TRAIL_POINTS_TICKS * spec.price_tick),
          "trail arms at fill plus Pine trail_points ticks");
    check(trail_offset && near(*trail_offset,
          PF_P2N_PINE_TRAIL_OFFSET_TICKS * spec.price_tick),
          "trail rides Pine trail_offset ticks");
    check(host.trade_count() == 1, "variant closes exactly one trade");
    if (host.trade_count() != 1) return;
    const auto& trade = host.get_trade(0);
    const double expected_exit = trailing
        ? 110.5 - PF_P2N_PINE_TRAIL_OFFSET_TICKS * spec.price_tick
        : fill - PF_P2N_PINE_LOSS_TICKS * spec.price_tick;
    std::printf("%s tape: entry=%.4f exit=%.4f exit_id=%s; armed tp=%.4f sl=%.4f trail=%.4f offset=%.4f\n",
                trailing ? "trail" : "stop", trade.entry_price, trade.exit_price,
                trade.exit_id.c_str(), take_profit.value_or(-1), stop_loss.value_or(-1),
                trail_arm.value_or(-1), trail_offset.value_or(-1));
    check(trade.exit_id == (trailing ? "trail" : "sl")
          && near(trade.exit_price, expected_exit),
          trailing ? "trail closes at running best minus Pine offset ticks"
                   : "stop closes at fill minus Pine loss ticks");
}

}  // namespace

int main() {
    if (!page_is_current()) {
        std::printf("FAIL %s changed after this binary was built: rebuild the "
                    "test_pine_to_native_worked target, then re-run the row\n", PF_P2N_PAGE_PATH);
        return 1;
    }

    const pineforge::NativeRunSpec spec = page_spec();
    WorkedMigration host;
    const pineforge::NativeSetupResult setup = host.configure_native(spec);
    std::printf("configure_native: %s (error %d, field %d)\n",
                setup.status == pineforge::NativeSetupStatus::Applied ? "Applied" : "Failed",
                static_cast<int>(setup.validation.error), static_cast<int>(setup.validation.field));
    check(setup.status == pineforge::NativeSetupStatus::Applied,
          "configure_native applies the spec steps 1 and 2 build");
    if (setup.status != pineforge::NativeSetupStatus::Applied) {
        std::printf("pine-to-native worked migration: %d check(s) failed\n", failures);
        return 1;
    }

    const std::vector<pineforge::Bar> bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    check(host.native_state().kind == pineforge::NativeLifecycleKind::Completed,
          "the run completes");

    int accepted = 0, rejected = 0, sibling_cancels = 0;
    for (const pineforge::NativeMarketEvent& row : host.native_events(0)) {
        if (!row.command) continue;
        namespace no = pineforge::native_order;
        if (std::holds_alternative<no::AcceptedEvent>(*row.command)) ++accepted;
        if (const auto* c = std::get_if<no::CancelledEvent>(&*row.command)) {
            const std::string& label = c->request().label;
            if (c->reason == no::CancelReason::OwnerGone && (label == "sl" || label == "trail"))
                ++sibling_cancels;
        }
        if (const auto* r = std::get_if<no::RejectedEvent>(&*row.command)) {
            ++rejected;
            std::printf("  refused at submission: %s reason %d\n", r->request.label.c_str(),
                        static_cast<int>(r->reason));
        }
        if (const auto* r = std::get_if<no::MatchRejectedEvent>(&*row.command)) {
            ++rejected;
            std::printf("  refused at a match: %s reason %d\n", r->request().label.c_str(),
                        static_cast<int>(r->reason));
        }
        if (const auto* r = std::get_if<no::ReplaceRejectedEvent>(&*row.command)) {
            ++rejected;
            std::printf("  replace refused: reason %d\n", static_cast<int>(r->reason));
        }
    }
    std::printf("requests accepted=%d rejected=%d owner-gone cancels of sl/trail=%d\n", accepted,
                rejected, sibling_cancels);
    check(accepted >= 4, "the entry and its three legs are accepted");
    check(rejected == 0, "no request is refused");
    check(sibling_cancels == 2, "the take-profit's two siblings are withdrawn (OwnerGone)");

    const double fee = PF_P2N_PINE_COMMISSION_PERCENT / 100.0;
    check(host.trade_count() >= 1, "the entry closes");
    if (host.trade_count() >= 1) {
        const pineforge::Trade& t = host.get_trade(0);
        const double units = PF_P2N_PINE_CASH / (kSignalClose * (1.0 + fee));
        const double take_profit = kSignalClose + PF_P2N_PINE_PROFIT_TICKS * spec.price_tick;
        const double commission = fee * t.qty * (t.entry_price + t.exit_price) *
                                  spec.point_value * spec.account_fx;
        std::printf("trade 0: %s qty=%.6f entry=%.4f exit=%.4f commission=%.6f exit_id=%s\n",
                    t.is_long ? "long" : "short", t.qty, t.entry_price, t.exit_price,
                    t.commission, t.exit_id.c_str());
        std::printf("expected: qty=%.6f = %g / (%g * (1 + %g %%)), exit=%.4f, commission=%.6f\n",
                    units, PF_P2N_PINE_CASH, kSignalClose, PF_P2N_PINE_COMMISSION_PERCENT,
                    take_profit, commission);
        check(t.is_long, "the entry is long");
        check(near(t.entry_price, kSignalClose), "the entry fills at the price it was sized at");
        check(near(t.qty, units),
              "qty == default_qty_value / (price * (1 + commission_value / 100))");
        check(t.exit_id == "tp", "the take-profit leg closes it (exit_id tp)");
        check(near(t.exit_price, take_profit), "at the entry plus `profit` ticks");
        check(near(t.commission, commission),
              "commission == commission_value percent of both notionals");
    }

    check_variant(spec, true);
    check_variant(spec, false);

    std::printf("pine-to-native worked migration: %s\n",
                failures == 0 ? "every check passed" : "checks failed");
    return failures == 0 ? 0 : 1;
}
