#pragma once

#include "descriptor.hpp"
#include <pineforge/run_failure.hpp>

namespace pineforge::selected_admit {
enum ExitCode {
    kAdmitted = 0, kUsage = 2, kNone = 10, kNotAdmitted = 11,
    kSourceRows = 21, kPrimaryRows = 22, kScriptBars = 23, kAuxiliaryRows = 24,
    kBytes = 25, kFormat = 30, kInput = 31, kIo = 32, kCustody = 33,
    kCalendar = 34, kVersion = 40, kDescriptor = 41, kInternal = 70
};

// Only scalars: counting does not retain a Bar or allocate per input record.
struct CountingState {
    std::uint64_t bars = 0;
    std::int64_t first_ts_ms = 0;
    std::int64_t last_ts_ms = 0;
    bool has_timestamp = false;
};

struct StreamObservation {
    std::uint64_t bytes_read = 0;
    bool complete = false;
};

struct Outcome {
    Descriptor descriptor;
    int exit = kInternal;
    std::string status = "internal_error";
    std::string reason = "helper:internal_error";
    sc::DriverResult native{};
    bool has_native = false;
    CountingState sink{};
    std::vector<StreamObservation> streams;
    std::size_t input_index = std::numeric_limits<std::size_t>::max();
    std::uint64_t row_index = 0;
    RunFailureValue failure{};
    std::string english;
    std::uint64_t forwarded_bytes = 0;
    std::string forward_sha256;
    bool custody_complete = false;
};

inline const char* role_name(sc::Role role) {
    switch (role) {
    case sc::Role::Source: return "source";
    case sc::Role::Primary: return "primary";
    case sc::Role::Auxiliary: return "auxiliary";
    }
    return "invalid";
}

inline const char* reason_name(sc::DriverReason r) {
    switch (r) {
    case sc::DriverReason::None: return "none";
    case sc::DriverReason::NoRequestedFields: return "no_requested_fields";
    case sc::DriverReason::MissingObserver: return "missing_observer";
    case sc::DriverReason::MissingSink: return "missing_sink";
    case sc::DriverReason::MissingInput: return "missing_input";
    case sc::DriverReason::DuplicateInput: return "duplicate_input";
    case sc::DriverReason::InvalidInput: return "invalid_input";
    case sc::DriverReason::NoData: return "no_data";
    case sc::DriverReason::RequestInvalid: return "request_invalid";
    case sc::DriverReason::CalendarUnsupported: return "calendar_unsupported";
    case sc::DriverReason::BoundaryUnaligned: return "boundary_unaligned";
    case sc::DriverReason::FeedRangeInvalid: return "feed_range_invalid";
    case sc::DriverReason::RowsUnordered: return "rows_unordered";
    case sc::DriverReason::CsvColumns: return "csv_columns";
    case sc::DriverReason::CsvValue: return "csv_value";
    case sc::DriverReason::CsvUtf8: return "csv_utf8";
    case sc::DriverReason::CsvSyntax: return "csv_syntax";
    case sc::DriverReason::ObserverFailure: return "observer_failure";
    case sc::DriverReason::SinkFailure: return "sink_failure";
    case sc::DriverReason::InternalError: return "internal_error";
    case sc::DriverReason::InputReadFailure: return "input_read_failure";
    }
    return "internal_error";
}

inline void native_outcome(Outcome& out, sc::DriverResult result) {
    out.reason = reason_name(result.reason);
    out.input_index = result.input_index;
    out.row_index = result.row_index;
    switch (result.status) {
    case sc::DriverStatus::Completed: out.exit = kAdmitted; out.status = "completed"; break;
    case sc::DriverStatus::NotAdmitted:
        out.exit = result.reason == sc::DriverReason::NoRequestedFields ? kNone : kNotAdmitted;
        out.status = "not_admitted";
        if (result.reason == sc::DriverReason::MissingObserver
            || result.reason == sc::DriverReason::MissingSink
            || result.reason == sc::DriverReason::DuplicateInput
            || result.reason == sc::DriverReason::InvalidInput
            || result.reason == sc::DriverReason::InternalError) {
            out.exit = kInternal;
            out.status = "internal_error";
        }
        break;
    case sc::DriverStatus::FormatError: out.exit = kFormat; out.status = "format_error"; break;
    case sc::DriverStatus::IoError: out.exit = kIo; out.status = "io_error"; break;
    case sc::DriverStatus::InputError: out.exit = kInput; out.status = "input_error"; break;
    case sc::DriverStatus::CalendarError:
        out.exit = kCalendar; out.status = "calendar_error"; break;
    }
    out.has_native = true;
    out.native = std::move(result);
}

inline void exception_outcome(Outcome& out, const std::exception& error) {
    out.exit = kInternal;
    out.status = "internal_error";
    out.reason = dynamic_cast<const std::bad_alloc*>(&error) ? "helper:bad_alloc"
                                                          : "helper:internal_error";
    if (const auto* coded = dynamic_cast<const RunFailureInfo*>(&error)) {
        out.failure = coded->run_failure();
        switch (out.failure.code) {
        case RunFailureCode::selected_source_rows_limit_exceeded: out.exit = kSourceRows; break;
        case RunFailureCode::selected_primary_rows_limit_exceeded: out.exit = kPrimaryRows; break;
        case RunFailureCode::selected_script_bars_limit_exceeded: out.exit = kScriptBars; break;
        case RunFailureCode::selected_auxiliary_rows_limit_exceeded: out.exit = kAuxiliaryRows; break;
        case RunFailureCode::selected_bytes_limit_exceeded: out.exit = kBytes; break;
        default: break;
        }
        out.reason = run_failure_code_name(out.failure.code);
        if (out.exit != kInternal) out.status = "refused";
    }
    out.has_native = false;
    out.english = error.what();
    out.custody_complete = false;
}

// Escape UTF-8 to ASCII JSON, preserving decoded Unicode scalar values.
inline std::string ascii_quote(std::string_view text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    const auto unit = [&](unsigned n) {
        out += "\\u";
        for (int shift : {12, 8, 4, 0}) out += hex[(n >> shift) & 15];
    };
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = text[i++];
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c >= 32 && c < 127) out += static_cast<char>(c);
        else if (c < 128) unit(c);
        else {
            unsigned n = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
            unsigned point = c & ((1U << (6 - n)) - 1);
            while (n-- && i < text.size())
                point = (point << 6) | (static_cast<unsigned char>(text[i++]) & 63);
            if (point <= 0xffff) unit(point);
            else {
                point -= 0x10000;
                unit(0xd800 + (point >> 10));
                unit(0xdc00 + (point & 1023));
            }
        }
    }
    return out + '"';
}

