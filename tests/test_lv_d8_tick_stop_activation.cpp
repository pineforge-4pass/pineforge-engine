#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <variant>
#include <vector>

using namespace pineforge;
namespace orders = pineforge::native_order;

namespace {

constexpr std::int64_t start_ms = 1704067200000LL;
constexpr double absent = std::numeric_limits<double>::quiet_NaN();

enum class Crossing { Beyond, Touch, GapOpen, MinuteClose, ScriptClose };

class StopHost final : public source::PineStrategyHost {
public:
    StopHost(bool long_side, int pyramid, double percent, bool reissue = false)
        : long_side_(long_side), reissue_(reissue) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(percent == 0.0 ? QtyType::FIXED : QtyType::PERCENT_OF_EQUITY);
        config.default_qty_value = percent == 0.0 ? 1.0 : percent;
        config.pyramiding = pyramid;
        configure_pine_strategy(config);
        qty_step_ = 0.0001;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1.0;
        fixture_retain_all_events();
    }

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 1 || (reissue_ && pine_bar_index() == 2)) {
            strategy_entry("Entry", long_side_, absent, long_side_ ? 101.0 : 99.0);
        }
    }

private:
    bool long_side_;
    bool reissue_;
};

struct Tape {
    std::vector<Bar> bars;
    std::vector<TradeTick> ticks;
    std::int64_t activation_time = 0;
};

Tape tape(bool long_side, Crossing crossing) {
    Tape result;
    std::uint64_t sequence = 0;
    const double direction = long_side ? 1.0 : -1.0;
    const double level = 100.0 + direction;
    const double beyond = crossing == Crossing::Touch ? level : 100.0 + direction * 1.5;
    const int trigger_minute = crossing == Crossing::GapOpen ? 10
        : crossing == Crossing::ScriptClose ? 14 : 11;
    for (int minute = 0; minute < 20; ++minute) {
        const std::int64_t opening = start_ms + minute * 60000;
        std::array<double, 3> prices{100.0, 100.0, 100.0};
        if (minute > trigger_minute || (minute == trigger_minute && crossing == Crossing::GapOpen)) {
            prices.fill(beyond);
        } else if (minute == trigger_minute) {
            prices[2] = beyond;
            if (crossing != Crossing::MinuteClose && crossing != Crossing::ScriptClose) {
                prices[1] = beyond;
            }
        }
        result.bars.push_back({prices[0], *std::max_element(prices.begin(), prices.end()),
            *std::min_element(prices.begin(), prices.end()), prices[2], 3.0, opening});
        if (minute < 5) continue;
        const std::array<std::int64_t, 3> offsets{1, 15000, 59999};
        for (std::size_t index = 0; index < prices.size(); ++index) {
            result.ticks.push_back({opening + offsets[index], ++sequence, prices[index], 1.0});
            if (minute == trigger_minute && result.activation_time == 0
                && prices[index] == beyond) {
                result.activation_time = opening + offsets[index];
            }
        }
    }
    return result;
}

struct Lifecycle {
    std::uint64_t incarnation = 0;
    std::uint64_t activated = 0;
    std::uint64_t terms = 0;
    std::uint64_t executed = 0;
    std::uint64_t rejected = 0;
    std::int64_t timestamp = 0;
    double price = 0.0;
    orders::MatchRejectReason reason = orders::MatchRejectReason::OpeningDirection;
};

Lifecycle lifecycle(const StopHost& host) {
    Lifecycle result;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (const auto* accepted = std::get_if<orders::AcceptedEvent>(&*event.command)) {
            if (result.incarnation == 0 && accepted->request().label == "Entry") {
                result.incarnation = accepted->handle().incarnation;
            }
        } else if (const auto* activated = std::get_if<orders::ActivatedEvent>(&*event.command)) {
            if (activated->definition->handle.incarnation == result.incarnation) result.activated = event.ordinal;
        } else if (const auto* terms = std::get_if<orders::TermsResolvedEvent>(&*event.command)) {
            if (terms->handle().incarnation == result.incarnation) {
                assert(result.terms == 0);
                result.terms = event.ordinal;
                result.price = terms->input.terms.resolved_price;
            }
        } else if (const auto* applied = std::get_if<orders::ExecutionAppliedEvent>(&*event.command)) {
            if (applied->handle().incarnation == result.incarnation) {
                assert(result.executed == 0);
                result.executed = event.ordinal;
                result.timestamp = applied->effective_time_ms();
            }
        } else if (const auto* rejected = std::get_if<orders::MatchRejectedEvent>(&*event.command)) {
            if (rejected->handle().incarnation == result.incarnation) {
                assert(result.rejected == 0);
                result.rejected = event.ordinal;
                result.timestamp = rejected->cursor.point.effective_time_ms;
                result.reason = rejected->reason;
            }
        }
    }
    return result;
}

