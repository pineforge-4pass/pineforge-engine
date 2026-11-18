// Strict external-name validation, independent of private strategy fixtures.
#include <pineforge/run_failure.hpp>
#include <pineforge/owned_storage.hpp>
#include <pineforge/detail/run_quota.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;

// Consume the generated catalog metadata, never a second list of names or
// validation implementation. The ordinary catalog guard checks this include
// against docker/run_failure_codes.json.
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
#include "../src/run_failure_registry.inc"

int checks = 0;
int failures = 0;
void check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
    }
}
#define CHECK(expression) check((expression), #expression, __LINE__)

std::string args_json(const RunFailureValue& value) {
    return value.args ? *value.args : "{}";
}

std::string quote(const std::string& text) {
    std::string result = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') {
            result += '\\';
            result += static_cast<char>(c);
        } else if (c < 0x20 || c == 0x7f) {
            char escaped[7];
            std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
            result += escaped;
        } else {
            result += static_cast<char>(c);
        }
    }
    return result + '"';
}

void accepted(const RegistryCode& row, RunFailureCode code, const RunFailureArgs& args,
              const std::string& json) {
    const auto result = validate_run_failure_name(row.name, args);
    CHECK(result.status == RunFailureValidationStatus::accepted);
    CHECK(result.value.has_value());
    if (!result.value) return;
    CHECK(result.value->code == code);
    CHECK(std::string_view(run_failure_code_name(result.value->code)) == row.name);
    CHECK(args_json(*result.value) == json);
    const auto typed = make_run_failure(code, args);
    CHECK(typed.code == code);
    CHECK(args_json(typed) == json);
}

void invalid(const RegistryCode& row, RunFailureCode code, const RunFailureArgs& args) {
    const auto result = validate_run_failure_name(row.name, args);
    CHECK(result.status == RunFailureValidationStatus::invalid_args);
    CHECK(!result.value.has_value());
    const auto typed = make_run_failure(code, args);
    CHECK(typed.code == RunFailureCode::engine_invariant);
    CHECK(!typed.args && typed.args.use_count() == 0);
}

struct Sample {
    RunFailureArg arg;
    std::string json;
};

Sample sample(const RegistryArg& spec, bool alternate) {
    switch (spec.kind) {
    case RunFailureArgKind::integer:
        return {{spec.name, alternate ? 19 : 7}, alternate ? "19" : "7"};
    case RunFailureArgKind::number:
        return {{spec.name, alternate ? -2.5 : 1.5}, alternate ? "-2.5" : "1.5"};
    case RunFailureArgKind::boolean:
        return {{spec.name, !alternate}, alternate ? "false" : "true"};
    case RunFailureArgKind::nullable_string:
        if (!alternate) return {{spec.name, nullptr}, "null"};
        break;
    default:
        break;
    }
    // Decimal text also satisfies the two runtime-ownership string refinements.
    std::string text = alternate ? "19" : "7";
    if (spec.kind == RunFailureArgKind::vocab) {
        CHECK(spec.value_count > 0);
        text = kRegistryValues[spec.first_value + (alternate ? spec.value_count - 1 : 0)];
    } else if (spec.kind != RunFailureArgKind::string) {
        text = alternate ? "different\t\"\\text" : "sample\ntext";
    }
    return {{spec.name, text}, quote(text)};
}

std::string object_json(const std::vector<Sample>& samples, bool required_only,
                        const RegistryCode& row) {
    std::string json = "{";
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const auto& spec = kRegistryArgs[row.first_arg + i];
        if (required_only && spec.optional) continue;
        if (json.size() > 1) json += ',';
        json += quote(spec.name) + ':' + samples[i].json;
    }
    return json + '}';
}

