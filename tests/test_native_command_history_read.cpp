// R5 lane PERF-P4: the kernel's in-place read of its own command history.
//
// A host that watches its receipts reads the command events above a cursor;
// the Pine adapter does it three times a bar. Until this lane the only read
// was native_events(after), which copies EVERY row above the cursor -- the
// command events, but also each driver point and account observation, 712
// bytes a row -- into a fresh vector, so a quiet bar paid an allocation and
// four or five row copies per read to learn that no command had arrived. The
// consumer now also answers the command rows in place:
//
//   first_command_after(after)      the first history index above `after`;
//   visit_commands_after(after, f)  a copy of each event from there on, in
//                                   history order, as the history stood when
//                                   the visit began.
//
// The in-place read must BE the materialised one, row for row, or a hashed
// receipt cursor moves. Witnessed here over a randomized command stream (a
// market, limit, stop, stop-limit, cancel-group and reduce-group mix, with
// replaces, cancels, flattens, armed brackets and host-refused candidates),
// with and without a synthesized intrabar path:
//
//  1. For every cursor from 0 to one past the high water after the run, and
//     for sampled cursors inside every callback kind during it, the visited
//     events are exactly the command rows of native_events(cursor), in order,
//     and first_command_after(cursor) is the count of events at or below it.
//  2. A reader that advances a cursor over the materialised rows -- to each
//     row's ordinal before its visit, as the Pine adapter does -- holds the
//     same cursor at every command visit, and ends on the same value, as one
//     that walks the in-place events and then takes the high water.
//  3. A visitor that appends to the history while it is visited (it issues
//     cancels, enough to reallocate the history) sees exactly the events the
//     materialised snapshot held, never what it appended, and the event it
//     holds stays intact across the reallocation.
//  4. A read over a span that holds driver points and account rows but no
//     command copies nothing and allocates nothing; native_events allocates.
//
// Fail-before: against the tree before this lane the TU does not compile --
// NativeExecutionConsumer has no first_command_after / visit_commands_after.
#include <pineforge/native_host.hpp>

#include "../src/native_execution_consumer.hpp"

// Counts the heap allocations of (4): every replaceable form, one allocator.
#include "global_allocation_replacement.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <string>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace no = pineforge::native_order;

int failures = 0;
int checks = 0;
#define CHECK(condition) do {                                                  \
    ++checks;                                                                  \
    if (!(condition)) {                                                        \
        std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #condition); \
        ++failures;                                                            \
    }                                                                          \
} while (0)

std::uint64_t ordinal_of(const no::CommandEvent& event) {
    return std::visit([](const auto& payload) { return payload.ordinal; }, event);
}

// What a cursor-advancing reader saw: the command events it visited, their
// variant alternatives, the cursor it held at each visit and where it ended.
struct Walk {
    std::vector<std::uint64_t> ordinals;
    std::vector<std::size_t> kinds;
    std::vector<std::uint64_t> cursors;
    std::uint64_t final_cursor = 0;
};

bool operator==(const Walk& a, const Walk& b) {
    return a.ordinals == b.ordinals && a.kinds == b.kinds && a.cursors == b.cursors
        && a.final_cursor == b.final_cursor;
}

