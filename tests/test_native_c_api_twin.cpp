// test_native_c_api_twin.cpp — the C++ half of the L13 twin, and the test's
// entry point.
//
// The same bars, the same run specification and the same two commands are
// driven twice: once through NativeStrategyHost directly, once through the C
// API in test_native_c_api.c. Closed trades, the physical position and the
// whole recorded event history must be identical, for a market entry and for
// a kernel-sized (L3) one. The pure-C behaviour suite runs after them.
//
// Nothing here reaches a source or compat header: this is a kernel test and
// builds in the kernel-only profile.

#include "native_c_api_twin.h"

#include <pineforge/native_host.hpp>

#include <cstdio>
#include <cstring>
#include <string>
#include <variant>

namespace {

namespace no = pineforge::native_order;

int failures = 0;

void fail(const char* what, const std::string& detail = {}) {
    std::fprintf(stderr, "FAIL %s%s%s\n", what, detail.empty() ? "" : ": ", detail.c_str());
    ++failures;
}

void check(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) fail(what, detail);
}

// Bit-exact: a twin that tolerates drift proves nothing.
bool same(double a, double b) {
    return std::memcmp(&a, &b, sizeof(double)) == 0;
}

pineforge::NativeRunSpec twin_spec() {
    pineforge::NativeRunSpec spec;
    spec.identity.session_key = "native-c-api-twin";
    spec.identity.run_number = 1;
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.ticker = "MOCK";
    spec.tickerid = "TEST:MOCK";
    spec.type = "crypto";
    spec.currency = "USDT";
    spec.basecurrency = "ETH";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 10000.0;
    spec.point_value = 1.0;
    spec.account_fx = 1.0;
    spec.price_tick = 0.01;
    spec.fee_kind = pineforge::NativeFeeKind::Percent;
    spec.fee_value = 0.0;
    return spec;
}

// The C++ twin of the C host in test_native_c_api.c: same hooks, same
// commands, same calculation indices.
class TwinHost final : public pineforge::NativeStrategyHost {
public:
    explicit TwinHost(bool sized) : sized_(sized) {}

private:
    void on_native_run_begin() override { calculations_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++calculations_;
        if (calculations_ == PF_TWIN_ENTRY_BAR) {
            no::Request request;
            if (sized_) {
                no::Sized sized;
                sized.side = no::Side::Long;
                sized.basis = no::CashValue{1000.0};
                sized.time = no::SizeTime::AtMatch;
                sized.grid_policy = no::ExecutionGridPolicy::SnapToGrid;
                sized.reserve_percent_fee = false;
                request.intent = sized;
                request.label = "twin-sized";
            } else {
                request.intent = no::Transact{1.0};
                request.label = "twin-long";
            }
            submit(request);
        } else if (calculations_ == PF_TWIN_EXIT_BAR) {
            no::Request request;
            request.intent = no::Flatten{};
            request.label = "twin-flat";
            submit(request);
        }
    }

    bool sized_ = false;
    int calculations_ = 0;
};

void collect_cpp(TwinHost& host, pf_twin_result& out) {
    std::memset(&out, 0, sizeof(out));

    const pf_bar_t* bars = pf_twin_bars(nullptr);
    int n = 0;
    pf_twin_bars(&n);
    host.run(reinterpret_cast<const pineforge::Bar*>(bars), n);
    out.completed =
        host.native_state().kind == pineforge::NativeLifecycleKind::Completed ? 1 : 0;

    pf_report_t report;
    std::memset(&report, 0, sizeof(report));
    host.fill_report(reinterpret_cast<pineforge::ReportC*>(&report));
    out.trade_count = report.total_trades;
    if (out.trade_count > PF_TWIN_MAX_TRADES) out.trade_count = PF_TWIN_MAX_TRADES;
    for (int i = 0; i < out.trade_count; ++i) {
        out.trades[i].entry_price = report.trades[i].entry_price;
        out.trades[i].exit_price = report.trades[i].exit_price;
        out.trades[i].qty = report.trades[i].qty;
        out.trades[i].pnl = report.trades[i].pnl;
        out.trades[i].entry_time = report.trades[i].entry_time;
        out.trades[i].exit_time = report.trades[i].exit_time;
        out.trades[i].is_long = report.trades[i].is_long;
    }
    pineforge::BacktestEngine::free_report(reinterpret_cast<pineforge::ReportC*>(&report));

    const auto position = host.physical_position();
    out.signed_units = position.signed_units;
    out.average_price = position.average_price;
    out.lots = static_cast<uint64_t>(position.lot_count);

    // The C tags are the CommandEvent alternative index plus one, and 19 / 20
    // for the driver and account rows: this is the same mapping the C side
    // reads back, computed independently here.
    for (const auto& event : host.native_events(0)) {
        if (out.event_count >= PF_TWIN_MAX_EVENTS) break;
        const int slot = out.event_count;
        switch (event.kind) {
        case pineforge::NativeEventKind::Command: {
            if (!event.command) continue;
            out.event_kind[slot] =
                static_cast<uint32_t>(event.command->index()) + 1u;
            if (const auto* applied =
                    std::get_if<no::ExecutionAppliedEvent>(&*event.command)) {
                out.applied_price[slot] = applied->resolved_price;
                out.applied_closed[slot] = applied->closed_units;
                out.applied_opened[slot] = applied->opened_units;
            }
            break;
        }
        case pineforge::NativeEventKind::Driver:
            if (!event.driver) continue;
            out.event_kind[slot] = 19u;
            break;
        case pineforge::NativeEventKind::Account:
            if (!event.account) continue;
            out.event_kind[slot] = 20u;
            break;
        }
        out.event_ordinal[slot] = event.ordinal;
        ++out.event_count;
    }
}

