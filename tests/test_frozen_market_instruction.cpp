#include "placement_observation_fixture.hpp"
// Literal native instruction and book transitions. No external data, reference
// trades, strategy compiler or campaign grading participates in this test.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, const MarketAdmissionJournal*,
                               pf_pending_order_v1_t*);
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
const pf_field_desc_t* pending_order_layout(int*);
}

namespace frozen_prefix {
#include "fixtures/frozen_market/ff54_pending_order_mirror.hpp"
}

using namespace pineforge;
using pineforge::source::PendingOrder;
using compat::pine::FrozenMarketInstruction;
using compat::pine::FrozenMarketInstructionKind;
#define PF_PREFIX_FIELD(name) \
    static_assert(offsetof(pf_pending_order_v1_t, name) == \
        offsetof(frozen_prefix::pf_pending_order_v1_t, name), "frozen prefix offset"); \
    static_assert(std::is_same<decltype(pf_pending_order_v1_t::name), \
        decltype(frozen_prefix::pf_pending_order_v1_t::name)>::value, "frozen prefix type"); \
    static_assert(sizeof(pf_pending_order_v1_t::name) == \
        sizeof(frozen_prefix::pf_pending_order_v1_t::name), "frozen prefix size");
#include "fixtures/frozen_market/ff54_fields.inc"
#undef PF_PREFIX_FIELD
static_assert(offsetof(pf_pending_order_v1_t, pine_frozen_market_instruction_kind) >=
    sizeof(frozen_prefix::pf_pending_order_v1_t), "preserve old trailing padding");

namespace {
const double nan = std::numeric_limits<double>::quiet_NaN();
const double inf = std::numeric_limits<double>::infinity();
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; \
} } while (0)

template<class Function> void invalid(Function make) {
    try { make(); CHECK(false); }
    catch (const std::invalid_argument&) {}
}

pf_pending_order_v1_t mirror(const PendingOrder& order,
                             const MarketAdmissionJournal* journal = nullptr) {
    pf_pending_order_v1_t result;
    fill_pending_order_mirror(order, journal, &result);
    return result;
}

void exclusive_instruction_and_revocation() {
    FrozenMarketInstruction ordinary;
    CHECK(ordinary.kind() == FrozenMarketInstructionKind::Ordinary);
    CHECK(!ordinary.active() && !ordinary.transaction() && !ordinary.targeted_close());
    for (double own : {0.0, -1.0, nan, inf})
        invalid([&] { return FrozenMarketInstruction::transaction(own, 2); });
    for (double total : {0.0, 1.0, nan, -inf})
        invalid([&] { return FrozenMarketInstruction::transaction(2, total); });
    auto transaction = FrozenMarketInstruction::transaction(2, 5);
    CHECK(transaction.active() && !transaction.targeted_close());
    CHECK(transaction.transaction()->own_units == 2);
    CHECK(transaction.transaction()->transaction_units == 5);
    transaction.revoke();
    CHECK(!transaction.active() && !transaction.transaction());
    transaction.revoke();
    CHECK(transaction.kind() == FrozenMarketInstructionKind::Ordinary);

    QuantityRequest request;
    invalid([&] { return FrozenMarketInstruction::targeted_close("E", request); });
    request.request(QuantityIntent::all());
    invalid([&] { return FrozenMarketInstruction::targeted_close("E", request); });
    request.request(QuantityIntent::fraction(1, 2));
    invalid([&] { return FrozenMarketInstruction::targeted_close("E", request); });
    for (double units : {0.0, -1.0, nan, inf}) {
        request.request(QuantityIntent::units(units));
        invalid([&] { return FrozenMarketInstruction::targeted_close("E", request); });
    }
    request.request(QuantityIntent::units(3));
    invalid([&] { return FrozenMarketInstruction::targeted_close("", request); });
    auto close = FrozenMarketInstruction::targeted_close("E", request);
    CHECK(close.kind() == FrozenMarketInstructionKind::TargetedClose);
    CHECK(close.active() && !close.transaction());
    CHECK(close.targeted_close()->target_id == "E");
    request.reserve(1, 3);
    CHECK(request.intent()->units() == 3); // Reservation does not change source units.
    close.revoke();
    CHECK(!close.active() && !close.targeted_close());
}

