// Semantic positive controls: these must also pass on the previous closure.
// Resource RED evidence comes from the separate growth/preparation rows.
#include <pineforge/drawing.hpp>
#include <pineforge/series.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/source/pine_ta_length.hpp>
#include "change_limit_fixture.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}
bool same(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
std::uint64_t next(std::uint64_t& state) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return state;
}
double sample(std::uint64_t word) {
    if (word % 17 == 0) {
        word = UINT64_C(0x7ff8000000000000) | (word & UINT64_C(0xffff));
        double result;
        std::memcpy(&result, &word, sizeof result);
        return result;
    }
    if (word % 13 == 0) return -0.0;
    return static_cast<double>(word % 8192) * 0.25;
}

void raw_series() {
    DynamicRingBuffer<double> value(3);
    std::vector<double> reference;
    std::size_t capacity = 3;
    std::array<std::optional<DynamicRingBuffer<double>>, 4> saved;
    std::array<std::vector<double>, 4> refs;
    std::array<std::size_t, 4> caps{};
    std::uint64_t rng = 3571;
    const auto check = [&] {
        require(value.size() == reference.size() && value.capacity() == capacity,
                "raw Series dimensions");
        for (std::size_t i = 0; i < reference.size(); ++i)
            require(same(value[i], reference[i]), "raw Series bits");
        require(is_na(value[reference.size()]), "raw Series out-of-window na");
    };
    for (int step = 0; step < 2400; ++step) {
        const auto word = next(rng);
        const auto slot = static_cast<std::size_t>((word >> 12) % saved.size());
        switch (word % 9) {
        case 0:
            capacity = (word >> 18) % 4097;
            value.resize(capacity);
            if (reference.size() > capacity) reference.resize(capacity);
            break;
        case 1:
            saved[slot] = value;
            refs[slot] = reference;
            caps[slot] = capacity;
            break;
        case 2:
            if (saved[slot]) {
                value = *saved[slot];
                reference = refs[slot];
                capacity = caps[slot];
            }
            break;
        case 3:
            value.clear(); reference.clear();
            break;
        case 4: {
            const auto source = sample(word);
            value.update_front(source);
            if (!reference.empty()) reference[0] = source;
            else if (capacity != 0) reference.push_back(source);
            break;
        }
        default: {
            const auto source = sample(word);
            value.push_front(source);
            if (capacity != 0) {
                reference.insert(reference.begin(), source);
                if (reference.size() > capacity) reference.pop_back();
            }
        }
        }
        check();
    }
    Series<double> numeric(3);
    Series<bool> flags(3);
    Series<std::string> text(3);
    Series<Series<double>> nested(3);
    numeric.push(-0.0); flags.push(true); text.push(std::string("a\0b", 3));
    nested.push(numeric);
    require(same(nested[0][0], -0.0) && flags[0] && text[0] == std::string("a\0b", 3),
            "non-numeric/nested Series value semantics");
    require(is_na(numeric[-1]) && is_na(numeric[3]), "Series negative/out-of-range");
}

void held_history() {
    source::HeldHistory held;
    std::vector<double> values;
    long long first = 0;
    std::uint64_t rng = 7703;
    const auto write = [&](long long bar, double value, std::size_t keep) {
        held.write(bar, value, keep);
        if (values.empty()) {
            first = bar;
            values.push_back(value);
        } else {
            const auto last = first + static_cast<long long>(values.size()) - 1;
            if (bar == last) values.back() = value;
            else if (bar > last) {
                const double hold = values.back();
                auto gap = bar - last - 1;
                if (gap >= static_cast<long long>(keep)) {
                    values.clear();
                    first = bar - static_cast<long long>(keep);
                    gap = static_cast<long long>(keep);
                }
                values.insert(values.end(), static_cast<std::size_t>(gap), hold);
                values.push_back(value);
                // Independent full-copy model of the original chunk boundary.
                while (values.size() > 1024 && values.size() - 1024 >= keep) {
                    values.erase(values.begin(), values.begin() + 1024);
                    first += 1024;
                }
            }
        }
        require(held.first_bar() == first &&
                held.last_bar() == first + static_cast<long long>(values.size()) - 1,
                "held history retention boundary");
        for (const auto offset : {std::size_t{0}, values.size() / 2, values.size() - 1})
            require(same(held.at(first + static_cast<long long>(offset)), values[offset]),
                    "held history held/rewrite bits");
    };
    long long bar = 11;
    for (int i = 0; i < 1400; ++i) {
        const auto word = next(rng);
        const auto keep = static_cast<std::size_t>(1 + word % 15001);
        bar += (i % 71 == 0) ? 17000 : static_cast<long long>(word % 19);
        write(bar, sample(word), keep);
        if (i % 23 == 0) {
            const auto checkpoint = held;
            const auto saved_values = values;
            const auto saved_first = first;
            write(bar + 3, sample(next(rng)), keep);
            held = checkpoint; values = saved_values; first = saved_first;
            write(bar, sample(next(rng)), keep);
        }
        if (i % 29 == 0) write(bar - 1, sample(next(rng)), keep);
    }
    for (std::size_t i = 0; i < values.size(); ++i)
        require(same(held.at(first + static_cast<long long>(i)), values[i]), "held late read");
}