void compare(const char* arm, const pf_twin_result& c, const pf_twin_result& cpp) {
    const std::string tag(arm);
    check(c.completed == 1, (tag + ": the C run did not complete").c_str());
    check(cpp.completed == 1, (tag + ": the C++ run did not complete").c_str());
    check(c.trade_count == cpp.trade_count, (tag + ": closed-trade count differs").c_str(),
          std::to_string(c.trade_count) + " vs " + std::to_string(cpp.trade_count));
    check(c.trade_count == 1, (tag + ": expected exactly one closed trade").c_str(),
          std::to_string(c.trade_count));
    for (int i = 0; i < c.trade_count && i < cpp.trade_count; ++i) {
        const auto& a = c.trades[i];
        const auto& b = cpp.trades[i];
        check(same(a.entry_price, b.entry_price), (tag + ": entry price differs").c_str());
        check(same(a.exit_price, b.exit_price), (tag + ": exit price differs").c_str());
        check(same(a.qty, b.qty), (tag + ": quantity differs").c_str());
        check(same(a.pnl, b.pnl), (tag + ": pnl differs").c_str());
        check(a.entry_time == b.entry_time, (tag + ": entry time differs").c_str());
        check(a.exit_time == b.exit_time, (tag + ": exit time differs").c_str());
        check(a.is_long == b.is_long, (tag + ": direction differs").c_str());
    }
    check(same(c.signed_units, cpp.signed_units), (tag + ": position units differ").c_str());
    check(same(c.average_price, cpp.average_price),
          (tag + ": position average price differs").c_str());
    check(c.lots == cpp.lots, (tag + ": lot count differs").c_str());
    check(same(c.signed_units, 0.0), (tag + ": the twin did not end flat").c_str());

    check(c.event_count == cpp.event_count, (tag + ": event count differs").c_str(),
          std::to_string(c.event_count) + " vs " + std::to_string(cpp.event_count));
    check(c.event_count > 0, (tag + ": no events were recorded").c_str());
    for (int i = 0; i < c.event_count && i < cpp.event_count; ++i) {
        check(c.event_ordinal[i] == cpp.event_ordinal[i],
              (tag + ": event ordinal differs").c_str(), std::to_string(i));
        check(c.event_kind[i] == cpp.event_kind[i], (tag + ": event kind differs").c_str(),
              std::to_string(i));
        check(same(c.applied_price[i], cpp.applied_price[i]),
              (tag + ": applied price differs").c_str(), std::to_string(i));
        check(same(c.applied_closed[i], cpp.applied_closed[i]),
              (tag + ": applied closed units differ").c_str(), std::to_string(i));
        check(same(c.applied_opened[i], cpp.applied_opened[i]),
              (tag + ": applied opened units differ").c_str(), std::to_string(i));
    }
}

// The C++ twin of cancel_where_on_bar in test_native_c_api.c: the same three
// crossed requests and the same three bulk calls, spelled with the C++
// overloads.
class CancelWhereHost final : public pineforge::NativeStrategyHost {
public:
    explicit CancelWhereHost(pf_twin_cancel_where& out) : out_(&out) {}

private:
    void on_native_run_begin() override { calculations_ = 0; }

    void on_native_bar(const pineforge::Bar&,
                       const pineforge::NativeDecisionContext&) override {
        ++calculations_;
        if (calculations_ != 5 || out_->ran) return;
        out_->ran = 1;
        put("leg", "entry");
        put("leg", "exit");
        put("other", "leg");
        out_->comment_hits = static_cast<int>(cancel_where("leg"));
        out_->live_after_comment = static_cast<int>(native_working_requests().size());
        out_->label_hits =
            static_cast<int>(cancel_where("leg", pineforge::NativeRequestField::Label));
        out_->live_after_label = static_cast<int>(native_working_requests().size());
        out_->unmatched_hits =
            static_cast<int>(cancel_where("nobody", pineforge::NativeRequestField::Label));
    }

