// A compiled module advertises capabilities v1 but breaks its receipt contract.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

namespace {
enum class Fault {
    size_query, changed_size, no_size, no_nul, invalid_json, missing_field,
    unresolved_not_array, unresolved_member_not_string, currency_not_string,
    unresolved_declarations_number, unresolved_declarations_missing, unresolved_declarations_array
};

class ReceiptHost final : public pineforge::source::PineStrategyHost {
public:
    explicit ReceiptHost(Fault fault) : fault(fault) { attach_pine_execution_adapter(); }
    void on_source_bar(const pineforge::Bar&) override { ++bars; }
    const Fault fault;
    int queries = 0, copies = 0, bars = 0;
};

std::string_view payload(Fault fault) {
    if (fault == Fault::invalid_json) return "{";
    if (fault == Fault::missing_field) return "{}";
    if (fault == Fault::unresolved_not_array)
        return R"({"declarations":{"currency":"currency.USDT"},"requests":[],"requirements":{},)"
               R"("unresolved":"currency","version":1})";
    if (fault == Fault::unresolved_member_not_string)
        return R"({"declarations":{"currency":null},"requests":[],"requirements":{},)"
               R"("unresolved":["currency",42],"version":1})";
    if (fault == Fault::currency_not_string)
        return R"({"declarations":{"currency":null},"requests":[],"requirements":{},)"
               R"("unresolved":[],"version":1})";
    if (fault == Fault::unresolved_declarations_number)
        return R"({"declarations":42,"requests":[],"requirements":{},"unresolved":["currency"],"version":1})";
    if (fault == Fault::unresolved_declarations_missing)
        return R"({"requests":[],"requirements":{},"unresolved":["currency"],"version":1})";
    if (fault == Fault::unresolved_declarations_array)
        return R"({"declarations":[],"requests":[],"requirements":{},"unresolved":["currency"],"version":1})";
    return R"({"declarations":{"currency":"currency.NONE"},"unresolved":[],"version":1})";
}

template<class A, class B>
bool expect(const char* verdict, const char* row, const char* field, const A& actual,
            const B& expected) {
    if (actual == expected) return true;
    std::cerr << verdict << ' ' << row << ": " << field << " expected=" << expected
              << " actual=" << actual << '\n';
    return false;
}

std::string_view result(const char* value) { return value ? value : "<null>"; }
} // namespace

extern "C" std::uint32_t strategy_capabilities_api_version() { return PF_CAPABILITIES_API_VERSION; }
extern "C" int strategy_capabilities_receipt(pf_strategy_t s, char* json, std::size_t capacity,
                                             std::size_t* required, char*, std::size_t) {
    if (!s || !required) return PF_SETTINGS_INVALID_ARGUMENT;
    auto& host = *static_cast<ReceiptHost*>(static_cast<pineforge::BacktestEngine*>(s));
    const auto text = payload(host.fault);
    if (!json && capacity == 0) {
        ++host.queries;
        if (host.fault == Fault::size_query) return PF_SETTINGS_EXCEPTION;
        *required = text.size() + 1;
        return PF_SETTINGS_BUFFER_TOO_SMALL;
    }
    if (!json || capacity != text.size() + 1) return PF_SETTINGS_INVALID_ARGUMENT;
    ++host.copies;
    std::memcpy(json, text.data(), text.size());
    if (host.fault != Fault::no_nul) json[text.size()] = '\0';
    if (host.fault != Fault::no_size)
        *required = text.size() + (host.fault == Fault::changed_size ? 0 : 1);
    return PF_SETTINGS_OK;
}

int main() {
    constexpr auto incomplete = "incomplete compiled strategy currency declaration";
    constexpr auto malformed = "invalid compiled strategy currency declaration";
    constexpr struct { Fault fault; const char* name; const char* error; } rows[] = {
        {Fault::size_query, "size query", "cannot read the compiled strategy's currency declaration"},
        {Fault::changed_size, "changed size", incomplete},
        {Fault::no_size, "missing copy size", incomplete},
        {Fault::no_nul, "missing terminator", incomplete},
        {Fault::invalid_json, "invalid JSON", malformed},
        {Fault::missing_field, "missing field", malformed},
        {Fault::unresolved_not_array, "unresolved is not an array", malformed},
        {Fault::unresolved_member_not_string, "unresolved contains a non-string", malformed},
        {Fault::currency_not_string, "resolved currency is not a string", malformed},
        {Fault::unresolved_declarations_number, "unresolved currency with numeric declarations", malformed},
        {Fault::unresolved_declarations_missing, "unresolved currency without declarations", malformed},
        {Fault::unresolved_declarations_array, "unresolved currency with array declarations", malformed},
    };
    int compared = 0;
    for (const auto& row : rows) {
        ReceiptHost host(row.fault);
        pineforge::BacktestEngine* handle = &host;
        if (!expect("NOT COMPARED", row.name, "set chart currency",
                    strategy_set_syminfo_string(handle, "currency", "USDT"), 0) ||
            !expect("NOT COMPARED", row.name, "set scalar",
                    strategy_set_account_currency_fx_scalar(handle, 1.25), 0)) continue;
        const pineforge::Bar bar{100, 100, 100, 100, 1, 1704067200000};
        host.run(&bar, 1, "1", "1");
        if (!expect("NOT COMPARED", row.name, "size queries", host.queries, 1) ||
            !expect("NOT COMPARED", row.name, "copies", host.copies,
                    row.fault == Fault::size_query ? 0 : 1)) continue;
        if (expect("FAIL", row.name, "code", result(strategy_get_last_error_code(handle)),
                   "strategy_library_incompatible") &&
            expect("FAIL", row.name, "args", result(strategy_get_last_error_args(handle)),
                   R"({"reason":"capabilities_receipt_invalid"})") &&
            expect("FAIL", row.name, "text", result(strategy_get_last_error(handle)), row.error) &&
            expect("FAIL", row.name, "evaluated bars", host.bars, 0)) ++compared;
    }
    if (compared != 12) return 1;
    std::cout << "PASS: " << compared << " currency receipt refusals\n";
    return 0;
}
