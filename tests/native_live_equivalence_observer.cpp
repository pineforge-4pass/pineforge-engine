#include <pineforge/source/pine_strategy_host.hpp>

#include "json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace {

using pineforge::live::Json;
using Host = pineforge::source::PineStrategyHost;
namespace orders = pineforge::native_order;

class SourceReadback : public Host {
public:
    static pineforge::Bar read(const Host& host) {
        const auto bar = &SourceReadback::current_bar_;
        return host.*bar;
    }
};

Host* host_of(void* state) {
    return dynamic_cast<Host*>(static_cast<pineforge::BacktestEngine*>(state));
}

Json number(double value) {
    std::ostringstream text;
    text << std::setprecision(17) << value;
    return Json::number(text.str());
}

Json integer(std::int64_t value) {
    return Json::number(std::to_string(value));
}

const char* trigger_name(const orders::Trigger& trigger) {
    if (std::holds_alternative<orders::Limit>(trigger)) return "limit";
    if (std::holds_alternative<orders::Stop>(trigger)) return "stop";
    if (std::holds_alternative<orders::StopLimit>(trigger)) return "stop_limit";
    if (std::holds_alternative<orders::Trail>(trigger)) return "trail";
    return "market";
}

Json trigger_levels(const orders::Trigger& trigger) {
    if (const auto* limit = std::get_if<orders::Limit>(&trigger))
        return Json::object({{"limit", number(limit->price)}});
    if (const auto* stop = std::get_if<orders::Stop>(&trigger))
        return Json::object({{"stop", number(stop->price)}});
    if (const auto* stop_limit = std::get_if<orders::StopLimit>(&trigger))
        return Json::object({{"stop", number(stop_limit->stop)}, {"limit", number(stop_limit->limit)}});
    if (const auto* trail = std::get_if<orders::Trail>(&trigger))
        return Json::object({{"offset", number(trail->offset)},
            {"arm", trail->arm_price ? number(*trail->arm_price) : Json{}},
            {"best_seed", trail->best_seed ? number(*trail->best_seed) : Json{}}});
    return Json::object({});
}

Json action(std::int64_t timestamp, int bar_index, bool is_entry,
            bool is_long, double units, double price,
            const std::string& label, std::uint64_t incarnation, int origin) {
    return Json::object({
        {"timestamp", integer(timestamp)},
        {"bar_index", integer(bar_index)},
        {"origin_input_index", integer(origin)},
        {"order", Json::object({
            {"id", Json::string(label)},
            {"action", Json::string(is_entry == is_long ? "buy" : "sell")},
            {"leg", Json::string(is_entry ? "entry" : "exit")},
            {"contracts", number(units)},
            {"price", number(price)},
            {"reduce_only", Json::boolean(!is_entry)},
            {"entry_incarnation", integer(static_cast<std::int64_t>(incarnation))}
        })}
    });
}

}

