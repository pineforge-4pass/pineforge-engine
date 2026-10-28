#pragma once

#include "json.hpp"

namespace pineforge::live {

inline void merge_report_fields(Json& destination, const Json& changes) {
    if (changes.kind != Json::Kind::Object) { destination = changes; return; }
    destination.kind = Json::Kind::Object;
    for (const auto& [name, value] : changes.members)
        merge_report_fields(destination.members[name], value);
}

inline Json changed_report_fields(const Json& previous, const Json& next) {
    Json changes = Json::object({});
    for (const auto& [name, value] : next.members) {
        const auto found = previous.members.find(name);
        if (found == previous.members.end()) changes.members[name] = value;
        else if (value.kind == Json::Kind::Object && found->second.kind == Json::Kind::Object) {
            auto nested = changed_report_fields(found->second, value);
            if (!nested.members.empty()) changes.members[name] = std::move(nested);
        } else if (found->second.dump() != value.dump()) changes.members[name] = value;
    }
    return changes;
}

inline void apply_report_delta(Json& document, const Json& delta) {
    if (delta.at("schema_version").text() != "pineforge-native-report-delta/v1")
        throw std::runtime_error("unsupported cumulative report delta");
    merge_report_fields(document, delta.at("fields"));
    if (const auto* as_of = delta.find("ratios_as_of_input")) {
        if (as_of->integer<std::uint64_t>() > document.at("input_cursor").integer<std::uint64_t>())
            throw std::runtime_error("invalid interval report ratio cursor");
        const auto& context = delta.at("ratio_context");
        const auto zone = context.at("chart_timezone").text();
        if (const auto* previous = document.find("ratio_context"))
            if (previous->at("chart_timezone").text() != zone)
                throw std::runtime_error("interval report ratio clock changed");
        document.members["ratios_as_of_input"] = *as_of;
        document.members["ratio_context"] = context;
    }
    for (const auto& [name, patch] : delta.at("arrays").members) {
        auto& owner = name == "closed_trades" ? document : document.members["report"];
        owner.kind = Json::Kind::Object;
        auto& values = owner.members[name];
        const auto offset = patch.at("offset").integer<std::size_t>();
        if (offset > values.items.size() || patch.at("items").kind != Json::Kind::Array)
            throw std::runtime_error("invalid cumulative report delta offset");
        values.kind = Json::Kind::Array;
        values.items.resize(offset);
        values.items.insert(values.items.end(), patch.at("items").items.begin(), patch.at("items").items.end());
    }
}

}