struct JsonObject {
    std::string value = "{";
    bool first = true;
    void raw(std::string_view key, std::string_view v) {
        if (!first) value += ',';
        first = false;
        value += ascii_quote(key) + ':' + std::string(v);
    }
    void text(std::string_view key, std::string_view v) { raw(key, ascii_quote(v)); }
    template<class T> void count(std::string_view key, T v) { text(key, std::to_string(v)); }
    void boolean(std::string_view key, bool v) { raw(key, v ? "true" : "false"); }
    std::string finish() const { return value + '}'; }
};

inline std::string render_state(const sc::State& s) {
    JsonObject j;
    j.count("source_rows", s.source_rows);
    j.count("primary_rows", s.primary_rows);
    j.count("script_bars", s.script_bars);
    j.count("auxiliary_rows", s.auxiliary_rows);
    j.count("bytes", s.bytes);
    return j.finish();
}

inline std::string render_coverage(const sc::DriverCoverage& c) {
    JsonObject j;
    j.count("source_rows", c.source_rows);
    j.count("primary_rows", c.primary_rows);
    j.count("auxiliary_rows", c.auxiliary_rows);
    j.count("primary_script_events", c.primary_script_events);
    j.count("source_bytes", c.source_bytes);
    j.count("primary_bytes", c.primary_bytes);
    j.count("auxiliary_bytes", c.auxiliary_bytes);
    j.count("source_streams", c.source_streams);
    j.count("primary_streams", c.primary_streams);
    j.count("auxiliary_streams", c.auxiliary_streams);
    j.boolean("source_complete", c.source_complete);
    j.boolean("primary_complete", c.primary_complete);
    j.boolean("auxiliary_complete", c.auxiliary_complete);
    return j.finish();
}

