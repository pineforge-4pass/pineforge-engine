// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// Literal native C-ABI contract: copying a resting-order POD never allocates.
// There is no strategy run, feed, reference output, or grading in this test.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
// Refuses the POD getter's heap allocations: every replaceable form, one
// allocator. The base body's two names read the shared state.
#include "global_allocation_replacement.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace {
bool& deny_allocation = global_allocation::refusing;
std::size_t& denied_allocations = global_allocation::refused;
}

namespace {
class LiteralBook : public pineforge::source::PineStrategyHost {
public:
    LiteralBook() {
        initial_capital_ = 1000;
        commission_value_ = 0;
        slippage_ = 0;
        pyramiding_ = 2;
        qty_step_ = 1;
        current_bar_ = {100, 100, 100, 100, 1, 0};
        default_qty_type_ = pineforge::QtyType::PERCENT_OF_EQUITY;
        default_qty_value_ = 100;
        const double missing = std::numeric_limits<double>::quiet_NaN();
        const std::string id(128, 'I'), oca(128, 'O'), comment(128, 'C');
        strategy_entry(id, true, missing, missing, missing, comment, oca, 0);
        strategy_entry("priced", false, missing, 150, 1);
    }
    void on_source_bar(const pineforge::Bar&) override {}
};

int failures = 0;
void require(bool value, const char* reason) {
    if (!value) {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", reason);
    }
}

void verify_copy(LiteralBook& book, int index, std::size_t bytes) {
    pf_pending_order_v1_t expected{}, actual{};
    require(strategy_pending_order_get(&book, index, &expected, sizeof(expected)) == 0,
            "baseline snapshot succeeds");
    std::memset(&actual, 0xA5, sizeof(actual));
    int status = -99;
    bool escaped = false;
    const auto prior_denials = denied_allocations;
    deny_allocation = true;
    try {
        status = strategy_pending_order_get(&book, index, &actual, bytes);
    } catch (...) {
        escaped = true;
    }
    deny_allocation = false;
    require(!escaped, "no exception crosses strategy_pending_order_get");
    require(denied_allocations == prior_denials, "POD getter attempts no allocation");
    require(status == 0, "POD getter succeeds with allocation unavailable");
    require(std::memcmp(&actual, &expected, bytes) == 0, "actual prefix bytes are preserved");
    const auto* raw = reinterpret_cast<const unsigned char*>(&actual);
    for (std::size_t i = bytes; i < sizeof(actual); ++i)
        require(raw[i] == 0xA5, "prefix reader writes no tail bytes");
}
}

int main() {
    LiteralBook book;
    require(book.pending_order_count() == 2, "both literal orders are admitted");
    pf_pending_order_v1_t snapshot{};
    require(strategy_pending_order_get(&book, 0, &snapshot, sizeof(snapshot)) == 0,
            "long-string snapshot succeeds");
    require(snapshot.market_admission_observation_present == 1,
            "real command observation is present");
    require(snapshot.market_admission_observation_original_sizing_present == 1,
            "real original default sizing is present");
    require(snapshot.market_admission_observation_id_truncated == 1 &&
            snapshot.market_admission_observation_oca_name_truncated == 1,
            "long canonical strings exercise allocation-free views");
    for (int i = 0; i < book.pending_order_count(); ++i) {
        verify_copy(book, i, sizeof(pf_pending_order_v1_t));
        verify_copy(book, i, 1272); // complete shipped cc0 public prefix
        verify_copy(book, i, 8);    // minimal supported header reader
    }
    std::printf("pending-order no-allocation mirror: %d failures, %zu allocation attempts\n",
                failures, denied_allocations);
    return failures ? 1 : 0;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
