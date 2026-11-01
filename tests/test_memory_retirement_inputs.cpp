// Retirement input buffers follow real admissions, fills and cancellations.
// This unit observes the two actual gate buffers. All admissions, fills,
// cancellations and closes go through source commands; no container is seeded.
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/source/pine_intent_state.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using Adapter = source::PineExecutionAdapter;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Gate { friend auto access(Gate); };
struct Placements { friend auto access(Placements); };
struct Live { friend auto access(Live); };
struct Sweep { friend auto access(Sweep); };
template struct Access<Gate, &Adapter::retirement_gate_>;
template struct Access<Placements, &Adapter::placement_>;
template struct Access<Live, &Adapter::live_handles_>;
template struct Access<Sweep, &Adapter::erase_retired_rows>;

enum class Row { Roots, Askable, Groups };
const char* row_id(Row row) {
    switch (row) {
    case Row::Roots: return "retirement_input_roots";
    case Row::Askable: return "retirement_input_askable";
    case Row::Groups: return "retirement_input_groups";
    }
    return "invalid-row";
}

std::size_t checks = 0, failures = 0;
const char* phase = "setup";
int trial_bars = 0, trial_pairs = 0;
void check(Row row, bool value, const char* expression, int line) {
    ++checks;
    if (value) return;
    ++failures;
    std::fprintf(stderr, "FAIL [%s] %s:%d: %s (phase=%s bars=%d pairs=%d)\n",
                 row_id(row), __FILE__, line, expression, phase, trial_bars, trial_pairs);
}
#define ROW_CHECK(row, expression) check(row, (expression), #expression, __LINE__)

constexpr double na = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t epoch = INT64_C(1736121600000);

struct Sample {
    std::array<std::size_t, 2> count{}, capacity{};
    bool operator==(const Sample& other) const {
        return count == other.count && capacity == other.capacity;
    }
};
struct Result {
    std::array<Sample, 4> samples{};
    std::array<bool, 4> reached{};
};

class Host final : public source::PineStrategyHost {
public:
    Host(Row row, int bars, int pairs) : row_(row), bars_(bars), pairs_(pairs) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2 * pairs + 2;
        config.process_orders_on_close = true;
        config.calc_on_order_fills = false;
        config.margin_long = config.margin_short = 0.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
        set_broker_state_hash_recording(false);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0) {
            strategy_entry("held", true, na, na, 1.0);
            return;
        }
        if (bar == 1) {
            ROW_CHECK(row_, live_position_size() == 1.0);
            origin_ = submitted_handle("held");
        }
        if (bar >= 1 && bar <= pairs_) {
            // At price 100, neither short LIMIT at 200 can fill. Each pair
            // is default-sized, reverses the held long and is published in
            // one callback. A different callback starts a different group.
            for (int member = 0; member < 2; ++member) {
                const auto id = order_id(bar, member);
                strategy_entry(id, false, 200.0);
                pending_.emplace(id, submitted_handle(id));
            }
        }
        if (bar == pairs_ + 2) {
            // Keep only the last callback's pair. The model removes exactly
            // the labels named by our commands, independently of gate data.
            for (int pair = 1; pair < pairs_; ++pair) {
                for (int member = 0; member < 2; ++member) {
                    const auto id = order_id(pair, member);
                    strategy_cancel(id);
                    pending_.erase(id);
                }
            }
            reduced_ = true;
        }
        if (bar == bars_ - 8) {
            strategy_cancel(order_id(pairs_, 0));
            strategy_cancel(order_id(pairs_, 1));
            pending_.clear();
            strategy_close("held");
        }
        if (bar == pairs_) observe(0, "peak", true);
        if (bar == pairs_ + 2) observe(1, "after-cancels", true);
        if (bar == bars_ - 12) observe(2, "quiet", true);
        if (bar == bars_ - 4) observe(3, "flat", false);
    }

    Result result;

