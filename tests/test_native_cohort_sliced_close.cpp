// R5 lane K-COHORT: a cohort close selects each opening on its roster once,
// however many lots that opening's fills left.
//
// An entry capped at one unit a point (PointBudget) fills in slices, and each
// slice books its own lot under the entry's one incarnation. A close bound to a
// roster (BindCohort) listed that incarnation once per lot, the selected
// settlement refuses a duplicate identity (InvalidCloseTarget), and the run
// failed: "native settlement inspection failed". Every cohort run here is
// judged against twins that close the same openings without a roster:
//
//   selected   BindOpenings over the roster's members -- the same selection,
//              so every closed row, the book after and the fill equal the
//              cohort run's bit for bit, and so do the events (one sibling's
//              fate aside, see judge);
//   per-entry  one BindOpening close per member, each taking that member's
//              share -- the rows equal the cohort run's lot for lot. Its two
//              tickets split exactly as the cohort's one only when the fees
//              are binary-exact, so it runs under the per-unit fee, and under
//              the percent fee only where the roster holds one opening.
//
// The shapes: long and short; the sliced entry alone, beside an entry outside
// the roster, or with that entry enrolled; a full close and a partial one; the
// entry filled whole in one slice (the control, which passed before the fix),
// in three slices, or in two with the third still working at the close; no
// sibling, an OCA pair of roster closes under either group effect, or a
// bracket on the second entry. The host's terms facts name each member once;
// a read-only preview of the roster close is refused, as its execution is,
// where the same selection's preview settles. Source-free: the kernel-only
// profile registers the row.
#include <pineforge/native_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

int checks = 0;
int failures = 0;
std::string scenario = "setup";

#define CHECK(condition) do {                                                    \
    ++checks;                                                                    \
    if (!(condition)) {                                                          \
        ++failures;                                                              \
        std::printf("FAIL [%s] line %d: %s\n", scenario.c_str(), __LINE__,      \
                    #condition);                                                 \
    }                                                                            \
} while (0)

constexpr std::int64_t kStart = 1736121600000LL;

enum class Side { Long, Short };
enum class Others { None, Outside, Enrolled };   // entry B: absent, off the roster, on it
enum class Slices { Whole, Three, TwoWorking };  // how entry A has filled at the close
enum class Size { Full, Partial };               // what the host resolves each close to
enum class Sibling { None, OcaCancel, OcaReduce, Bracket };
enum class Fee { PerUnit, Percent };
enum class Form { Cohort, Selected, PerEntry };  // how the close binds its openings

struct Shape {
    Side side = Side::Long;
    Others others = Others::None;
    Slices slices = Slices::Three;
    Size size = Size::Full;
    Sibling sibling = Sibling::None;
    Fee fee = Fee::PerUnit;
};

std::string name(const Shape& s) {
    static const char* sides[] = {"long", "short"};
    static const char* others[] = {"alone", "outside", "enrolled"};
    static const char* slices[] = {"whole", "three", "two-working"};
    static const char* sizes[] = {"full", "partial"};
    static const char* siblings[] = {"none", "oca-cancel", "oca-reduce", "bracket"};
    static const char* fees[] = {"per-unit", "percent"};
    return std::string(sides[int(s.side)]) + "/" + others[int(s.others)] + "/"
        + slices[int(s.slices)] + "/" + sizes[int(s.size)] + "/"
        + siblings[int(s.sibling)] + "/" + fees[int(s.fee)];
}

bool oca(const Shape& s) {
    return s.sibling == Sibling::OcaCancel || s.sibling == Sibling::OcaReduce;
}

// One closed row, every field the kernel books.
struct Row {
    std::int64_t entry_time = 0, exit_time = 0;
    double entry_price = 0, exit_price = 0, qty = 0, pnl = 0, pnl_pct = 0;
    double commission = 0, runup = 0, drawdown = 0;
    bool is_long = false, from_bracket = false, open_at_end = false;
    int entry_bar = -1, exit_bar = -1;
    std::string entry_id, entry_comment, exit_id, exit_comment;
    std::uint64_t entry_incarnation = 0;
    ex::CloseCause cause = ex::CloseCause::Unspecified;

    auto key() const {
        return std::tie(entry_time, exit_time, entry_price, exit_price, qty, pnl, pnl_pct,
                        commission, runup, drawdown, is_long, from_bracket, open_at_end,
                        entry_bar, exit_bar, entry_id, entry_comment, exit_id, exit_comment,
                        entry_incarnation, cause);
    }
    bool operator==(const Row& other) const { return key() == other.key(); }
};

