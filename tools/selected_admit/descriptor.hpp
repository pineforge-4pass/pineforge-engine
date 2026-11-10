#pragma once

#include "../../runner/json.hpp"
#include "../../src/selected_capacity_driver.hpp"

#include <climits>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace pineforge::selected_admit {
namespace sc = detail::selected_capacity;
using live::Json;

inline constexpr char kRequestSchema[] = "pineforge-selected-capacity-request/v1";
inline constexpr char kResultSchema[] = "pineforge-selected-capacity-result/v1";
inline constexpr char kManifestSchema[] = "pineforge-selected-capacity-helper-manifest/v1";
inline constexpr std::size_t kDescriptorBytes = 262144;
inline constexpr std::size_t kMaxStreams = 256;

enum class Stage { Source, Selected };
enum class DecodeStatus { Ok, UnknownVersion, HelperMismatch, TooLarge, HashMismatch, Invalid };

struct StreamSpec {
    std::string id;
    sc::Role role = sc::Role::Source;
    bool file = true;
    int fd = -1;
    int forward_fd = -1;
    std::uint64_t bytes = 0;
    std::string sha256;
    std::string label;
};

struct Descriptor {
    Stage stage = Stage::Selected;
    std::string expect_git_sha;
    std::string compiled_sha;
    std::string run_id;
    std::string attempt_id;
    std::string descriptor_sha256;
    sc::Limits bounds{};
    SelectedPlanRequest calendar{};
    std::uint64_t max_record_bytes = 0;
    std::vector<StreamSpec> streams;
};

inline bool any_requested(const sc::Limits& b) noexcept {
    return b.source_rows.present || b.primary_rows.present || b.script_bars.present
        || b.auxiliary_rows.present || b.bytes.present;
}