class Book : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 1000000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 1;
        bar_index_ = 0;
        current_bar_ = {100, 100, 100, 100, 1, 0};
    }
    void on_source_bar(const Bar&) override {}
    void entry(const std::string& id, bool buy, double units) {
        strategy_entry(id, buy, nan, nan, units);
    }
    void step() {
        ++bar_index_;
        current_bar_ = {100, 100, 100, 100, 1, int64_t(bar_index_) * 60000};
        process_pending_orders(current_bar_);
    }
    void seed(bool buy, double units) { entry("seed", buy, units); step(); }
    void close_seed() { strategy_close("seed"); }
    void cancel(const std::string& id) { strategy_cancel(id); }
    void finalize() { compat::pine::finalize_frozen_market_book(pending_orders_, true); }

    void priced() { strategy_entry("priced", true, 90, nan, 1); }
    void raw() { strategy_order("raw", true, 1); }
    void bracket() { strategy_exit("bracket", "A", 120, 90); }
    void close_all() { strategy_close_all(); }
    void leave_scope() { compat::pine::finalize_frozen_market_book(pending_orders_, false); }
    double position() const { return position_qty_; }
    PositionSide side() const { return position_side_; }
    const std::vector<PendingOrder>& orders() const { return pending_orders_; }
    PendingOrder& order(const std::string& id) {
        for (auto& o : pending_orders_) if (o.id == id) return o;
        std::string present;
        for (const auto& o : pending_orders_) present += " " + o.id;
        throw std::logic_error("missing literal order " + id + "; present:" + present);
    }
    void put(PendingOrder order) { pending_orders_.push_back(std::move(order)); }
};

void capture_amounts_and_immutable_placement() {
    for (bool seed_buy : {false, true}) {
        Book book; book.seed(seed_buy, 3);
        CHECK(book.position() == 3);
        book.entry("opposite", !seed_buy, 2);
        const auto& first = book.order("opposite");
        CHECK(first.pine_frozen_market_instruction.transaction()->own_units == 2);
        CHECK(first.pine_frozen_market_instruction.transaction()->transaction_units == 5);
        CHECK(!mirror(first).sbmt_kept_over_cap);
        book.entry("same", seed_buy, 4);
        const auto& retained = book.order("same");
        CHECK(retained.pine_frozen_market_instruction.transaction()->own_units == 4);
        CHECK(retained.pine_frozen_market_instruction.transaction()->transaction_units == 6);
        CHECK(placement_at_entry_capacity(retained));
        CHECK(mirror(retained).sbmt_kept_over_cap == 1);
        const auto old_incarnation = book.order("opposite").incarnation;
        book.entry("opposite", !seed_buy, 5);
        const auto& replacement = book.order("opposite");
        CHECK(replacement.incarnation != old_incarnation);
        CHECK(replacement.replaced_order_incarnation == old_incarnation);
        // Own 5 + held opposite 3 + pending opposite's own 4, never its total 6.
        CHECK(replacement.pine_frozen_market_instruction.transaction()->own_units == 5);
        CHECK(replacement.pine_frozen_market_instruction.transaction()->transaction_units == 12);
        CHECK(replacement.created_position_side == (seed_buy ? PositionSide::LONG : PositionSide::SHORT));
        book.finalize();
        CHECK(book.order("same").pine_frozen_market_instruction.active());
        CHECK(mirror(book.order("same")).sbmt_kept_over_cap == 1);
    }
}

void targeted_close_has_one_quantity_authority() {
    for (bool buy : {false, true}) {
        Book book; book.seed(buy, 3); book.close_seed();
        auto& close = book.order("__close__seed");
        CHECK(close.pine_frozen_market_instruction.targeted_close()->target_id == "seed");
        CHECK(!close.pine_frozen_market_instruction.transaction());
        CHECK(close.quantity_request.intent()->units() == 3);
        const auto before = mirror(close);
        CHECK(before.sbmt_member == 1 && before.sbmt_close_qty == 3);
        CHECK(before.sbmt_close_buy == (buy ? 0 : 1));
        CHECK(!before.sbmt_kept_over_cap && std::isnan(before.sbmt_tx_qty));
        close.quantity_request.reserve(1, 3);
        close.qty = 1; // Execution reservation differs; original close still resolves to 3.
        CHECK(mirror(close).sbmt_close_qty == 3);
        CHECK(close.quantity_request.reservation()->units == 1);
        book.step();
        CHECK(book.position() == 0);
        CHECK(book.orders().empty());
        book.entry("fresh", !buy, 2);
        CHECK(book.order("fresh").pine_frozen_market_instruction.transaction()->own_units == 2);
        CHECK(book.order("fresh").pine_frozen_market_instruction.transaction()->transaction_units == 2);
    }
}

