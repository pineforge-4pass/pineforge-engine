// Literal working-request core: event, counter, handle, grid, and eligibility
// transitions. No engine host, matching, settlement, calendar, or C ABI.
#include <pineforge/native_order.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

using pineforge::execution::Action;
using pineforge::execution::Flatten;
using pineforge::native_order::AcceptedEvent;
using pineforge::native_order::Birth;
using pineforge::native_order::CancelStatus;
using pineforge::native_order::CancelledEvent;
using pineforge::native_order::CommandEvent;
using pineforge::native_order::InvalidHandleEvent;
using pineforge::native_order::LiveRequest;
using pineforge::native_order::NotWorkingEvent;
using pineforge::native_order::quantity_on_grid;
using pineforge::native_order::RejectedEvent;
using pineforge::native_order::ReplaceRejectedEvent;
using pineforge::native_order::ReplaceStatus;
using pineforge::native_order::ReplacedEvent;
using pineforge::native_order::Request;
using pineforge::native_order::RequestHandle;
using pineforge::native_order::RequestRejectReason;
using pineforge::native_order::RunIdentity;
using pineforge::native_order::SubmitStatus;
using pineforge::native_order::WorkingRequestCore;
using pineforge::native_order::point_eligible;
using pineforge::order_action::Reduce;
using pineforge::order_action::Transact;

namespace {
int checks = 0;
int failures = 0;
#define CHECK(x)                                                        \
    do {                                                                \
        ++checks;                                                       \
        if (!(x)) {                                                     \
            ++failures;                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x);    \
        }                                                               \
    } while (0)

constexpr double NaN = std::numeric_limits<double>::quiet_NaN();
constexpr double Inf = std::numeric_limits<double>::infinity();
const RunIdentity kRun{"session-a", 1};

bool bits_eq(double a, double b) {
    std::uint64_t x = 0;
    std::uint64_t y = 0;
    std::memcpy(&x, &a, sizeof(x));
    std::memcpy(&y, &b, sizeof(y));
    return x == y;
}

double step_n(double x, double toward, int n) {
    for (int i = 0; i < n; ++i) x = std::nextafter(x, toward);
    return x;
}

Request tx(double q, std::string label = {}, std::string comment = {}) {
    return Request{Action{Transact{q}}, std::move(label), std::move(comment)};
}
Request rd(double q, std::string label = {}, std::string comment = {}) {
    return Request{Action{Reduce{q}}, std::move(label), std::move(comment)};
}
Request fl(std::string label = {}, std::string comment = {}) {
    return Request{Action{Flatten{}}, std::move(label), std::move(comment)};
}

double transact_qty(const Action& action) {
    return std::get<Transact>(action).signed_units;
}
double reduce_qty(const Action& action) { return std::get<Reduce>(action).units; }

template <class T>
const T* as(const CommandEvent& event) {
    return std::get_if<T>(&event);
}

void identity_binding() {
    try {
        WorkingRequestCore bad(RunIdentity{"", 1});
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    try {
        WorkingRequestCore bad(RunIdentity{"s", 0});
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    WorkingRequestCore core(kRun);
    CHECK(core.identity() == kRun);
    CHECK(core.live().empty());
    CHECK(core.history().empty());
}

void submit_accept_and_reject() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const Request buy = tx(2.5, "entry", "first");
    const auto accepted = core.submit(buy, 1000, inc, ord);
    CHECK(accepted.status == SubmitStatus::Accepted);
    CHECK(accepted.event_ordinal == 1);
    CHECK(accepted.handle.has_value());
    CHECK(accepted.handle->incarnation == 1);
    CHECK(accepted.handle->run == kRun);
    CHECK(!accepted.reason);
    CHECK(inc == 2);
    CHECK(ord == 2);
    CHECK(core.live().size() == 1);
    CHECK(core.history().size() == 1);
    const LiveRequest& live = core.live().front();
    CHECK(live.handle == *accepted.handle);
    CHECK(live.birth.acceptance_ordinal == 1);
    CHECK(live.birth.decision_time_lower_bound == 1000);
    CHECK(!live.predecessor);
    CHECK(live.request.label == "entry");
    CHECK(live.request.comment == "first");
    CHECK(bits_eq(transact_qty(live.request.action), 2.5));
    const auto* event = as<AcceptedEvent>(core.history().back());
    CHECK(event);
    if (event) {
        CHECK(event->ordinal == 1);
        CHECK(event->handle == live.handle);
        CHECK(event->birth == live.birth);
        CHECK(event->request.label == "entry");
        CHECK(bits_eq(transact_qty(event->request.action), 2.5));
    }
    CHECK(core.find_live(*accepted.handle) == &core.live().front());

    const auto rejected = core.submit(tx(0.0, "zero", "nope"), 1000, inc, ord);
    CHECK(rejected.status == SubmitStatus::Rejected);
    CHECK(rejected.event_ordinal == 2);
    CHECK(!rejected.handle);
    CHECK(rejected.reason == RequestRejectReason::InvalidQuantity);
    CHECK(inc == 2);
    CHECK(ord == 3);
    CHECK(core.live().size() == 1);
    CHECK(core.live().front().handle.incarnation == 1);
    CHECK(core.history().size() == 2);
    const auto* rej = as<RejectedEvent>(core.history().back());
    CHECK(rej);
    if (rej) {
        CHECK(rej->ordinal == 2);
        CHECK(rej->reason == RequestRejectReason::InvalidQuantity);
        CHECK(rej->request.label == "zero");
        CHECK(rej->request.comment == "nope");
        CHECK(bits_eq(transact_qty(rej->request.action), 0.0));
    }
}

void quantity_rejections_preserve_bits() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 7;
    uint64_t ord = 3;
    struct Case {
        Request request;
        RequestRejectReason reason;
    };
    const double neg_zero = std::copysign(0.0, -1.0);
    const double neg_nan = std::copysign(NaN, -1.0);
    const Case cases[] = {
            {tx(0.0, "t0"), RequestRejectReason::InvalidQuantity},
            {tx(neg_zero, "t-0"), RequestRejectReason::InvalidQuantity},
            {tx(NaN, "tnan"), RequestRejectReason::InvalidQuantity},
            {tx(neg_nan, "t-nan"), RequestRejectReason::InvalidQuantity},
            {tx(Inf, "tpinf"), RequestRejectReason::InvalidQuantity},
            {tx(-Inf, "tninf"), RequestRejectReason::InvalidQuantity},
            {rd(0.0, "r0"), RequestRejectReason::InvalidQuantity},
            {rd(-1.0, "rneg"), RequestRejectReason::InvalidQuantity},
            {rd(NaN, "rnan"), RequestRejectReason::InvalidQuantity},
            {rd(Inf, "rinf"), RequestRejectReason::InvalidQuantity},
    };
    const std::size_t n = sizeof(cases) / sizeof(cases[0]);
    for (std::size_t i = 0; i < n; ++i) {
        const uint64_t inc_before = inc;
        const uint64_t ord_before = ord;
        const auto result = core.submit(cases[i].request, 50, inc, ord);
        CHECK(result.status == SubmitStatus::Rejected);
        CHECK(result.event_ordinal == ord_before);
        CHECK(!result.handle);
        CHECK(result.reason == cases[i].reason);
        CHECK(inc == inc_before);
        CHECK(ord == ord_before + 1);
        CHECK(core.live().empty());
        const auto* rej = as<RejectedEvent>(core.history().back());
        CHECK(rej);
        if (!rej) continue;
        CHECK(rej->ordinal == ord_before);
        CHECK(rej->reason == cases[i].reason);
        CHECK(rej->request.label == cases[i].request.label);
        if (std::holds_alternative<Transact>(cases[i].request.action)) {
            CHECK(bits_eq(transact_qty(rej->request.action),
                          transact_qty(cases[i].request.action)));
        } else {
            CHECK(bits_eq(reduce_qty(rej->request.action),
                          reduce_qty(cases[i].request.action)));
        }
    }
    CHECK(core.history().size() == n);
    CHECK(inc == 7);
}