private:
    Row row_;
    int bars_, pairs_;
    bool reduced_ = false;
    std::uint64_t origin_ = 0;
    std::map<std::string, std::uint64_t> pending_;

    static std::string order_id(int pair, int member) {
        return "reversal-" + std::to_string(pair) + "-" + std::to_string(member);
    }

    std::uint64_t submitted_handle(const std::string& id) {
        // Capture opaque identities from the authoritative placement created
        // by each command, never from a vector being proved. Numeric handle
        // allocation is not part of the test's arithmetic oracle.
        const auto& placements = adapter_.*access(Placements{});
        std::uint64_t handle = 0;
        std::size_t matches = 0;
        placements.visit_rows([&](std::uint64_t incarnation,
                                  const source::PlacementSnapshot& value) {
            if (value.source_id == id) {
                handle = incarnation;
                ++matches;
            }
            return false;
        });
        ROW_CHECK(row_, matches == 1 && handle != 0);
        return handle;
    }

    std::vector<std::uint64_t> expected(bool held) const {
        std::vector<std::uint64_t> values;
        if (row_ == Row::Groups) {
            if (held) {
                // The first pair allocates group 1, and each later callback
                // allocates one more. Both members share that one identity.
                const int first = reduced_ ? pairs_ : 1;
                for (int pair = first; pair <= pairs_; ++pair)
                    values.push_back(static_cast<std::uint64_t>(pair));
            }
        } else {
            // One held opening and one distinct identity per still-resting
            // submitted order: 2*pairs+1 at peak, 2+1 after cancellations.
            if (held) values.push_back(origin_);
            for (const auto& item : pending_) values.push_back(item.second);
            std::sort(values.begin(), values.end());
        }
        return values;
    }

    template<class Inputs>
    const std::vector<std::uint64_t>& values(const Inputs& inputs) const {
        switch (row_) {
        case Row::Roots: return inputs.roots;
        case Row::Askable: return inputs.askable;
        case Row::Groups: return inputs.groups;
        }
        return inputs.roots;
    }

    void observe(std::size_t slot, const char* label, bool held) {
        phase = label;
        // Drain real terminal notifications, then use the production gate
        // entrypoint, including its swap, second capture and compaction.
        // The held opening leaves a nonlive placement so capture is reached.
        adapter_.observe_terminal_receipts();
        const auto* point = source::detail::intent_live_point(*this);
        ROW_CHECK(row_, point != nullptr);
        if (!point) return;
        (adapter_.*access(Sweep{}))(point->decision);
        (adapter_.*access(Sweep{}))(point->decision);

        ROW_CHECK(row_, live_position_size() == (held ? 1.0 : 0.0));
        ROW_CHECK(row_, (adapter_.*access(Live{})).size() == pending_.size());
        const auto oracle = expected(held);
        const auto& gate = adapter_.*access(Gate{});
        const auto& saved = values(gate.saved);
        const auto& current = values(gate.current);

        // Each row has its own named full-container assertion. No prefix,
        // selected member, lower bound or work counter serves as evidence.
        if (row_ == Row::Roots) {
            ROW_CHECK(Row::Roots, saved == oracle && current == oracle);
        } else if (row_ == Row::Askable) {
            ROW_CHECK(Row::Askable, saved == oracle && current == oracle);
        } else {
            ROW_CHECK(Row::Groups, saved == oracle && current == oracle);
        }

        const std::size_t live = pending_.size();
        // Before unique, roots have three references per resting order
        // (adapter live, source-key map, native live) plus the held origin
        // twice (cohort opening and pyramid lot). Askable has live+held.
        // Groups has two equal references per callback's resting pair.
        // Receipt draining above leaves no unobserved-command references.
        const std::size_t raw = row_ == Row::Roots ? 3 * live + (held ? 2 : 0)
            : row_ == Row::Askable ? live + (held ? 1 : 0) : live;
        // The production policy compacts above four times the RAW refill
        // population. saved is also compacted after distinct-set formation.
        // This is a current-population bound, not an absolute pending cap.
        if (row_ == Row::Roots) {
            ROW_CHECK(Row::Roots, saved.capacity() <= 4 * raw && current.capacity() <= 4 * raw);
        } else if (row_ == Row::Askable) {
            ROW_CHECK(Row::Askable, saved.capacity() <= 4 * raw && current.capacity() <= 4 * raw);
        } else {
            ROW_CHECK(Row::Groups, saved.capacity() <= 4 * raw && current.capacity() <= 4 * raw);
        }
        if (!held) {
            // raw==0 requires released allocations as well as zero sizes.
            ROW_CHECK(row_, saved.empty() && current.empty()
                      && saved.capacity() == 0 && current.capacity() == 0);
            ROW_CHECK(row_, closed_trade_count() == 1);
        }
        result.samples[slot] = {{saved.size(), current.size()},
                                {saved.capacity(), current.capacity()}};
        result.reached[slot] = true;
    }
};

Result run(Row row, int bars, int pairs) {
    trial_bars = bars;
    trial_pairs = pairs;
    phase = "run";
    Host host(row, bars, pairs);
    std::vector<Bar> tape;
    tape.reserve(static_cast<std::size_t>(bars));
    for (int i = 0; i < bars; ++i)
        tape.push_back({100.0, 101.0, 99.0, 100.0, 1.0, epoch + i * INT64_C(60000)});
    host.run(tape.data(), bars, "1", "1", false);
    ROW_CHECK(row, host.last_error().empty());
    ROW_CHECK(row, std::all_of(host.result.reached.begin(), host.result.reached.end(),
                              [](bool reached) { return reached; }));
    ROW_CHECK(row, host.live_position_size() == 0.0 && host.closed_trade_count() == 1);
    if (host.closed_trade_count() == 1) {
        const auto trade = host.closed_trade(0);
        // A unit bought and sold at 100 has zero P&L with zero charges.
        ROW_CHECK(row, trade.entry_id == "held" && trade.qty == 1.0
                  && trade.entry_price == 100.0 && trade.exit_price == 100.0
                  && trade.pnl == 0.0 && trade.commission == 0.0);
    }
    return host.result;
}
} // namespace

int main() {
    for (const Row row : {Row::Roots, Row::Askable, Row::Groups}) {
        for (const int pairs : {12, 24}) {
            try {
                const auto short_run = run(row, 96, pairs);
                const auto long_run = run(row, 4 * 96, pairs);
                phase = "N-versus-4N";
                // Same script, same pending population, four times as many
                // bars: both retained copies have identical counts and
                // capacities at peak, after reduction, quiet and flat.
                if (row == Row::Roots) {
                    ROW_CHECK(Row::Roots, short_run.samples == long_run.samples);
                } else if (row == Row::Askable) {
                    ROW_CHECK(Row::Askable, short_run.samples == long_run.samples);
                } else {
                    ROW_CHECK(Row::Groups, short_run.samples == long_run.samples);
                }
            } catch (const std::exception& error) {
                check(row, false, error.what(), __LINE__);
            } catch (...) {
                check(row, false, "unknown exception", __LINE__);
            }
        }
    }
    std::printf("memory proof retirement_inputs: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