std::uint64_t splitmix(std::uint64_t& state) {
    std::uint64_t z = (state += 0x9e3779b97f4a7c15ull);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

class Probe final : public NativeStrategyHost {
public:
    explicit Probe(std::uint64_t seed) : rng_(seed) {}

    const NativeExecutionConsumer& kernel() const {
        return as_native_consumer(const_cast<IExecutionConsumer&>(execution_consumer()));
    }

    // The materialised read: native_events, a cursor advanced over every row.
    Walk materialised(std::uint64_t after) const {
        Walk walk;
        walk.final_cursor = after;
        for (const auto& row : native_events(after)) {
            walk.final_cursor = std::max(walk.final_cursor, row.ordinal);
            if (!row.command) continue;
            walk.ordinals.push_back(ordinal_of(*row.command));
            walk.kinds.push_back(row.command->index());
            walk.cursors.push_back(walk.final_cursor);
        }
        return walk;
    }

    // The in-place read: the command events, a cursor advanced over each one
    // before its visit, then to the high water the materialised rows reach.
    Walk in_place(std::uint64_t after) const {
        Walk walk;
        walk.final_cursor = after;
        const std::uint64_t high_water = kernel().event_high_water();
        if (high_water <= after) return walk;
        kernel().visit_commands_after(after, [&](const no::CommandEvent& event) {
            walk.final_cursor = std::max(walk.final_cursor, ordinal_of(event));
            walk.ordinals.push_back(ordinal_of(event));
            walk.kinds.push_back(event.index());
            walk.cursors.push_back(walk.final_cursor);
            return false;
        });
        walk.final_cursor = std::max(walk.final_cursor, high_water);
        return walk;
    }

    // Every command ordinal, in history order, read in place from the start.
    std::vector<std::uint64_t> history_ordinals() const {
        std::vector<std::uint64_t> out;
        kernel().visit_commands_after(0, [&](const no::CommandEvent& event) {
            out.push_back(ordinal_of(event));
            return false;
        });
        return out;
    }

    // `history` is history_ordinals() at the time of the call.
    void compare(std::uint64_t after, const std::vector<std::uint64_t>& history) const {
        const Walk old_read = materialised(after);
        const Walk new_read = in_place(after);
        CHECK(old_read == new_read);
        const auto at_or_below = static_cast<std::size_t>(
            std::upper_bound(history.begin(), history.end(), after) - history.begin());
        CHECK(kernel().first_command_after(after) == at_or_below);
        CHECK(new_read.ordinals.size() == history.size() - at_or_below);
        ++compared_;
    }

    // Sampled cursors at a callback: 0, the high water and its neighbours,
    // every command of the last few, and random ordinals.
    void cross_check() const {
        const std::uint64_t high_water = kernel().event_high_water();
        std::vector<std::uint64_t> cursors{0, high_water, high_water + 1};
        if (high_water > 0) cursors.push_back(high_water - 1);
        const auto history = history_ordinals();
        for (std::size_t i = history.size() > 3 ? history.size() - 3 : 0; i < history.size(); ++i) {
            cursors.push_back(history[i] - 1);
            cursors.push_back(history[i]);
        }
        for (int i = 0; i < 3; ++i) cursors.push_back(splitmix(rng_) % (high_water + 2));
        for (const auto cursor : cursors) compare(cursor, history);
    }

    void on_native_bar_open(const Bar&, const NativeDecisionContext&) override {
        cross_check();
    }

    void on_native_applied(const no::ExecutionAppliedEvent&,
                           const NativeDecisionContext&) override {
        cross_check();
        if (bars_ < kQuietFrom && splitmix(rng_) % 4 == 0) act(last_close_);
    }

    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView&) const override {
        cross_check();
        return ++precommits_ % 7 == 0 ? NativePrecommitVerdict::Refuse
                                      : NativePrecommitVerdict::Admit;
    }

    // The tape ends quietly: everything working is cancelled and the book
    // flattened, so the last bars append driver points and no command.
    static constexpr int kQuietFrom = 72;

    void on_native_bar(const Bar& bar, const NativeDecisionContext&) override {
        last_close_ = bar.close;
        cross_check();
        if (!appended_ && bars_ == 40) append_while_visiting();
        if (bars_ < kQuietFrom) {
            for (int i = static_cast<int>(splitmix(rng_) % 3); i > 0; --i) act(bar.close);
        } else if (bars_ == kQuietFrom) {
            (void)cancel_all();
            (void)submit(no::Request{no::Flatten{}, "quiet", ""});
        }
        ++bars_;
    }

    int bars() const { return bars_; }
    int compared() const { return compared_; }
    bool appended() const { return appended_; }

private:
    double level(double close, int ticks) const { return close + 0.25 * ticks; }

    void remember(const no::SubmitResult& result) {
        if (result.handle) working_.push_back(*result.handle);
    }

    // One random command. Refusals and no-effect answers are fine: they are
    // history events too.
    void act(double close) {
        const auto pick = splitmix(rng_) % 100;
        const double units = 1.0 + static_cast<double>(splitmix(rng_) % 3);
        const bool buy = splitmix(rng_) % 2 == 0;
        const int ticks = static_cast<int>(splitmix(rng_) % 9) - 4;
        if (pick < 20) {
            remember(submit(no::Request{no::Transact{buy ? units : -units}, "mkt", ""}));
        } else if (pick < 38) {
            auto request = no::Request{no::Transact{buy ? units : -units}, "lmt", ""};
            request.trigger = no::Limit{level(close, buy ? -std::abs(ticks) : std::abs(ticks))};
            remember(submit(request));
        } else if (pick < 48) {
            auto request = no::Request{no::Transact{buy ? units : -units}, "stp", ""};
            request.trigger = no::Stop{level(close, buy ? std::abs(ticks) + 1 : -std::abs(ticks) - 1)};
            remember(submit(request));
        } else if (pick < 55) {
            auto request = no::Request{no::Transact{buy ? units : -units}, "stl", ""};
            const double stop = level(close, buy ? 1 : -1);
            request.trigger = no::StopLimit{stop, stop + (buy ? 0.5 : -0.5)};
            remember(submit(request));
        } else if (pick < 65) {
            // A group of two buy limits; under Reduce the second carries more
            // units, so the first one's fill reduces it rather than ending it.
            const auto effect = splitmix(rng_) % 2 == 0 ? no::GroupEffect::Reduce
                                                        : no::GroupEffect::Cancel;
            const std::uint64_t group = ++groups_;
            for (int side = 0; side < 2; ++side) {
                auto request = no::Request{no::Transact{units + 2.0 * side}, "oca", ""};
                request.trigger = no::Limit{level(close, -1 - side)};
                request.group = no::Member{group, 0, effect};
                remember(submit(request));
            }
        } else if (pick < 75) {
            if (working_.empty()) return;
            const auto index = static_cast<std::size_t>(splitmix(rng_) % working_.size());
            (void)cancel(working_[index]);
            working_.erase(working_.begin() + static_cast<std::ptrdiff_t>(index));
        } else if (pick < 82) {
            // Re-price a resting limit (or a stale handle: a refused replace
            // is a history event too).
            auto request = no::Request{no::Transact{units}, "rpl", ""};
            request.trigger = no::Limit{level(close, -20)};
            const auto resting = submit(request);
            request.trigger = no::Limit{level(close, -8 - ticks)};
            const bool stale = working_.size() > 1 && splitmix(rng_) % 4 == 0;
            const auto replaced = replace(stale ? working_.front() : *resting.handle, request);
            if (replaced.successor) working_.push_back(*replaced.successor);
        } else if (pick < 88) {
            remember(submit(no::Request{no::Flatten{}, "flat", ""}));
        } else {
            const auto parent = submit(no::Request{no::Transact{buy ? units : -units}, "par", ""});
            remember(parent);
            if (!parent.handle) return;
            auto child = no::Request{no::Reduce{no::ExplicitUnits{units}}, "leg", ""};
            child.trigger = no::Stop{level(close, buy ? -6 : 6)};
            child.owner = no::WaitForApplied{*parent.handle};
            remember(submit(child));
        }
    }

    // (3): the visitor cancels enough to outgrow the history's capacity while
    // it is being visited.
    void append_while_visiting() {
        const auto history = history_ordinals();
        if (history.size() < 4) return;
        const std::uint64_t after = history[history.size() - 4];
        std::vector<std::uint64_t> snapshot;
        for (const auto& row : native_events(after))
            if (row.command) snapshot.push_back(ordinal_of(*row.command));
        const std::size_t appends = history.size() + 1;
        std::vector<std::uint64_t> seen;
        bool issued = false;
        kernel().visit_commands_after(after, [&](const no::CommandEvent& event) {
            const std::uint64_t ordinal = ordinal_of(event);
            const std::size_t kind = event.index();
            if (!issued) {
                issued = true;
                // A handle of this run that names no request: every cancel of
                // it is refused, and every refusal is one appended event.
                no::RequestHandle stale = working_.empty() ? no::RequestHandle{} : working_.front();
                stale.incarnation = std::numeric_limits<std::uint64_t>::max() / 2;
                for (std::size_t i = 0; i < appends; ++i) (void)cancel(stale);
            }
            CHECK(ordinal_of(event) == ordinal);
            CHECK(event.index() == kind);
            seen.push_back(ordinal);
            return false;
        });
        CHECK(issued);
        CHECK(seen == snapshot);
        std::size_t after_append = 0;
        for (const auto& row : native_events(after))
            if (row.command) ++after_append;
        CHECK(after_append >= snapshot.size() + appends);
        appended_ = true;
    }

    mutable std::uint64_t rng_;
    mutable int precommits_ = 0;
    mutable int compared_ = 0;
    std::vector<no::RequestHandle> working_;
    std::uint64_t groups_ = 0;
    double last_close_ = 100.0;
    int bars_ = 0;
    bool appended_ = false;
};

