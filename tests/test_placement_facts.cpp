// Literal native placement views, with no tape, reference trades or grader.
#include "admission_literal_book.hpp"
#include "placement_observation_fixture.hpp"
#include <array>
#include <cstdio>

using namespace pineforge;
using pineforge::source::PendingOrder;
using pineforge::source::placement_at_entry_capacity;
using pineforge::source::placement_has_prior_close;
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; \
    std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); } } while (0)

void exact_original_operands() {
    const double eps = 1e-10;
    const auto infinity = std::numeric_limits<double>::infinity();
    const std::array<double,8> close_quantities{{
        -infinity, -1, 0, eps, std::nextafter(eps, infinity), 1,
        infinity, std::numeric_limits<double>::quiet_NaN()}};
    for (const auto side : {PositionSide::FLAT, PositionSide::LONG, PositionSide::SHORT})
    for (bool buy : {false,true}) for (int held : {0,1,2})
    for (int cap : {-1,0,1,2,3}) for (double close : close_quantities) {
        PendingOrder order{};
        auto observation = std::make_shared<admission::CommandObservation>();
        observation->command=1; observation->placement_side=static_cast<int>(side);
        observation->buy=buy; observation->held_entries=held;
        observation->configuration.pyramiding=cap;
        observation->prior_close_quantity=close;
        order.market_admission.bind(observation);
        const bool expected_close = close > eps;
        const bool expected_cap = side != PositionSide::FLAT
            && side == (buy ? PositionSide::LONG : PositionSide::SHORT) && held >= cap;
        CHECK(placement_has_prior_close(order)==expected_close);
        CHECK(placement_at_entry_capacity(order)==expected_cap);
        // Later order state cannot rewrite the original request's meaning.
        order.is_long=!buy; order.created_position_side=PositionSide::FLAT;
        order.sizing_equity=123; order.qty=321; order.frozen_default_qty=456;
        CHECK(placement_has_prior_close(order)==expected_close);
        CHECK(placement_at_entry_capacity(order)==expected_cap);
    }
    for (auto kind : {OrderType::MARKET,OrderType::ENTRY,OrderType::RAW_ORDER,OrderType::EXIT}) {
        PendingOrder no_observation{}; no_observation.type=kind;
        CHECK(!placement_has_prior_close(no_observation));
        CHECK(!placement_at_entry_capacity(no_observation));
    }
}

class Book : public admission_test::Book {
public:
    void prior_close(double value) { pending_close_qty_in_bar_=value; }
    void capacity(int value) { pyramiding_=value; }
    void priced_mode() { process_orders_on_close_=true; }
    void close(const char* id) { strategy_close(id); }
    const auto& journal() const { return market_admission_journal(); }
};

void accepted_orders_keep_placement_facts() {
    for (bool buy : {false,true}) {
        Book book; book.capacity(1); book.priced_mode();
        book.add("seed",1,buy); book.fire("seed");
        CHECK(std::abs(book.position())==1);
        book.add("same",1,buy);
        book.raw("raw",1,buy);
        book.add("opposite",1,!buy);
        CHECK(placement_at_entry_capacity(book.get("same")));
        CHECK(placement_at_entry_capacity(book.get("raw")));
        CHECK(!placement_at_entry_capacity(book.get("opposite")));
        const auto original = book.get("same");
        const auto original_observation = original.market_admission.observation();
        book.capacity(5);
        CHECK(placement_at_entry_capacity(book.get("same")));
        book.add("same",1,buy);
        CHECK(!placement_at_entry_capacity(book.get("same")));
        CHECK(placement_at_entry_capacity(original));
        CHECK(original_observation->configuration.pyramiding==1);
        CHECK(book.get("same").market_admission.observation()->configuration.pyramiding==5);
        book.cancel("raw"); book.raw("raw",1,buy);
        CHECK(!placement_at_entry_capacity(book.get("raw")));
    }
    // The explicit sizing qualification happens before the Draft is bound.
    // This pins that consumer independently from later getter substitutions.
    Book before; before.prior_close(1); before.add("E",1);
    CHECK(placement_has_prior_close(before.get("E")));
    CHECK(std::isnan(before.get("E").explicit_placement_equity));
    Book independent; independent.prior_close(0); independent.add("E",1);
    CHECK(!placement_has_prior_close(independent.get("E")));
    CHECK(std::isfinite(independent.get("E").explicit_placement_equity));
    const auto old=before.get("E"); before.prior_close(0); before.add("E",1);
    CHECK(placement_has_prior_close(old));
    CHECK(!placement_has_prior_close(before.get("E")));

    Book close_book;
    close_book.add("seed",2); close_book.fire("seed"); close_book.close("seed");
    close_book.add("after-close",1);
    CHECK(placement_has_prior_close(close_book.get("after-close")));
    CHECK(!placement_has_prior_close(close_book.get("__close__seed")));
    const auto hash = close_book.broker_state_hash();
    const auto events = close_book.journal().events().size();
    for (int i=0;i<3;++i) {
        CHECK(placement_has_prior_close(close_book.get("after-close")));
        CHECK(close_book.mirror("after-close").created_after_position_close_in_bar==1);
    }
    CHECK(close_book.broker_state_hash()==hash);
    CHECK(close_book.journal().events().size()==events);
}
}

int main() {
    exact_original_operands();
    accepted_orders_keep_placement_facts();
    std::printf("placement facts: %d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