    void put(const char* label, const char* comment) {
        no::Request request;
        request.intent = no::Transact{1.0};
        request.trigger = no::Limit{1.0};
        request.label = label;
        request.comment = comment;
        if (submit(request).status != no::SubmitStatus::Accepted) {
            fail("the cancel_where twin could not rest a request", label);
        }
    }

    pf_twin_cancel_where* out_;
    int calculations_ = 0;
};

void run_cancel_where_twin() {
    pf_twin_cancel_where from_c;
    pf_twin_cancel_where from_cpp;
    std::memset(&from_c, 0, sizeof(from_c));
    std::memset(&from_cpp, 0, sizeof(from_cpp));

    const int rc = pf_twin_run_c_cancel_where(&from_c);
    check(rc == 0, "cancel_where: the C arm reported a command error", std::to_string(rc));

    CancelWhereHost host(from_cpp);
    if (host.configure_native(twin_spec()).status != pineforge::NativeSetupStatus::Applied) {
        fail("configure_native refused the cancel_where twin", host.last_error());
        return;
    }
    const pf_bar_t* bars = pf_twin_bars(nullptr);
    int n = 0;
    pf_twin_bars(&n);
    host.run(reinterpret_cast<const pineforge::Bar*>(bars), n);
    from_cpp.completed =
        host.native_state().kind == pineforge::NativeLifecycleKind::Completed ? 1 : 0;

    // The counts the kernel answered, field by field across the two surfaces.
    check(from_c.completed == 1, "cancel_where: the C run did not complete");
    check(from_cpp.completed == 1, "cancel_where: the C++ run did not complete");
    check(from_c.ran == 1 && from_cpp.ran == 1, "cancel_where: an arm never ran");
    check(from_c.comment_hits == from_cpp.comment_hits,
          "cancel_where: comment count differs",
          std::to_string(from_c.comment_hits) + " vs " + std::to_string(from_cpp.comment_hits));
    check(from_c.live_after_comment == from_cpp.live_after_comment,
          "cancel_where: live rows after the comment call differ");
    check(from_c.label_hits == from_cpp.label_hits,
          "cancel_where: label count differs",
          std::to_string(from_c.label_hits) + " vs " + std::to_string(from_cpp.label_hits));
    check(from_c.live_after_label == from_cpp.live_after_label,
          "cancel_where: live rows after the label call differ");
    check(from_c.unmatched_hits == from_cpp.unmatched_hits,
          "cancel_where: unmatched count differs");

    // What those counts must be: the comment form takes the one request whose
    // COMMENT is "leg" and leaves the two labelled ones; the label form then
    // takes exactly those two; an unmatched text is not a command.
    check(from_c.comment_hits == 1, "cancel_where: the comment form did not take one row",
          std::to_string(from_c.comment_hits));
    check(from_c.live_after_comment == 2,
          "cancel_where: the comment form did not leave the two labelled rows",
          std::to_string(from_c.live_after_comment));
    check(from_c.label_hits == 2, "cancel_where: the label form did not take both rows",
          std::to_string(from_c.label_hits));
    check(from_c.live_after_label == 0, "cancel_where: the label form left a live request",
          std::to_string(from_c.live_after_label));
    check(from_c.unmatched_hits == 0, "cancel_where: an unmatched label cancelled something");

    // C-only rows: the header's two documented refusals.
    check(from_c.refused_unknown_field == 1,
          "cancel_where: an unknown field selector was not refused as PF_NATIVE_E_TAG");
    check(from_c.refused_null_text == 1,
          "cancel_where: a NULL text was not refused as PF_NATIVE_E_ARGUMENT");
}

void run_twin(const char* arm, bool sized, int (*c_arm)(pf_twin_result*)) {
    pf_twin_result from_c;
    pf_twin_result from_cpp;
    std::memset(&from_c, 0, sizeof(from_c));
    std::memset(&from_cpp, 0, sizeof(from_cpp));

    const int rc = c_arm(&from_c);
    check(rc == 0, (std::string(arm) + ": the C arm reported a command error").c_str(),
          std::to_string(rc));

    TwinHost host(sized);
    if (host.configure_native(twin_spec()).status != pineforge::NativeSetupStatus::Applied) {
        fail("configure_native refused the twin specification", host.last_error());
        return;
    }
    collect_cpp(host, from_cpp);
    compare(arm, from_c, from_cpp);
}

}  // namespace

int main() {
    run_twin("market", false, &pf_twin_run_c_market);
    run_twin("sized", true, &pf_twin_run_c_sized);
    run_cancel_where_twin();

    const int suite = pf_native_c_api_checks();
    if (suite != 0) {
        std::fprintf(stderr, "FAIL pure-C suite reported %d failures\n", suite);
        failures += suite;
    }

    if (failures != 0) {
        std::fprintf(stderr, "test_native_c_api: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_native_c_api: ok\n");
    return 0;
}