Row row_of(const Trade& t) {
    Row r;
    r.entry_time = t.entry_time; r.exit_time = t.exit_time;
    r.entry_price = t.entry_price; r.exit_price = t.exit_price;
    r.qty = t.qty; r.pnl = t.pnl; r.pnl_pct = t.pnl_pct; r.commission = t.commission;
    r.runup = t.max_runup; r.drawdown = t.max_drawdown;
    r.is_long = t.is_long; r.from_bracket = t.exit_from_bracket; r.open_at_end = t.open_at_end;
    r.entry_bar = t.entry_bar_index; r.exit_bar = t.exit_bar_index;
    r.entry_id = t.entry_id; r.entry_comment = t.entry_comment;
    r.exit_id = t.exit_id; r.exit_comment = t.exit_comment;
    r.entry_incarnation = t.entry_incarnation; r.cause = t.close_cause;
    return r;
}

// One open lot as native_open_lots copies it out.
struct Lot {
    std::uint64_t incarnation = 0;
    std::int64_t cycle = 0, entry_time = 0;
    double price = 0, units = 0, commission = 0;
    std::string label;
    auto key() const { return std::tie(incarnation, cycle, entry_time, price, units, commission, label); }
    bool operator==(const Lot& other) const { return key() == other.key(); }
};

struct Outcome {
    bool completed = false;
    std::string error;
    NativeFailure failure{};
    std::uint64_t a = 0, b = 0;           // the two entries' incarnations
    std::vector<Row> rows;
    std::vector<Lot> lots;                // the book the run ended with
    std::vector<Lot> at_close;            // the book the close was submitted against
    double units = 0, equity = 0;
    std::vector<std::size_t> kinds;       // every command event, by alternative
    std::size_t kinds_through_fill = 0;   // how many of them end with the close's last fill
    std::vector<no::ExecutionAppliedEvent> closes;  // applied X / TP / SL fills
    std::size_t group_cancels = 0, owner_gone = 0;
    std::vector<std::vector<std::uint64_t>> scopes;  // the terms facts' selection, per resolve
    std::vector<double> exposures;                    // ... and its exposure
    std::optional<NativeCurrentExecutionPreview> preview;
};

class Host : public NativeStrategyHost {
public:
    Shape shape;
    std::map<std::uint64_t, double> shares;  // a per-entry partial close's units, by opening

    // The preview scenario: at this print, submit a market close bound as
    // `preview_owner`, preview it, and withdraw it before it can match.
    int preview_at = -1;
    std::optional<no::Owner> preview_owner;
    std::optional<NativeCurrentExecutionPreview> preview;
    int prints = 0;

    mutable std::vector<std::vector<std::uint64_t>> scopes;
    mutable std::vector<double> exposures;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    void on_native_tick(const Bar&, const NativeTickContext&) override {
        if (prints++ != preview_at || !preview_owner) return;
        no::Request close{no::HostSized{no::HostSizedKind::Close, std::nullopt}, "X", ""};
        close.owner = *preview_owner;
        const auto accepted = submit(close);
        if (!accepted.handle) return;
        preview = inspect_current_execution(NativeCurrentExecution{
            *accepted.handle, NativeCurrentPriceRule::AsPresented});
        cancel(*accepted.handle);
    }

    no::ExecutionTerms resolve_execution_terms(const NativeExecutionTermsFacts& facts) const override {
        no::ExecutionTerms terms{facts.default_resolved_price, std::nullopt,
                                 no::OpeningShape::Transact};
        const auto& request = facts.definition->request;
        if (!std::holds_alternative<no::HostSized>(request.intent)) return terms;
        const auto* selected = std::get_if<no::SelectedExposure>(&facts.scope);
        scopes.push_back(selected ? selected->incarnations : std::vector<std::uint64_t>{});
        exposures.push_back(facts.scope_exposure_units);
        double units = facts.scope_exposure_units;
        if (shape.size == Size::Partial) {
            if (const auto* one = std::get_if<no::BindOpening>(&request.owner)) {
                const auto share = shares.find(one->opening.incarnation);
                units = share == shares.end() ? 0.0 : share->second;
            } else {
                units = std::min(1.5, facts.scope_exposure_units);
            }
        }
        terms.units = units;
        return terms;
    }
};