void registry_contract() {
    CHECK(sizeof(kRegistryCodes) / sizeof(kRegistryCodes[0]) == kRunFailureCodeCount);
    for (std::size_t c = 1; c < kRunFailureCodeCount; ++c) {
        const auto code = static_cast<RunFailureCode>(c);
        const auto& row = kRegistryCodes[c];
        CHECK(std::string_view(run_failure_code_name(code)) == row.name);
        for (bool alternate : {false, true}) {
            std::vector<Sample> samples;
            RunFailureArgs all, required;
            for (std::uint16_t i = 0; i < row.arg_count; ++i) {
                const auto& spec = kRegistryArgs[row.first_arg + i];
                samples.push_back(sample(spec, alternate));
                all.push_back(samples.back().arg);
                if (!spec.optional) required.push_back(samples.back().arg);
            }
            // Reverse input order: expected output remains sorted catalog order.
            std::reverse(all.begin(), all.end());
            accepted(row, code, all, object_json(samples, false, row));
            accepted(row, code, required, object_json(samples, true, row));
            for (std::size_t i = 0; i < required.size(); ++i) {
                auto missing = required;
                missing.erase(missing.begin() + static_cast<std::ptrdiff_t>(i));
                invalid(row, code, missing);
            }
            auto extra = all;
            extra.emplace_back("not_an_argument", 7);
            invalid(row, code, extra);
            extra.back().name = nullptr;
            invalid(row, code, extra);
            extra.back().name = "";
            invalid(row, code, extra);

            // Restore metadata order for one-argument substitutions.
            all.clear();
            for (const auto& value : samples) all.push_back(value.arg);
            for (std::size_t i = 0; i < all.size(); ++i) {
                const auto& spec = kRegistryArgs[row.first_arg + i];
                const auto replaced = [&](RunFailureArg arg) {
                    auto changed = all;
                    changed[i] = std::move(arg);
                    return changed;
                };
                const auto good = [&](RunFailureArg arg, std::string json) {
                    auto expected = samples;
                    expected[i].json = std::move(json);
                    accepted(row, code, replaced(std::move(arg)),
                             object_json(expected, false, row));
                };
                auto duplicate = all;
                duplicate.push_back(all[i]);
                invalid(row, code, duplicate);
                auto bad_name = all;
                bad_name[i].name = nullptr;
                invalid(row, code, bad_name);

                const char* name = spec.name;
                switch (spec.kind) {
                case RunFailureArgKind::integer:
                    invalid(row, code, replaced({name, "7"}));
                    invalid(row, code, replaced({name, 7.0}));
                    invalid(row, code, replaced({name, true}));
                    invalid(row, code, replaced({name, nullptr}));
                    break;
                case RunFailureArgKind::number:
                    good({name, -3}, "-3");
                    invalid(row, code, replaced({name, "1.5"}));
                    invalid(row, code, replaced({name, true}));
                    invalid(row, code, replaced({name, nullptr}));
                    break;
                case RunFailureArgKind::boolean:
                    invalid(row, code, replaced({name, "true"}));
                    invalid(row, code, replaced({name, 1}));
                    invalid(row, code, replaced({name, 1.0}));
                    invalid(row, code, replaced({name, nullptr}));
                    break;
                case RunFailureArgKind::scalar:
                    good({name, -3}, "-3");
                    good({name, 2.5}, "2.5");
                    good({name, true}, "true");
                    good({name, false}, "false");
                    good({name, nullptr}, "null");
                    break;
                case RunFailureArgKind::nullable_string:
                    invalid(row, code, replaced({name, 7}));
                    invalid(row, code, replaced({name, 1.5}));
                    invalid(row, code, replaced({name, true}));
                    break;
                default:
                    invalid(row, code, replaced({name, 7}));
                    invalid(row, code, replaced({name, 1.5}));
                    invalid(row, code, replaced({name, true}));
                    invalid(row, code, replaced({name, nullptr}));
                    break;
                }
                for (double nonfinite : {std::numeric_limits<double>::quiet_NaN(),
                                         std::numeric_limits<double>::infinity(),
                                         -std::numeric_limits<double>::infinity()}) {
                    invalid(row, code, replaced({name, nonfinite}));
                }
                RunFailureArg bad_boolean{name, true};
                bad_boolean.integer = 2;
                invalid(row, code, replaced(bad_boolean));
                auto bad_type = all[i];
                bad_type.type = static_cast<RunFailureArg::Type>(255);
                invalid(row, code, replaced(bad_type));
                if (spec.kind == RunFailureArgKind::vocab) {
                    for (std::uint16_t v = 0; v < spec.value_count; ++v) {
                        const char* value = kRegistryValues[spec.first_value + v];
                        good({name, value}, quote(value));
                    }
                    invalid(row, code, replaced({name, "not_a_listed_value"}));
                }
                if ((code == RunFailureCode::run_resource_quota_exceeded
                     || code == RunFailureCode::run_descriptor_limit_exceeded)
                    && spec.kind == RunFailureArgKind::integer) {
                    good({name, 0}, "0");
                    const auto max = detail::kRunQuotaMax;
                    good({name, max}, std::to_string(max));
                    invalid(row, code, replaced({name, -1}));
                    invalid(row, code, replaced({name, max + 1}));
                }
                if ((code == RunFailureCode::runtime_history_budget_exhausted
                     || code == RunFailureCode::runtime_storage_observation_incomplete)
                    && spec.kind == RunFailureArgKind::string) {
                    for (const char* decimal : {"0", "18446744073709551615"}) {
                        good({name, decimal}, quote(decimal));
                    }
                    for (const char* bad : {"", "00", "01", "-1", "+1", "1.0", " 1",
                                            "18446744073709551616"}) {
                        invalid(row, code, replaced({name, bad}));
                    }
                }
            }
        }
    }
}