inline bool lower_hex(std::string_view s, std::size_t lo, std::size_t hi) noexcept {
    if (s.size() < lo || s.size() > hi) return false;
    for (char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline std::uint64_t decimal(std::string_view s) {
    if (s.empty() || (s.size() > 1 && s.front() == '0'))
        throw std::runtime_error("noncanonical unsigned decimal");
    std::uint64_t n = 0;
    for (char c : s) {
        if (c < '0' || c > '9'
            || n > (std::numeric_limits<std::uint64_t>::max()
                    - static_cast<unsigned>(c - '0')) / 10)
            throw std::runtime_error("invalid unsigned decimal");
        n = n * 10 + static_cast<unsigned>(c - '0');
    }
    return n;
}

inline std::int64_t signed_decimal(std::string_view s) {
    const bool negative = !s.empty() && s.front() == '-';
    const auto magnitude_text = negative ? s.substr(1) : s;
    const auto magnitude = decimal(magnitude_text);
    const auto positive_max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if ((negative && magnitude == 0) || magnitude > positive_max + (negative ? 1U : 0U))
        throw std::runtime_error("invalid signed decimal");
    if (negative && magnitude == positive_max + 1U)
        return std::numeric_limits<std::int64_t>::min();
    return negative ? -static_cast<std::int64_t>(magnitude)
                    : static_cast<std::int64_t>(magnitude);
}

inline int descriptor_number(std::string_view s) {
    const auto n = decimal(s);
    if (n < 3 || n > INT_MAX) throw std::runtime_error("invalid descriptor number");
    return static_cast<int>(n);
}

inline void reject_number_null(const Json& j) {
    if (j.kind == Json::Kind::Number || j.kind == Json::Kind::Null)
        throw std::runtime_error("JSON numbers and null are forbidden");
    for (const auto& item : j.items) reject_number_null(item);
    for (const auto& member : j.members) reject_number_null(member.second);
}

inline std::string bounded_text(const Json& j, std::size_t minimum, std::size_t maximum) {
    auto text = j.text();
    if (text.size() < minimum || text.size() > maximum)
        throw std::runtime_error("string length is outside its range");
    return text;
}

inline bool descriptor_fds_distinct(const Descriptor& d, int descriptor_fd) {
    if (descriptor_fd < 3) return false;
    std::set<int> seen{descriptor_fd};
    for (const auto& stream : d.streams) {
        if (!seen.insert(stream.fd).second) return false;
        if (!stream.file && !seen.insert(stream.forward_fd).second) return false;
    }
    return true;
}

inline DecodeStatus decode_descriptor(std::string_view bytes, Stage stage,
                                      std::string_view compiled_git_sha,
                                      Descriptor& out, std::string& why,
                                      int descriptor_fd = -1) {
    why.clear();
    if (bytes.size() > kDescriptorBytes) {
        why = "helper:descriptor_too_large";
        return DecodeStatus::TooLarge;
    }
    try {
        const auto root = live::parse_json(bytes);
        const auto schema = root.kind == Json::Kind::Object ? root.find("schema") : nullptr;
        if (!schema || schema->kind != Json::Kind::String || schema->value != kRequestSchema) {
            why = "helper:unknown_descriptor_version";
            return DecodeStatus::UnknownVersion;
        }
        reject_number_null(root);
        live::only_fields(root, {"schema", "expect", "identity", "bounds", "calendar",
                                 "limits", "streams"});
        Descriptor d;
        d.stage = stage;
        d.compiled_sha = std::string(compiled_git_sha);
        const auto& expect = root.at("expect");
        live::only_fields(expect, {"git_sha"});
        d.expect_git_sha = expect.at("git_sha").text();
        if (!lower_hex(d.expect_git_sha, 12, 40))
            throw std::runtime_error("invalid expected identity");
        const auto& identity = root.at("identity");
        live::only_fields(identity, {"run_id", "attempt_id"});
        d.run_id = bounded_text(identity.at("run_id"), 1, 200);
        d.attempt_id = bounded_text(identity.at("attempt_id"), 1, 200);
        const auto& bounds = root.at("bounds");
        live::only_fields(bounds, {"source_rows", "primary_rows", "script_bars",
                                   "auxiliary_rows", "bytes"});
        const auto bound = [&](const char* name) {
            const auto* v = bounds.find(name);
            return v ? sc::Bound{true, decimal(v->text())} : sc::Bound{};
        };
        d.bounds = {bound("source_rows"), bound("primary_rows"), bound("script_bars"),
                    bound("auxiliary_rows"), bound("bytes")};
        const auto* cal = root.find("calendar");
        if (!cal && d.bounds.script_bars.present)
            throw std::runtime_error("calendar is required");
        if (cal) {
            live::only_fields(*cal, {"start_ms", "end_ms", "fed_start_ms", "preroll_bars",
                                    "input_tf", "script_tf", "chart_timezone",
                                    "engine_timezone", "session", "feed_tolerant"});
            auto& c = d.calendar;
            c.start_ms = signed_decimal(cal->at("start_ms").text());
            c.end_ms = signed_decimal(cal->at("end_ms").text());
            c.fed_start_ms = signed_decimal(cal->at("fed_start_ms").text());
            const auto preroll = decimal(cal->at("preroll_bars").text());
            if (preroll > 5000) throw std::runtime_error("invalid preroll");
            c.preroll_bars = static_cast<std::uint32_t>(preroll);
            c.input_tf = cal->at("input_tf").text();
            c.script_tf = cal->at("script_tf").text();
            c.chart_timezone = cal->at("chart_timezone").text();
            c.engine_timezone = cal->at("engine_timezone").text();
            c.session = cal->at("session").text();
            const auto& tolerant = cal->at("feed_tolerant");
            if (tolerant.kind != Json::Kind::Bool)
                throw std::runtime_error("invalid feed_tolerant");
            c.feed_tolerant = tolerant.value == "true";
        }
        const auto& limits = root.at("limits");
        live::only_fields(limits, {"max_record_bytes"});
        d.max_record_bytes = decimal(limits.at("max_record_bytes").text());
        if (d.max_record_bytes < 1 || d.max_record_bytes > 1048576)
            throw std::runtime_error("invalid record cap");
        const auto& streams = root.at("streams");
        if (streams.kind != Json::Kind::Array || streams.items.empty()
            || streams.items.size() > kMaxStreams)
            throw std::runtime_error("invalid stream count");
        std::set<int> fds;
        std::set<std::string> ids;
        d.streams.reserve(streams.items.size());
        for (const auto& item : streams.items) {
            live::only_fields(item, {"id", "role", "kind", "fd", "bytes", "sha256",
                                    "forward_fd", "label"});
            StreamSpec spec;
            spec.id = bounded_text(item.at("id"), 1, 200);
            if (!ids.insert(spec.id).second) throw std::runtime_error("duplicate input id");
            const auto role = item.at("role").text();
            if (role == "source") spec.role = sc::Role::Source;
            else if (role == "primary") spec.role = sc::Role::Primary;
            else if (role == "auxiliary") spec.role = sc::Role::Auxiliary;
            else throw std::runtime_error("invalid role");
            const auto kind = item.at("kind").text();
            spec.file = kind == "file";
            if (kind != (stage == Stage::Selected ? "file" : "pipe")
                || (stage == Stage::Source && spec.role != sc::Role::Source))
                throw std::runtime_error("stream kind or role disagrees with stage");
            spec.fd = descriptor_number(item.at("fd").text());
            if (!fds.insert(spec.fd).second) throw std::runtime_error("duplicate fd");
            if (const auto* label = item.find("label"))
                spec.label = bounded_text(*label, 0, 64);
            if (spec.file) {
                if (item.find("forward_fd")) throw std::runtime_error("file has forward fd");
                spec.bytes = decimal(item.at("bytes").text());
                spec.sha256 = item.at("sha256").text();
                if (!lower_hex(spec.sha256, 64, 64)) throw std::runtime_error("invalid digest");
            } else {
                if (item.find("bytes") || item.find("sha256"))
                    throw std::runtime_error("pipe has file identity");
                spec.forward_fd = descriptor_number(item.at("forward_fd").text());
                if (!fds.insert(spec.forward_fd).second) throw std::runtime_error("duplicate fd");
            }
            d.streams.push_back(std::move(spec));
        }
        if (descriptor_fd >= 0 && !descriptor_fds_distinct(d, descriptor_fd))
            throw std::runtime_error("descriptor aliases a stream");
        if (!lower_hex(compiled_git_sha, 12, 40)
            || !(compiled_git_sha.substr(0, d.expect_git_sha.size()) == d.expect_git_sha
                 || std::string_view(d.expect_git_sha).substr(0, compiled_git_sha.size())
                        == compiled_git_sha)) {
            why = "helper:helper_mismatch";
            return DecodeStatus::HelperMismatch;
        }
        out = std::move(d);
        return DecodeStatus::Ok;
    } catch (const std::bad_alloc&) {
        throw;
    } catch (const std::exception&) {
        why = "helper:descriptor_invalid";
        return DecodeStatus::Invalid;
    }
}
}  // namespace pineforge::selected_admit
