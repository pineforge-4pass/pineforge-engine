#pragma once

#include "json.hpp"
#include <optional>

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

inline std::optional<std::uint64_t> stored_report_ratio_cursor(
        std::string_view stored, bool report_equality_checked) {
    // Canonical deltas end with these two envelope members (Json::dump sorts
    // keys). Only equality-checked rows may skip the full syntax parse here:
    // below a verified checkpoint that parse is their only syntax check.
    // Inspect at most 20 decimal digits beside that exact suffix, never a
    // similarly named field in an array row or a quoted string.
    constexpr std::string_view tail =
        ",\"schema_version\":\"pineforge-native-report-delta/v1\"}";
    constexpr std::string_view key = ",\"ratios_as_of_input\":";
    if (report_equality_checked && stored.size() <= 1024 * 1024 && stored.size() >= tail.size() &&
        stored.substr(stored.size() - tail.size()) == tail) {
        const auto end = stored.size() - tail.size();
        auto begin = end;
        while (begin && end - begin < 20 && stored[begin - 1] >= '0' && stored[begin - 1] <= '9')
            --begin;
        if (begin < end && (stored[begin] != '0' || end - begin == 1) &&
            begin >= key.size() && stored.substr(begin - key.size(), key.size()) == key &&
            stored.find("\"ratios_as_of_input\"") == begin - key.size() + 1 &&
            stored.find('\\') == std::string_view::npos) {
            std::uint64_t cursor = 0;
            const auto parsed = std::from_chars(stored.data() + begin, stored.data() + end, cursor);
            if (parsed.ec != std::errc{} || parsed.ptr != stored.data() + end)
                throw std::runtime_error("invalid or overflowing integer");
            return cursor;
        }
    }
    // Legacy full documents, noncanonical JSON, missing markers and malformed
    // tokens keep the previous parser/accessor semantics, including its errors
    // and frame limit. Rows below a verified checkpoint always take this path.
    const auto payload = parse_json(stored);
    if (const auto* as_of = payload.find("ratios_as_of_input"))
        return as_of->integer<std::uint64_t>();
    return std::nullopt;
}

}