NativeRunSpec make_spec(const Shape& shape) {
    NativeRunSpec spec;
    spec.identity = {"k-cohort", 1};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:KCOHORT";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.01;
    spec.fee_kind = shape.fee == Fee::PerUnit ? NativeFeeKind::CashPerUnit : NativeFeeKind::Percent;
    spec.fee_value = shape.fee == Fee::PerUnit ? 0.25 : 0.1;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    return spec;
}

Lot lot_of(const NativeOpenLot& lot) {
    return {lot.entry_incarnation, lot.cycle, lot.entry_time_ms, lot.entry_price,
            lot.signed_units, lot.entry_commission, lot.entry_label};
}

// One run. Entry A is a limit at 100 for three units, one a print unless it
// fills whole; entry B is a market order for two units that fills while A
// waits above its limit, so the book reads A, B, A, A. Short mirrors every
// price about 100.
Outcome run(const Shape& shape, Form form, const std::map<std::uint64_t, double>& shares = {},
            bool preview_only = false) {
    Outcome out;
    Host h;
    h.shape = shape;
    h.shares = shares;
    const double s = shape.side == Side::Long ? 1.0 : -1.0;
    const auto px = [&](double p) { return shape.side == Side::Long ? p : 200.0 - p; };
    if (h.configure_native(make_spec(shape)).status != NativeSetupStatus::Applied) {
        out.error = "configure_native refused the spec";
        return out;
    }
    const Bar warmup{100, 100, 100, 100, 1, kStart - 60000};
    if (!h.stream_begin(&warmup, 1, "1", "1")) {
        out.error = "stream_begin refused";
        return out;
    }
    int n = 0;
    const auto tick = [&](double price) {
        h.stream_push_tick(TradeTick{kStart + n, std::uint64_t(n + 1), px(price), 1});
        ++n;
        // The text the failure latched with; later calls answer another.
        if (out.error.empty() && h.native_state().kind == NativeLifecycleKind::Failed)
            out.error = h.last_error();
    };
    const auto put = [&](const no::Request& request) {
        const auto result = h.submit(request);
        if (!result.handle && out.error.empty()) out.error = "a request was rejected: " + request.label;
        return result.handle;
    };

    no::Request first{no::Transact{3 * s}, "A", ""};
    first.trigger = no::Limit{100.0};
    if (shape.slices != Slices::Whole) first.capacity = no::PointBudget{1.0};
    const auto a = put(first);
    if (!a) return out;
    out.a = a->incarnation;
    // Every form keeps the same roster calls, so the twins differ in the
    // close's owner alone.
    const auto cohort = h.cohort_open();
    h.cohort_add(cohort, *a);
    tick(100);                                           // A: its first slice (or all of it)
    std::optional<no::RequestHandle> b;
    if (shape.others != Others::None) {
        b = put(no::Request{no::Transact{2 * s}, "B", ""});
        if (!b) return out;
        out.b = b->incarnation;
        if (shape.others == Others::Enrolled) h.cohort_add(cohort, *b);
        if (shape.sibling == Sibling::Bracket) {
            no::WaitForApplied wait;
            wait.parent = *b;
            no::Request take{no::Reduce{no::ExplicitUnits{2}}, "B-tp", ""};
            take.trigger = no::Limit{px(130)};
            take.owner = wait;
            take.group = no::Member{50, 1, no::GroupEffect::Cancel};
            no::Request stop{no::Reduce{no::ExplicitUnits{2}}, "B-sl", ""};
            stop.trigger = no::Stop{px(70)};
            stop.owner = wait;
            stop.group = no::Member{50, 2, no::GroupEffect::Cancel};
            if (!put(take) || !put(stop)) return out;
        }
    }
    tick(104);                                           // B fills; A waits at its limit
    tick(100);                                           // A: its second slice
    if (shape.slices != Slices::TwoWorking) tick(100);   // A: its third slice

    const auto book = h.native_open_lots(px(100));
    for (const auto& lot : book) out.at_close.push_back(lot_of(lot));
    const std::int64_t cycle = book.empty() ? 0 : book.front().cycle;
    std::vector<no::RequestHandle> members{*a};
    if (shape.others == Others::Enrolled) members.push_back(*b);

    std::vector<no::Owner> owners;
    if (form == Form::Cohort) owners.push_back(no::BindCohort{cohort});
    if (form == Form::Selected) owners.push_back(no::BindOpenings{members, cycle});
    if (form == Form::PerEntry) {
        for (const auto& member : members) {
            if (shape.size == Size::Partial && !shares.count(member.incarnation)) continue;
            owners.push_back(no::BindOpening{member, cycle});
        }
    }
    const no::HostSized sized{no::HostSizedKind::Close, std::nullopt};
    if (preview_only) {
        h.preview_at = n;
        h.preview_owner = owners.front();
    } else {
        for (const auto& owner : owners) {
            if (oca(shape)) {
                const auto effect = shape.sibling == Sibling::OcaCancel ? no::GroupEffect::Cancel
                                                                         : no::GroupEffect::Reduce;
                no::Request take{sized, "TP", ""};
                take.trigger = no::Limit{px(108)};
                take.owner = owner;
                take.group = no::Member{60, 1, effect};
                no::Request stop{sized, "SL", ""};
                stop.trigger = no::Stop{px(90)};
                stop.owner = owner;
                stop.group = no::Member{60, 2, effect};
                if (!put(take) || !put(stop)) return out;
            } else {
                no::Request close{sized, "X", ""};
                close.owner = owner;
                if (!put(close)) return out;
            }
        }
    }
    tick(110);                                           // the close fills (TP, for a pair)
    if (shape.slices == Slices::TwoWorking) tick(100);   // A: its third slice, after the close
    tick(110);
    h.stream_end(false);

    const auto state = h.native_state();
    out.completed = state.kind == NativeLifecycleKind::Completed;
    out.failure = state.failure;
    if (out.error.empty()) out.error = h.last_error();
    for (int i = 0; i < h.trade_count(); ++i) out.rows.push_back(row_of(h.get_trade(i)));
    for (const auto& lot : h.native_open_lots(px(100))) out.lots.push_back(lot_of(lot));
    out.units = h.physical_position().signed_units;
    out.equity = h.native_marked_equity(px(100));
    for (const auto& event : h.native_events(0)) {
        if (!event.command) continue;
        out.kinds.push_back(event.command->index());
        if (const auto* applied = std::get_if<no::ExecutionAppliedEvent>(&*event.command)) {
            const auto& label = applied->request().label;
            if (label == "X" || label == "TP" || label == "SL") {
                out.closes.push_back(*applied);
                out.kinds_through_fill = out.kinds.size();
            }
        } else if (const auto* cancelled = std::get_if<no::CancelledEvent>(&*event.command)) {
            if (cancelled->reason == no::CancelReason::Group) ++out.group_cancels;
            if (cancelled->reason == no::CancelReason::OwnerGone) ++out.owner_gone;
        }
    }
    out.scopes = h.scopes;
    out.exposures = h.exposures;
    out.preview = h.preview;
    return out;
}