void whole_book_revocation() {
    using Add = void (Book::*)();
    for (Add add : {&Book::priced, &Book::raw, &Book::bracket, &Book::leave_scope}) {
        Book book; book.entry("A", true, 2); book.entry("B", false, 3);
        (book.*add)(); book.finalize();
        CHECK(book.orders().size() >= 2);
        for (const auto& order : book.orders()) {
            CHECK(!order.pine_frozen_market_instruction.active());
            const auto legacy = mirror(order, &book.market_admission_journal());
            CHECK(!legacy.sbmt_member && !legacy.sbmt_kept_over_cap && !legacy.sbmt_close_buy);
            CHECK(std::isnan(legacy.sbmt_own_qty) && std::isnan(legacy.sbmt_tx_qty));
            CHECK(std::isnan(legacy.sbmt_close_qty));
        }
    }
    Book third; third.entry("A", true, 1); third.entry("B", false, 2);
    third.entry("C", true, 3); third.finalize();
    CHECK(third.orders().size() == 3);
    for (const auto& order : third.orders()) CHECK(!order.pine_frozen_market_instruction.active());
    Book all; all.seed(true, 3); all.entry("A", false, 1); all.close_all(); all.finalize();
    for (const auto& order : all.orders()) CHECK(!order.pine_frozen_market_instruction.active());
    // The generic cap snapshot alone never creates the removed retention permit.
    PendingOrder ordinary{}; placement_fixture::at_capacity(ordinary);
    CHECK(!mirror(ordinary).sbmt_kept_over_cap);
}

void cancellation_recreation_and_noop() {
    Book book; book.entry("A", true, 2); book.entry("B", false, 3);
    const auto cancelled = book.order("B").incarnation;
    book.cancel("B"); book.entry("B", false, 4);
    const auto& fresh = book.order("B");
    CHECK(fresh.incarnation != cancelled);
    // That separate receipt only records a priced-entry cancel with a
    // surviving exit. A MARKET cancellation must not borrow such a receipt.
    CHECK(fresh.recreated_after_named_cancelled_entry_incarnation == 0);
    CHECK(fresh.pine_frozen_market_instruction.transaction()->own_units == 4);
    CHECK(fresh.pine_frozen_market_instruction.transaction()->transaction_units == 6);
    book.cancel("A"); book.cancel("B");
    CHECK(book.orders().empty());
    book.close_seed(); // No position/owner: no close payload or permit is created.
    CHECK(book.orders().empty());
    book.entry("B", false, 1);
    CHECK(book.order("B").pine_frozen_market_instruction.transaction()->transaction_units == 1);
    book.cancel("B"); book.priced();
    CHECK(!book.order("priced").pine_frozen_market_instruction.active());
}

