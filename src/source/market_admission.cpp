#include <pineforge/source/market_admission.hpp>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <type_traits>

namespace pineforge::admission {
inline namespace market_admission_v2 {
namespace {
bool same(const ReviewReceipt& a, const ReviewReceipt& b) {
    return a.sequence == b.sequence && a.checkpoint == b.checkpoint &&
        a.bar == b.bar && a.target_command == b.target_command;
}
bool same(const SizingRevision& a, const SizingRevision& b) {
    return a.sequence == b.sequence && a.cause_fill == b.cause_fill &&
        a.bar == b.bar && a.target_command == b.target_command;
}
bool known_checkpoint(Checkpoint checkpoint) {
    return checkpoint == Checkpoint::DefaultGross ||
        checkpoint == Checkpoint::ExplicitPair || checkpoint == Checkpoint::TerminalGross;
}
void require_origin(const std::shared_ptr<const CommandObservation>& origin,
                    uint64_t target, uint64_t event, int bar) {
    if (!origin || target != origin->command || event <= origin->command ||
        bar < origin->bar)
        throw std::invalid_argument("admission receipt requires its exact earlier command");
}
}
void Draft::bind(std::shared_ptr<const CommandObservation> observation) {
    if(observation_ || !observation || observation->command==0)
        throw std::logic_error("market draft requires one original command observation");
    observation_=std::move(observation);
}
void Draft::reviewed(ReviewReceipt receipt) {
    require_origin(observation_, receipt.target_command, receipt.sequence, receipt.bar);
    if (!known_checkpoint(receipt.checkpoint))
        throw std::invalid_argument("review requires a known checkpoint");
    if (review_) {
        if (same(*review_, receipt)) return;
        throw std::logic_error("first admission review cannot be replaced");
    }
    review_ = receipt;
}
void Draft::sizing_revised(SizingRevision receipt) {
    require_origin(observation_, receipt.target_command, receipt.sequence, receipt.bar);
    if (receipt.cause_fill == 0)
        throw std::invalid_argument("sizing revision requires committed cause");
    if (sizing_revision_) {
        if (same(*sizing_revision_, receipt)) return;
        if (receipt.sequence <= sizing_revision_->sequence ||
            receipt.bar < sizing_revision_->bar || receipt.cause_fill < sizing_revision_->cause_fill)
            throw std::logic_error("sizing revision cannot rewrite or precede its latest cause");
    }
    sizing_revision_ = receipt;
}
uint64_t sequence(const Event& event) {
    return std::visit([](const auto& value)->uint64_t {
        using T=std::decay_t<decltype(value)>;
        if constexpr(std::is_same_v<T,CommandEvent>) {
            if(!value.observation)throw std::invalid_argument("command event requires an observation");
            return value.observation->command;
        }
        else return value.receipt.sequence;
    },event);
}
Journal::Journal(const Journal& other) {
    if (other.active_allocations_ != 0)
        throw std::logic_error("cannot copy admission journal with active captures");
    next_sequence_ = other.next_sequence_;
    outstanding_sequences_ = other.outstanding_sequences_;
    events_ = other.events_;
}
Journal& Journal::operator=(const Journal& other) {
    if (this == &other) return *this;
    if (active_allocations_ != 0)
        throw std::logic_error("cannot replace admission journal with active captures");
    Journal copy(other);
    std::swap(next_sequence_, copy.next_sequence_);
    outstanding_sequences_.swap(copy.outstanding_sequences_);
    events_.swap(copy.events_);
    return *this;
}
Journal::Journal(Journal&& other) {
    if (other.active_allocations_ != 0)
        throw std::logic_error("cannot move admission journal with active captures");
    next_sequence_ = other.next_sequence_;
    outstanding_sequences_ = std::move(other.outstanding_sequences_);
    events_ = std::move(other.events_);
    other.outstanding_sequences_.clear();
    other.events_.clear();
    other.next_sequence_ = 1;
}
Journal& Journal::operator=(Journal&& other) {
    if (this == &other) return *this;
    if (active_allocations_ != 0)
        throw std::logic_error("cannot replace admission journal with active captures");
    Journal moved(std::move(other));
    std::swap(next_sequence_, moved.next_sequence_);
    outstanding_sequences_.swap(moved.outstanding_sequences_);
    events_.swap(moved.events_);
    return *this;
}
uint64_t Journal::next_sequence() {
    if(next_sequence_==std::numeric_limits<uint64_t>::max())throw std::overflow_error("admission event sequence exhausted");
    outstanding_sequences_.push_back(next_sequence_);
    return next_sequence_++;
}
Allocation Journal::reserve() {
    const auto id = next_sequence();
    ++active_allocations_;
    return Allocation(*this, id);
}
void Journal::abandon(uint64_t id) noexcept {
    const auto at = std::find(outstanding_sequences_.begin(), outstanding_sequences_.end(), id);
    if (at != outstanding_sequences_.end()) outstanding_sequences_.erase(at);
}
Allocation::Allocation(Allocation&& other) noexcept
    : journal_(other.journal_), sequence_(other.sequence_) {
    other.journal_ = nullptr;
    other.sequence_ = 0;
}
void Journal::release_allocation(uint64_t sequence) noexcept {
    abandon(sequence);
    --active_allocations_;
}
Allocation::~Allocation() noexcept { if (journal_) journal_->release_allocation(sequence_); }
void Journal::append(Event event) {
    const auto id=sequence(event);
    const auto pending = std::find(outstanding_sequences_.begin(), outstanding_sequences_.end(), id);
    if (pending == outstanding_sequences_.end())
        throw std::logic_error("admission event has no outstanding allocation");
    auto at=std::lower_bound(events_.begin(),events_.end(),id,[](const auto& e,uint64_t n){return sequence(e)<n;});
    events_.insert(at,std::move(event));
    // Consume only after insertion succeeds. Allocation failure leaves a caller
    // free to retry this still-outstanding event; retain never restores an ID.
    outstanding_sequences_.erase(pending);
}
void Journal::retain(const std::vector<uint64_t>& retained) {
    events_.erase(std::remove_if(events_.begin(),events_.end(),[&](const auto& e){
        return std::find(retained.begin(),retained.end(),sequence(e))==retained.end();
    }),events_.end());
}
void Journal::reset() {
    if (!outstanding_sequences_.empty() || active_allocations_ != 0)
        throw std::logic_error("cannot reset admission journal with unfinished events");
    events_.clear();next_sequence_=1;
}
CommandCapture::CommandCapture(Allocation allocation,CommandObservation input,std::vector<BookObservation> before,
        std::function<void(CommandEvent)> complete):allocation_(std::move(allocation)),input_(std::move(input)),before_(std::move(before)),complete_(std::move(complete)) {
    if (input_.command != allocation_.sequence())
        throw std::invalid_argument("command capture requires its allocated event");
}
CommandCapture::~CommandCapture() noexcept(false) {
    // Never run a potentially throwing completion while another exception is
    // unwinding. The Allocation member abandons this unfinished event instead.
    if (std::uncaught_exceptions() != 0) return;
    complete_({std::make_shared<const CommandObservation>(input_),outcome_,0,std::move(before_),{}});
}
void CommandCapture::bind(Draft& draft,std::optional<SizingObservation> sizing,double equity,double price) {
    input_.original_sizing=std::move(sizing);input_.explicit_equity=equity;input_.explicit_price=price;
    draft.bind(std::make_shared<const CommandObservation>(input_));
    if(outcome_!=Outcome::OpeningRejectedReductionAdmitted)outcome_=Outcome::Admitted;
}
ReviewCapture::ReviewCapture(Allocation allocation,ReviewEvent event,
        std::function<void(ReviewEvent)> complete)
    : allocation_(std::move(allocation)),event_(std::move(event)),complete_(std::move(complete)) {
    if (event_.receipt.sequence != allocation_.sequence() || event_.receipt.target_command != 0 ||
        !known_checkpoint(event_.receipt.checkpoint))
        throw std::invalid_argument("review capture requires an allocated batch checkpoint");
}
ReviewCapture::~ReviewCapture() noexcept(false) {
    if (std::uncaught_exceptions() != 0) return;
    complete_(std::move(event_));
}
ReviewReceipt ReviewCapture::receipt_for(const Draft& draft) const {
    const auto& origin = draft.observation();
    if (!origin || std::none_of(event_.reviewed.begin(), event_.reviewed.end(), [&](const auto& order) {
            return order.draft.observation() && order.draft.observation()->command == origin->command;
        }))
        throw std::invalid_argument("draft does not belong to this admission review");
    auto receipt = event_.receipt;
    receipt.target_command = origin->command;
    require_origin(origin, receipt.target_command, receipt.sequence, receipt.bar);
    return receipt;
}

namespace {
struct Reflect {
    const FieldVisitor& visit;
    template<class T>void field(const std::string& path,const char* name,const T& value) const {
        if constexpr(std::is_same_v<T,bool>)visit({path+"."+name,uint64_t(value?1:0)});
        else if constexpr(std::is_enum_v<T>||std::is_same_v<T,int>)visit({path+"."+name,int64_t(value)});
        else visit({path+"."+name,value});
    }
    void birth(const OrderBirth& o,const std::string& p)const {
        field(p,"cause",o.cause());field(p,"bar",o.bar());field(p,"timestamp",o.timestamp());
        field(p,"cursor_domain",o.cursor().domain());field(p,"cursor_position",o.cursor().position());
        field(p,"cursor_index",o.cursor().index());field(p,"cursor_count",o.cursor().count());
        field(p,"cursor_price",o.cursor_price());field(p,"first_fill",o.first_fill());
        field(p,"last_fill",o.last_fill());field(p,"evaluation_ordinal",o.evaluation_ordinal());
    }
    void config(const Configuration& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(process_on_close);F(calc_on_fills);F(magnifier);F(fill_recalculation);F(scheduler);
        F(slippage);F(pyramiding);F(default_quantity_type);F(default_quantity_value);
        F(long_margin);F(short_margin);F(commission_value);F(commission_type);F(pointvalue);F(fx);F(quantity_step);F(mintick);F(risk_direction);F(loss_days_limit);
        F(drawdown_limit);F(intraday_loss_limit);F(position_limit);F(fill_cap_active);F(risk_halted);
#undef F
    }
    void sizing(const SizingObservation& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(quantity);F(equity);F(price);F(mark);F(fx);
#undef F
    }
    void command(const CommandObservation& o,const std::string& p)const {
#define F(name) field(p,#name,o.name)
        F(command);F(kind);birth(o.birth,p+".birth");F(id);F(requested_quantity);F(quantity_type);F(buy);
        field(p+".prices","limit",o.prices.limit);field(p+".prices","stop",o.prices.stop);
        F(oca_name);F(oca_type);config(o.configuration,p+".configuration");F(bar);F(placement_side);F(placement_cycle);
        F(prior_close_quantity);F(held_quantity);F(held_entries);F(realized_equity);F(placement_equity);F(signal_close);F(quantized_fixed_quantity);
        field(p,"original_sizing_present",o.original_sizing.has_value());
        if(o.original_sizing)sizing(*o.original_sizing,p+".original_sizing");
        F(explicit_equity);F(explicit_price);
#undef F
    }
    void review(const ReviewReceipt& o,const std::string& p)const {
        field(p,"sequence",o.sequence);field(p,"checkpoint",o.checkpoint);field(p,"bar",o.bar);
        field(p,"target_command",o.target_command);
    }
    void revision(const SizingRevision& o,const std::string& p)const {
        field(p,"sequence",o.sequence);field(p,"cause_fill",o.cause_fill);field(p,"bar",o.bar);
        field(p,"target_command",o.target_command);
    }
    void draft(const Draft& o,const std::string& p)const {
        field(p,"observation_present",bool(o.observation()));
        if(o.observation())command(*o.observation(),p+".observation");
        field(p,"review_present",o.review().has_value());if(o.review())review(*o.review(),p+".review");
        field(p,"sizing_revision_present",o.sizing_revision().has_value());if(o.sizing_revision())revision(*o.sizing_revision(),p+".sizing_revision");
    }
    void book(const BookObservation& o,const std::string& p)const {
        field(p,"incarnation",o.incarnation);field(p,"priority",o.priority);field(p,"bar",o.bar);
        field(p,"type",o.type);field(p,"placement_side",o.placement_side);field(p,"buy",o.buy);field(p,"id",o.id);
        field(p,"oca_name",o.oca_name);field(p,"oca_type",o.oca_type);birth(o.birth,p+".birth");
        field(p+".prices","limit",o.prices.limit);field(p+".prices","stop",o.prices.stop);
        field(p+".prices","trail_points",o.prices.trail_points);field(p+".prices","trail_price",o.prices.trail_price);
        field(p+".prices","trail_offset",o.prices.trail_offset);draft(o.draft,p+".draft");
    }
    void resolution(const InstructionResolution& o,const std::string& p)const {
        field(p,"incarnation",o.incarnation);field(p,"kind",o.kind);field(p,"peer_incarnation",o.peer_incarnation);
        field(p,"own_priority",o.own_priority);field(p,"peer_priority",o.peer_priority);field(p,"transaction_quantity",o.transaction_quantity);
    }
    template<class T,class Fn>void array(const std::vector<T>& values,const std::string& p,Fn fn)const {
        field(p,"size",uint64_t(values.size()));for(size_t i=0;i<values.size();++i)fn(values[i],p+"["+std::to_string(i)+"]");
    }
    void event(const Event& value,const std::string& p)const {
        field(p,"kind",uint64_t(value.index()));
        std::visit([&](const auto& o){using T=std::decay_t<decltype(o)>;
            if constexpr(std::is_same_v<T,CommandEvent>){
                command(*o.observation,p+".observation");field(p,"outcome",o.outcome);field(p,"admitted_incarnation",o.admitted_incarnation);
                array(o.before,p+".before",[&](const auto& x,const auto& q){book(x,q);});
                array(o.removed,p+".removed",[&](auto x,const auto& q){field(q,"incarnation",x);});
            }else if constexpr(std::is_same_v<T,ReviewEvent>){
                review(o.receipt,p+".receipt");config(o.configuration,p+".configuration");field(p,"open_price",o.open_price);field(p,"position_side",o.position_side);field(p,"position_cycle",o.position_cycle);array(o.book,p+".book",[&](const auto& x,const auto& q){book(x,q);});array(o.reviewed,p+".reviewed",[&](const auto& x,const auto& q){book(x,q);});
                array(o.resolutions,p+".resolutions",[&](const auto& x,const auto& q){resolution(x,q);});
                array(o.causes,p+".causes",[&](auto x,const auto& q){field(q,"sequence",x);});
            }else{
                revision(o.receipt,p+".receipt");field(p,"incarnation",o.incarnation);
                sizing(o.before,p+".before");sizing(o.after,p+".after");
                field(p,"affordability_equity_before",o.affordability_equity_before);field(p,"affordability_equity_after",o.affordability_equity_after);
            }
        },value);
    }
};
}
void reflect(const Draft& value,const std::string& path,const FieldVisitor& visit){Reflect{visit}.draft(value,path);}
void reflect(const Event& value,const std::string& path,const FieldVisitor& visit){Reflect{visit}.event(value,path);}
void Journal::reflect(const std::string& path,const FieldVisitor& visit)const {
    Reflect r{visit};r.field(path,"next_sequence",next_sequence_);
    r.field(path,"active_allocations",active_allocations_);
    r.array(outstanding_sequences_,path+".outstanding_sequences",[&](auto sequence,const auto& p){r.field(p,"sequence",sequence);});
    r.array(events_,path+".events",[&](const auto& event,const auto& p){r.event(event,p);});
}
std::vector<Field> fields(const Draft& draft) {
    std::vector<Field> result;reflect(draft,"draft",[&](const auto& field){result.push_back(field);});return result;
}
namespace {
template<class T>T read_field(const std::vector<Field>& fields,const std::string& path) {
    for(const auto& field:fields)if(field.path==path)return std::get<T>(field.value);
    return T{}; // absent optional payload; the presence leaf is always reflected
}
}
uint64_t read_unsigned(const std::vector<Field>& f,const std::string& p){return read_field<uint64_t>(f,p);}
int64_t read_integer(const std::vector<Field>& f,const std::string& p){return read_field<int64_t>(f,p);}
double read_double(const std::vector<Field>& f,const std::string& p){return read_field<double>(f,p);}
std::string read_string(const std::vector<Field>& f,const std::string& p){return read_field<std::string>(f,p);}
} // inline namespace market_admission_v2
} // namespace pineforge::admission