// The first few cohort runs that did not complete say why, verbatim.
int reported = 0;
void report_failure(const Outcome& o) {
    if (o.completed || ++reported > 3) return;
    std::printf("  [%s] cohort run failed: code=%u operation=%u discriminator=%u ordinal=%llu"
                " last_error=\"%s\"\n",
                scenario.c_str(), unsigned(o.failure.code), unsigned(o.failure.operation),
                unsigned(o.failure.discriminator),
                static_cast<unsigned long long>(o.failure.ordinal), o.error.c_str());
}

std::vector<Row> by_lot(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
        return std::tie(x.exit_time, x.entry_time, x.entry_incarnation, x.qty)
             < std::tie(y.exit_time, y.entry_time, y.entry_incarnation, y.qty);
    });
    return rows;
}

void print_rows(const char* title, const Outcome& left, const Outcome& right) {
    std::printf("  %s [%s]\n", title, scenario.c_str());
    const auto l = by_lot(left.rows);
    const auto r = by_lot(right.rows);
    for (std::size_t i = 0; i < std::max(l.size(), r.size()); ++i) {
        const auto print = [](const std::vector<Row>& rows, std::size_t at) {
            if (at >= rows.size()) { std::printf("%-58s", "  (none)"); return; }
            const Row& row = rows[at];
            std::printf("  %s@t%lld qty=%-4g in=%-6g out=%-6g pnl=%-9.6g fee=%-7.5g cause=%u",
                        row.entry_id.c_str(),
                        static_cast<long long>(row.entry_time - kStart), row.qty,
                        row.entry_price, row.exit_price, row.pnl, row.commission,
                        unsigned(row.cause));
        };
        print(l, i);
        std::printf(" |");
        print(r, i);
        std::printf("\n");
    }
}

