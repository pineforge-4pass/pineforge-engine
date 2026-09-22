// R5 lane F6 item 1: the dead kernel code the third audit named is gone.
//
// AUDIT3-opus (kres GAP-3, GAP-20) and AUDIT3-opus2 (H11) listed members of
// the kernel that no code in the repository calls or reads -- not src/, not
// the source adapter, not tests/, examples/, runner/, tools/, not the codegen
// transpiler and not one of the 312 generated corpus strategies:
//
//   BacktestEngine::settle_position_after_partial_exit  the partial-exit
//       position settlement with TradingView's pyramid-slot rule for a
//       bracket-leg drain, and its argument type
//   BacktestEngine::PositionReductionCause              {SCRIPT_ORDER,
//       BRACKET_EXIT, MARGIN_CALL}: a Pine command vocabulary with no reader
//   BacktestEngine::append_same_side_fill               "Test-only
//       compatibility construction seam; no production caller" -- and no
//       test either
//   BacktestEngine::apply_percent_exit_qty_step         the one-lot
//       qty_percent floor of a strategy.exit leg; the adapter sizes its own
//   BacktestEngine::broker_tick_bar                     a bar quantized to
//       the tick, never called
//   internal::kClosePrefix ("__close__")                the adapter's close
//       command id prefix; zero readers, and the adapter spells its own
//       literal. As an inline std::string it put the literal, an external
//       data symbol and a guard variable into every kernel object including
//       engine_internal.hpp.
//
// (A sixth, `minute_in_window`, was a file-static function of
// src/session_time.cpp that nothing called; the compiler already said so --
// "unused function 'minute_in_window' [-Wunused-function]" -- and no
// translation unit outside that file can name it, so it has no type witness
// here. Its witness is that warning, gone from the build.)
//
// Source-free, so this TU also runs in the kernel-only profile.
//
// Witness 1 -- the type witnesses. No translation unit can call or name a
// deleted member, because the member does not exist: a caller anywhere would
// have failed to compile. That is the whole proof of unreachability.
// (Fail-before: against the header closure at fd785928 every static_assert
// below fires.)
//
// Witness 2 -- the survivors. The helpers the audit listed beside them are
// NOT dead: the twin-parity-frozen suites (test_qty_step_epsilon_floor_l4b,
// test_stop_tick_rounding_l4d, test_level_grid_snap_l4d, the frozen-size
// oracle rows) reach apply_qty_step, apply_exit_qty_step, tick_grid_price,
// price_grid_decimals and level_on_price_grid, so they stay, and so do
// append_quoted_lot and open_quoted_position, which the kernel's own
// settlement calls. The deletion must not have taken one of them, and their
// arithmetic -- read through the public syminfo ingress, never by writing a
// protected field -- must be the one those suites pin.
#include "../src/engine_internal.hpp"

#include <pineforge/native_host.hpp>

#include <cmath>
#include <cstdio>
#include <string>
#include <type_traits>
#include <utility>

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

}  // namespace

// The protected surface is detected from inside a host subclass, where the
// kernel's protected members are accessible: each trait asks whether the
// name resolves through the subclass (a substitution failure means it does
// not exist at all).
struct F6Probe final : pineforge::NativeStrategyHost {
    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {}

#define F6_MEMBER_TRAIT(trait, name)                                          \
    template <class T, class = void>                                          \
    struct trait : std::false_type {};                                        \
    template <class T>                                                        \
    struct trait<T, std::void_t<decltype(&T::name)>> : std::true_type {};

    F6_MEMBER_TRAIT(has_settle_position_after_partial_exit,
                    settle_position_after_partial_exit)
    F6_MEMBER_TRAIT(has_append_same_side_fill, append_same_side_fill)
    F6_MEMBER_TRAIT(has_apply_percent_exit_qty_step, apply_percent_exit_qty_step)
    F6_MEMBER_TRAIT(has_broker_tick_bar, broker_tick_bar)
    F6_MEMBER_TRAIT(has_apply_qty_step, apply_qty_step)
    F6_MEMBER_TRAIT(has_apply_exit_qty_step, apply_exit_qty_step)
    F6_MEMBER_TRAIT(has_tick_grid_price, tick_grid_price)
    F6_MEMBER_TRAIT(has_price_grid_decimals, price_grid_decimals)
    F6_MEMBER_TRAIT(has_level_on_price_grid, level_on_price_grid)
    F6_MEMBER_TRAIT(has_append_quoted_lot, append_quoted_lot)
    F6_MEMBER_TRAIT(has_open_quoted_position, open_quoted_position)
#undef F6_MEMBER_TRAIT

