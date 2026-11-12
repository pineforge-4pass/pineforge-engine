// Stable run-failure codes (include/pineforge/run_failure.hpp): the registry
// every raised code is validated against, the canonical argument JSON, the
// classifier, and the forwards to the record the execution consumer keeps
// beside BacktestEngine::last_error_.

#include <pineforge/run_failure.hpp>
#include <pineforge/detail/run_quota.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/ta.hpp>

#include "native_execution_consumer.hpp"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
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
    case RunFailureArgKind::string:
        return true;
    case RunFailureArgKind::integer:
    case RunFailureArgKind::number:
    case RunFailureArgKind::nullable_string:
    case RunFailureArgKind::scalar:
    case RunFailureArgKind::boolean:
        return false;
    }
    return false;
}

// Runtime ownership counters use the entire uint64_t domain. Their wire
// representation is decimal text, so no byte is lost through int64_t or JSON
// floating-point conversion. This contract is independent of host run quotas.
bool canonical_u64_decimal(const std::string& text) noexcept {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
    return parsed.ec == std::errc() && parsed.ptr == text.data() + text.size();
}

// A scalar argument is text, an integer, a finite number, a boolean (held as 0
// or 1) or a null. A non-finite number, a boolean outside 0 and 1 and any
// type value this build does not know are refused.
bool scalar_value_ok(const RunFailureArg& arg) noexcept {
    switch (arg.type) {
    case RunFailureArg::Type::text:
    case RunFailureArg::Type::integer:
    case RunFailureArg::Type::null_value:
        return true;
    case RunFailureArg::Type::number:
        return std::isfinite(arg.number);
    case RunFailureArg::Type::boolean:
        return arg.integer == 0 || arg.integer == 1;
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

// The factory's JSON ownership domain. Private to this translation unit: only
// make_run_failure mints it, and only RunFailureValue::observe_owned_storage
// recognizes it, through std::get_deleter on this exact type plus its stable
// tag. The deleter and the shared control each hold a copy of the same
// OwnedAllocator, so the ledger outlives every weak owner until the control
// request itself is deallocated. Records are non-owning: there is no cycle.
constexpr std::uint64_t kRunFailureJsonTag = 0x50464a534f4e7631ull;  // "PFJSONv1"

struct TrackedJsonDeleter {
    OwnedAllocator<std::string> allocation;
    const std::string* owned;
    std::uint64_t tag;

    void operator()(const std::string* object) noexcept {
        if (object == nullptr) return;
        // own_json constructed a non-const std::string in this request.
        auto* constructed = const_cast<std::string*>(object);
        std::destroy_at(constructed);
        allocation.deallocate(constructed, 1);
    }
};

// Moves the canonical text into a string object allocated through the domain's
// real allocator, then shares it with that same allocator for the control
// request. If the control allocation throws, shared_ptr calls the deleter, so
// nothing leaks; every failure here is std::bad_alloc for the caller.
std::shared_ptr<const std::string> own_json(std::string&& text) {
    OwnedAllocator<std::string> allocation;
    std::string* object = allocation.allocate(1);
    ::new (static_cast<void*>(object)) std::string(std::move(text));
    return std::shared_ptr<const std::string>(
        object, TrackedJsonDeleter{allocation, object, kRunFailureJsonTag}, allocation);
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
                if ((code == RunFailureCode::run_resource_quota_exceeded
                     || code == RunFailureCode::run_descriptor_limit_exceeded)
                    && (arg.integer < 0
                        || static_cast<std::uint64_t>(arg.integer) > detail::kRunQuotaMax)) {
                    return invariant();
                }
                break;
            case RunFailureArgKind::number:
                // An integer or a finite number: never text, a boolean or a null.
                if (arg.type != RunFailureArg::Type::integer
                    && arg.type != RunFailureArg::Type::number) {
                    return invariant();
                }
                if (arg.type == RunFailureArg::Type::number && !std::isfinite(arg.number)) {
                    return invariant();
                }
                break;
            case RunFailureArgKind::string:
                if (arg.type != RunFailureArg::Type::text) return invariant();
                if ((code == RunFailureCode::runtime_history_budget_exhausted
                     || code == RunFailureCode::runtime_storage_observation_incomplete)
                    && !canonical_u64_decimal(arg.text)) {
                    return invariant();
                }
                break;
            case RunFailureArgKind::nullable_string:
                if (arg.type != RunFailureArg::Type::text
                    && arg.type != RunFailureArg::Type::null_value) {
                    return invariant();
                }
                break;
            case RunFailureArgKind::scalar:
                if (!scalar_value_ok(arg)) return invariant();
                break;
            case RunFailureArgKind::boolean:
                if (arg.type != RunFailureArg::Type::boolean
                    || (arg.integer != 0 && arg.integer != 1)) return invariant();
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
        std::string json;
        json.push_back('{');
        bool first = true;
        for (const auto& [declared, arg] : matched) {
            if (!first) json.push_back(',');
            first = false;
            append_json_string(json, declared->name);
            json.push_back(':');
            switch (arg->type) {
            case RunFailureArg::Type::integer:
                json += std::to_string(arg->integer);
                break;
            case RunFailureArg::Type::number:
                if (!append_json_number(json, arg->number)) return invariant();
                break;
            case RunFailureArg::Type::text:
                append_json_string(json, arg->text);
                break;
            case RunFailureArg::Type::boolean:
                if (arg->integer != 0) {
                    json += "true";
                } else {
                    json += "false";
                }
                break;
            case RunFailureArg::Type::null_value:
                json += "null";
                break;
            }
        }
        json.push_back('}');
        // The same canonical bytes, now held in the tracked ownership domain.
        return RunFailureValue{code, own_json(std::move(json))};
    } catch (const std::bad_alloc&) {
        return RunFailureValue{RunFailureCode::out_of_memory, nullptr};
    } catch (...) {
        return invariant();
    }
}

void RunFailureValue::observe_owned_storage(OwnedStorage& out) const noexcept {
    struct PreserveErrno {
        int value;
        ~PreserveErrno() { errno = value; }
    } preserve_errno{errno};
    // No control block: nothing is owned. A non-owning alias names storage
    // this value does not own and proves no lower bound for it.
    if (args.use_count() == 0) {
        if (args) out.incomplete(OwnedStorageUnsupported::opaque_type);
        return;
    }
    // Only this translation unit's deleter type and tag identify the factory
    // domain. An unknown control, or a lost RTTI/module identity, stays unknown
    // with no guessed bytes and no read of its pointee.
    const TrackedJsonDeleter* tracked = std::get_deleter<TrackedJsonDeleter>(args);
    if (tracked == nullptr || tracked->tag != kRunFailureJsonTag || tracked->owned == nullptr) {
        out.incomplete(OwnedStorageUnsupported::allocator);
        out.incomplete(OwnedStorageUnsupported::opaque_type);
        return;
    }
    // The ledger, its records and the real requests (string object, control),
    // claimed once per walk however many owners share the domain.
    tracked->allocation.observe_owned_storage(out);
    // Contain the original object in a real request before reading it. Its
    // inline bytes are already in that request; only an out-of-object buffer
    // is added, once per walk, by the existing string accounting.
    if (!tracked->allocation.contains(tracked->owned, sizeof(std::string))) {
        out.incomplete(OwnedStorageUnsupported::allocator);
        out.incomplete(OwnedStorageUnsupported::opaque_type);
        return;
    }
    ::pineforge::observe_owned_storage(out, *tracked->owned);
    // A null alias retains exactly this domain. Any other pointee is foreign:
    // the known domain stays counted; the pointee is neither owned nor read.
    const std::string* pointee = args.get();
    if (pointee != nullptr && pointee != tracked->owned)
        out.incomplete(OwnedStorageUnsupported::opaque_type);
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

std::string run_error_bar_prefix() {
    const ta::BarContext& context = ta::bar_context();
    if (!context.installed) return "Error before the first bar: ";
    return "Error on bar " + std::to_string(context.bar_index) + ": ";
}

std::int64_t run_error_bar_index() noexcept {
    const ta::BarContext& context = ta::bar_context();
    return context.installed ? context.bar_index : -1;
}

RunFailureValue classify_run_failure(const std::exception& error) noexcept {
    const auto quota = detail::run_quota_failure();
    if (quota.code != RunFailureCode::none) return quota;
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
    const auto quota = detail::run_quota_failure();
    if (quota.code != RunFailureCode::none) {
        NativeExecutionConsumer::note_failure_record(engine, "run quota refused", quota);
        return;
    }
    NativeExecutionConsumer::note_failure_record(engine, std::move(text),
                                                 make_run_failure(code, args));
}

void note_run_failure(BacktestEngine& engine, const char* text, RunFailureCode code,
                      const RunFailureArgs& args) {
    note_run_failure(engine, std::string(text ? text : ""), code, args);
}

void note_run_failure(BacktestEngine& engine, std::string text, const RunFailureValue& value) {
    const auto quota = detail::run_quota_failure();
    NativeExecutionConsumer::note_failure_record(
        engine, quota.code != RunFailureCode::none ? "run quota refused" : std::move(text),
        quota.code != RunFailureCode::none ? quota : value);
}

void note_run_failure(BacktestEngine& engine, const char* entrypoint,
                      const std::exception& error) {
    std::string text = entrypoint ? entrypoint : "";
    text += ": ";
    text += error.what();
    note_run_failure(engine, std::move(text), classify_run_failure(error));
}

void note_run_failure_unknown(BacktestEngine& engine, const char* entrypoint) {
    std::string text = entrypoint ? entrypoint : "";
    text += ": unknown C++ exception";
    note_run_failure(
        engine, std::move(text),
        RunFailureValue{RunFailureCode::engine_unclassified_error, nullptr});
}

void clear_run_failure(BacktestEngine& engine) noexcept {
    NativeExecutionConsumer::clear_failure_record(engine);
}

const char* run_failure_code_of(const BacktestEngine& engine) noexcept {
    const auto quota = detail::run_quota_failure();
    if (quota.code != RunFailureCode::none) return run_failure_code_name(quota.code);
    return NativeExecutionConsumer::failure_code_of(engine);
}

const char* run_failure_args_of(const BacktestEngine& engine) noexcept {
    const auto quota = detail::run_quota_failure();
    if (quota.code != RunFailureCode::none) return quota.args ? quota.args->c_str() : "{}";
    return NativeExecutionConsumer::failure_args_of(engine);
}

RunFailureValue run_failure_value_of(const BacktestEngine& engine) noexcept {
    const auto quota = detail::run_quota_failure();
    if (quota.code != RunFailureCode::none) return quota;
    return NativeExecutionConsumer::failure_value_of(engine);
}

}  // namespace pineforge