long shapes_run = 0, cohort_completed = 0, selected_rows = 0, per_entry_rows = 0;

void judge(const Shape& shape, bool show) {
    scenario = name(shape);
    ++shapes_run;
    const Outcome cohort = run(shape, Form::Cohort);
    const Outcome selected = run(shape, Form::Selected);
    CHECK(selected.completed);
    CHECK(cohort.completed);
    report_failure(cohort);
    if (!cohort.completed || !selected.completed) return;
    ++cohort_completed;

    // The roster's members as the close saw them: each opening once, in
    // incarnation order, with every open unit of each.
    std::vector<std::uint64_t> members{cohort.a};
    if (shape.others == Others::Enrolled) members.push_back(cohort.b);
    double exposure = 0;
    for (const auto& lot : cohort.at_close) {
        if (std::find(members.begin(), members.end(), lot.incarnation) != members.end())
            exposure += lot.units < 0 ? -lot.units : lot.units;
    }
    CHECK(!cohort.scopes.empty());
    for (const auto& scope : cohort.scopes) CHECK(scope == members);
    for (const double units : cohort.exposures) CHECK(units == exposure);
    CHECK(!cohort.closes.empty());
    for (const auto& close : cohort.closes) {
        const auto* scope = std::get_if<no::SelectedExposure>(&close.scope);
        CHECK(scope && scope->incarnations == members);
    }
    const double wanted = shape.size == Size::Full ? exposure : std::min(1.5, exposure);
    double closed = 0;
    for (const auto& close : cohort.closes) closed += close.closed_units;
    CHECK(closed == wanted);
    // A full close leaves no lot of a member that was open at the close.
    if (shape.size == Size::Full) {
        for (const auto& lot : cohort.lots) {
            bool at_close = false;
            for (const auto& before : cohort.at_close) at_close = at_close || before == lot;
            CHECK(!at_close || std::find(members.begin(), members.end(), lot.incarnation)
                                    == members.end());
        }
    }

    // The same selection without a roster books the same run.
    CHECK(cohort.rows == selected.rows);
    CHECK(cohort.lots == selected.lots);
    CHECK(cohort.at_close == selected.at_close);
    CHECK(cohort.units == selected.units);
    CHECK(cohort.equity == selected.equity);
    CHECK(cohort.closes.size() == selected.closes.size());
    for (std::size_t i = 0; i < std::min(cohort.closes.size(), selected.closes.size()); ++i) {
        const auto& x = cohort.closes[i];
        const auto& y = selected.closes[i];
        CHECK(x.request().label == y.request().label);
        CHECK(x.ordinal == y.ordinal);
        CHECK(x.resolved_price == y.resolved_price);
        CHECK(x.current_ticket == y.current_ticket);
        CHECK(x.closed_units == y.closed_units);
        CHECK(x.opened_units == y.opened_units);
        CHECK(x.first_trade_index == y.first_trade_index);
        CHECK(x.closed_trade_count == y.closed_trade_count);
        CHECK(x.terminal == y.terminal);
        CHECK(x.cycle_before == y.cycle_before && x.cycle_after == y.cycle_after);
        const auto* xs = std::get_if<no::SelectedExposure>(&x.scope);
        const auto* ys = std::get_if<no::SelectedExposure>(&y.scope);
        CHECK(xs && ys && xs->cycle == ys->cycle && xs->incarnations == ys->incarnations);
    }
    CHECK(cohort.group_cancels == selected.group_cancels);
    // A roster is read at the match; a fixed selection expires with its
    // openings. So once a full close leaves a reduce-effect sibling live, the
    // selection's sibling is cancelled (OwnerGone) where the roster's keeps
    // waiting for its members. Every event through the close's fill is the
    // same one, and with no such sibling every event is.
    CHECK(cohort.kinds_through_fill == selected.kinds_through_fill);
    const std::size_t through = std::min({cohort.kinds_through_fill, selected.kinds_through_fill,
                                          cohort.kinds.size(), selected.kinds.size()});
    CHECK(std::equal(cohort.kinds.begin(), cohort.kinds.begin() + std::ptrdiff_t(through),
                     selected.kinds.begin()));
    if (!(shape.sibling == Sibling::OcaReduce && shape.size == Size::Full)) {
        CHECK(cohort.owner_gone == selected.owner_gone);
        CHECK(cohort.kinds == selected.kinds);
    }
    selected_rows += long(cohort.rows.size());
    if (show) print_rows("rows, cohort | selected", cohort, selected);

    // Closing each member by itself books the same rows, lot for lot.
    if (oca(shape) || (shape.fee == Fee::Percent && members.size() > 1)) return;
    std::map<std::uint64_t, double> shares;
    if (shape.size == Size::Partial) {
        for (const auto& row : selected.rows) {
            if (row.exit_id == "X") shares[row.entry_incarnation] += row.qty;
        }
    }
    const Outcome per_entry = run(shape, Form::PerEntry, shares);
    CHECK(per_entry.completed);
    if (!per_entry.completed) return;
    CHECK(by_lot(cohort.rows) == by_lot(per_entry.rows));
    CHECK(cohort.lots == per_entry.lots);
    CHECK(cohort.units == per_entry.units);
    CHECK(cohort.equity == per_entry.equity);
    CHECK(cohort.owner_gone == per_entry.owner_gone);
    double per_entry_closed = 0;
    for (const auto& close : per_entry.closes) per_entry_closed += close.closed_units;
    CHECK(per_entry_closed == closed);
    per_entry_rows += long(per_entry.rows.size());
    if (show) print_rows("rows, cohort | per-entry", cohort, per_entry);
}