    template <class T, class = void>
    struct has_position_reduction_cause : std::false_type {};
    template <class T>
    struct has_position_reduction_cause<
        T, std::void_t<typename T::PositionReductionCause>> : std::true_type {};

    static void type_witnesses();

    double qty_floor(double qty) const { return apply_qty_step(qty); }
    double exit_qty_floor(double qty) const { return apply_exit_qty_step(qty); }
    double grid(double price) const { return tick_grid_price(price); }
    double level(double price) const { return level_on_price_grid(price); }
    int decimals() const { return price_grid_decimals(); }
};

void F6Probe::type_witnesses() {
    static_assert(!has_settle_position_after_partial_exit<F6Probe>::value,
                  "BacktestEngine still carries the dead "
                  "settle_position_after_partial_exit");
    static_assert(!has_position_reduction_cause<F6Probe>::value,
                  "BacktestEngine still carries the dead PositionReductionCause");
    static_assert(!has_append_same_side_fill<F6Probe>::value,
                  "BacktestEngine still carries the dead append_same_side_fill");
    static_assert(!has_apply_percent_exit_qty_step<F6Probe>::value,
                  "BacktestEngine still carries the dead "
                  "apply_percent_exit_qty_step");
    static_assert(!has_broker_tick_bar<F6Probe>::value,
                  "BacktestEngine still carries the dead broker_tick_bar");

    static_assert(has_apply_qty_step<F6Probe>::value,
                  "apply_qty_step is reached by twin-frozen suites and must stay");
    static_assert(has_apply_exit_qty_step<F6Probe>::value,
                  "apply_exit_qty_step is reached by twin-frozen suites and must stay");
    static_assert(has_tick_grid_price<F6Probe>::value,
                  "tick_grid_price is reached by twin-frozen suites and must stay");
    static_assert(has_price_grid_decimals<F6Probe>::value,
                  "price_grid_decimals is reached by twin-frozen suites and must stay");
    static_assert(has_level_on_price_grid<F6Probe>::value,
                  "level_on_price_grid is reached by twin-frozen suites and must stay");
    static_assert(has_append_quoted_lot<F6Probe>::value,
                  "append_quoted_lot is the kernel settlement's own and must stay");
    static_assert(has_open_quoted_position<F6Probe>::value,
                  "open_quoted_position is the kernel settlement's own and must stay");
}

// internal::kClosePrefix is a namespace-scope variable, so its witness is a
// lookup one: qualified lookup of pineforge::internal::kClosePrefix finds a
// direct member first and follows the using-directive below only when there
// is none, in which case it resolves to this marker.
namespace f6_dead_kernel_members {
struct absent {};
inline constexpr absent kClosePrefix{};
}  // namespace f6_dead_kernel_members
namespace pineforge::internal {
using namespace ::f6_dead_kernel_members;
}  // namespace pineforge::internal
static_assert(std::is_same_v<std::decay_t<decltype(pineforge::internal::kClosePrefix)>,
                             f6_dead_kernel_members::absent>,
              "the kernel still defines the dead internal::kClosePrefix (\"__close__\")");

int main() {
    F6Probe::type_witnesses();

    // Witness 2's arithmetic, read through the public ingress: a lot step of
    // 0.0001 and a 0.01 tick, the configuration the frozen suites use.
    F6Probe probe;
    probe.set_syminfo_metadata("qty_step", 0.0001);
    probe.set_syminfo_mintick(0.01);

    // A genuinely off-grid quantity floors a whole step; an on-grid one that
    // is binary64 residue below the grid ratio does not.
    CHECK(std::abs(probe.qty_floor(2.70515) - 2.7051) < 1e-12);
    CHECK(probe.qty_floor(2.70515) < 2.70515);
    CHECK(std::abs(probe.exit_qty_floor(2.70515) - 2.7051) < 1e-12);
    // A no-op floor returns the ORIGINAL double, never floor(...)*step
    // (0.3 would come back as 0.30000000000000004).
    CHECK(probe.exit_qty_floor(0.3) == 0.3);
    CHECK(probe.qty_floor(0.3) == 0.3);

    // Nearest tick, materialized as k / (1/tick): a 0.01 tick puts 14.035 on
    // 14.04 and 13.745 on 13.75, the literal doubles.
    CHECK(probe.grid(14.0351) == 14.04);
    CHECK(probe.grid(13.7449) == 13.74);
    // A level within the grid band IS the grid point; one outside it is
    // untouched.
    CHECK(probe.decimals() == 2);
    CHECK(probe.level(10.040000000000001) == 10.04);
    CHECK(probe.level(10.0400012) == 10.0400012);

    std::printf("test_f6_dead_kernel_members: %d passed, %d failed\n",
                checks - failures, failures);
    return failures ? 1 : 0;
}