void flatten_and_signed_reduce() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto flat = core.submit(fl("flat", "all"), 8, inc, ord, 0.25);
    CHECK(flat.status == SubmitStatus::Accepted);
    CHECK(std::holds_alternative<Flatten>(core.live().back().request.action));
    CHECK(core.live().back().request.label == "flat");
    CHECK(inc == 2);
    CHECK(ord == 2);

    const double denorm = std::numeric_limits<double>::denorm_min();
    const auto reduce = core.submit(rd(denorm, "dust"), 8, inc, ord);
    CHECK(reduce.status == SubmitStatus::Accepted);
    CHECK(bits_eq(reduce_qty(core.live().back().request.action), denorm));

    const auto sell = core.submit(tx(-4.0, "sell"), 9, inc, ord);
    CHECK(sell.status == SubmitStatus::Accepted);
    CHECK(bits_eq(transact_qty(core.live().back().request.action), -4.0));
    CHECK(core.live().size() == 3);
}

void grid_accepts_and_preserves_bits() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const double step = 0.1;
    const double q = 0.1 + 0.2;
    CHECK(quantity_on_grid(q, step));
    const auto result = core.submit(tx(q, "sum"), 1, inc, ord, step);
    CHECK(result.status == SubmitStatus::Accepted);
    CHECK(bits_eq(transact_qty(core.live().back().request.action), q));
    CHECK(!bits_eq(q, 0.3));

    const auto sell = core.submit(tx(-2.0, "sell-grid"), 1, inc, ord, 1.0);
    CHECK(sell.status == SubmitStatus::Accepted);
    CHECK(bits_eq(transact_qty(core.live().back().request.action), -2.0));

    const double exact = 2.0;
    const double one_ulp = step_n(exact, Inf, 1);
    CHECK(quantity_on_grid(one_ulp, 1.0));
    const auto near = core.submit(tx(one_ulp, "ulp1"), 1, inc, ord, 1.0);
    CHECK(near.status == SubmitStatus::Accepted);
    CHECK(bits_eq(transact_qty(core.live().back().request.action), one_ulp));
    CHECK(!bits_eq(one_ulp, exact));

    const double below = step_n(exact, 0.0, 1);
    CHECK(quantity_on_grid(below, 1.0));
    const auto near_lo = core.submit(rd(below, "ulp-1"), 1, inc, ord, 1.0);
    CHECK(near_lo.status == SubmitStatus::Accepted);
    CHECK(bits_eq(reduce_qty(core.live().back().request.action), below));

    const double pow2 = 0x1p53;
    CHECK(quantity_on_grid(pow2, 1.0));
    const auto edge = core.submit(tx(pow2, "n53"), 1, inc, ord, 1.0);
    CHECK(edge.status == SubmitStatus::Accepted);
    CHECK(bits_eq(transact_qty(core.live().back().request.action), pow2));
}

