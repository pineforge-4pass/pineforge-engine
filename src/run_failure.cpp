// Stable run-failure codes (include/pineforge/run_failure.hpp): the registry
// every raised code is validated against, the canonical argument JSON, the
// classifier, and the forwards to the record the execution consumer keeps
// beside BacktestEngine::last_error_.

#include <pineforge/run_failure.hpp>

#include "native_execution_consumer.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>
#include <utility>
#include <vector>

namespace pineforge {
namespace {

struct RegistryArg {
    const char* name;
    RunFailureArgKind kind;
    bool optional;
    std::uint16_t first_value;
    std::uint16_t value_count;
};

struct RegistryCode {
    const char* name;
    RunFailureClass cls;
    bool retryable;
    std::uint16_t first_arg;
    std::uint16_t arg_count;
};

#include "run_failure_registry.inc"

constexpr std::size_t kCodeRows = sizeof(kRegistryCodes) / sizeof(kRegistryCodes[0]);
static_assert(kCodeRows == kRunFailureCodeCount, "registry and enum disagree");

const RegistryCode* registry_row(RunFailureCode code) noexcept {
    const auto index = static_cast<std::size_t>(code);
    if (index == 0 || index >= kCodeRows) return nullptr;
    return &kRegistryCodes[index];
}

bool text_kind(RunFailureArgKind kind) noexcept {
    switch (kind) {
    case RunFailureArgKind::identifier:
    case RunFailureArgKind::keyword:
    case RunFailureArgKind::vocab:
    case RunFailureArgKind::pine_source:
    case RunFailureArgKind::symbol:
    case RunFailureArgKind::timeframe:
        return true;
    case RunFailureArgKind::integer:
    case RunFailureArgKind::number:
        return false;
    }
    return false;
}

bool string_equals(const char* a, const std::string& b) noexcept {
    return b.compare(a) == 0;
}

// One UTF-8 sequence starting at `i`, or 0 when it is not well-formed.
std::size_t utf8_sequence(const std::string& text, std::size_t i) noexcept {
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char lead = byte(i);
    std::size_t length = 0;
    unsigned char low = 0x80, high = 0xBF;
    if (lead < 0x80) return 1;
    if (lead >= 0xC2 && lead <= 0xDF) length = 2;
    else if (lead == 0xE0) { length = 3; low = 0xA0; }
    else if (lead >= 0xE1 && lead <= 0xEC) length = 3;
    else if (lead == 0xED) { length = 3; high = 0x9F; }
    else if (lead >= 0xEE && lead <= 0xEF) length = 3;
    else if (lead == 0xF0) { length = 4; low = 0x90; }
    else if (lead >= 0xF1 && lead <= 0xF3) length = 4;
    else if (lead == 0xF4) { length = 4; high = 0x8F; }
    else return 0;
    if (i + length > text.size()) return 0;
    for (std::size_t k = 1; k < length; ++k) {
        const unsigned char c = byte(i + k);
        if (c < (k == 1 ? low : 0x80) || c > (k == 1 ? high : 0xBF)) return 0;
    }
    return length;
}

// A JSON string: quotes, backslashes and control characters escaped; a byte
// that is not well-formed UTF-8 becomes U+FFFD.
void append_json_string(std::string& out, const std::string& text) {
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c == '"' || c == '\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
            ++i;
        } else if (c < 0x20 || c == 0x7F) {
            char buffer[8];
            std::snprintf(buffer, sizeof buffer, "\\u%04x", static_cast<unsigned>(c));
            out += buffer;
            ++i;
        } else {
            const std::size_t length = utf8_sequence(text, i);
            if (length == 0) {
                out += "\xEF\xBF\xBD";
                ++i;
            } else {
                out.append(text, i, length);
                i += length;
            }
        }
    }
    out.push_back('"');
}

bool append_json_number(std::string& out, double value) {
    if (!std::isfinite(value)) return false;
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof buffer, value);
    if (converted.ec != std::errc()) return false;
    out.append(buffer, converted.ptr);
    return true;
}

RunFailureValue invariant() noexcept {
    return RunFailureValue{RunFailureCode::engine_invariant, nullptr};
}

}  // namespace

RunFailureArg::RunFailureArg(const char* arg_name, const char* value)
    : name(arg_name), text(value ? value : "") {}

