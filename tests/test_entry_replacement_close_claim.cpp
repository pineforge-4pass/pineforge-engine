#include "entry_amount_fixture.hpp"

namespace {
using namespace entry_amount;

class Probe final : public Host {
public:
    using Host::Host;
    void on_source_bar(const Bar&) override {
        last_bar = pine_bar_index();
        if (last_bar != 0 && last_bar != 1) return;
        const double limit = last_bar == 0 ? 96 : 92;
        entry("limit", true, limit);
        auto& row = rows[last_bar];
        const auto working = native_working_requests();
        observed[last_bar] = working.size() == 1 && pending_order_count() == 1
            && pending_intent_view().copy_v1(0, &row) == 0
            && std::string(row.id) == "limit" && row.incarnation != 0
            && row.incarnation == working.front().definition->handle.incarnation
            && row.limit_price == limit && row.qty_type == type(amount)
            && row.qty == operand(amount);
    }
    pf_pending_order_v1_t rows[2]{};
    bool observed[2]{};
};

bool check(Amount a) {
    Probe p(a);
    const bool done = p.completed({flat(0, 120), flat(1, 120), flat(2, 120)});
    const bool flat_book = done && p.fills.empty() && p.live_position_size() == 0;
    const auto& first = p.rows[0];
    const auto& next = p.rows[1];
    const bool placed = flat_book && p.observed[0] && first.replaced_order_incarnation == 0
        && first.cancellation_cause == static_cast<int>(source::PineCancellationCause::None);
    const bool replaced = placed && p.observed[1] && next.incarnation != first.incarnation
        && next.replaced_order_incarnation == first.incarnation
        && next.cancellation_cause == static_cast<int>(source::PineCancellationCause::Replacement)
        && next.cancellation_target_incarnation == first.incarnation;
    const auto no_debit = [](const pf_pending_order_v1_t& row) {
        return std::isnan(row.cancellation_close_claim_consumed)
            && std::isnan(row.suppressed_close_consumed_ledger_qty);
    };
    bool ok = report(!placed ? "NOT COMPARED" : no_debit(first) ? "PASS" : "FAIL",
                     "first unfilled limit has no close claim", a);
    ok &= report(!replaced ? "NOT COMPARED" : no_debit(next) ? "PASS" : "FAIL",
                 "replaced unfilled limit has no close claim", a);
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (auto a : {Amount::Cash, Amount::Equity}) ok &= check(a);
    return ok ? 0 : 1;
}