void drive(StopHost& host, const Tape& data, bool ticks) {
    if (!ticks) {
        host.run(data.bars.data(), static_cast<int>(data.bars.size()), "1", "5", false, 4,
                 MagnifierDistribution::ENDPOINTS);
    } else {
        assert(host.stream_begin(data.bars.data(), 5, "1", "5"));
        for (const auto& tick : data.ticks) assert(host.stream_push_tick(tick));
        assert(host.stream_advance_time(start_ms + 20 * 60000));
    }
    assert(host.last_error().empty());
}

void matrix() {
    int rows = 0;
    for (double percent : {0.0, 50.0, 100.0}) {
        for (int pyramid : {0, 1}) {
            for (bool long_side : {true, false}) {
                for (Crossing crossing : {Crossing::Beyond, Crossing::Touch, Crossing::GapOpen,
                                          Crossing::MinuteClose, Crossing::ScriptClose}) {
                    const auto data = tape(long_side, crossing);
                    StopHost batch(long_side, pyramid, percent);
                    StopHost stream(long_side, pyramid, percent);
                    drive(batch, data, false);
                    drive(stream, data, true);
                    const auto historical = lifecycle(batch);
                    const auto observed = lifecycle(stream);
                    if (percent == 100.0 && !long_side) {
                        assert(historical.incarnation == 0 && observed.incarnation == 0);
                        ++rows;
                        continue;
                    }
                    assert(observed.incarnation != 0);
                    assert(observed.activated != 0 && observed.terms > observed.activated);
                    const double level = long_side ? 101.0 : 99.0;
                    const double print = crossing == Crossing::Touch ? level
                        : (long_side ? 101.5 : 98.5);
                    assert(std::abs(observed.price - print) < 1e-9);
                    assert(std::abs(historical.price - (crossing == Crossing::GapOpen ? print : level)) < 1e-9);
                    const bool refusal = percent == 100.0 && crossing != Crossing::Touch;
                    if (refusal) {
                        assert(observed.executed == 0 && observed.rejected > observed.terms);
                        assert(observed.timestamp == data.activation_time);
                        assert(observed.reason == orders::MatchRejectReason::HostPrecommit);
                        assert(stream.physical_position().signed_units == 0.0);
                        assert(stream.native_working_requests().empty());
                    } else {
                        assert(observed.rejected == 0 && observed.executed > observed.terms);
                        assert(observed.timestamp == data.activation_time);
                        assert(stream.physical_position().signed_units != 0.0);
                    }
                    assert((historical.executed != 0) == !(refusal && crossing == Crossing::GapOpen));
                    ++rows;
                }
            }
        }
    }
    std::printf("LV-D8 stop lifecycle: %d ENDPOINTS/tick rows passed\n", rows);
}

void reissue_after_terminal_refusal() {
    StopHost host(true, 0, 100.0, true);
    drive(host, tape(true, Crossing::Beyond), true);
    int accepted = 0;
    int replaced = 0;
    for (const auto& event : host.native_events(0)) {
        if (!event.command) continue;
        if (std::holds_alternative<orders::AcceptedEvent>(*event.command)) ++accepted;
        if (std::holds_alternative<orders::ReplacedEvent>(*event.command)) ++replaced;
    }
    assert(accepted == 2);
    assert(replaced == 0);
    assert(lifecycle(host).rejected != 0);
    std::puts("LV-D8 reissue after HostPrecommit: new incarnation, not replacement");
}

}

int main() {
    matrix();
    reissue_after_terminal_refusal();
}