extern "C" int equivalence_retain_events(void* state) {
    try {
        auto* host = host_of(state);
        if (!host) return -1;
        host->fixture_retain_all_events();
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int equivalence_pending_priced(void* state) {
    try {
        auto* host = host_of(state);
        if (!host) return -1;
        int count = 0;
        for (const auto& row : host->native_working_requests()) {
            if (!std::holds_alternative<orders::Market>(row.definition->request.trigger)) ++count;
        }
        return count;
    } catch (...) {
        return -1;
    }
}

extern "C" int equivalence_open_lots(void* state) {
    try {
        auto* host = host_of(state);
        if (!host) return -1;
        return static_cast<int>(host->native_open_lots(
            std::numeric_limits<double>::quiet_NaN()).size());
    } catch (...) {
        return -1;
    }
}

extern "C" int equivalence_source_bar(void* state, std::int64_t timeframe_ms,
                                      pineforge::Bar* out) {
    try {
        const auto* host = host_of(state);
        if (!host || !out || timeframe_ms <= 0) return -1;
        *out = SourceReadback::read(*host);
        return 0;
    } catch (...) {
        return -1;
    }
}

extern "C" int equivalence_export_actions(void* state, const char* path) {
    try {
        auto* host = host_of(state);
        if (!host || !path) return -1;
        std::ofstream output(path);
        if (!output) return -1;
        const auto lots = host->native_open_lots(std::numeric_limits<double>::quiet_NaN());
        std::vector<bool> exported(host->closed_trade_count(), false);
        for (const auto& event : host->native_events(0)) {
            if (!event.command) continue;
            const auto* applied = std::get_if<orders::ExecutionAppliedEvent>(&*event.command);
            if (!applied) continue;
            const int origin = applied->cursor.point.input_interval_index;
            std::size_t closed = 0;
            for (std::size_t index = 0; index < host->closed_trade_count() && closed < applied->closed_trade_count; ++index) {
                const auto& trade = host->closed_trade(index);
                if (exported[index] || trade.open_at_end || trade.exit_id != applied->request().label ||
                    trade.exit_price != applied->resolved_price || trade.exit_bar_index != applied->interval_index()) continue;
                if (const auto* opening = std::get_if<pineforge::execution::OpeningExposure>(&applied->scope)) {
                    if (trade.entry_incarnation != opening->incarnation) continue;
                }
                if (const auto* selected = std::get_if<orders::SelectedExposure>(&applied->scope)) {
                    if (std::find(selected->incarnations.begin(), selected->incarnations.end(), trade.entry_incarnation)
                        == selected->incarnations.end()) continue;
                }
                output << action(applied->effective_time_ms(), applied->cursor.point.interval_index, false,
                    trade.is_long, trade.qty, applied->resolved_price, applied->request().label,
                    trade.entry_incarnation, origin).dump() << '\n';
                exported[index] = true;
                ++closed;
            }
            if (closed != applied->closed_trade_count) throw std::runtime_error("closing metadata unavailable");
            if (!applied->opened_lot_incarnation) continue;
            bool found = false;
            for (const auto& lot : lots) {
                if (lot.entry_incarnation != applied->opened_lot_incarnation) continue;
                output << action(applied->effective_time_ms(), applied->cursor.point.interval_index, true,
                    lot.signed_units > 0, std::abs(applied->opened_units), lot.entry_price,
                    lot.entry_label, lot.entry_incarnation, origin).dump() << '\n';
                found = true;
                break;
            }
            if (found) continue;
            for (std::size_t index = 0; index < host->closed_trade_count(); ++index) {
                const auto& trade = host->closed_trade(index);
                if (trade.entry_incarnation != applied->opened_lot_incarnation) continue;
                output << action(applied->effective_time_ms(), applied->cursor.point.interval_index, true,
                    trade.is_long, std::abs(applied->opened_units), trade.entry_price,
                    trade.entry_id, trade.entry_incarnation, origin).dump() << '\n';
                found = true;
                break;
            }
            if (!found) throw std::runtime_error("opening metadata unavailable");
        }
        return output ? 0 : -1;
    } catch (...) {
        return -1;
    }
}

extern "C" int equivalence_export_receipts(void* state, const char* path) {
    try {
        auto* host = host_of(state);
        if (!host || !path) return -1;
        std::ofstream output(path);
        if (!output) return -1;
        int origin = -1;
        std::int64_t timestamp = -1;
        std::uint64_t sequence = 0;
        double raw_price = 0.0;
        double resolved_price = 0.0;
        int provenance = -1;
        int path_phase = -1;
        for (const auto& event : host->native_events(0)) {
            if (event.driver) {
                origin = event.driver->coordinate.input_interval_index;
                timestamp = event.driver->coordinate.effective_time_ms;
                sequence = event.driver->sequence.value_or(0);
                raw_price = event.driver->raw_price;
                provenance = static_cast<int>(event.driver->coordinate.provenance);
                path_phase = static_cast<int>(event.driver->coordinate.path_phase);
            }
            if (!event.command) continue;
            orders::DefinitionRef definition;
            const char* kind = nullptr;
            int match_reject_reason = -1;
            if (const auto* accepted = std::get_if<orders::AcceptedEvent>(&*event.command)) {
                definition = accepted->definition;
                kind = "accepted";
            } else if (const auto* replaced = std::get_if<orders::ReplacedEvent>(&*event.command)) {
                definition = replaced->successor_definition;
                kind = "replaced";
            } else if (const auto* cancelled = std::get_if<orders::CancelledEvent>(&*event.command)) {
                definition = cancelled->definition;
                kind = "cancelled";
            } else if (const auto* activated = std::get_if<orders::ActivatedEvent>(&*event.command)) {
                definition = activated->definition;
                origin = activated->cursor.point.input_interval_index;
                kind = "activated";
            } else if (const auto* terms = std::get_if<orders::TermsResolvedEvent>(&*event.command)) {
                definition = terms->definition;
                origin = terms->cursor.point.input_interval_index;
                timestamp = terms->cursor.point.effective_time_ms;
                raw_price = terms->input.raw_price;
                resolved_price = terms->input.terms.resolved_price;
                provenance = static_cast<int>(terms->cursor.point.provenance);
                path_phase = static_cast<int>(terms->cursor.point.path_phase);
                kind = "terms_resolved";
            } else if (const auto* rejected = std::get_if<orders::MatchRejectedEvent>(&*event.command)) {
                definition = rejected->definition;
                origin = rejected->cursor.point.input_interval_index;
                timestamp = rejected->cursor.point.effective_time_ms;
                provenance = static_cast<int>(rejected->cursor.point.provenance);
                path_phase = static_cast<int>(rejected->cursor.point.path_phase);
                match_reject_reason = static_cast<int>(rejected->reason);
                kind = "match_rejected";
            } else if (const auto* no_effect = std::get_if<orders::NoEffectEvent>(&*event.command)) {
                definition = no_effect->definition;
                origin = no_effect->cursor.point.input_interval_index;
                timestamp = no_effect->cursor.point.effective_time_ms;
                provenance = static_cast<int>(no_effect->cursor.point.provenance);
                path_phase = static_cast<int>(no_effect->cursor.point.path_phase);
                kind = "no_effect";
            } else if (const auto* applied = std::get_if<orders::ExecutionAppliedEvent>(&*event.command)) {
                definition = applied->definition;
                origin = applied->cursor.point.input_interval_index;
                timestamp = applied->effective_time_ms();
                raw_price = applied->raw_price;
                resolved_price = applied->resolved_price;
                provenance = applied->provenance();
                path_phase = static_cast<int>(applied->cursor.point.path_phase);
                kind = "executed";
            }
            if (!kind || !definition) continue;
            output << Json::object({
                {"ordinal", integer(static_cast<std::int64_t>(event.ordinal))},
                {"kind", Json::string(kind)},
                {"match_reject_reason", integer(match_reject_reason)},
                {"type", Json::string(trigger_name(definition->request.trigger))},
                {"id", Json::string(definition->request.label)},
                {"origin_input_index", integer(origin)},
                {"timestamp", integer(timestamp)},
                {"sequence", integer(static_cast<std::int64_t>(sequence))},
                {"request_incarnation", integer(static_cast<std::int64_t>(definition->handle.incarnation))},
                {"not_before", integer(definition->birth.decision_time_lower_bound)},
                {"levels", trigger_levels(definition->request.trigger)},
                {"raw_price", number(raw_price)},
                {"resolved_price", number(resolved_price)},
                {"provenance", integer(provenance)},
                {"path_phase", integer(path_phase)}
            }).dump() << '\n';
        }
        return output ? 0 : -1;
    } catch (...) {
        return -1;
    }
}
