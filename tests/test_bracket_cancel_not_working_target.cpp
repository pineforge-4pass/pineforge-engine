// A-4b contract pin: source bracket cancellation normalizes every non-working
// member to the same-run UINT64_MAX target, including before roster settlement.
// A-4b changes only a production contract comment, so this pin cannot be red
// on BASE; its sentinel mutation changes the never-issued-target constant.
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace {
using namespace pineforge;
namespace ps = source;
namespace no = native_order;

constexpr double missing = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t start_ms = INT64_C(1736121600000);

template<class Tag, typename Tag::type Member>
struct Access {
    friend typename Tag::type member(Tag) { return Member; }
};
struct Families {
    using type = std::unordered_map<std::uint64_t, ps::BracketRoster>
        ps::PineExecutionAdapter::*;
    friend type member(Families);
};
template struct Access<Families, &ps::PineExecutionAdapter::bracket_families_>;
struct Archive {
    using type = std::unordered_map<std::uint64_t, ps::BracketRoster::Settled>
        ps::PineExecutionAdapter::*;
    friend type member(Archive);
};
template struct Access<Archive, &ps::PineExecutionAdapter::settled_bracket_families_>;
struct Key {
    using type = std::uint64_t (ps::PineExecutionAdapter::*)(const ps::SourceId&,
                                                          const ps::SourceId&) const noexcept;
    friend type member(Key);
};
template struct Access<Key, &ps::PineExecutionAdapter::key_for>;
struct LiveHandles {
    using type = ps::detail::LiveHandleSequence ps::PineExecutionAdapter::*;
    friend type member(LiveHandles);
};
template struct Access<LiveHandles, &ps::PineExecutionAdapter::live_handles_>;

enum class Storage { NonWorkingTail, SettledArchive, MixedTail };

class Host final : public ps::PineStrategyHost {
public:
    explicit Host(Storage storage) : storage_(storage) {
        ps::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 8.0;
        config.pyramiding = 2;
        config.commission_value = 0.0;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_session("24x7");
        set_syminfo_timezone("UTC");
        set_syminfo_mintick(0.25);
        fixture_retain_all_events();
    }

    void require(bool condition, const std::string& message) const {
        if (!condition)
            throw std::runtime_error(message + fixture_state()
                                     + (cancel_fixture_.empty() ? std::string{}
                                         : " before_cancel=" + cancel_fixture_));
    }

    bool checked = false;
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_entry("E", true, missing, missing, 8.0);
        if (index == 1) {
            if (storage_ == Storage::NonWorkingTail) {
                // Close four of eight units: a full close purges the family.
                // The bar-2 limit fill and OCA cancellation leave both members
                // in the unsealed roster at this bar's source callback.
                strategy_exit("X", "E", 100.5, 50.0,
                              missing, missing, missing, 50.0);
            } else {
                // A named limit-only partial exit has no revival protection.
                // O1W later archives it; MixedTail cancels on the fill bar,
                // before that retirement pass, after adding a live stop.
                strategy_exit("X", "E", 100.5, missing,
                              missing, missing, missing, 50.0);
            }
        }
        const int cancel_bar = storage_ == Storage::SettledArchive ? 6 : 2;
        if (index != cancel_bar) return;
        if (storage_ == Storage::MixedTail) {
            // The four-unit limit filled earlier on this bar. A new stop
            // appends to the same (X, E) roster while four entry units remain;
            // it was not present for the fill's OCA/sibling cancellation.
            strategy_exit("X", "E", missing, 50.0);
        }

        const auto key = (adapter_.*member(Key{}))("X", "E");
        const auto& active = adapter_.*member(Families{});
        const auto& archived = adapter_.*member(Archive{});
        const std::size_t expected_count = storage_ == Storage::SettledArchive ? 1 : 2;
        require(trade_count() == 1, "fixture did not fill its bracket exit");
        const auto trade = get_trade(0);
        require(trade.entry_id == "E" && trade.exit_id == "X"
                && trade.entry_bar_index == 1 && trade.exit_bar_index == 2
                && trade.qty == 4.0 && live_position_size() == 4.0,
                "fixture did not retain half its entry after the bar-2 exit: entry_bar="
                    + std::to_string(trade.entry_bar_index)
                    + " exit_bar=" + std::to_string(trade.exit_bar_index)
                    + " qty=" + std::to_string(trade.qty)
                    + " entry_id=" + trade.entry_id + " exit_id=" + trade.exit_id);
        if (storage_ != Storage::SettledArchive) {
            const auto found = active.find(key);
            require(found != active.end() && found->second.settled_count() == 0
                    && found->second.members().size() == 2,
                    "fixture did not retain both unsealed tail members");
            require(archived.find(key) == archived.end(),
                    "unsealed family also appeared in the settled archive");
        } else {
            const auto found = archived.find(key);
            require(found != archived.end() && found->second.count == 1,
                    "fixture did not reach a genuinely settled archived member");
            require(active.find(key) == active.end(),
                    "archived family also remained active");
        }

        const auto events = native_events(0);
        std::vector<no::AcceptedEvent> accepted;
        std::uint64_t cursor = 0;
        for (const auto& row : events) {
            if (row.ordinal > cursor) cursor = row.ordinal;
            if (!row.command) continue;
            if (const auto* event = std::get_if<no::AcceptedEvent>(&*row.command);
                event && event->request().label == "X") {
                accepted.push_back(*event);
            }
        }
        require(accepted.size() == expected_count,
                "fixture did not issue the designed bracket member count: accepted="
                    + std::to_string(accepted.size())
                    + " expected=" + std::to_string(expected_count));
        require(std::holds_alternative<no::Limit>(accepted[0].request().trigger),
                "first accepted bracket member is not a limit");
        if (storage_ != Storage::SettledArchive) {
            require(std::holds_alternative<no::Stop>(accepted[1].request().trigger),
                    "second accepted bracket member is not a stop");
            const auto members = active.at(key).members();
            require(members[0] == accepted[0].handle() && members[1] == accepted[1].handle(),
                    "unsealed roster does not hold the accepted limit and stop in order");
        }

        std::size_t limit_fills = 0;
        std::size_t stop_cancels = 0;
        for (const auto& row : events) {
            if (!row.command) continue;
            if (const auto* event = std::get_if<no::ExecutionAppliedEvent>(&*row.command);
                event && event->request().label == "X") {
                require(event->handle() == accepted[0].handle() && event->terminal
                        && event->closed_units == 4.0 && event->closed_trade_count == 1,
                        "fixture bracket did not terminally fill its four-unit limit: incarnation="
                            + std::to_string(event->handle().incarnation)
                            + " terminal=" + std::to_string(event->terminal)
                            + " closed_units=" + std::to_string(event->closed_units)
                            + " closed_trade_count=" + std::to_string(event->closed_trade_count));
                ++limit_fills;
            }
            if (const auto* event = std::get_if<no::CancelledEvent>(&*row.command);
                event && event->request().label == "X") {
                require(storage_ == Storage::NonWorkingTail
                        && event->handle() == accepted[1].handle()
                        && event->reason == no::CancelReason::Group,
                        "fixture bracket cancellation was not its OCA stop: incarnation="
                            + std::to_string(event->handle().incarnation)
                            + " reason=" + std::to_string(static_cast<int>(event->reason)));
                ++stop_cancels;
            }
        }
        require(limit_fills == 1
                && stop_cancels == (storage_ == Storage::NonWorkingTail ? 1U : 0U),
                "fixture did not observe its limit fill and designed OCA cancellation: limit_fills="
                    + std::to_string(limit_fills) + " stop_cancels=" + std::to_string(stop_cancels));
        const auto working = native_working_requests();
        for (std::size_t member_index = 0; member_index < accepted.size(); ++member_index) {
            const auto& handle = accepted[member_index].handle();
            require(handle.incarnation != 0
                    && handle.incarnation != std::numeric_limits<std::uint64_t>::max(),
                    "accepted member has an invalid incarnation: "
                        + std::to_string(handle.incarnation));
            if (storage_ == Storage::MixedTail && member_index == 1) {
                require(working.size() == 1 && working[0].definition->handle == handle,
                        "mixed roster does not have exactly its live stop working");
                const auto& live = adapter_.*member(LiveHandles{});
                require(std::find(live.begin(), live.end(), handle) != live.end(),
                        "mixed roster's working stop is absent from source live handles");
            } else {
                for (const auto& request : working)
                    require(request.definition->handle != handle,
                            "fixture bracket member is still working: incarnation="
                                + std::to_string(handle.incarnation));
            }
        }

        // This is the source command facade that reaches exit_cancel_bracket.
        // strategy_cancel(id) has a separate live-handle-only implementation.
        cancel_fixture_ = fixture_state();
        strategy_exit_cancel_bracket("X", "E");
        std::size_t count = 0;
        for (const auto& row : native_events(cursor)) {
            if (!row.command) continue;
            require(count < accepted.size(), "bracket cancel emitted an extra member outcome");
            if (storage_ == Storage::MixedTail && count == 1) {
                const auto* event = std::get_if<no::CancelledEvent>(&*row.command);
                require(event != nullptr, "live member did not produce its CancelledEvent");
                require(event->handle() == accepted[1].handle()
                        && event->request().label == "X"
                        && std::holds_alternative<no::Stop>(event->request().trigger)
                        && event->reason == no::CancelReason::User
                        && event->cause.has_value()
                        && event->cause->run == event->handle().run
                        && event->cause->ordinal == event->ordinal,
                        "live member cancellation lost its own handle or explicit-cancel reason");
                ++count;
                continue;
            }
            const auto* event = std::get_if<no::NotWorkingEvent>(&*row.command);
            require(event != nullptr, "bracket cancel produced a non-NotWorking command event");
            require(event->target.run == accepted[count].handle().run,
                    "normalized target lost its member's run identity");
            require(event->target.incarnation == std::numeric_limits<std::uint64_t>::max(),
                    "non-working target is not the contracted UINT64_MAX sentinel: observed="
                        + std::to_string(event->target.incarnation)
                        + " expected="
                        + std::to_string(std::numeric_limits<std::uint64_t>::max()));
            require(!event->attempted.has_value() && event->surface == no::CommandSurface::General,
                    "bracket cancel was recorded as a replacement or another command surface");
            ++count;
        }
        require(count == expected_count,
                "bracket cancel lost a member's cancellation outcome: observed="
                    + std::to_string(count) + " expected=" + std::to_string(expected_count));
        if (storage_ == Storage::MixedTail) {
            require(native_working_requests().empty(),
                    "cancelled live stop remains working in the kernel");
            const auto& live = adapter_.*member(LiveHandles{});
            require(std::find(live.begin(), live.end(), accepted[1].handle()) == live.end(),
                    "cancelled live stop was not retired from source live handles");
            require(trade_count() == 1 && live_position_size() == 4.0,
                    "cancelling the mixed roster changed its trades or remaining position");
        }
        require(active.find(key) == active.end() && archived.find(key) == archived.end(),
                "cancelled bracket family remains active or archived");
        checked = true;
    }

private:
    Storage storage_;
    std::string cancel_fixture_;

    std::string fixture_state() const {
        const auto key = (adapter_.*member(Key{}))("X", "E");
        const auto& active = adapter_.*member(Families{});
        const auto& archived = adapter_.*member(Archive{});
        const auto found = active.find(key);
        const auto settled = archived.find(key);
        return " [storage=" + std::string(storage_ == Storage::NonWorkingTail
                                             ? "NonWorkingTail"
                                             : storage_ == Storage::SettledArchive
                                                 ? "SettledArchive" : "MixedTail")
            + " bar=" + std::to_string(pine_bar_index())
            + " settled_count="
            + (found == active.end() ? std::string{"absent"}
                                    : std::to_string(found->second.settled_count()))
            + " members.size="
            + (found == active.end() ? std::string{"absent"}
                                    : std::to_string(found->second.members().size()))
            + " archived_count="
            + (settled == archived.end() ? std::string{"absent"}
                                        : std::to_string(settled->second.count))
            + " trade_count=" + std::to_string(trade_count())
            + " position_size=" + std::to_string(live_position_size()) + "]";
    }
};

void exercise(Storage storage) {
    Host host(storage);
    std::vector<Bar> bars;
    for (int i = 0; i < 9; ++i)
        bars.push_back({100.0, i == 2 ? 101.0 : 100.25, 99.75, 100.0,
                        1.0, start_ms + i * INT64_C(60000)});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    host.require(host.last_error().empty(), "run failed: " + host.last_error());
    host.require(host.checked, "target-contract assertion was never reached");
}
} // namespace

int main() {
    try {
        exercise(Storage::NonWorkingTail);
        exercise(Storage::SettledArchive);
        exercise(Storage::MixedTail);
        std::puts("PASS test_bracket_cancel_not_working_target");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_bracket_cancel_not_working_target: %s\n", error.what());
        return 1;
    }
}
