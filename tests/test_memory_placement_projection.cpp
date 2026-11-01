// Placement projections follow the submitted orders without product test hooks.
// Checks observe container counts and capacities; any byte-layout
// interpretation is an estimate from source, not an allocation measurement.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;
using Snapshot = source::PlacementSnapshot;
using Family = source::PineOrderFamily;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
int checks = 0;
int failures = 0;
const char* active_row = "setup";
int active_bars = 0;
int active_width = 0;

void check(bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (bars=%d width=%d)\n",
                 active_row, __FILE__, line, expression, active_bars, active_width);
}
#define CHECK(expression) check((expression), #expression, __LINE__)

// Same explicit-instantiation pattern as test_id_adapter_storage.cpp.
// Only production commands populate storage; these member pointers expose
// observations and the existing capture/invalidation operations.
template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(Tag, member) \
    struct Tag { friend auto access(Tag); }; \
    template struct Access<Tag, &Adapter::member>
MEMBER(Gate, retirement_gate_);
MEMBER(Capture, capture_retirement_inputs);
MEMBER(Invalidate, invalidate_retirement);
MEMBER(Indexes, projected_pending_indexes_);
MEMBER(Next, projected_pending_next_);
MEMBER(Valid, projected_pending_valid_);
MEMBER(Placements, placement_);
MEMBER(Live, live_handles_);
MEMBER(SameBar, pending_same_bar_commands_);
MEMBER(Entries, pending_entries_);
MEMBER(Brackets, pending_bracket_legs_);
MEMBER(Coof, pending_coof_requests_);
MEMBER(Shadows, source_shadow_pending_);
#undef MEMBER

std::vector<Bar> tape(int count) {
    std::vector<Bar> result;
    for (int i = 0; i < count; ++i)
        result.push_back({100.0, 100.0, 100.0, 100.0, 1.0,
                          kStart + static_cast<std::int64_t>(i) * 60000});
    return result;
}

class Host : public source::PineStrategyHost {
public:
    explicit Host(int width) {
        source::PineStrategyConfig config;
        config.initial_capital = 10000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = width + 4;
        config.close_entries_rule_any = true;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }

    Adapter& adapter() { return adapter_; }

    void capture_both() {
        // Explicitly exercise the uncertified capture door on the actual
        // two resident buffers. This changes derived scratch only. Calling
        // it immediately after cancellation prevents a later gate reset
        // from hiding missing refill retirement or compaction.
        (adapter_.*access(Invalidate{}))();
        auto& gate = adapter_.*access(Gate{});
        (adapter_.*access(Capture{}))(gate.saved);
        (adapter_.*access(Capture{}))(gate.current);
    }
};

struct Result {
    std::size_t positive = 0;
    std::size_t terminal = 0;
    bool populated = false;
    bool finished = false;
};

// retirement_input_marks: one base fill is not an addition; every subsequent market
// opening while the base stays open contributes precisely one distinct mark.
// The oracle records opaque handles from real fill notifications, never
// reads market_pyramid_adds_, and retires a whole wave after its named closes.
class MarksHost final : public Host {
public:
    MarksHost(int bars, int width) : Host(width), bars_(bars), width_(width) {}
    Result result;

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) strategy_entry("base", true, kNa, kNa, 1.0);
        if (bar == 1) {
            CHECK(live_position_size() == 1.0);
            for (int i = 0; i < width_; ++i)
                strategy_entry("add-" + std::to_string(i), true, kNa, kNa, 1.0);
        }
        if (bar >= 2 && bar <= bars_ - 14) {
            CHECK(live_position_size() == width_ + 1.0);
            CHECK(additions_.size() == static_cast<std::size_t>(width_));
            verify(additions_);
            result.positive = (adapter().*access(Gate{})).current.marks.size();
            result.populated = true;
        }
        if (bar == bars_ - 14)
            for (int i = 0; i < width_; ++i)
                strategy_close("add-" + std::to_string(i));
        if (bar == bars_ - 10) {
            CHECK(live_position_size() == 1.0);
            CHECK(closed_trade_count() == static_cast<std::size_t>(width_));
            verify({});
        }
        if (bar == bars_ - 9) strategy_entry("last-add", true, kNa, kNa, 1.0);
        if (bar == bars_ - 8) {
            CHECK(live_position_size() == 2.0);
            CHECK(additions_.size() == static_cast<std::size_t>(width_ + 1));
            std::vector<std::uint64_t> expected;
            if (!additions_.empty()) expected.push_back(additions_.back());
            verify(expected);
        }
        if (bar == bars_ - 7) strategy_close("last-add");
        if (bar == bars_ - 3) {
            CHECK(live_position_size() == 1.0);
            verify({});
            strategy_close("base");
        }
        if (bar == bars_ - 1) {
            CHECK(live_position_size() == 0.0);
            // width first-wave units + one later addition + one base.
            CHECK(closed_trade_count() == static_cast<std::size_t>(width_ + 2));
            CHECK(net_profit() == 0.0); // all entries/exits at 100, no fees
            verify({});
            result.terminal = (adapter().*access(Gate{})).current.marks.size();
            result.finished = true;
        }
    }