void exact_names_and_invariant_pair() {
    const RunFailureArgs bad{{nullptr, true}, {"undeclared", 7}};
    for (std::string_view name : {"", "none", "unknown_code", "ENGINE_INVARIANT",
                                  "engine_invariant ", " engine_invariant", "engine_",
                                  "engine_invariant\n", "engine-invariant", "\xff"}) {
        for (const auto& args : {RunFailureArgs{}, bad}) {
            const auto result = validate_run_failure_name(name, args);
            CHECK(result.status == RunFailureValidationStatus::unknown_code);
            CHECK(!result.value);
        }
    }
    std::string embedded = "engine_invariant";
    embedded.push_back('\0');
    for (const char* tail : {"", "suffix"}) {
        const std::string name = embedded + tail;
        const auto result = validate_run_failure_name(std::string_view(name.data(), name.size()), bad);
        CHECK(result.status == RunFailureValidationStatus::unknown_code);
        CHECK(!result.value);
    }
    const auto code = RunFailureCode::engine_invariant;
    const auto& row = kRegistryCodes[static_cast<std::size_t>(code)];
    accepted(row, code, {}, "{}");
    const std::string bounded = "engine_invariant/ignored";
    const auto bounded_name = std::string_view(bounded.data(), std::string_view(row.name).size());
    const auto bounded_result = validate_run_failure_name(bounded_name, {});
    CHECK(bounded_result.status == RunFailureValidationStatus::accepted);
    CHECK(bounded_result.value && bounded_result.value->code == code);
    CHECK(bounded_result.value && args_json(*bounded_result.value) == "{}");
    accepted(row, code, {{"reason", "order_key_hash_collision"}},
             "{\"reason\":\"order_key_hash_collision\"}");
    invalid(row, code, {{"undeclared", 7}});
    invalid(row, code, {{"reason", "not_a_listed_value"}});
    CHECK(make_run_failure(RunFailureCode::none, {}).code == code);
    CHECK(make_run_failure(static_cast<RunFailureCode>(65535), {}).code == code);
    CHECK(std::string_view(run_failure_code_name(RunFailureCode::none)).empty());
    CHECK(std::string_view(run_failure_code_name(static_cast<RunFailureCode>(65535))).empty());
    // The catalog's legitimate resource code is separate from inability to validate.
    const auto oom = validate_run_failure_name("out_of_memory", {});
    CHECK(oom.status == RunFailureValidationStatus::accepted);
    CHECK(oom.value && oom.value->code == RunFailureCode::out_of_memory);
}

void strict_value_owns_canonical_json() {
    for (std::size_t width : {std::size_t{1}, std::size_t{97}, std::size_t{1031}}) {
        RunFailureArgs args{{"line", 19}, {"call", std::string(width, 'c')},
                            {"function", "request.earnings"}};
        auto strict = validate_run_failure_name("no_data_request", args);
        CHECK(strict.status == RunFailureValidationStatus::accepted && strict.value);
        if (!strict.value) continue;
        const std::string json = "{\"call\":\"" + std::string(width, 'c')
            + "\",\"function\":\"request.earnings\",\"line\":19}";
        CHECK(args_json(*strict.value) == json);
        args[1].text.assign(width, 'x');
        CHECK(args_json(*strict.value) == json);
        errno = ERANGE;
        OwnedStorage storage;
        strict.value->observe_owned_storage(storage);
        const auto usage = storage.usage();
        CHECK(errno == ERANGE);
        CHECK(usage.complete() && usage.backing_bytes == 0);
        CHECK(usage.resident_payload_bytes >= sizeof(std::string));
        auto copy = *strict.value;
        copy.observe_owned_storage(storage);
        CHECK(storage.usage().resident_bytes() == usage.resident_bytes());
        const RunFailureValue alias{copy.code, std::shared_ptr<const std::string>(copy.args, nullptr)};
        alias.observe_owned_storage(storage);
        CHECK(storage.usage().complete());
        CHECK(storage.usage().resident_bytes() == usage.resident_bytes());
        strict.value.reset();
        CHECK(args_json(copy) == json);
        OwnedStorage copied;
        copy.observe_owned_storage(copied);
        CHECK(copied.usage().complete());
        CHECK(copied.usage().resident_bytes() == usage.resident_bytes());
    }
}
}  // namespace

int main() {
    static_assert(PINEFORGE_HAS_RUN_FAILURE_VALIDATION_V1 == 1, "header availability marker");
    try {
        registry_contract();
        exact_names_and_invariant_pair();
        strict_value_owns_canonical_json();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL unexpected exception: %s\n", error.what());
        return 1;
    }
    std::printf("run_failure_validation: codes=%zu checks=%d failures=%d\n",
                pineforge::kRunFailureCodeCount - 1, checks, failures);
    return failures == 0 && checks > 0 ? 0 : 1;
}
