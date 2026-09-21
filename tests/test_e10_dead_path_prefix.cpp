// R5 lane E10 item 2: the settlement context carries no path prefix.
//
// `execution::PhysicalExecutionContext::preceding_exit_path_prefix` was read
// in one place — the non-hook excursion fold of
// BacktestEngine::build_close_trade_with_costs — and written NOWHERE in the
// tree: not in src/, tests/, examples/, runner/, tools/, benchmarks/,
// tutorial/ or the source adapter. A `std::optional<bool>` nobody assigns is
// empty at every call, so the branch behind it never ran in any run this
// engine has ever performed. Lane E6 measured that and recorded it; this lane
// deletes the member and its reader.
//
// The witnesses, source-free so this TU also runs in the kernel-only profile:
//
//   1. the type witness. No translation unit can set a path prefix, because
//      the member does not exist. This is the whole proof of unreachability:
//      a writer anywhere would have failed to compile. (Fail-before: against
//      the header closure at 8a5273a9 the static_assert below fires.) The
//      surviving sibling is asserted to be unchanged, so the deletion cannot
//      have taken the wrong field.
//
//   2. the behaviour witness. The branch existed to fold a bar-path extreme
//      the modeled path reaches BEFORE a priced exit's mid-bar fill, on the
//      theory that per-bar sampling never sees it. It does see it: on the
//      high-first bar below (|H-O| < |O-L|, high 8.00 above the lot), the
//      closed row of a stop exit filling at 98.00 reports favorable 16.00 on
//      two contracts — the bar's high — whether the lot was opened on an
//      earlier bar or on the exit's own bar. So the fold covered nothing that
//      the live per-trade extremes do not already cover, which is the reason
//      it could sit unwritten. These two rows are the same before and after
//      the deletion; the whole-corpus byte-identity run is the wider proof.
//
//   3. the sibling witness. `preceding_exit_trail_peak`, the field beside the
//      deleted one, HAS writers (engine_execution.cpp, and the consumer's
//      mark-to-market producer) and its fold is live. A trail exit still
//      reports its peak-derived favorable excursion; deleting its neighbour
//      leaves the carry and its branch exactly as they were.
#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

namespace {

int checks = 0;
int failures = 0;

#define CHECK(expr) do {                                                      \
    ++checks;                                                                 \
    if (!(expr)) {                                                            \
        ++failures;                                                           \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);            \
    }                                                                         \
} while (false)

// Witness 1 -- the type witness.
template <class T, class = void>
struct has_path_prefix : std::false_type {};
template <class T>
struct has_path_prefix<
    T, std::void_t<decltype(std::declval<T&>().preceding_exit_path_prefix)>>
    : std::true_type {};

static_assert(!has_path_prefix<ex::PhysicalExecutionContext>::value,
              "PhysicalExecutionContext still carries the dead "
              "preceding_exit_path_prefix member");
static_assert(
    std::is_same<decltype(ex::PhysicalExecutionContext{}.preceding_exit_trail_peak),
                 std::optional<double>>::value,
    "the surviving preceding_exit_trail_peak carry must be untouched");

constexpr std::int64_t T0 = 1736121600000LL;
constexpr double kQty = 2.0;

bool near(double a, double b, double tol = 1e-9) { return std::abs(a - b) <= tol; }

Bar mk(std::int64_t t, double o, double h, double l, double c) {
    return {o, h, l, c, 1.0, t};
}

NativeRunSpec spec(const char* key, NativeCloseExecution close_execution) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.input_tf = "1";
    s.script_tf = "1";
    s.tickerid = "TEST:E10";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.close_execution = close_execution;
    return s;
}

// Enter long at the close of bar 1 and rest one priced exit for bar 2.
struct PricedExitHost final : NativeStrategyHost {
    no::Trigger exit_trigger = no::Market{};
    int bars = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int index = bars++;
        if (index == 1) {
            (void)submit({no::Transact{kQty}, "enter", ""});
            (void)submit({no::Flatten{}, "exit", "", exit_trigger});
        }
    }
};

// bar 1 is flat, so the lot carries no excursion into bar 2; bar 2 is
// high-first with its high 8.00 above the lot and its low 10.00 below.
std::vector<Bar> tape() {
    return {
        mk(T0, 100.0, 100.0, 100.0, 100.0),
        mk(T0 + 60000, 100.0, 100.0, 100.0, 100.0),
        mk(T0 + 120000, 100.0, 108.0, 90.0, 95.0),
        mk(T0 + 180000, 95.0, 95.0, 95.0, 95.0),
    };
}

void run_case(const char* tag, no::Trigger trigger,
              NativeCloseExecution close_execution, int want_entry_bar,
              double want_exit, double want_favorable, double want_adverse) {
    PricedExitHost host;
    host.exit_trigger = trigger;
    CHECK(host.configure_native(spec(tag, close_execution)).status
          == NativeSetupStatus::Applied);
    host.run(tape().data(), 4);
    CHECK(host.last_error().empty());
    CHECK(host.trade_count() == 1);
    if (host.trade_count() != 1) return;
    const Trade& t = host.get_trade(0);
    std::printf("%-18s L @%.4f->%.4f entry_bar=%d mfe=%.6f mae=%.6f"
                " (want ->%.4f entry_bar=%d mfe=%.6f mae=%.6f)\n",
                tag, t.entry_price, t.exit_price, t.entry_bar_index,
                t.max_runup, t.max_drawdown,
                want_exit, want_entry_bar, want_favorable, want_adverse);
    CHECK(t.is_long);
    CHECK(t.entry_bar_index == want_entry_bar);
    CHECK(near(t.entry_price, 100.0));
    CHECK(near(t.exit_price, want_exit));
    CHECK(near(t.max_runup, want_favorable));
    CHECK(near(t.max_drawdown, want_adverse));
}

}  // namespace

int main() {
    // Witness 2: the pre-fill high is on the row either way. `later-bar`
    // opens the lot on bar 1 and exits on bar 2; `same-bar` opens and exits
    // on bar 2 (NextEligiblePoint), the shape the deleted fold named as the
    // one per-bar sampling misses. Both report the bar's high.
    run_case("later-bar-stop", no::Stop{98.0}, NativeCloseExecution::AfterCalculation,
             1, 98.0, 16.0, 4.0);
    run_case("same-bar-stop", no::Stop{98.0}, NativeCloseExecution::NextEligiblePoint,
             2, 98.0, 16.0, 4.0);

    // Witness 3: the sibling carry is untouched. A zero-offset trail rides
    // the best and still books its peak-derived favorable excursion.
    run_case("trail-exit", no::Trail{0.0, std::nullopt, std::nullopt},
             NativeCloseExecution::AfterCalculation, 1, 108.0, 16.0, 0.0);

    std::printf("test_e10_dead_path_prefix: %d passed, %d failed\n",
                checks - failures, failures);
    return failures ? 1 : 0;
}