// A read-only preview of the cohort close is refused as its execution is: a
// current execution admits no roster owner, whose members are read at the
// match (R5 lane PAR-ORDERS-2; expectation corrected: the cohort preview
// answered the selection's settlement -> it is refused as UnsupportedRequest,
// because the preview settled a request whose execute_current failed the run,
// "native current evaluated allowance mismatch"). The same selection's preview
// settles, and the run each preview is taken in completes either way.
long previews = 0;
void preview(Side side, Others others) {
    Shape shape;
    shape.side = side;
    shape.others = others;
    scenario = "preview/" + name(shape);
    const Outcome cohort = run(shape, Form::Cohort, {}, true);
    const Outcome selected = run(shape, Form::Selected, {}, true);
    CHECK(cohort.completed);
    CHECK(selected.completed);
    CHECK(cohort.rows.empty() && selected.rows.empty());
    CHECK(cohort.preview.has_value());
    CHECK(selected.preview.has_value());
    if (!cohort.preview || !selected.preview) return;
    const auto& x = *cohort.preview;
    const auto& y = *selected.preview;
    CHECK(x.refusal == NativeCurrentRefusal::UnsupportedRequest);
    CHECK(!x.terms_rejection && !x.settlement_readiness && x.closed_row_pnl.empty());
    CHECK(!y.refusal && !y.terms_rejection);
    CHECK(y.settlement_readiness == ex::Status::Applied);
    CHECK(y.closed_row_pnl.size() == (others == Others::Enrolled ? 4u : 3u));
    ++previews;
}

}  // namespace

int main() {
    for (const Fee fee : {Fee::PerUnit, Fee::Percent})
    for (const Side side : {Side::Long, Side::Short})
    for (const Others others : {Others::None, Others::Outside, Others::Enrolled})
    for (const Slices slices : {Slices::Whole, Slices::Three, Slices::TwoWorking})
    for (const Size size : {Size::Full, Size::Partial})
    for (const Sibling sibling : {Sibling::None, Sibling::OcaCancel, Sibling::OcaReduce,
                                  Sibling::Bracket}) {
        if (sibling == Sibling::Bracket && others == Others::None) continue;
        const Shape shape{side, others, slices, size, sibling, fee};
        const bool show = fee == Fee::PerUnit && others == Others::Enrolled
            && sibling == Sibling::None && slices != Slices::Whole
            && ((side == Side::Long && size == Size::Full)
                || (side == Side::Short && size == Size::Partial));
        judge(shape, show);
    }
    for (const Side side : {Side::Long, Side::Short})
        for (const Others others : {Others::None, Others::Enrolled}) preview(side, others);

    std::printf("%ld shapes, %ld cohort runs completed, %ld rows matched against the selected"
                " twin, %ld against the per-entry twin, %ld previews\n",
                shapes_run, cohort_completed, selected_rows, per_entry_rows, previews);
    std::printf("%d checks, %d failures\n", checks, failures);
    if (failures == 0) std::printf("test_native_cohort_sliced_close: ok\n");
    return failures == 0 ? 0 : 1;
}