RunFailureValue make_run_failure(RunFailureCode code, const RunFailureArgs& args) noexcept {
    try {
        const RegistryCode* row = registry_row(code);
        if (row == nullptr) return invariant();
        if (args.empty()) {
            for (std::uint16_t a = 0; a < row->arg_count; ++a) {
                if (!kRegistryArgs[row->first_arg + a].optional) return invariant();
            }
            return RunFailureValue{code, nullptr};
        }
        // Each raised arg must be declared once, of its declared kind, a vocab
        // value from its closed list; every required arg must be raised.
        std::vector<std::pair<const RegistryArg*, const RunFailureArg*>> matched;
        matched.reserve(args.size());
        for (const RunFailureArg& arg : args) {
            const RegistryArg* declared = nullptr;
            for (std::uint16_t a = 0; a < row->arg_count; ++a) {
                const RegistryArg& candidate = kRegistryArgs[row->first_arg + a];
                if (arg.name != nullptr && std::string(candidate.name) == arg.name) {
                    declared = &candidate;
                    break;
                }
            }
            if (declared == nullptr) return invariant();
            for (const auto& seen : matched) {
                if (seen.first == declared) return invariant();
            }
            switch (declared->kind) {
            case RunFailureArgKind::integer:
                if (arg.type != RunFailureArg::Type::integer) return invariant();
                break;
            case RunFailureArgKind::number:
                if (arg.type == RunFailureArg::Type::text) return invariant();
                if (arg.type == RunFailureArg::Type::number && !std::isfinite(arg.number)) {
                    return invariant();
                }
                break;
            case RunFailureArgKind::vocab: {
                if (arg.type != RunFailureArg::Type::text) return invariant();
                bool known = false;
                for (std::uint16_t v = 0; v < declared->value_count; ++v) {
                    if (string_equals(kRegistryValues[declared->first_value + v], arg.text)) {
                        known = true;
                        break;
                    }
                }
                if (!known) return invariant();
                break;
            }
            default:
                if (!text_kind(declared->kind) || arg.type != RunFailureArg::Type::text) {
                    return invariant();
                }
                break;
            }
            matched.emplace_back(declared, &arg);
        }
        for (std::uint16_t a = 0; a < row->arg_count; ++a) {
            const RegistryArg* declared = &kRegistryArgs[row->first_arg + a];
            if (declared->optional) continue;
            const bool raised = std::any_of(matched.begin(), matched.end(),
                [&](const auto& seen) { return seen.first == declared; });
            if (!raised) return invariant();
        }
        // Canonical: keys in byte order (the registry lists them sorted, so
        // registry order is key order), no whitespace.
        std::sort(matched.begin(), matched.end(), [](const auto& l, const auto& r) {
            return std::string(l.first->name) < std::string(r.first->name);
        });
        auto json = std::make_shared<std::string>();
        json->push_back('{');
        bool first = true;
        for (const auto& [declared, arg] : matched) {
            if (!first) json->push_back(',');
            first = false;
            append_json_string(*json, declared->name);
            json->push_back(':');
            switch (arg->type) {
            case RunFailureArg::Type::integer:
                *json += std::to_string(arg->integer);
                break;
            case RunFailureArg::Type::number:
                if (!append_json_number(*json, arg->number)) return invariant();
                break;
            case RunFailureArg::Type::text:
                append_json_string(*json, arg->text);
                break;
            }
        }
        json->push_back('}');
        return RunFailureValue{code, std::move(json)};
    } catch (const std::bad_alloc&) {
        return RunFailureValue{RunFailureCode::out_of_memory, nullptr};
    } catch (...) {
        return invariant();
    }
}

const char* run_failure_code_name(RunFailureCode code) noexcept {
    const RegistryCode* row = registry_row(code);
    return row == nullptr ? "" : row->name;
}

RunFailureClass run_failure_code_class(RunFailureCode code) noexcept {
    const RegistryCode* row = registry_row(code);
    return row == nullptr ? RunFailureClass::engine_fault : row->cls;
}

bool run_failure_code_retryable(RunFailureCode code) noexcept {
    const RegistryCode* row = registry_row(code);
    return row != nullptr && row->retryable;
}

RunFailureValue classify_run_failure(const std::exception& error) noexcept {
    if (const auto* info = dynamic_cast<const RunFailureInfo*>(&error)) {
        return info->run_failure();
    }
    if (dynamic_cast<const std::bad_alloc*>(&error) != nullptr) {
        return RunFailureValue{RunFailureCode::out_of_memory, nullptr};
    }
    if (dynamic_cast<const std::logic_error*>(&error) != nullptr) {
        return RunFailureValue{RunFailureCode::engine_invariant, nullptr};
    }
    return RunFailureValue{RunFailureCode::engine_unclassified_error, nullptr};
}

RunFailureInfo::~RunFailureInfo() = default;

void note_run_failure(BacktestEngine& engine, std::string text, RunFailureCode code,
                      const RunFailureArgs& args) {
    NativeExecutionConsumer::note_failure_record(engine, std::move(text),
                                                 make_run_failure(code, args));
}

void note_run_failure(BacktestEngine& engine, const char* text, RunFailureCode code,
                      const RunFailureArgs& args) {
    NativeExecutionConsumer::note_failure_record(engine, std::string(text ? text : ""),
                                                 make_run_failure(code, args));
}

void note_run_failure(BacktestEngine& engine, std::string text, const RunFailureValue& value) {
    NativeExecutionConsumer::note_failure_record(engine, std::move(text), value);
}

void note_run_failure(BacktestEngine& engine, const char* entrypoint,
                      const std::exception& error) {
    std::string text = entrypoint ? entrypoint : "";
    text += ": ";
    text += error.what();
    NativeExecutionConsumer::note_failure_record(engine, std::move(text),
                                                 classify_run_failure(error));
}

void note_run_failure_unknown(BacktestEngine& engine, const char* entrypoint) {
    std::string text = entrypoint ? entrypoint : "";
    text += ": unknown C++ exception";
    NativeExecutionConsumer::note_failure_record(
        engine, std::move(text),
        RunFailureValue{RunFailureCode::engine_unclassified_error, nullptr});
}

void clear_run_failure(BacktestEngine& engine) noexcept {
    NativeExecutionConsumer::clear_failure_record(engine);
}

const char* run_failure_code_of(const BacktestEngine& engine) noexcept {
    return NativeExecutionConsumer::failure_code_of(engine);
}

const char* run_failure_args_of(const BacktestEngine& engine) noexcept {
    return NativeExecutionConsumer::failure_args_of(engine);
}

RunFailureValue run_failure_value_of(const BacktestEngine& engine) noexcept {
    return NativeExecutionConsumer::failure_value_of(engine);
}

}  // namespace pineforge
