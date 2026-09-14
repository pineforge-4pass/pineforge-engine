// Literal cross-component controls. No Pine compilation, reference tape,
// external broker, campaign, or generated expected values.
#include <pineforge/pineforge.h>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cstdio>
#include <stdexcept>
#include <string>

using namespace pineforge;
using pineforge::source::PendingOrder;
namespace {
int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::fprintf(stderr,"FAIL %d %s\n",__LINE__,#x); } } while (0)
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

template<class Tag, typename Tag::Type Member>
struct PrivateMember { friend typename Tag::Type access(Tag) { return Member; } };
struct BindLayers {
    using Type = void (pineforge::source::PineStrategyHost::*)(const std::string&, std::vector<uint64_t>&);
    friend Type access(BindLayers);
};
template struct PrivateMember<BindLayers, &pineforge::source::PineStrategyHost::reconcile_deferred_layered_exits>;

class Book : public pineforge::source::PineStrategyHost {
public:
    Book() {
        initial_capital_ = 100000;
        commission_value_ = 0;
        margin_long_ = margin_short_ = 0;
        pyramiding_ = 10;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 4;
        close_entries_rule_any_ = true;
        current_bar_ = {100,100,100,100,1,0};
    }
    void on_source_bar(const Bar&) override {}
    void step() {
        ++bar_index_;
        current_bar_ = {100,100,100,100,1,int64_t(bar_index_)*60000};
        process_pending_orders(current_bar_);
    }
    void seed(double qty=2) { strategy_entry("E",true,missing,missing,qty); step(); }
    void import_pending(const PendingOrder& order) { pending_orders_.push_back(order); }
    void bind_layers() { std::vector<uint64_t> retired; (this->*access(BindLayers{}))("E",retired); }

    void reverse() { strategy_entry("E",false); }
    void close_one() { strategy_close("E","",1); }
    void bracket(double percent) { strategy_exit("X","E",120,80,missing,missing,missing,percent); }
    void deferred_bracket() { strategy_exit("X","E",missing,150,missing,missing,missing,50); }
    const PendingOrder& order(const std::string& id) const {
        for (const auto& o : pending_orders_) if (o.id==id) return o;
        throw std::logic_error("missing literal pending order");
    }
    double quantity() const { return position_qty_; }
    PositionSide side() const { return position_side_; }
    pf_pending_order_v1_t mirror(const std::string& id) const {
        for (size_t i=0;i<pending_orders_.size();++i) if (pending_orders_[i].id==id) {
            pf_pending_order_v1_t result{};
            CHECK(strategy_pending_order_get(const_cast<Book*>(this),int(i),&result,sizeof(result))==0);
            return result;
        }
        throw std::logic_error("missing mirror owner");
    }
};

void deferred_close_can_join_later_layer_binding() {
    Book b; b.seed();
    b.reverse(); b.close_one(); b.deferred_bracket();
    CHECK(b.side()==PositionSide::LONG && b.quantity()==2);
    CHECK(std::isnan(b.order("__close__E").qty));
    CHECK(b.order("__close__E").qty_percent==50);
    const auto& initial=b.order("__close__E").quantity_request;
    CHECK(initial.intent().has_value());
    if (!initial.intent()) return;
    CHECK(initial.intent()->kind()==QuantityIntent::Kind::Units && initial.intent()->units()==1);
    CHECK(!initial.reservation().has_value());
    CHECK(!initial.is_partial(1e-9,1e-9) && !initial.requests_all());
    const auto before=b.mirror("__close__E");
    CHECK(before.requested_partial==0 && before.full_percent_exit_request==0);
    CHECK(before.quantity_intent_kind==1 && before.quantity_intent_units==1);
    CHECK(before.quantity_reservation_present==0);
    // The normal pass may retire the deferred market close after binding it.
    // Independently inspect that real binder with the exact pending values on
    // a synthetic E4 book, before retirement hides its numeric receipt.
    Book bound; bound.seed(4);
    bound.import_pending(b.order("__close__E"));
    bound.import_pending(b.order("X"));
    bound.bind_layers();
    bool threw=false;
    try { b.step(); }
    catch (const std::exception& error) {
        threw=true; std::fprintf(stderr,"layer-binding exception: %s\n",error.what());
    }
    CHECK(!threw);
    if (threw) return;
    CHECK(b.side()==PositionSide::SHORT && b.quantity()==4);
    const auto& close=bound.order("__close__E");
    CHECK(close.quantity_request.intent().has_value());
    if (!close.quantity_request.intent()) return;
    CHECK(close.quantity_request.intent()->kind()==QuantityIntent::Kind::Units);
    CHECK(close.quantity_request.intent()->units()==1);
    CHECK(close.quantity_request.reservation().has_value());
    if (!close.quantity_request.reservation()) return;
    CHECK(close.quantity_request.reservation()->units==2);
    CHECK(close.quantity_request.reservation()->basis_units==4);
    CHECK(close.qty==2 && bound.order("X").qty==2);
    const auto after=bound.mirror("__close__E");
    CHECK(after.requested_partial==1 && after.full_percent_exit_request==0);
    CHECK(after.quantity_intent_kind==1 && after.quantity_intent_units==1);
    CHECK(after.quantity_reservation_present==1);
    CHECK(after.quantity_reservation_units==2 && after.quantity_reservation_basis_units==4);
}

void replacement_preserves_independent_quantity_and_birth() {
    Book b; b.seed(); b.bracket(25);
    const PendingOrder first=b.order("X");
    b.step(); b.bracket(50);
    const auto& next=b.order("X");
    CHECK(next.incarnation!=first.incarnation);
    CHECK(next.replaced_order_incarnation==first.incarnation);
    CHECK(next.created_seq==first.created_seq);
    CHECK(next.birth.timestamp()>first.birth.timestamp());
    CHECK(next.birth.cause()==OrderBirthCause::DirectCommand);
    CHECK(next.quantity_request.intent()->kind()==QuantityIntent::Kind::Fraction);
    CHECK(next.quantity_request.intent()->numerator()==50);
    CHECK(next.quantity_request.reservation()->basis_units==2);
    // Existing partial replacement retains the prior admitted amount, while
    // the current caller's original fraction is recorded independently.
    CHECK(next.quantity_request.reservation()->units==0.5 && next.qty==0.5);
    CHECK(first.quantity_request.intent()->numerator()==25);
    const auto mirrored=b.mirror("X");
    CHECK(mirrored.created_by_same_id_replacement==1);
    CHECK(mirrored.replaced_exit_order_incarnation==first.incarnation);
    CHECK(mirrored.replaced_order_incarnation==first.incarnation);
    CHECK(mirrored.quantity_intent_numerator==50);
    CHECK(mirrored.quantity_reservation_units==0.5);
    CHECK(mirrored.birth_timestamp==next.birth.timestamp());
    CHECK(mirrored.created_during_coof_recalc==0);
}
}

int main() {
    const std::pair<const char*,void(*)()> cases[] = {
        {"deferred close binding",deferred_close_can_join_later_layer_binding},
        {"replacement quantity birth",replacement_preserves_independent_quantity_and_birth},
    };
    for (const auto& test : cases) {
        try { test.second(); }
        catch (const std::exception& error) { ++failures; std::fprintf(stderr,"case %s exception: %s\n",test.first,error.what()); }
    }
    std::printf("%d checks, %d failures\n",checks,failures);
    return failures ? 1 : 0;
}