void hash_and_mirror_live_facts() {
    Book base; PendingOrder order{}; order.id = "M";
    base.put(order);
    const auto empty_hash = base.broker_state_hash();
    Book transaction = base;
    transaction.order("M").pine_frozen_market_instruction = FrozenMarketInstruction::transaction(2, 5);
    const auto tx_hash = transaction.broker_state_hash();
    CHECK(tx_hash != empty_hash);
    auto tx = mirror(transaction.order("M"));
    CHECK(tx.pine_frozen_market_instruction_kind == 1);
    CHECK(tx.pine_frozen_market_instruction_own_units == 2 && tx.sbmt_own_qty == 2);
    CHECK(tx.pine_frozen_market_instruction_transaction_units == 5 && tx.sbmt_tx_qty == 5);
    CHECK(std::strcmp(tx.pine_frozen_market_instruction_target_id, "") == 0);
    for (auto amounts : {std::pair<double,double>{3, 5}, {2, 6}}) {
        Book changed = transaction;
        changed.order("M").pine_frozen_market_instruction = FrozenMarketInstruction::transaction(amounts.first, amounts.second);
        CHECK(changed.broker_state_hash() != tx_hash);
    }
    transaction.order("M").pine_frozen_market_instruction.revoke();
    CHECK(transaction.broker_state_hash() == empty_hash);

    Book close = base;
    auto& c = close.order("M");
    c.type = OrderType::EXIT;
    c.created_position_side = PositionSide::SHORT;
    c.quantity_request.request(QuantityIntent::units(3));
    const std::string target(90, 'x');
    c.pine_frozen_market_instruction = FrozenMarketInstruction::targeted_close(target, c.quantity_request);
    const auto close_hash = close.broker_state_hash();
    auto cm = mirror(c);
    CHECK(cm.pine_frozen_market_instruction_kind == 2);
    CHECK(cm.pine_frozen_market_instruction_target_id_truncated == 1);
    CHECK(std::strlen(cm.pine_frozen_market_instruction_target_id) == 63);
    uint64_t expected = 1469598103934665603ULL;
    for (unsigned char ch : target) { expected ^= ch; expected *= 1099511628211ULL; }
    CHECK(cm.pine_frozen_market_instruction_target_id_hash64 == expected);
    Book changed = close;
    changed.order("M").pine_frozen_market_instruction = FrozenMarketInstruction::targeted_close(target + "y", c.quantity_request);
    CHECK(changed.broker_state_hash() != close_hash);
    CHECK(mirror(changed.order("M")).pine_frozen_market_instruction_target_id_hash64 != expected);
    changed = close; changed.order("M").quantity_request.request(QuantityIntent::units(4));
    CHECK(changed.broker_state_hash() != close_hash);
    changed = close; changed.order("M").created_position_side = PositionSide::LONG;
    CHECK(changed.broker_state_hash() != close_hash && !mirror(changed.order("M")).sbmt_close_buy);
    c.pine_frozen_market_instruction.revoke();
    cm = mirror(c);
    CHECK(!cm.sbmt_member && !cm.sbmt_close_buy && std::isnan(cm.sbmt_close_qty));
    CHECK(cm.pine_frozen_market_instruction_kind == 0 && !cm.pine_frozen_market_instruction_target_id[0]);

    int count = 0;
    const auto* layout = pending_order_layout(&count);
    CHECK(count==PF_PENDING_ORDER_FIELD_COUNT);
    CHECK(std::strcmp(layout[149].name, "pine_frozen_market_instruction_kind") == 0);
}

void overflowed_total_retains_existing_finite_execution_guard() {
    Book book; book.entry("M", true, 2);
    auto& order = book.order("M");
    order.pine_frozen_market_instruction = FrozenMarketInstruction::transaction(2, inf);
    CHECK(order.pine_frozen_market_instruction.active());
    CHECK(std::isinf(mirror(order).sbmt_tx_qty));
    double qty = 0; int close_only = -1, partition = -1;
    CHECK(book.observe_probe_fill_qty(0, 100, &qty, &close_only, &partition) == 0);
    CHECK(qty == 2 && partition == 0 && close_only == 0);
    // Positive source-total overflow cannot enable the expanded-transaction arm.
    book.step();
    CHECK(book.position() == 2 && book.side() == PositionSide::LONG);
}
} // namespace

int main() {
    const std::pair<const char*, void(*)()> cases[] = {
        {"exclusive operation", exclusive_instruction_and_revocation},
        {"capture and placement", capture_amounts_and_immutable_placement},
        {"target quantity", targeted_close_has_one_quantity_authority},
        {"whole book revocation", whole_book_revocation},
        {"cancel and recreate", cancellation_recreation_and_noop},
        {"hash and mirror", hash_and_mirror_live_facts},
        {"overflow total", overflowed_total_retains_existing_finite_execution_guard},
    };
    for (const auto& item : cases) {
        try { item.second(); }
        catch (const std::exception& error) {
            std::fprintf(stderr, "FAIL %s: %s\n", item.first, error.what());
            ++failures;
        }
    }
    std::printf("frozen market instruction: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