NativeRunSpec spec_for(bool path, std::uint64_t run) {
    NativeRunSpec spec;
    // Reads its whole event record once the run has ended (V19-B).
    spec.event_retention = NativeEventRetention::Full;
    spec.identity = {"perf-p4-history-read", run};
    spec.input_tf = "1";
    spec.script_tf = "1";
    spec.tickerid = "TEST:P4";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.initial_capital = 100000;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    spec.fee_kind = NativeFeeKind::CashPerExecution;
    spec.fee_value = 0.5;
    spec.close_execution = NativeCloseExecution::AfterCalculation;
    if (path) spec.intrabar.value = IntrabarPath::synthesized{4};
    return spec;
}

std::vector<Bar> tape(int count, std::uint64_t seed) {
    std::vector<Bar> bars;
    double price = 100.0;
    for (int i = 0; i < count; ++i) {
        const int step = static_cast<int>(splitmix(seed) % 9) - 4;
        const double open = price;
        const double close = price + 0.25 * step;
        const double high = std::max(open, close) + 0.25 * static_cast<double>(splitmix(seed) % 4);
        const double low = std::min(open, close) - 0.25 * static_cast<double>(splitmix(seed) % 4);
        bars.push_back({open, high, low, close, 1.0,
                        1736121600000LL + static_cast<std::int64_t>(i) * 60000});
        price = close;
    }
    return bars;
}