inline std::string render_result(const Outcome& out) {
    const auto& d = out.descriptor;
    JsonObject j;
    j.text("schema", kResultSchema);
    j.raw("exit", std::to_string(out.exit));
    j.text("stage", d.stage == Stage::Source ? "source" : "selected");
    j.text("outcome", out.exit == kAdmitted ? "admitted"
                         : out.exit >= kSourceRows && out.exit <= kBytes ? "refused"
                         : out.exit == kNone || out.exit == kNotAdmitted ? "not_admitted" : "error");
    j.text("status", out.status);
    j.text("reason", out.reason);
    JsonObject identity;
    identity.text("run_id", d.run_id);
    identity.text("attempt_id", d.attempt_id);
    j.raw("identity", identity.finish());
    j.text("descriptor_sha256", d.descriptor_sha256);
    JsonObject helper;
    helper.text("git_sha", d.compiled_sha);
    helper.text("request_schema", kRequestSchema);
    j.raw("helper", helper.finish());
    if (out.has_native && (out.exit == kAdmitted || out.exit == kNone
        || out.exit == kNotAdmitted || out.exit == kFormat || out.exit == kInput
        || out.exit == kIo || out.exit == kCalendar)) {
        j.raw("state", render_state(out.native.state));
        j.raw("coverage", render_coverage(out.native.coverage));
    }
    if (out.failure.code != RunFailureCode::none) {
        j.text("code", run_failure_code_name(out.failure.code));
        if (out.exit >= kSourceRows && out.exit <= kBytes) {
            j.text("class", "resource");
            j.boolean("retryable", run_failure_code_retryable(out.failure.code));
        }
        j.raw("args", out.failure.args ? *out.failure.args : "{}");
        j.text("english", out.english);
    }
    if (out.input_index < d.streams.size() || out.has_native) {
        JsonObject where;
        if (out.input_index < d.streams.size())
            where.text("input_id", d.streams[out.input_index].id);
        where.count("row_index", out.row_index);
        if (out.has_native) {
            where.text("role", role_name(out.native.role));
            if (out.native.has_field) {
                constexpr const char* fields[] = {"source_rows", "primary_rows", "script_bars",
                                                 "auxiliary_rows", "bytes"};
                where.text("field", fields[static_cast<unsigned>(out.native.field)]);
            }
            where.text("option", out.native.option);
            where.count("bound", out.native.bound);
            where.count("value_ms", out.native.value_ms);
            where.count("previous_boundary_ms", out.native.previous_boundary_ms);
            where.count("next_boundary_ms", out.native.next_boundary_ms);
        }
        j.raw("where", where.finish());
    }
    std::string streams = "[";
    for (std::size_t i = 0; i < out.streams.size(); ++i) {
        if (i) streams += ',';
        JsonObject item;
        item.text("id", d.streams[i].id);
        item.count("bytes_read", out.streams[i].bytes_read);
        streams += item.finish();
    }
    j.raw("streams", streams + ']');
    if (d.stage == Stage::Selected) {
        JsonObject sink;
        sink.text("kind", "counting");
        sink.count("bars", out.sink.bars);
        sink.count("first_ts_ms", out.sink.first_ts_ms);
        sink.count("last_ts_ms", out.sink.last_ts_ms);
        j.raw("sink", sink.finish());
    } else {
        j.count("forwarded_bytes", out.forwarded_bytes);
        j.text("forward_sha256", out.forward_sha256);
        std::string deferred = "[";
        const auto add = [&](const char* name) {
            if (deferred.size() > 1) deferred += ',';
            deferred += ascii_quote(name);
        };
        if (d.bounds.primary_rows.present) add("primary_rows");
        if (d.bounds.script_bars.present) add("script_bars");
        if (d.bounds.auxiliary_rows.present) add("auxiliary_rows");
        if (d.bounds.bytes.present) add("bytes");
        j.raw("deferred", deferred + ']');
    }
    j.boolean("custody_complete", out.custody_complete);
    return j.finish() + '\n';
}

inline std::string manifest_projection(std::string_view helper_path,
                                       std::string_view version_full,
                                       std::string_view version,
                                       std::string_view git_sha, bool dirty,
                                       std::string_view spec_digest,
                                       std::string_view catalog_digest,
                                       std::string_view stages_json) {
    JsonObject j, helper, engine, descriptor, catalog;
    j.text("schema", kManifestSchema);
    helper.text("name", "pineforge-selected-admit");
    helper.text("path", helper_path);
    j.raw("helper", helper.finish());
    engine.text("version_full", version_full);
    engine.text("version", version);
    engine.text("git_sha", git_sha);
    engine.boolean("dirty", dirty);
    j.raw("engine", engine.finish());
    descriptor.text("request_schema", kRequestSchema);
    descriptor.text("result_schema", kResultSchema);
    descriptor.text("spec_sha256", spec_digest);
    j.raw("descriptor", descriptor.finish());
    catalog.raw("codes", "[\"selected_auxiliary_rows_limit_exceeded\","
        "\"selected_bytes_limit_exceeded\",\"selected_primary_rows_limit_exceeded\","
        "\"selected_script_bars_limit_exceeded\",\"selected_source_rows_limit_exceeded\"]");
    catalog.text("catalog_sha256", catalog_digest);
    j.raw("failure_catalog", catalog.finish());
    j.raw("stages", stages_json);
    return j.finish() + '\n';
}
}  // namespace pineforge::selected_admit
