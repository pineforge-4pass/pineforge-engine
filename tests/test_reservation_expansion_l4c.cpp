#include "placement_observation_fixture.hpp"
#include "exit_lifecycle_fixture.hpp"
// Ten literal native contracts, derived from causal facts and integer/dyadic
// quantities. No canonical trades, tape, Pine/corpus, reference or grader.
#include "reservation_expansion_test_access.hpp"
#include <cstdio>
#include <cstring>
#include <functional>
namespace prior_growth_mirror {
#include "fixtures/reservation_expansion/ff54_pending_order_mirror.hpp"
}
using namespace reservation_test;
namespace {
int checks=0,failures=0;
#define CHECK(x) do {++checks;if(!(x)){++failures;std::fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);}}while(0)
#define OLD_FIELD(name, type) static_assert(offsetof(pf_pending_order_v1_t,name)==offsetof(prior_growth_mirror::pf_pending_order_v1_t,name),"ff54 offset " #name);
#include "fixtures/reservation_expansion/ff54-fields.inc"
#undef OLD_FIELD
static_assert(offsetof(pf_pending_order_v1_t,reservation_expansion_position_cycle)>=sizeof(prior_growth_mirror::pf_pending_order_v1_t),"preserve ff54 trailing padding");
void history(const PendingOrder& e,double units=10,double basis=10,bool partial=false) {
    CHECK(e.quantity_request.reservation());
    if(!e.quantity_request.reservation())return;
    CHECK(e.quantity_request.reservation()->units==units);
    CHECK(e.quantity_request.reservation()->basis_units==basis);
    CHECK(e.quantity_request.is_partial(1e-9,1e-9)==partial);
}
void capture_and_committed_growth() {
    Book b;b.standard(3,"source");
    CHECK(b.owner()==50 && b.get("A").incarnation==41);
    CHECK(b.get("E").reservation_expansion.capture()->position_cycle==7);
    CHECK(b.get("E").reservation_expansion.capture()->side==PositionSide::LONG);
    CHECK(b.live_all() && b.get("E").quantity_request.requests_all());
    // A requested 3. Native OCA reduces its executable capacity to 2 before
    // dispatch; growth must consume committed delta, not the original request.
    CHECK(b.get("A").qty==3);b.reduce("source",1);CHECK(b.get("A").qty==2);
    b.fire("A");CHECK(b.quantity()==12 && b.get("E").qty==12);history(b.get("E"));
    CHECK(b.live_all());b.close_partial(2);CHECK(b.quantity()==10);history(b.get("E"));
    b.fire("E");CHECK(!b.has("E") && b.quantity()==0);
    Book short_side;short_side.add("seed",10,false);short_side.fire("seed");short_side.advance();
    short_side.add("A",2,false);short_side.exit();CHECK(short_side.live_all());
    CHECK(short_side.get("E").reservation_expansion.capture()->side==PositionSide::SHORT);
    short_side.fire("A");CHECK(short_side.quantity()==12 && short_side.get("E").qty==12);
    ReservationGrowthSource invalid_source;
    for(auto ids:std::vector<std::pair<uint64_t,uint64_t>>{{0,50},{41,0},{41,41}}) {
        bool threw=false;try{invalid_source.assign_capture(ids.first,ids.second);}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
    }
    ReservationExpansion invalid;
    for(auto f:std::vector<std::function<void()>>{
        [&]{invalid.capture(0,7,PositionSide::LONG,10);},
        [&]{invalid.capture(50,0,PositionSide::LONG,10);},
        [&]{invalid.capture(50,7,PositionSide::FLAT,10);},
        [&]{invalid.capture(50,7,PositionSide::LONG,0);},
        [&]{invalid.close_population(0);}}) {
        bool threw=false;try{f();}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
    }
    invalid.capture(50,7,PositionSide::LONG,10);
    bool threw=false;try{invalid.capture(50,8,PositionSide::LONG,10);}catch(const std::invalid_argument&){threw=true;}CHECK(threw);
}
void closure_is_causal() {
    Book b;b.standard();b.add("B",4);const auto cause=b.get("B").incarnation;
    CHECK(cause==51 && b.closure()==51 && !b.live_all());
    b.cancel("B");CHECK(b.closure()==51);b.fire("A");CHECK(b.get("E").qty==12);
    b.add("C",4);CHECK(b.closure()==51);b.fire("C");CHECK(b.quantity()==16 && b.get("E").qty==12);
    b.fire("E");CHECK(!b.has("E") && b.quantity()==4);
    Book rejected;rejected.standard();rejected.cap(1);rejected.pooc(false);rejected.priced("B");
    CHECK(!rejected.has("B") && rejected.closure()==0 && rejected.live_all());
    Book declined;declined.standard();declined.add("B");auto first=declined.get("B").incarnation;
    declined.halt(true);declined.fire("B");CHECK(!declined.has("B") && declined.closure()==first);
    CHECK(declined.get("E").qty==10);declined.halt(false);declined.fire("A");CHECK(declined.get("E").qty==12);
    for(bool raw:{false,true}) {
        Book later;later.standard();later.advance();
        if(raw)later.raw("later");else later.priced("later");
        CHECK(later.closure()==later.get("later").incarnation);
        later.cancel("later");CHECK(!later.live_all());
    }
}
void priority_is_not_identity() {
    Book b;b.standard();const auto priority=b.get("A").created_seq;
    CHECK(priority<b.get("E").created_seq);b.add("A",2);
    CHECK(b.get("A").incarnation==51 && b.get("A").created_seq==priority);
    CHECK(b.closure()==51 && b.owner()==0);b.fire("A");CHECK(b.quantity()==12 && b.get("E").qty==10);
    Book rejected;rejected.standard();rejected.cap(1);rejected.pooc(false);rejected.priced("A");
    CHECK(!rejected.has("A") && rejected.closure()==0);CHECK(rejected.get("E").qty==10);
}
void target_death_and_recapture() {
    Book dead;dead.standard();dead.cancel("E");CHECK(dead.owner()==50);dead.fire("A");CHECK(dead.quantity()==12 && !dead.has("E"));
    Book recapture;recapture.standard();recapture.cancel("E");recapture.exit();auto fresh=recapture.get("E").incarnation;
    CHECK(fresh!=50 && recapture.owner()==fresh && recapture.closure()==0);
    recapture.sort_book();recapture.fire("A");CHECK(recapture.get("E").qty==12);
    Book replacement;replacement.standard();auto priority=replacement.get("E").created_seq;replacement.exit();
    CHECK(replacement.get("E").created_seq==priority && replacement.owner()==replacement.get("E").incarnation);
    replacement.fire("A");CHECK(replacement.get("E").qty==12);
    for(int variant=0;variant<4;++variant) {
        Book b;b.standard();
        if(variant==0)b.exit("E",10);
        if(variant==1){b.raw("mixed");b.exit();}
        if(variant==2){b.advance();b.exit();}
        if(variant==3){b.priced("mixed");b.exit();}
        CHECK(!b.get("E").reservation_expansion.capture() && b.owner()==50);
        b.fire("A");CHECK(b.get("E").qty==10);
    }
    Book zero;zero.standard();zero.exit("E",0);CHECK(!zero.has("E") && zero.owner()==50);
    Book inert;inert.standard();inert.inert();CHECK(!inert.has("E") && inert.owner()==50);
    inert.exit("E",10);inert.fire("A");CHECK(inert.get("E").qty==10);
    Book overwritten;overwritten.standard();overwritten.raw("E");CHECK(overwritten.get("E").type==OrderType::RAW_ORDER);
    overwritten.fire("A");CHECK(overwritten.get("E").qty==2 && overwritten.owner("E")==0);
    Book canceled_source;canceled_source.standard();canceled_source.cancel("A");CHECK(canceled_source.live_all());
    canceled_source.cancel_all();CHECK(canceled_source.book().empty());
}
void attempts_and_retirement() {
    Book no_effect;no_effect.standard();no_effect.cap(1);no_effect.fire("A",false);
    CHECK(no_effect.quantity()==10 && no_effect.get("E").qty==10 && no_effect.owner()==50);
    CHECK(no_effect.retired.size()==1);no_effect.cap(20);no_effect.fire("A",false);
    CHECK(no_effect.quantity()==10 && no_effect.get("E").qty==10 && no_effect.retired.size()==1);no_effect.finish();CHECK(!no_effect.has("A"));
    Book zero;zero.standard(0);zero.fire("A",false);CHECK(zero.quantity()==10 && zero.get("E").qty==10 && zero.retired.size()==1);
    Book decline;decline.standard();decline.halt(true);decline.fire("A",false);CHECK(decline.retired.size()==1 && decline.get("E").qty==10);
    decline.halt(false);decline.fire("A",false);CHECK(decline.quantity()==10 && decline.retired.size()==1);
    Book paid;paid.standard();paid.fire("A",false);CHECK(paid.quantity()==12 && paid.get("E").qty==12 && paid.owner()==50);
    const auto fingerprint=paid.broker_state_hash();paid.fire("A",false);CHECK(paid.broker_state_hash()==fingerprint && paid.retired.size()==1);
    paid.finish();CHECK(!paid.has("A"));
    Book flat;flat.standard();flat.flatten();flat.fire("A");CHECK(flat.quantity()==2);CHECK(flat.get("E").qty==10);
    Book reverse;reverse.standard();reverse.flatten();reverse.open(false,10);reverse.fire("A");
    CHECK(reverse.cycle()!=7);if(reverse.has("E"))CHECK(reverse.get("E").qty==10);
    // Placement side is independently required, even with an exact live owner.
    Book wrong_side;wrong_side.standard();wrong_side.get("A").created_position_side=PositionSide::SHORT;
    wrong_side.fire("A");CHECK(wrong_side.get("E").qty==10);
}
void oca_capacity_and_history() {
    Book b;b.seed();b.add("A",2);b.add("D",1);b.exit("E",missing,"G");b.add("B");b.cancel("B");
    b.fire("A");CHECK(b.get("E").qty==12);b.reduce("G",4);CHECK(b.get("E").qty==8);history(b.get("E"));
    b.fire("D");CHECK(b.get("E").qty==9);history(b.get("E"));
    Book terminal;terminal.standard(2,"","G");terminal.reduce("G",10);CHECK(!terminal.has("E") && terminal.owner()==50);
    terminal.fire("A");CHECK(!terminal.has("E"));
    Book open;open.standard(2,"","G");open.reduce("G",4);CHECK(open.get("E").qty==6 && open.live_all());
    open.fire("A");CHECK(open.get("E").qty==8);history(open.get("E"));open.fire("E");CHECK(open.quantity()==0 && !open.has("E"));
    // Shared OCA membership is eligible. Growth happens BEFORE source reduction.
    Book ordered;ordered.standard(2,"G","G");ordered.get("A").oca_type=2;
    ordered.fire("A");CHECK(ordered.quantity()==12 && ordered.get("E").qty==10);
    Book cancel;cancel.standard(2,"G","G");cancel.get("A").oca_type=1;cancel.fire("A");CHECK(!cancel.has("E"));
    Book source;source.standard(2,"S");source.cancel_group("S");CHECK(!source.has("A") && source.live_all());
}
void multiple_tracker_witness() {
    Book b;const double d=std::ldexp(1.0,-31);b.seed(1);b.add("A",1);b.exit("E1",missing,"G");b.step();
    CHECK(b.quantity()==2 && b.get("E1").qty==2);
    b.raw("R",2-d,true,"G",2);b.step();CHECK(b.get("E1").qty==d && b.quantity()==4-d);
    b.advance();b.add("B",1);b.exit("E2");CHECK(b.owner("B")==b.get("E2").incarnation);
    CHECK(b.get("E2").qty==4-2*d && !b.get("E2").quantity_request.is_partial(1e-9,1e-9));
    CHECK(b.get("E1").reservation_expansion.capture() && b.get("E2").reservation_expansion.capture());
    b.step();CHECK(b.quantity()==5-d && b.get("E1").qty==d && b.get("E2").qty==5-2*d);
    std::printf("NEW WITNESS: d=%.17g live=%.17g E1=%.17g E2=%.17g; exact E2 receives B\n",d,b.quantity(),b.get("E1").qty,b.get("E2").qty);
    // A still-pending source can be explicitly reassigned by a second valid
    // tolerance-edge capture; only the new target receives the dispatch.
    Book rebind;rebind.seed(1);rebind.add("A",1);rebind.exit("E1",missing,"G");
    rebind.reduce("G",1-d);rebind.exit("E2");CHECK(rebind.owner()==rebind.get("E2").incarnation);
    rebind.fire("A");CHECK(rebind.get("E1").qty==d && rebind.get("E2").qty==2-d);
}
void cycle_retirement_and_dormancy() {
    Book b;b.standard();b.flatten();CHECK(b.get("E").reservation_expansion.capture()->position_cycle==7);
    CHECK(!b.live_all());b.open(true,20);CHECK(b.cycle()==8 && b.closure()==0 && !b.live_all());
    CHECK(b.get("E").leg_activation.bounds()->position_cycle==8);
    b.fire("A");CHECK(b.quantity()==22 && b.get("E").qty==10);
    b.fire("E");CHECK(b.quantity()==12 && !b.has("E")); // finite reservation still works
    Book raw;raw.standard();raw.flatten();raw.raw("fresh",20);raw.fire("fresh");CHECK(raw.cycle()==8);
    raw.fire("A");CHECK(raw.get("E").qty==10 && !raw.live_all());
    Book recaptured;recaptured.standard();recaptured.flatten();recaptured.open(true,20);recaptured.exit();
    CHECK(recaptured.get("E").reservation_expansion.capture()->position_cycle==8 && recaptured.live_all());
    // Source birth cycle 7 is not a Pine exclusion: intentional cycle-8 capture.
    CHECK(recaptured.get("A").created_position_cycle_seq==7);recaptured.fire("A");CHECK(recaptured.get("E").qty==22);
    Book retired;retired.standard();retired.retired.push_back(50);retired.fire("A",false);
    CHECK(retired.has("E") && retired.get("E").qty==10 && retired.quantity()==12);retired.finish();CHECK(!retired.has("E"));
    Book dormant;dormant.standard();lifecycle_fixture::suspend(dormant.get("E"));
    dormant.fire("E",false);CHECK(dormant.retired.empty() && dormant.live_all());
    dormant.fire("A");CHECK(dormant.get("E").qty==12 && dormant.get("E").legs.dormant());
    dormant.revive(100);CHECK(!dormant.get("E").legs.dormant() && dormant.live_all());
    dormant.fire("E");CHECK(dormant.quantity()==0 && !dormant.has("E"));
    Book direct;direct.standard();lifecycle_fixture::suspend(direct.get("E"));direct.revive(80);
    CHECK(direct.quantity()==0 && !direct.has("E")); // direct revival close erases receiver
    if(direct.has("A")){CHECK(direct.owner()==50);direct.fire("A");CHECK(!direct.has("E"));}
    Book rearmed;rearmed.standard();lifecycle_fixture::suspend(rearmed.get("E"));rearmed.exit();
    CHECK(rearmed.get("E").legs.dormant() && rearmed.get("E").legs.pending_replacement());
    CHECK(rearmed.owner()==rearmed.get("E").incarnation && rearmed.closure()==0);
    rearmed.fire("A");CHECK(rearmed.get("E").qty==12);
    Book copy;copy.standard();Book same=copy;CHECK(copy.broker_state_hash()==same.broker_state_hash());
    copy.reset();CHECK(copy.book().empty());CHECK(same.owner()==50 && same.live_all());
    copy.seed();copy.add("A");CHECK(copy.owner()==0);
}
void mirror_and_fingerprint() {
    Book b;b.standard();pf_pending_order_v1_t m{};fill_pending_order_mirror(b.get("E"),&m);
    CHECK(m.pooc_global_full_exit_dynamic_qty==1 && m.pooc_global_full_exit_tracks_bound_adds==1);
    CHECK(m.reservation_expansion_present==1 && m.reservation_expansion_position_cycle==7 && m.reservation_expansion_side==1);
    CHECK(m.reservation_expansion_first_later_admission_present==0 && m.reservation_expansion_first_later_admission==0);
    b.add("B");fill_pending_order_mirror(b.get("E"),&m);
    CHECK(m.pooc_global_full_exit_dynamic_qty==0 && m.reservation_expansion_first_later_admission_present==1 && m.reservation_expansion_first_later_admission==51);
    b.cancel("E");fill_pending_order_mirror(b.get("A"),&m);
    CHECK(m.pooc_global_full_exit_bound_add==1 && m.reservation_growth_source_present==1 && m.reservation_growth_source_reservation_owner==50);
    fill_pending_order_mirror(b.get("B"),&m);CHECK(m.pooc_global_full_exit_bound_add==0 && m.reservation_growth_source_present==0 && m.reservation_growth_source_reservation_owner==0);
    Book base;base.standard();base.add("B");const auto hash=base.broker_state_hash();
    const std::vector<std::pair<const char*,std::function<void(Book&)>>> mutations={
        {"capture presence",[](Book& x){x.get("E").reservation_expansion={};}},
        {"cycle",[](Book& x){auto& c=x.get("E").reservation_expansion;c={};c.capture(50,8,PositionSide::LONG,10);c.close_population(51);}},
        {"side",[](Book& x){auto& c=x.get("E").reservation_expansion;c={};c.capture(50,7,PositionSide::SHORT,10);c.close_population(51);}},
        {"closure presence",[](Book& x){auto& c=x.get("E").reservation_expansion;c={};c.capture(50,7,PositionSide::LONG,10);}},
        {"closure incarnation",[](Book& x){auto& c=x.get("E").reservation_expansion;c={};c.capture(50,7,PositionSide::LONG,10);c.close_population(53);}},
        {"source presence",[](Book& x){x.get("A").reservation_growth_source={};}},
        {"source owner",[](Book& x){x.get("A").reservation_growth_source.assign_capture(41,52);}},
    };
    const char* mutation_fields[]={"reservation_expansion_present", "reservation_expansion_position_cycle",
        "reservation_expansion_side", "reservation_expansion_first_later_admission_present",
        "reservation_expansion_first_later_admission", "reservation_growth_source_present",
        "reservation_growth_source_reservation_owner"};
    size_t mutation_index=0;
    for(const auto& test:mutations){
        Book changed=base;test.second(changed);CHECK(changed.broker_state_hash()!=hash);
        const char* id=mutation_index<5?"E":"A";
        pf_pending_order_v1_t before{},after{};
        fill_pending_order_mirror(base.get(id),&before);fill_pending_order_mirror(changed.get(id),&after);
        int count=0;const auto* fields=pending_order_layout(&count);const pf_field_desc_t* field=nullptr;
        for(int j=0;j<count;++j)if(std::strcmp(fields[j].name,mutation_fields[mutation_index])==0)field=&fields[j];
        CHECK(field!=nullptr);
        if(field) CHECK(std::memcmp(reinterpret_cast<unsigned char*>(&before)+field->offset,
                                  reinterpret_cast<unsigned char*>(&after)+field->offset,field->size)!=0);
        ++mutation_index;std::printf("hash/mirror mutation: %s\n",test.first);
    }
    int n=0;const auto* layout=pending_order_layout(&n);CHECK(n==PF_PENDING_ORDER_FIELD_COUNT);
    int i=0;
#define OLD_FIELD(field, ctype) CHECK(std::strcmp(layout[i].name,#field)==0); CHECK(std::strcmp(layout[i].type,ctype)==0); CHECK(layout[i].offset==offsetof(prior_growth_mirror::pf_pending_order_v1_t,field)); ++i;
#include "fixtures/reservation_expansion/ff54-fields.inc"
#undef OLD_FIELD
    CHECK(i==142);
    const char* new_fields[]={"reservation_expansion_position_cycle","reservation_expansion_present","reservation_expansion_side","reservation_expansion_first_later_admission_present","reservation_expansion_first_later_admission","reservation_growth_source_present","reservation_growth_source_reservation_owner"};
    for(auto name:new_fields){CHECK(std::strcmp(layout[i].name,name)==0);CHECK(layout[i].offset>=sizeof(prior_growth_mirror::pf_pending_order_v1_t));++i;}
    CHECK(strategy_pending_order_get(&base,0,&m,sizeof(m))==0);
    std::vector<unsigned char> bytes(sizeof(m)+8,0xA5);
    CHECK(strategy_pending_order_get(&base,0,bytes.data(),sizeof(prior_growth_mirror::pf_pending_order_v1_t))==0);
    CHECK(std::memcmp(bytes.data(),&m,sizeof(prior_growth_mirror::pf_pending_order_v1_t))==0);
    for(size_t j=sizeof(prior_growth_mirror::pf_pending_order_v1_t);j<bytes.size();++j)CHECK(bytes[j]==0xA5);
    std::printf("mirror: old_fields=142 new_fields=155 old_size=%zu new_size=%zu first_append=%zu\n",sizeof(prior_growth_mirror::pf_pending_order_v1_t),sizeof(m),offsetof(pf_pending_order_v1_t,reservation_expansion_position_cycle));
}
void historical_quantity_and_selection() {
    Book finite;finite.seed();finite.exit("U",4,"G");history(finite.get("U"),4,10,true);
    finite.reduce("G",2);finite.close_partial(8);CHECK(finite.quantity()==2 && finite.get("U").qty==2);history(finite.get("U"),4,10,true);
    Book clipped;clipped.seed();clipped.exit("partial",4);clipped.add("A");clipped.exit();
    CHECK(!clipped.get("E").reservation_expansion.capture() && clipped.owner()==0);history(clipped.get("E"),6,10,true);
    Book explicit_all;explicit_all.seed();explicit_all.add("A");explicit_all.exit("E",10);CHECK(!explicit_all.live_all() && explicit_all.owner()==0);
    Book fraction;fraction.seed();fraction.add("A");fraction.exit("E",missing,"",100-std::ldexp(1.0,-31));
    CHECK(fraction.live_all() && !fraction.get("E").quantity_request.requests_all());
    CHECK(fraction.get("E").quantity_request.intent()->kind()==QuantityIntent::Kind::Fraction);
    const auto prior=fraction.get("E");fraction.exit();CHECK(fraction.get("E").incarnation!=prior.incarnation);
    CHECK(fraction.get("E").quantity_request.requests_all() && !prior.quantity_request.requests_all());
    // Other selection exclusions, unchanged Pine ownership.
    for(int kind=0;kind<5;++kind){Book x;x.seed();x.add("A");
        if(kind==0)x.pooc(false);
        if(kind==1)placement_fixture::at_capacity(x.get("A"));
        if(kind==2)x.get("A").is_long=false;
        if(kind==3)x.get("A").created_position_side=PositionSide::SHORT;
        if(kind==4)x.get("A").created_bar-=1;
        x.exit();CHECK(!x.get("E").reservation_expansion.capture() && x.owner()==0);
    }
}
}
int main(){
    const std::pair<const char*,void(*)()> cases[]={
        {"1 capture/committed growth",capture_and_committed_growth},{"2 causal closure",closure_is_causal},
        {"3 priority versus incarnation",priority_is_not_identity},{"4 death/rearm/recapture",target_death_and_recapture},
        {"5 attempts/retirement",attempts_and_retirement},{"6 OCA capacity",oca_capacity_and_history},
        {"7 old/new multiple-tracker witness",multiple_tracker_witness},{"8 cycle/retirement/dormancy",cycle_retirement_and_dormancy},
        {"9 mirror/fingerprint",mirror_and_fingerprint},{"10 historical intent/selection",historical_quantity_and_selection}};
    for(auto test:cases){std::printf("contract: %s\n",test.first);try{test.second();}catch(const std::exception& e){++failures;std::fprintf(stderr,"FAIL contract %s: %s\n",test.first,e.what());}}
    std::printf("%d checks, %d failures\n",checks,failures);return failures?1:0;
}
