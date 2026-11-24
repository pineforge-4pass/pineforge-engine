#pragma once

#include "../strategy_module_symbol.hpp"
#include "../../runner/json.hpp"
#include <pineforge/run_failure.hpp>

#include <stdexcept>
#include <string_view>
#include <vector>

namespace pineforge::source::detail {

extern "C" std::uint32_t strategy_capabilities_api_version() PF_WEAK_STRATEGY_EXPORT;
extern "C" int strategy_capabilities_receipt(pf_strategy_t, char*, std::size_t, std::size_t*,
                                             char*, std::size_t) PF_WEAK_STRATEGY_EXPORT;

[[noreturn]] static void refuse_currency_receipt(const char* message) {
    throw coded<std::runtime_error>(RunFailureCode::strategy_library_incompatible,
                                    {{"reason", "capabilities_receipt_invalid"}}, message);
}

// Missing/older capabilities give no evidence about the account currency.
// Hand-written hosts without a declaration keep their existing FX contract.
// A v1 exporter that fails its buffer protocol cannot provide a usable run.
static bool compiled_account_uses_quote(BacktestEngine& host, const std::string& quote) {
    using ::pineforge::detail::strategy_module_symbol;
    const auto version = strategy_module_symbol(host, "strategy_capabilities_api_version",
                                                strategy_capabilities_api_version);
    if (!version || version() != PF_CAPABILITIES_API_VERSION) return false;
    const auto read = strategy_module_symbol(host, "strategy_capabilities_receipt",
                                             strategy_capabilities_receipt);
    std::size_t required = 0;
    if (!read || read(&host, nullptr, 0, &required, nullptr, 0) != PF_SETTINGS_BUFFER_TOO_SMALL ||
        required < 2 || required > 1024 * 1024)
        refuse_currency_receipt("cannot read the compiled strategy's currency declaration");
    std::vector<char> receipt(required, '\1'); // The exporter must write the terminating NUL.
    std::size_t written = 0;
    if (read(&host, receipt.data(), receipt.size(), &written, nullptr, 0) != PF_SETTINGS_OK ||
        written != required || receipt.back() != '\0')
        refuse_currency_receipt("incomplete compiled strategy currency declaration");

    constexpr auto invalid = "invalid compiled strategy currency declaration";
    live::Json document;
    try {
        document = live::parse_json(std::string_view(receipt.data(), written - 1));
        if (document.at("version").integer<unsigned>() != 1) return false;
    } catch (const std::runtime_error&) {
        refuse_currency_receipt(invalid);
    }
    const auto* unresolved = document.find("unresolved");
    if (!unresolved || unresolved->kind != live::Json::Kind::Array)
        refuse_currency_receipt(invalid);
    bool currency_unresolved = false;
    for (const auto& field : unresolved->items) {
        if (field.kind != live::Json::Kind::String) refuse_currency_receipt(invalid);
        if (field.value == "currency") currency_unresolved = true;
    }
    const auto* declarations = document.find("declarations");
    if (!declarations || declarations->kind != live::Json::Kind::Object)
        refuse_currency_receipt(invalid);
    if (currency_unresolved) return false;
    const auto* currency = declarations->find("currency");
    if (!currency || currency->kind != live::Json::Kind::String)
        refuse_currency_receipt(invalid);
    const std::string_view spelling = currency->value;
    constexpr std::string_view prefix = "currency.";
    if (spelling.substr(0, prefix.size()) != prefix) return false;
    const auto code = spelling.substr(prefix.size());
    return code == "NONE" || code == quote;
}

} // namespace pineforge::source::detail
