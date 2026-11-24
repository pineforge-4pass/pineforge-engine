#include <pineforge/source/pine_strategy_host.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace orders = pineforge::native_order;

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start = 1736121600000LL;
constexpr std::int64_t minute = 60000;
constexpr int entry_bar = 1;
constexpr int release_bar = 2;
constexpr int target_cross = 3;
constexpr int control_cross = 4;
constexpr int bar_count = 6;

enum class Cancel { None, Bracket, Id };

struct Input {
    std::string target;
    double open;
    double parent_qty;
    double target_qty;
    double control_qty;
};

constexpr char parent[] = "parent";
constexpr char control[] = "control";

struct Book {
    int target = 0;
    int control = 0;
};

class Host final : public source::PineStrategyHost {
public:
    Host(const Input& input, Cancel cancel) : input_(input), cancel_(cancel) {
        configure_pine_strategy(source::PineStrategyConfig{});
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        fixture_retain_all_events();
    }

    std::array<Book, bar_count> book{};
    std::array<bool, bar_count> seen{};
    double position_at_cancel = na;

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar < 0 || bar >= bar_count) return;
        if (bar == 0) {
            strategy_entry(parent, true, na, na, input_.parent_qty);
            strategy_exit(input_.target, parent, na, input_.open - 5, na, na, na,
                          100, {}, input_.target_qty, input_.target);
            strategy_exit(control, parent, na, input_.open - 10, na, na, na,
                          100, {}, input_.control_qty, control);
            // Prearm both partial stops on the signal bar. These real adapter
            // flushes submit orders; they do not inject a delayed row or fill it.
            adapter_.flush_pending_entries();
            adapter_.flush_pending_bracket_legs();
        }
        for (const auto& row : native_working_requests()) {
            book[bar].target += row.definition->request.label == input_.target;
            book[bar].control += row.definition->request.label == control;
        }
        seen[bar] = true;
        if (bar == entry_bar) {
            position_at_cancel = live_position_size();
            if (cancel_ == Cancel::Bracket) strategy_exit_cancel_bracket(input_.target, parent);
            if (cancel_ == Cancel::Id) strategy_cancel(input_.target);
        }
    }

private:
    const Input& input_;
    Cancel cancel_;
};

struct History {
    orders::RequestHandle first{};
    int accepts = 0;
    int ends_of_first = 0;
    int late_submissions = 0;
    int fills = 0;
    int fill_bar = -1;
    double fill_qty = 0.0;

    void read(const orders::CommandEvent& event, const std::string& id) {
        if (const auto* add = std::get_if<orders::AcceptedEvent>(&event);
            add && add->request().label == id) {
            if (++accepts == 1) first = add->handle();
            else ++late_submissions;
        }
        if (const auto* replace = std::get_if<orders::ReplacedEvent>(&event);
            replace && replace->successor_request().label == id && ends_of_first != 0) {
            ++late_submissions;
        }
        if (const auto* end = std::get_if<orders::CancelledEvent>(&event);
            end && first.incarnation != 0 && end->handle() == first) {
            ++ends_of_first;
        }
        if (const auto* fill = std::get_if<orders::ExecutionAppliedEvent>(&event);
            fill && fill->request().label == id) {
            ++fills;
            fill_bar = fill->interval_index();
            fill_qty += fill->closed_units;
        }
    }
};

bool exercise(const Input& input, Cancel cancel) {
    const char* mode = cancel == Cancel::None ? "no-cancel"
                     : cancel == Cancel::Bracket ? "bracket" : "id";
    Host host(input, cancel);
    std::vector<Bar> bars;
    for (int i = 0; i < bar_count; ++i) {
        const double open = i == target_cross ? input.open - 7
                          : i == control_cross ? input.open - 12 : input.open;
        bars.push_back({open, open + 1, open - 1, open, 1, start + i * minute});
    }
    host.run(bars.data(), bar_count, "1", "1");
    if (!host.last_error().empty() || host.native_state().kind != NativeLifecycleKind::Completed
        || !host.seen[entry_bar] || !host.seen[release_bar] || !host.seen[target_cross]
        || !host.seen[control_cross] || !host.seen[bar_count - 1]) {
        std::fprintf(stderr, "FAIL setup %s %s: run did not reach every read point: %s\n",
                     mode, input.target.c_str(), host.last_error().c_str());
        return false;
    }

    History target;
    History survivor;
    for (const auto& row : host.native_events(0)) {
        if (!row.command) continue;
        target.read(*row.command, input.target);
        survivor.read(*row.command, control);
    }
    // Both stops start accepted then end when the parent fills. At the
    // cancellation bar neither original request is working. The control must
    // then return on a new acceptance, work through the target's crossing,
    // and fill on its own later bar. This witnesses a real queue drain.
    if (host.position_at_cancel != input.parent_qty || target.ends_of_first != 1
        || survivor.ends_of_first != 1 || host.book[entry_bar].target != 0
        || host.book[entry_bar].control != 0) {
        std::fprintf(stderr,
                     "FAIL setup %s %s: parent=%.17g (expected %.17g), original ends=%d/%d, "
                     "working at cancellation=%d/%d\n",
                     mode, input.target.c_str(), host.position_at_cancel, input.parent_qty,
                     target.ends_of_first, survivor.ends_of_first,
                     host.book[entry_bar].target, host.book[entry_bar].control);
        return false;
    }
    const bool cancelled = cancel != Cancel::None;
    bool ok = true;
    const auto equal = [&](const char* what, double observed, double expected) {
        if (observed == expected) return;
        ok = false;
        std::fprintf(stderr, "FAIL delayed exit %s %s: %s: observed %.17g, expected %.17g\n",
                     mode, input.target.c_str(), what, observed, expected);
    };
    equal("target working after delayed release", host.book[release_bar].target, cancelled ? 0 : 1);
    equal("target acceptances", target.accepts, cancelled ? 1 : 2);
    equal("target submissions after original request ended", target.late_submissions, cancelled ? 0 : 1);
    equal("target fills after cancellation point", target.fills, cancelled ? 0 : 1);
    equal("target fill bar", target.fill_bar, cancelled ? -1 : target_cross);
    equal("target closed units", target.fill_qty, cancelled ? 0.0 : input.target_qty);
    equal("control working after delayed release", host.book[release_bar].control, 1);
    equal("control still working after target crossing", host.book[target_cross].control, 1);
    equal("control acceptances", survivor.accepts, 2);
    equal("control fills", survivor.fills, 1);
    equal("control fill bar", survivor.fill_bar, control_cross);
    equal("control closed units", survivor.fill_qty, input.control_qty);
    equal("remaining position", host.live_position_size(),
          input.parent_qty - input.control_qty - (cancelled ? 0.0 : input.target_qty));
    return ok;
}
} // namespace

int main() {
    bool ok = true;
    for (const Input& input : {Input{"take-out", 100, 10, 2, 3},
                               Input{"remove-me", 124, 13, 4, 2}}) {
        for (const auto mode : {Cancel::None, Cancel::Bracket, Cancel::Id}) {
            if (!exercise(input, mode)) ok = false;
        }
    }
    return ok ? 0 : 1;
}