protected:
    void on_native_applied_event(const native_order::ExecutionAppliedEvent& event,
                                 const NativeDecisionContext&) override {
        if (event.opened_units == 0.0) return;
        CHECK(event.opened_units == 1.0 && event.closed_units == 0.0);
        if (openings_++ != 0) additions_.push_back(event.handle().incarnation);
    }

private:
    void verify(std::vector<std::uint64_t> expected) {
        std::sort(expected.begin(), expected.end());
        capture_both();
        const auto& gate = adapter().*access(Gate{});
        // Whole-container equality excludes historical marks and duplicate
        // captures. The source's compactor fires at capacity > 4*raw_count;
        // here each independently known addition contributes one raw mark.
        CHECK(gate.saved.marks == expected); // retirement_input_marks saved
        CHECK(gate.current.marks == expected); // retirement_input_marks current
        CHECK(gate.saved.marks.capacity() <= 4 * expected.size());
        CHECK(gate.current.marks.capacity() <= 4 * expected.size());
    }

    int bars_, width_, openings_ = 0;
    std::vector<std::uint64_t> additions_;
};

// retirement_input_bar_numbers: each never-filled Entry placed on a distinct script bar
// contributes that bar once; entries placed together share a single key.
// The independent model is updated at the strategy command, not by reading
// the adapter snapshots that capture_retirement_inputs itself consumes.
class BarsHost final : public Host {
public:
    BarsHost(int bars, int width) : Host(width), bars_(bars), width_(width) {}
    Result result;

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar < width_) {
            strategy_entry("pending-" + std::to_string(bar), true, 1.0, kNa, 1.0);
            created_.push_back(bar);
        }
        adapter().flush_pending_entries();
        if (bar < bars_ - 8) {
            verify();
            if (bar >= width_) {
                CHECK(native_working_requests().size() == static_cast<std::size_t>(width_));
                result.positive = (adapter().*access(Gate{})).current.bar_numbers.size();
                result.populated = true;
            }
        }
        if (bar == bars_ - 8) {
            // Warm both actual buffers immediately before the real cancel.
            // There is no engine callback/gate reset between these captures.
            verify();
            strategy_cancel_all();
            created_.clear();
            verify(); // retirement_input_bar_numbers obsolete keys and capacity must be gone

            // Two distinct live handles at the SAME bar give one distinct
            // bar key but two raw references. Exercise regrowth and dedupe.
            strategy_entry("twin-a", true, 1.0, kNa, 1.0);
            strategy_entry("twin-b", true, 1.0, kNa, 1.0);
            adapter().flush_pending_entries();
            created_ = {bar, bar};
            verify();
            CHECK(native_working_requests().size() == 2);
            strategy_cancel("twin-a");
            created_.pop_back();
            verify();
            strategy_cancel("twin-b");
            created_.clear();
            verify();
        }
        if (bar >= bars_ - 7) {
            verify();
            CHECK(native_working_requests().empty());
            CHECK(live_position_size() == 0.0 && closed_trade_count() == 0);
        }
        if (bar == bars_ - 1) {
            result.terminal = (adapter().*access(Gate{})).current.bar_numbers.size();
            result.finished = true;
        }
    }

private:
    void verify() {
        auto expected = created_;
        std::sort(expected.begin(), expected.end());
        expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
        capture_both();
        const auto& gate = adapter().*access(Gate{});
        CHECK(gate.saved.bar_numbers == expected); // retirement_input_bar_numbers saved
        CHECK(gate.current.bar_numbers == expected); // retirement_input_bar_numbers current
        // Compaction precedes distinct-key formation. There is one raw
        // reference per modeled live Entry, including the two same-bar twins.
        // retirement_capacity_excessive uses capacity > 4*size.
        CHECK(gate.saved.bar_numbers.capacity() <= 4 * created_.size());
        CHECK(gate.current.bar_numbers.capacity() <= 4 * created_.size());
    }

    int bars_, width_;
    std::vector<std::int32_t> created_;
};