void run_one(bool path, std::uint64_t seed) {
    Probe probe(seed);
    CHECK(probe.configure_native(spec_for(path, seed)).status == NativeSetupStatus::Applied);
    const auto bars = tape(80, seed ^ 0x5eedull);
    probe.run(bars.data(), static_cast<int>(bars.size()));
    CHECK(probe.last_error().empty());
    CHECK(probe.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(probe.bars() == 80);
    CHECK(probe.appended());
    CHECK(probe.compared() > 1000);

    // The history is in ordinal order, strictly: the in-place read's premise.
    const auto history = probe.history_ordinals();
    CHECK(history.size() > 200);
    CHECK(std::adjacent_find(history.begin(), history.end(),
        [](std::uint64_t a, std::uint64_t b) { return a >= b; }) == history.end());

    // (1)(2): every cursor after the run.
    const std::uint64_t high_water = probe.kernel().event_high_water();
    for (std::uint64_t cursor = 0; cursor <= high_water + 1; ++cursor) probe.compare(cursor, history);

    // (4): a span of driver points and account rows only.
    const std::uint64_t last_command = history.back();
    CHECK(!probe.native_events(last_command).empty());
    std::size_t visited = 0;
    std::size_t allocations = global_allocation::allocations;
    probe.kernel().visit_commands_after(last_command, [&](const no::CommandEvent&) {
        ++visited;
        return false;
    });
    const std::size_t first = probe.kernel().first_command_after(last_command);
    allocations = global_allocation::allocations - allocations;
    CHECK(visited == 0);
    CHECK(first == history.size());
    CHECK(allocations == 0);
    allocations = global_allocation::allocations;
    const auto rows = probe.native_events(last_command);
    allocations = global_allocation::allocations - allocations;
    CHECK(!rows.empty());
    CHECK(allocations > 0);

    // A visitor's true stops the visit at that event.
    std::uint64_t stopped_at = 0;
    CHECK(probe.kernel().visit_commands_after(0, [&](const no::CommandEvent& event) {
        stopped_at = ordinal_of(event);
        return stopped_at == history[history.size() / 2];
    }));
    CHECK(stopped_at == history[history.size() / 2]);
    CHECK(!probe.kernel().visit_commands_after(high_water, [](const no::CommandEvent&) {
        return true;
    }));
}

}  // namespace

int main() {
    run_one(false, 1);
    run_one(true, 2);
    run_one(false, 3);
    run_one(true, 4);
    if (failures == 0) {
        std::printf("test_native_command_history_read: ok (%d checks)\n", checks);
        return 0;
    }
    std::fprintf(stderr, "test_native_command_history_read: %d of %d checks failed\n",
                 failures, checks);
    return 1;
}