void grid_rejects_off_grid_and_half() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 11;
    uint64_t ord = 4;
    const double five_ulp = step_n(2.0, Inf, 5);
    CHECK(!quantity_on_grid(five_ulp, 1.0));
    const uint64_t inc_before = inc;
    const auto off = core.submit(tx(five_ulp, "five"), 2, inc, ord, 1.0);
    CHECK(off.status == SubmitStatus::Rejected);
    CHECK(off.reason == RequestRejectReason::OffGrid);
    CHECK(inc == inc_before);
    CHECK(ord == 5);
    const auto* rej = as<RejectedEvent>(core.history().back());
    CHECK(rej);
    if (rej) {
        CHECK(rej->reason == RequestRejectReason::OffGrid);
        CHECK(bits_eq(transact_qty(rej->request.action), five_ulp));
        CHECK(rej->request.label == "five");
    }
    CHECK(core.live().empty());

    const auto half = core.submit(tx(1.5, "half"), 2, inc, ord, 1.0);
    CHECK(half.status == SubmitStatus::Rejected);
    CHECK(half.reason == RequestRejectReason::OffGrid);
    CHECK(inc == inc_before);
    const auto* half_e = as<RejectedEvent>(core.history().back());
    CHECK(half_e);
    if (half_e) CHECK(bits_eq(transact_qty(half_e->request.action), 1.5));

    const auto dust = core.submit(rd(0.4, "dust"), 2, inc, ord, 1.0);
    CHECK(dust.status == SubmitStatus::Rejected);
    CHECK(dust.reason == RequestRejectReason::OffGrid);

    const auto over = core.submit(tx(0x1p53 + 2.0, "over"), 2, inc, ord, 1.0);
    CHECK(over.status == SubmitStatus::Rejected);
    CHECK(over.reason == RequestRejectReason::OffGrid);
    CHECK(inc == inc_before);
    CHECK(core.live().empty());
}

void submit_before_point_eligibility() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 5;
    const auto first = core.submit(tx(1.0, "a"), 1000, inc, ord);
    const auto second = core.submit(tx(-1.0, "b"), 1000, inc, ord);
    CHECK(first.event_ordinal == 5);
    CHECK(second.event_ordinal == 6);
    const LiveRequest& a = core.live()[0];
    const LiveRequest& b = core.live()[1];
    CHECK(a.birth.acceptance_ordinal == 5);
    CHECK(b.birth.acceptance_ordinal == 6);
    CHECK(!point_eligible(a, 5, 1000));
    CHECK(!point_eligible(a, 6, 999));
    CHECK(point_eligible(a, 6, 1000));
    CHECK(point_eligible(a, 7, 1001));
    CHECK(!point_eligible(b, 6, 1000));
    CHECK(!point_eligible(b, 7, 999));
    CHECK(point_eligible(b, 7, 1000));
    CHECK(!point_eligible(a.birth, a.birth.acceptance_ordinal, a.birth.decision_time_lower_bound));
}

void replace_valid_invalid_and_stale_cancel() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(tx(1.0, "old", "keep-old"), 10, inc, ord);
    const auto second = core.submit(tx(2.0, "peer"), 10, inc, ord);
    CHECK(core.live().size() == 2);
    const RequestHandle h1 = *first.handle;
    const RequestHandle h2 = *second.handle;

    const auto bad = core.replace(h1, tx(0.0, "bad", "no"), 20, inc, ord);
    CHECK(bad.status == ReplaceStatus::ReplaceRejected);
    CHECK(bad.event_ordinal == 3);
    CHECK(!bad.successor);
    CHECK(bad.reason == RequestRejectReason::InvalidQuantity);
    CHECK(inc == 3);
    CHECK(ord == 4);
    CHECK(core.live().size() == 2);
    CHECK(core.find_live(h1));
    CHECK(core.find_live(h1)->request.label == "old");
    CHECK(bits_eq(transact_qty(core.find_live(h1)->request.action), 1.0));
    const auto* rr = as<ReplaceRejectedEvent>(core.history().back());
    CHECK(rr);
    if (rr) {
        CHECK(rr->target == h1);
        CHECK(rr->live_request.label == "old");
        CHECK(rr->attempted.label == "bad");
        CHECK(rr->attempted.comment == "no");
        CHECK(bits_eq(transact_qty(rr->attempted.action), 0.0));
        CHECK(rr->reason == RequestRejectReason::InvalidQuantity);
    }

    const auto off = core.replace(h1, tx(1.1, "off"), 20, inc, ord, 1.0);
    CHECK(off.status == ReplaceStatus::ReplaceRejected);
    CHECK(off.reason == RequestRejectReason::OffGrid);
    CHECK(core.find_live(h1));
    CHECK(inc == 3);

    const Request next = tx(-3.0, "new", "keep-new");
    const auto ok = core.replace(h1, next, 30, inc, ord);
    CHECK(ok.status == ReplaceStatus::Replaced);
    CHECK(ok.successor.has_value());
    CHECK(ok.successor->incarnation == 3);
    CHECK(ok.event_ordinal == 5);
    CHECK(inc == 4);
    CHECK(ord == 6);
    CHECK(!core.find_live(h1));
    CHECK(core.find_live(h2));
    CHECK(core.live().size() == 2);
    CHECK(core.live()[0].handle == h2);
    const LiveRequest& successor = core.live()[1];
    CHECK(successor.handle == *ok.successor);
    CHECK(successor.birth.acceptance_ordinal == 5);
    CHECK(successor.birth.decision_time_lower_bound == 30);
    CHECK(successor.predecessor == h1);
    CHECK(successor.request.label == "new");
    CHECK(successor.request.comment == "keep-new");
    CHECK(bits_eq(transact_qty(successor.request.action), -3.0));
    CHECK(point_eligible(successor, 6, 30));
    CHECK(!point_eligible(successor, 5, 30));
    const auto* replaced = as<ReplacedEvent>(core.history().back());
    CHECK(replaced);
    if (replaced) {
        CHECK(replaced->ordinal == 5);
        CHECK(replaced->predecessor == h1);
        CHECK(replaced->predecessor_request.label == "old");
        CHECK(replaced->successor == successor.handle);
        CHECK(replaced->successor_request.comment == "keep-new");
        CHECK(replaced->successor_birth == successor.birth);
        CHECK(bits_eq(transact_qty(replaced->successor_request.action), -3.0));
    }

    const auto stale = core.cancel(h1, ord);
    CHECK(stale.status == CancelStatus::NotWorking);
    CHECK(stale.event_ordinal == 6);
    CHECK(ord == 7);
    CHECK(core.find_live(h2));
    CHECK(core.find_live(successor.handle));
    const auto* nw = as<NotWorkingEvent>(core.history().back());
    CHECK(nw);
    if (nw) {
        CHECK(nw->target == h1);
        CHECK(!nw->attempted);
    }

    const auto stale_rep = core.replace(h1, tx(9.0, "ghost"), 40, inc, ord);
    CHECK(stale_rep.status == ReplaceStatus::NotWorking);
    CHECK(!stale_rep.successor);
    CHECK(inc == 4);
    CHECK(core.find_live(successor.handle)->request.label == "new");
    const auto* nw2 = as<NotWorkingEvent>(core.history().back());
    CHECK(nw2);
    if (nw2) {
        CHECK(nw2->target == h1);
        CHECK(nw2->attempted.has_value());
        CHECK(nw2->attempted->label == "ghost");
    }
}