// Keep raw slot numbers even if a live handle has no placement: an absent
// row consumes a slot in projected_raw_pending_size too. Never filter raw
// slots merely because the expected projection has fewer rows.
std::vector<std::optional<Snapshot>> raw_slots(const Adapter& adapter) {
    std::vector<std::optional<Snapshot>> slots;
    const auto& placements = adapter.*access(Placements{});
    for (const auto& handle : adapter.*access(Live{})) {
        const auto row = placements.find(handle.incarnation);
        if (row == placements.end()) slots.emplace_back(std::nullopt);
        else slots.emplace_back(row->second);
    }
    const auto append = [&](const auto& rows) {
        for (const auto& row : rows) slots.emplace_back(row.snapshot);
    };
    append(adapter.*access(SameBar{}));
    append(adapter.*access(Entries{}));
    append(adapter.*access(Brackets{}));
    append(adapter.*access(Coof{}));
    append(adapter.*access(Shadows{}));
    return slots;
}

bool exit_family(Family family) {
    return family == Family::ExitLimit || family == Family::ExitStop
        || family == Family::ExitTrail;
}

bool same_exit(const Snapshot& a, const Snapshot& b) {
    // Independently compare the complete identity, without the product's
    // same_projected_order helper or its hash/key implementation.
    return exit_family(a.family) && exit_family(b.family)
        && a.source_id == b.source_id && a.from_entry == b.from_entry
        && a.bracket_origin.incarnation == b.bracket_origin.incarnation
        && a.bracket_origin.run.run_number == b.bracket_origin.run.run_number
        && a.bracket_origin.run.session_key == b.bracket_origin.run.session_key;
}

struct Projection {
    std::vector<int> first;
    std::vector<int> next;
};

Projection model_projection(const Adapter& adapter) {
    const auto raw = raw_slots(adapter);
    Projection expected;
    expected.next.resize(raw.size(), -1);
    // Quadratic pairwise reference model: a raw row belongs in first iff
    // no earlier row shares its group. Its next is the first later peer.
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (!raw[i]) continue;
        bool earlier_peer = false;
        for (std::size_t j = 0; j < i; ++j)
            if (raw[j] && same_exit(*raw[i], *raw[j])) earlier_peer = true;
        if (!earlier_peer) expected.first.push_back(static_cast<int>(i));
        for (std::size_t j = i + 1; j < raw.size(); ++j) {
            if (raw[j] && same_exit(*raw[i], *raw[j])) {
                expected.next[i] = static_cast<int>(j);
                break;
            }
        }
    }
    return expected;
}

// Both projection rows get separate dispatches of the same parametrized
// fixture, so a failure for one row never suppresses checks for its sibling.
class ProjectionHost final : public Host {
public:
    ProjectionHost(int bars, int width, bool next)
        : Host(width), bars_(bars), width_(width), next_(next) {}
    Result result;

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) {
            strategy_entry("held", true, kNa, kNa, static_cast<double>(width_));
            return;
        }
        if (bar >= 1 && bar < bars_ - 6) {
            CHECK(live_position_size() == static_cast<double>(width_));
            if (bar == 1) {
                for (int i = 0; i < width_; ++i) bracket(i, bar);
            } else {
                bracket(bar % width_, bar);
            }
            verify(static_cast<std::size_t>(width_), 2 * static_cast<std::size_t>(width_));
            // Repeat the read with no writer: the disposable vectors must
            // contain exactly the same current slots, not an appended copy.
            verify(static_cast<std::size_t>(width_), 2 * static_cast<std::size_t>(width_));
            result.positive = observed_size();
            result.populated = true;
        }
        if (bar == bars_ - 6) {
            verify(static_cast<std::size_t>(width_), 2 * static_cast<std::size_t>(width_));
            strategy_cancel_all();
            released(); // before any observer can rebuild and mask retention
            verify(0, 0);
            bracket(0, bar);
            verify(1, 2);
            strategy_cancel_all();
            released();
            verify(0, 0);
            strategy_close("held");
        }
        if (bar >= bars_ - 3) {
            CHECK(live_position_size() == 0.0);
            CHECK(closed_trade_count() == 1);
            // A single width-unit parent closes at its entry price.
            CHECK(net_profit() == 0.0);
            verify(0, 0);
        }
        if (bar == bars_ - 1) {
            result.terminal = observed_size();
            result.finished = true;
        }
    }

