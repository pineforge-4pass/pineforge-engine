// A-4b contract pin: source bracket cancellation normalizes every non-working
// member to the same-run UINT64_MAX target, including before roster settlement.
// This is intentionally already implemented on BASE, so this pin cannot serve
// as a BASE-red test. REPORT-fix-a1.md records that unresolved brief constraint.
#include <pineforge/source/pine_strategy_host.hpp>

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

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

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

enum class Storage { NonWorkingTail, SettledArchive };

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

    bool checked = false;
    void on_source_bar(const Bar&) override {
        const int index = pine_bar_index();
        if (index == 0) strategy_entry("E", true, missing, missing, 8.0);
        if (index == 1) {
            if (storage_ == Storage::NonWorkingTail) {
                // One filled limit and its OCA-cancelled stop remain in the
                // unsealed family until the following bar's retirement pass.
                strategy_exit("X", "E", 100.5, 50.0);
            } else {
                // A named limit-only partial exit has no revival protection.
                // O1W releases its terminal row and archives the family while
                // the four remaining entry units are still open.
                strategy_exit("X", "E", 100.5, missing,
                              missing, missing, missing, 50.0);
            }
        }
        const int cancel_bar = storage_ == Storage::NonWorkingTail ? 2 : 6;
        if (index != cancel_bar) return;

        const auto key = (adapter_.*member(Key{}))("X", "E");
        const auto& active = adapter_.*member(Families{});
        const auto& archived = adapter_.*member(Archive{});
        const std::size_t expected_count = storage_ == Storage::NonWorkingTail ? 2 : 1;
        require(trade_count() == 1, "fixture did not fill its bracket exit");
        if (storage_ == Storage::NonWorkingTail) {
            const auto found = active.find(key);
            require(found != active.end() && found->second.settled_count() == 0
                    && found->second.members().size() == 2,
                    "fixture did not retain both non-working tail members");
        } else {
            const auto found = archived.find(key);
            require(found != archived.end() && found->second.count == 1,
                    "fixture did not reach a genuinely settled archived member");
        }

        std::vector<no::RequestHandle> accepted;
        std::uint64_t cursor = 0;
        for (const auto& row : native_events(0)) {
            if (row.ordinal > cursor) cursor = row.ordinal;
            if (!row.command) continue;
            if (const auto* event = std::get_if<no::AcceptedEvent>(&*row.command);
                event && event->request().label == "X") {
                accepted.push_back(event->handle());
            }
        }
        require(accepted.size() == expected_count,
                "fixture did not issue the designed bracket member count");
        for (const auto& handle : accepted)
            require(handle.incarnation != 0
                    && handle.incarnation != std::numeric_limits<std::uint64_t>::max(),
                    "accepted member already has the cancellation sentinel");

        // This is the source command facade that reaches exit_cancel_bracket.
        // strategy_cancel(id) has a separate live-handle-only implementation.
        strategy_exit_cancel_bracket("X", "E");
        std::size_t count = 0;
        for (const auto& row : native_events(cursor)) {
            if (!row.command) continue;
            const auto* event = std::get_if<no::NotWorkingEvent>(&*row.command);
            require(event != nullptr, "bracket cancel produced a non-NotWorking command event");
            require(count < accepted.size(), "bracket cancel emitted an extra member outcome");
            require(event->target.run == accepted[count].run,
                    "normalized target lost its member's run identity");
            require(event->target.incarnation == std::numeric_limits<std::uint64_t>::max(),
                    "non-working target is not the contracted UINT64_MAX sentinel");
            require(!event->attempted.has_value() && event->surface == no::CommandSurface::General,
                    "bracket cancel was recorded as a replacement or another command surface");
            ++count;
        }
        require(count == expected_count, "bracket cancel lost a member's NotWorking outcome");
        require(active.find(key) == active.end() && archived.find(key) == archived.end(),
                "cancelled bracket family remains active or archived");
        checked = true;
    }

private:
    Storage storage_;
};

void exercise(Storage storage) {
    Host host(storage);
    std::vector<Bar> bars;
    for (int i = 0; i < 9; ++i)
        bars.push_back({100.0, i == 2 ? 101.0 : 100.25, 99.75, 100.0,
                        1.0, start_ms + i * INT64_C(60000)});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.checked, "target-contract assertion was never reached");
}
} // namespace

int main() {
    try {
        exercise(Storage::NonWorkingTail);
        exercise(Storage::SettledArchive);
        std::puts("PASS test_bracket_cancel_not_working_target");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_bracket_cancel_not_working_target: %s\n", error.what());
        return 1;
    }
}