void foreign_zero_and_absent_handles() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto live = core.submit(tx(1.0, "live"), 1, inc, ord);
    const RequestHandle zero{};
    const RequestHandle empty_key{RunIdentity{"", 1}, 1};
    const RequestHandle zero_run{RunIdentity{"session-a", 0}, 1};
    const RequestHandle foreign{RunIdentity{"session-b", 1}, 1};
    const RequestHandle other_run{RunIdentity{"session-a", 2}, live.handle->incarnation};
    const RequestHandle absent{kRun, 99};

    const auto c0 = core.cancel(zero, ord);
    CHECK(c0.status == CancelStatus::InvalidHandle);
    CHECK(as<InvalidHandleEvent>(core.history().back()));
    CHECK(as<InvalidHandleEvent>(core.history().back())->target.incarnation == 0);

    const auto r_empty = core.replace(empty_key, tx(2.0, "x"), 1, inc, ord);
    CHECK(r_empty.status == ReplaceStatus::InvalidHandle);
    CHECK(inc == 2);

    const auto c_run = core.cancel(zero_run, ord);
    CHECK(c_run.status == CancelStatus::InvalidHandle);

    const auto c_foreign = core.cancel(foreign, ord);
    CHECK(c_foreign.status == CancelStatus::InvalidHandle);
    CHECK(as<InvalidHandleEvent>(core.history().back())->target.run.session_key == "session-b");

    const auto r_other = core.replace(other_run, tx(0.0, "no"), 1, inc, ord);
    CHECK(r_other.status == ReplaceStatus::InvalidHandle);
    CHECK(!r_other.reason);
    CHECK(inc == 2);
    CHECK(as<InvalidHandleEvent>(core.history().back())->attempted->label == "no");

    const auto c_absent = core.cancel(absent, ord);
    CHECK(c_absent.status == CancelStatus::NotWorking);
    CHECK(as<NotWorkingEvent>(core.history().back())->target.incarnation == 99);

    CHECK(core.live().size() == 1);
    CHECK(core.find_live(*live.handle));
    CHECK(core.live().front().request.label == "live");
}

void metadata_and_const_views() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const std::string label = "α-entry";
    const std::string comment = "keep me";
    core.submit(tx(1.0, label, comment), 4, inc, ord);
    const WorkingRequestCore& view = core;
    CHECK(view.live().size() == 1);
    CHECK(view.history().size() == 1);
    CHECK(view.live().front().request.label == label);
    CHECK(view.live().front().request.comment == comment);
    CHECK(view.find_live(view.live().front().handle));
    const auto* accepted = as<AcceptedEvent>(view.history().front());
    CHECK(accepted);
    if (accepted) {
        CHECK(accepted->request.label == label);
        CHECK(accepted->request.comment == comment);
    }
}