private:
    std::size_t observed_size() {
        return next_ ? (adapter().*access(Next{})).size()
                     : (adapter().*access(Indexes{})).size();
    }

    void bracket(int slot, int generation) {
        // width * 1 unit = the held parent's width units, so all brackets
        // reserve real exposure. Flat price 100 reaches neither level.
        strategy_exit("exit-" + std::to_string(slot), "held",
                      200.0 + generation, 50.0 - slot * 0.25,
                      kNa, kNa, kNa, 100.0, {}, 1.0);
    }

    void verify(std::size_t groups, std::size_t raw_count) {
        const auto expected = model_projection(adapter());
        // Fixture arithmetic is separate from the raw-walk oracle:
        // each pending bracket has two legs and one projected group.
        CHECK(expected.first.size() == groups);
        CHECK(expected.next.size() == raw_count);
        CHECK(observe_pending_count_v1() == static_cast<int>(groups));
        CHECK(adapter().*access(Valid{}));
        if (next_) {
            const auto& actual = adapter().*access(Next{});
            CHECK(actual == expected.next); // pending_projection_links whole container
            CHECK(actual.size() == raw_count);
        } else {
            const auto& actual = adapter().*access(Indexes{});
            CHECK(actual == expected.first); // pending_projection_indexes whole container
            CHECK(actual.size() == groups);
        }
    }

    void released() {
        if (next_) {
            const auto& actual = adapter().*access(Next{});
            CHECK(actual.empty() && actual.capacity() == 0); // pending_projection_links release
        } else {
            const auto& actual = adapter().*access(Indexes{});
            CHECK(actual.empty() && actual.capacity() == 0); // pending_projection_indexes release
        }
    }

    int bars_, width_;
    bool next_;
};

enum class Kind { Marks, Bars, Indexes, Next };
struct Row {
    const char* id;
    Kind kind;
};

template<class Probe>
Result run_probe(Probe& host, int count) {
    const auto bars = tape(count);
    host.run(bars.data(), count, "1", "1");
    CHECK(host.last_error().empty());
    CHECK(host.result.populated && host.result.finished);
    return host.result;
}

Result trial(Kind kind, int bars, int width) {
    switch (kind) {
    case Kind::Marks: {
        MarksHost host(bars, width);
        return run_probe(host, bars);
    }
    case Kind::Bars: {
        BarsHost host(bars, width);
        return run_probe(host, bars);
    }
    case Kind::Indexes:
    case Kind::Next: {
        ProjectionHost host(bars, width, kind == Kind::Next);
        return run_probe(host, bars);
    }
    }
    CHECK(false);
    return {};
}
} // namespace

int main() {
    const std::array<Row, 4> rows{{
        {"retirement_input_marks", Kind::Marks},
        {"retirement_input_bar_numbers", Kind::Bars},
        {"pending_projection_indexes", Kind::Indexes},
        {"pending_projection_links", Kind::Next},
    }};
    for (const auto& row : rows) {
        active_row = row.id;
        for (const int width : {3, 17}) {
            active_width = width;
            std::array<Result, 2> results{};
            for (std::size_t run = 0; run < results.size(); ++run) {
                active_bars = run == 0 ? 64 : 256;
                try {
                    results[run] = trial(row.kind, active_bars, width);
                } catch (const std::exception& error) {
                    CHECK(false);
                    std::fprintf(stderr, "FAIL [%s] %s:%d: exception: %s\n",
                                 active_row, __FILE__, __LINE__, error.what());
                } catch (...) {
                    CHECK(false);
                }
            }
            // N and 4N bars have the same simultaneous live population.
            // Marks: width additions; bars: width distinct live Entry bars;
            // indexes: width groups; next: two raw legs for each group.
            const auto expected = static_cast<std::size_t>(
                row.kind == Kind::Next ? 2 * width : width);
            CHECK(results[0].populated && results[1].populated);
            CHECK(results[0].positive == expected && results[1].positive == expected);
            CHECK(results[0].positive == results[1].positive); // All placement projection cases use N and 4N inputs.
            CHECK(results[0].finished && results[1].finished);
            CHECK(results[0].terminal == 0 && results[1].terminal == 0);
            std::printf("[%s] width=%d N=64 count=%zu 4N=256 count=%zu final=%zu/%zu\n",
                        row.id, width, results[0].positive, results[1].positive,
                        results[0].terminal, results[1].terminal);
        }
    }
    std::printf("memory proof placement_projection: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