void dynamic_change() {
    ta::Change change(3);
    std::deque<double> reference;
    std::uint64_t rng = 9173;
    for (int bar = 0; bar < 2200; ++bar) {
        const auto word = next(rng);
        const int length = static_cast<int>(word % 1801);
        const double src = sample(word);
        reference.push_back(src);
        while (reference.size() > static_cast<std::size_t>(std::max(3, length)) + 1)
            reference.pop_front();
        const auto expected = [&](double current) {
            if (reference.size() <= static_cast<std::size_t>(length)) return na<double>();
            const double prior = reference[reference.size() - length - 1];
            return is_na(current) || is_na(prior) ? na<double>() : current - prior;
        };
        require(same(change.compute(src, length), expected(src)), "Change dynamic keep/read");
        const double again = sample(next(rng));
        reference.back() = again;
        require(same(change.recompute(again, length), expected(again)), "Change recompute");
    }
}

void first_call_validation() {
    source::FirstCallBound<ta::EMA> first;
    int attempts = 0;
    const auto invalid = [&] {
        ++attempts;
        return ta::EMA(source::simple_ta_length(0, "ta.ema", "length"));
    };
    require(!first.bound() && attempts == 0, "unexecuted first call was validated");
    for (int retry = 1; retry <= 2; ++retry) {
        bool threw = false;
        try { (void)first.compute(invalid, 100.0); }
        catch (const std::exception&) { threw = true; }
        require(threw && !first.bound() && attempts == retry,
                "first-execution validation/retry changed");
    }
}

class FillHost final : public source::PineStrategyHost {
    struct State {
        Series<double> history{2053};
        DrawingArena<LineRec> lines{7};
        DrawingArena<BoxRec> boxes{7};
        DrawingArena<LabelRec> labels{7};
        DrawingArena<LinefillRec> fills{7};
        int count = 0;
    } state_;
    std::optional<State> saved_;
    std::deque<double> reference_, saved_reference_;
    void check() {
        require(state_.history.size() == static_cast<int>(reference_.size()), "fill history size");
        for (std::size_t i = 0; i < reference_.size(); ++i)
            require(same(state_.history[static_cast<int>(i)], reference_[i]), "fill history bits");
        require(state_.lines.size() == state_.count && state_.boxes.size() == state_.count &&
                state_.labels.size() == state_.count && state_.fills.size() == state_.count,
                "fill drawing ids");
        if (state_.count == 0) return;
        require(pf_line_get_x2(state_.lines, Line{0}) == state_.count &&
                pf_box_get_right(state_.boxes, Box{0}) == state_.count &&
                pf_label_get_text(state_.labels, Label{0}) == std::to_string(state_.count) &&
                pf_linefill_get_line1(state_.fills, Linefill{0}).id == state_.count - 1,
                "fill dead drawing rollback");
    }
public:
    int executions = 0;
    int restores = 0;
    FillHost() {
        source::PineStrategyConfig cfg{};
        cfg.calc_on_order_fills = true;
        configure_pine_strategy(cfg);
    }
    void snapshot_script_state() override {
        check();
        saved_.emplace(state_);
        saved_reference_ = reference_;
    }
    void restore_script_state() override {
        if (!saved_) return;
        state_ = *saved_;
        reference_ = saved_reference_;
        ++restores;
        check();
    }
    void commit_script_state() override { snapshot_script_state(); }
    void on_source_bar(const Bar& bar) override {
        ++executions;
        ++state_.count;
        const double value = state_.count * 2.0 + bar.close;
        state_.history.push(value);
        reference_.push_front(value);
        if (reference_.size() > 2053) reference_.pop_back();
        const auto line = pf_line_new(state_.lines, state_.count, value,
                                      state_.count + 1, value + 0.5);
        pf_box_new(state_.boxes, state_.count, value + 1, state_.count + 1, value - 1);
        pf_label_new(state_.labels, state_.count, value, std::to_string(state_.count));
        pf_linefill_new(state_.fills, line, Line{line.id - 1});
        pf_line_set_x2(state_.lines, Line{0}, state_.count);
        pf_box_set_right(state_.boxes, Box{0}, state_.count);
        pf_label_set_text(state_.labels, Label{0}, std::to_string(state_.count));
        state_.fills.at(0).line1 = state_.count - 1;
        check();
        if (pine_bar_index() % 10 == 0 && signed_position_size() == 0.0)
            strategy_entry("L", true);
        if (pine_bar_index() % 10 == 5) strategy_close("L");
    }
};

void calc_on_fill() {
    constexpr int count = 140;
    std::vector<Bar> bars(count);
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + (i * 7 % 101) * 0.25;
        bars[static_cast<std::size_t>(i)] = {price, price + 1, price - 1, price, 1000,
                                            INT64_C(1736121600000) + i * INT64_C(60000)};
    }
    FillHost host;
    host.run(bars.data(), count, "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.trade_count() > 0 && host.executions > count && host.restores > count,
            "calc-on-fill path was not exercised");
}
}

int main() {
    try {
        raw_series();
        held_history();
        dynamic_change();
        change_limit_test::boundary();
        first_call_validation();
        calc_on_fill();
        std::printf("history_drawing_exactness: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "history_drawing_exactness: %s\n", error.what());
        return 1;
    }
}