void counters_do_not_partially_mutate() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(tx(1.0, "keep"), 1, inc, ord);
    CHECK(first.status == SubmitStatus::Accepted);
    const RequestHandle kept = *first.handle;
    const std::size_t live_n = core.live().size();
    const std::size_t hist_n = core.history().size();

    auto expect_frozen = [&](uint64_t inc_now, uint64_t ord_now) {
        CHECK(inc == inc_now);
        CHECK(ord == ord_now);
        CHECK(core.live().size() == live_n);
        CHECK(core.history().size() == hist_n);
        CHECK(core.find_live(kept));
        CHECK(core.find_live(kept)->request.label == "keep");
    };

    inc = 0;
    try {
        core.submit(tx(1.0, "no"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    expect_frozen(0, 2);

    inc = std::numeric_limits<uint64_t>::max();
    try {
        core.submit(tx(1.0, "no"), 1, inc, ord);
        CHECK(false);
    } catch (const std::overflow_error&) { CHECK(true); }
    expect_frozen(std::numeric_limits<uint64_t>::max(), 2);

    inc = 4;
    const uint64_t frozen_ord = ord;
    ord = 0;
    try {
        core.submit(tx(1.0, "no"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    expect_frozen(4, 0);

    ord = std::numeric_limits<uint64_t>::max();
    try {
        core.submit(tx(0.0, "rej"), 1, inc, ord);
        CHECK(false);
    } catch (const std::overflow_error&) { CHECK(true); }
    expect_frozen(4, std::numeric_limits<uint64_t>::max());

    ord = frozen_ord;
    inc = 0;
    const auto rej = core.submit(tx(0.0, "rej-ok"), 1, inc, ord);
    CHECK(rej.status == SubmitStatus::Rejected);
    CHECK(inc == 0);
    CHECK(ord == frozen_ord + 1);
    CHECK(core.find_live(kept));
    CHECK(core.live().size() == live_n);
    CHECK(as<RejectedEvent>(core.history().back())->request.label == "rej-ok");

    inc = std::numeric_limits<uint64_t>::max();
    try {
        core.replace(kept, tx(2.0, "succ"), 2, inc, ord);
        CHECK(false);
    } catch (const std::overflow_error&) { CHECK(true); }
    CHECK(inc == std::numeric_limits<uint64_t>::max());
    CHECK(core.find_live(kept));
    CHECK(core.find_live(kept)->request.label == "keep");
    CHECK(bits_eq(transact_qty(core.find_live(kept)->request.action), 1.0));

    inc = 8;
    const uint64_t ord_now = ord;
    ord = 0;
    try {
        core.replace(kept, tx(2.0, "succ"), 2, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(inc == 8);
    CHECK(ord == 0);
    CHECK(core.find_live(kept));

    ord = 0;
    try {
        core.cancel(kept, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(ord == 0);
    CHECK(core.find_live(kept));
    CHECK(core.find_live(kept)->request.label == "keep");

    ord = std::numeric_limits<uint64_t>::max();
    try {
        core.cancel(kept, ord);
        CHECK(false);
    } catch (const std::overflow_error&) { CHECK(true); }
    CHECK(ord == std::numeric_limits<uint64_t>::max());
    CHECK(core.find_live(kept));
    CHECK(core.history().size() == hist_n + 1);

    ord = ord_now;
    const auto still = core.replace(kept, tx(0.0, "bad"), 2, inc, ord);
    CHECK(still.status == ReplaceStatus::ReplaceRejected);
    CHECK(inc == 8);
    CHECK(core.find_live(kept)->request.label == "keep");
}

void reset_makes_old_handles_invalid() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(tx(1.0, "old-run"), 1, inc, ord);
    const RequestHandle old = *first.handle;
    try {
        core.reset(RunIdentity{"", 2});
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(core.identity() == kRun);
    CHECK(core.find_live(old));
    CHECK(core.find_live(old)->request.label == "old-run");

    try {
        core.reset(RunIdentity{"session-a", 0});
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(core.find_live(old));

    const RunIdentity next{"session-a", 2};
    core.reset(next);
    CHECK(core.identity() == next);
    CHECK(core.live().empty());
    CHECK(core.history().empty());
    CHECK(!core.find_live(old));

    const auto cancel_old = core.cancel(old, ord);
    CHECK(cancel_old.status == CancelStatus::InvalidHandle);
    CHECK(as<InvalidHandleEvent>(core.history().back())->target == old);

    const auto replace_old = core.replace(old, tx(2.0, "no"), 1, inc, ord);
    CHECK(replace_old.status == ReplaceStatus::InvalidHandle);
    CHECK(inc == 2);

    const auto fresh = core.submit(tx(3.0, "new-run"), 9, inc, ord);
    CHECK(fresh.status == SubmitStatus::Accepted);
    CHECK(fresh.handle->run == next);
    CHECK(fresh.handle->incarnation == 2);
    CHECK(core.live().front().request.label == "new-run");
    CHECK(old != *fresh.handle);
}

void moved_from_commands_preserve_state(WorkingRequestCore& core,
                                        const RequestHandle& transferred,
                                        uint64_t& inc,
                                        uint64_t& ord) {
    const uint64_t inc_before = inc;
    const uint64_t ord_before = ord;
    const auto unchanged = [&] {
        CHECK(core.identity().session_key.empty());
        CHECK(core.identity().run_number == 0);
        CHECK(core.live().empty());
        CHECK(core.history().empty());
        CHECK(!core.find_live(transferred));
        CHECK(inc == inc_before);
        CHECK(ord == ord_before);
    };
    unchanged();
    for (double quantity : {1.0, 0.0}) {
        try {
            core.submit(tx(quantity, "unbound"), 100, inc, ord);
            CHECK(false);
        } catch (const std::invalid_argument&) { CHECK(true); }
        unchanged();
    }
    try {
        core.replace(transferred, tx(2.0, "unbound-replace"), 100, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    unchanged();
    try {
        core.cancel(transferred, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    unchanged();
    try {
        core.reset(RunIdentity{"", 2});
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    unchanged();
}

void move_construction_transfers_and_reset_rebinds() {
    WorkingRequestCore source(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto original = source.submit(tx(1.5, "original", "move me"), 10, inc, ord);
    const auto rejected = source.submit(tx(0.0, "rejected"), 11, inc, ord);
    CHECK(rejected.status == SubmitStatus::Rejected);
    const auto successor = source.replace(*original.handle, tx(-2.5, "successor", "kept"),
                                          12, inc, ord);
    CHECK(successor.status == ReplaceStatus::Replaced);

    WorkingRequestCore destination(std::move(source));
    moved_from_commands_preserve_state(source, *successor.successor, inc, ord);
    CHECK(destination.identity() == kRun);
    CHECK(destination.live().size() == 1);
    CHECK(destination.history().size() == 3);
    CHECK(!destination.find_live(*original.handle));
    const auto* transferred = destination.find_live(*successor.successor);
    CHECK(transferred);
    if (transferred) {
        CHECK(transferred->predecessor == original.handle);
        CHECK(transferred->birth == (Birth{3, 12}));
        CHECK(transferred->request.label == "successor");
        CHECK(transferred->request.comment == "kept");
        CHECK(bits_eq(transact_qty(transferred->request.action), -2.5));
    }
    const auto* accepted_event = as<AcceptedEvent>(destination.history().front());
    CHECK(accepted_event);
    if (accepted_event) {
        CHECK(accepted_event->handle == *original.handle);
        CHECK(accepted_event->request.comment == "move me");
    }
    CHECK(as<RejectedEvent>(destination.history()[1]));
    const auto* replaced_event = as<ReplacedEvent>(destination.history().back());
    CHECK(replaced_event);
    if (replaced_event) {
        CHECK(replaced_event->predecessor == *original.handle);
        CHECK(replaced_event->successor == *successor.successor);
        CHECK(replaced_event->ordinal == 3);
    }
    const auto cancelled = destination.cancel(*successor.successor, ord);
    CHECK(cancelled.status == CancelStatus::Cancelled);
    CHECK(cancelled.event_ordinal == 4);
    CHECK(destination.live().empty());
    CHECK(destination.history().size() == 4);
    CHECK(inc == 3);
    CHECK(ord == 5);

    const RunIdentity rebound{"session-a", 2};
    source.reset(rebound);
    const auto fresh = source.submit(tx(4.0, "recovered"), 20, inc, ord);
    CHECK(fresh.status == SubmitStatus::Accepted);
    CHECK(fresh.handle->run == rebound);
    CHECK(fresh.handle->incarnation == 3);
    CHECK(fresh.event_ordinal == 5);
    CHECK(source.find_live(*fresh.handle));
    CHECK(!source.find_live(*successor.successor));
    CHECK(source.history().size() == 1);
    CHECK(source.live().size() == 1);
    CHECK(destination.history().size() == 4);
}

void move_assignment_replaces_destination_and_preserves_self_move() {
    WorkingRequestCore source(kRun);
    uint64_t inc = 7;
    uint64_t ord = 11;
    const auto request = source.submit(rd(2.0, "transferred", "assign me"), 80, inc, ord);
    WorkingRequestCore destination(RunIdentity{"replaced-session", 3});
    uint64_t previous_inc = 40;
    uint64_t previous_ord = 60;
    const auto previous = destination.submit(fl("discarded"), 50, previous_inc, previous_ord);

    destination = std::move(source);
    moved_from_commands_preserve_state(source, *request.handle, inc, ord);
    CHECK(destination.identity() == kRun);
    CHECK(destination.live().size() == 1);
    CHECK(destination.history().size() == 1);
    CHECK(!destination.find_live(*previous.handle));
    CHECK(previous_inc == 41);
    CHECK(previous_ord == 61);
    const auto* transferred = destination.find_live(*request.handle);
    CHECK(transferred);
    if (transferred) {
        CHECK(transferred->birth == (Birth{11, 80}));
        CHECK(transferred->request.label == "transferred");
        CHECK(transferred->request.comment == "assign me");
        CHECK(bits_eq(reduce_qty(transferred->request.action), 2.0));
    }
    auto* self = &destination;
    destination = std::move(*self);
    CHECK(destination.identity() == kRun);
    CHECK(destination.find_live(*request.handle));
    CHECK(destination.live().size() == 1);
    CHECK(destination.history().size() == 1);
    CHECK(inc == 8);
    CHECK(ord == 12);
    const auto replaced = destination.replace(*request.handle, fl("continued"), 81, inc, ord);
    CHECK(replaced.status == ReplaceStatus::Replaced);
    CHECK(replaced.successor);
    if (!replaced.successor) return;
    CHECK(replaced.successor->incarnation == 8);
    CHECK(replaced.event_ordinal == 12);
    CHECK(destination.find_live(*replaced.successor));

    // Moving an already unbound core transfers that empty state too.
    destination = std::move(source);
    moved_from_commands_preserve_state(destination, *replaced.successor, inc, ord);
    moved_from_commands_preserve_state(source, *request.handle, inc, ord);
    destination.reset(RunIdentity{"recovered-assignment", 1});
    const auto recovered = destination.submit(tx(3.0, "recovered"), 90, inc, ord);
    CHECK(recovered.status == SubmitStatus::Accepted);
    CHECK(recovered.handle->run == (RunIdentity{"recovered-assignment", 1}));
    CHECK(recovered.handle->incarnation == 9);
    CHECK(recovered.event_ordinal == 13);
    CHECK(destination.find_live(*recovered.handle));
    CHECK(!destination.find_live(*replaced.successor));
    CHECK(destination.live().size() == 1);
    CHECK(destination.history().size() == 1);
}

void cancel_live_erases_only_that_request() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto a = core.submit(tx(1.0, "a"), 1, inc, ord);
    const auto b = core.submit(rd(2.0, "b", "stay"), 1, inc, ord);
    const auto c = core.submit(fl("c"), 1, inc, ord);
    const auto gone = core.cancel(*a.handle, ord);
    CHECK(gone.status == CancelStatus::Cancelled);
    CHECK(gone.event_ordinal == 4);
    CHECK(!core.find_live(*a.handle));
    CHECK(core.find_live(*b.handle));
    CHECK(core.find_live(*c.handle));
    CHECK(core.live().size() == 2);
    CHECK(core.live()[0].request.label == "b");
    CHECK(core.live()[1].request.label == "c");
    const auto* cancelled = as<CancelledEvent>(core.history().back());
    CHECK(cancelled);
    if (cancelled) {
        CHECK(cancelled->handle == *a.handle);
        CHECK(cancelled->request.label == "a");
        CHECK(bits_eq(transact_qty(cancelled->request.action), 1.0));
    }
    const auto again = core.cancel(*a.handle, ord);
    CHECK(again.status == CancelStatus::NotWorking);
    CHECK(core.find_live(*b.handle)->request.comment == "stay");
}

void same_counter_alias() {
    WorkingRequestCore core(kRun);
    uint64_t both = 1;
    try {
        core.submit(tx(1.0, "alias"), 1, both, both);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(both == 1);
    CHECK(core.live().empty());
    CHECK(core.history().empty());

    try {
        core.submit(tx(0.0, "alias-rej"), 1, both, both);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(core.history().empty());

    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(tx(1.0, "kept"), 1, inc, ord);
    CHECK(first.status == SubmitStatus::Accepted);
    uint64_t same = 8;
    try {
        core.replace(*first.handle, tx(2.0, "no"), 1, same, same);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(same == 8);
    CHECK(inc == 2);
    CHECK(ord == 2);
    CHECK(core.find_live(*first.handle));
    CHECK(core.find_live(*first.handle)->request.label == "kept");
    CHECK(core.history().size() == 1);
}

void reused_and_gapped_counters() {
    {
        WorkingRequestCore core(kRun);
        uint64_t inc = 1;
        uint64_t ord = 1;
        const auto only = core.submit(tx(1.0, "only"), 1, inc, ord);
        CHECK(core.cancel(*only.handle, ord).status == CancelStatus::Cancelled);
        CHECK(core.live().empty());
        inc = 1;
        try {
            core.submit(tx(2.0, "reuse-cancelled"), 1, inc, ord);
            CHECK(false);
        } catch (const std::invalid_argument&) { CHECK(true); }
        CHECK(inc == 1);
        CHECK(core.live().empty());
        CHECK(core.history().size() == 2);
        inc = 2;
        const auto after = core.submit(tx(2.0, "after-cancel"), 1, inc, ord);
        CHECK(after.status == SubmitStatus::Accepted);
        CHECK(after.handle->incarnation == 2);
        CHECK(core.live().front().request.label == "after-cancel");
    }

    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    const auto first = core.submit(tx(1.0, "one"), 1, inc, ord);
    CHECK(first.handle->incarnation == 1);
    CHECK(first.event_ordinal == 1);

    inc = 1;
    try {
        core.submit(tx(2.0, "reuse-inc"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(inc == 1);
    CHECK(ord == 2);
    CHECK(core.live().size() == 1);
    CHECK(core.history().size() == 1);

    ord = 1;
    inc = 2;
    try {
        core.submit(tx(2.0, "reuse-ord"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(ord == 1);
    CHECK(core.history().size() == 1);

    inc = 10;
    ord = 50;
    const auto gapped = core.submit(tx(3.0, "gap"), 1, inc, ord);
    CHECK(gapped.status == SubmitStatus::Accepted);
    CHECK(gapped.handle->incarnation == 10);
    CHECK(gapped.event_ordinal == 50);
    CHECK(inc == 11);
    CHECK(ord == 51);
    CHECK(core.live().size() == 2);

    const auto cancelled = core.cancel(*first.handle, ord);
    CHECK(cancelled.status == CancelStatus::Cancelled);
    CHECK(!core.find_live(*first.handle));
    inc = 1;
    try {
        core.submit(tx(4.0, "after-cancel"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(inc == 1);
    CHECK(core.live().size() == 1);
    CHECK(core.live().front().handle.incarnation == 10);

    inc = 10;
    try {
        core.submit(tx(4.0, "after-cancel-gap"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }

    const uint64_t last_ord = ord;
    ord = 50;
    inc = 12;
    try {
        core.submit(tx(4.0, "old-ord"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(ord == 50);

    ord = last_ord;
    inc = 12;
    const auto replaced = core.replace(*gapped.handle, tx(5.0, "succ"), 2, inc, ord);
    CHECK(replaced.status == ReplaceStatus::Replaced);
    CHECK(replaced.successor->incarnation == 12);
    CHECK(!core.find_live(*gapped.handle));

    inc = 12;
    try {
        core.submit(tx(6.0, "reuse-succ"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    inc = 10;
    try {
        core.replace(*replaced.successor, tx(6.0, "reuse-pred"), 1, inc, ord);
        CHECK(false);
    } catch (const std::invalid_argument&) { CHECK(true); }
    CHECK(core.find_live(*replaced.successor)->request.label == "succ");

    inc = 20;
    const auto later = core.submit(tx(7.0, "later"), 3, inc, ord);
    CHECK(later.status == SubmitStatus::Accepted);
    CHECK(later.handle->incarnation == 20);
}

void aliased_const_arguments() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    CHECK(core.submit(tx(1.0, "orig", "c1"), 10, inc, ord).status == SubmitStatus::Accepted);
    CHECK(core.submit(tx(2.0, "peer", "c2"), 11, inc, ord).status == SubmitStatus::Accepted);

    const auto copied = core.submit(core.live().front().request, 12, inc, ord);
    CHECK(copied.status == SubmitStatus::Accepted);
    CHECK(copied.handle->incarnation == 3);
    CHECK(core.live().size() == 3);
    CHECK(core.live()[0].request.label == "orig");
    CHECK(core.live()[0].request.comment == "c1");
    CHECK(core.live()[2].request.label == "orig");
    CHECK(core.live()[2].request.comment == "c1");
    CHECK(bits_eq(transact_qty(core.live()[2].request.action), 1.0));

    const auto repl = core.replace(core.live().front().handle, core.live().front().request, 13, inc,
                                   ord);
    CHECK(repl.status == ReplaceStatus::Replaced);
    CHECK(repl.successor->incarnation == 4);
    CHECK(!core.find_live(RequestHandle{kRun, 1}));
    CHECK(core.live().size() == 3);
    CHECK(core.live()[0].request.label == "peer");
    CHECK(core.live().back().request.label == "orig");
    CHECK(core.live().back().request.comment == "c1");
    CHECK(core.live().back().predecessor->incarnation == 1);

    const auto cancelled = core.cancel(core.live().front().handle, ord);
    CHECK(cancelled.status == CancelStatus::Cancelled);
    CHECK(core.live().size() == 2);
    CHECK(core.live().front().request.label == "orig");

    const auto* first_accepted = as<AcceptedEvent>(core.history().front());
    CHECK(first_accepted);
    const auto from_history = core.submit(first_accepted->request, 14, inc, ord);
    CHECK(from_history.status == SubmitStatus::Accepted);
    CHECK(core.live().back().request.label == "orig");
    CHECK(core.live().back().request.comment == "c1");
}

void history_prefix_append_stable() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    for (int i = 0; i < 32; ++i) {
        const auto result = core.submit(tx(1.0, "p" + std::to_string(i)), 1, inc, ord);
        CHECK(result.status == SubmitStatus::Accepted);
        CHECK(result.handle->incarnation == static_cast<uint64_t>(i + 1));
    }
    const CommandEvent* prefix = &core.history().front();
    CHECK(as<AcceptedEvent>(*prefix)->request.label == "p0");
    CHECK(as<AcceptedEvent>(*prefix)->ordinal == 1);

    CHECK(core.submit(tx(1.0, "p32"), 1, inc, ord).status == SubmitStatus::Accepted);
    if (&core.history().front() != prefix) prefix = &core.history().front();
    CHECK(as<AcceptedEvent>(*prefix)->request.label == "p0");

    const auto rejected = core.submit(tx(0.0, "rej"), 1, inc, ord);
    CHECK(rejected.status == SubmitStatus::Rejected);
    CHECK(&core.history().front() == prefix);
    CHECK(as<AcceptedEvent>(core.history().front())->request.label == "p0");
    CHECK(as<AcceptedEvent>(core.history().front())->ordinal == 1);

    const auto extra = core.submit(tx(2.0, "more"), 1, inc, ord);
    CHECK(extra.status == SubmitStatus::Accepted);
    CHECK(&core.history().front() == prefix);
    CHECK(as<AcceptedEvent>(core.history()[1])->request.label == "p1");
    CHECK(core.live().front().request.label == "p0");
}

void bounded_many_commands() {
    WorkingRequestCore core(kRun);
    uint64_t inc = 1;
    uint64_t ord = 1;
    RequestHandle first{};
    for (int i = 0; i < 64; ++i) {
        const auto result = core.submit(tx(1.0, "m" + std::to_string(i)), i, inc, ord);
        CHECK(result.status == SubmitStatus::Accepted);
        CHECK(result.event_ordinal == static_cast<uint64_t>(i + 1));
        if (i == 0) first = *result.handle;
    }
    CHECK(core.live().size() == 64);
    CHECK(core.history().size() == 64);
    CHECK(as<AcceptedEvent>(core.history().front())->handle == first);
    CHECK(as<AcceptedEvent>(core.history().front())->request.label == "m0");
    CHECK(core.live().back().request.label == "m63");

    for (int i = 0; i < 32; ++i) {
        const RequestHandle handle = core.live().front().handle;
        const auto cancelled = core.cancel(handle, ord);
        CHECK(cancelled.status == CancelStatus::Cancelled);
    }
    CHECK(core.live().size() == 32);
    CHECK(core.history().size() == 96);
    CHECK(core.live().front().request.label == "m32");
    CHECK(as<AcceptedEvent>(core.history().front())->request.label == "m0");

    const auto replaced = core.replace(core.live().front().handle, tx(2.0, "rep"), 100, inc, ord);
    CHECK(replaced.status == ReplaceStatus::Replaced);
    CHECK(core.live().size() == 32);
    CHECK(core.live().back().request.label == "rep");
    CHECK(core.live().back().predecessor->incarnation == 33);
    CHECK(as<AcceptedEvent>(core.history().front())->request.label == "m0");
    CHECK(core.history().size() == 97);
}
}  // namespace

int main() {
    identity_binding();
    submit_accept_and_reject();
    quantity_rejections_preserve_bits();
    flatten_and_signed_reduce();
    grid_accepts_and_preserves_bits();
    grid_rejects_off_grid_and_half();
    submit_before_point_eligibility();
    replace_valid_invalid_and_stale_cancel();
    foreign_zero_and_absent_handles();
    metadata_and_const_views();
    counters_do_not_partially_mutate();
    reset_makes_old_handles_invalid();
    move_construction_transfers_and_reset_rebinds();
    move_assignment_replaces_destination_and_preserves_self_move();
    cancel_live_erases_only_that_request();
    same_counter_alias();
    reused_and_gapped_counters();
    aliased_const_arguments();
    history_prefix_append_stable();
    bounded_many_commands();
    std::printf("native order core: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
