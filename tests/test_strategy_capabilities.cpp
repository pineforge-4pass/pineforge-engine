#include <pineforge/pineforge.h>
#include "../runner/capabilities.hpp"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {
std::string receipt(pf_strategy_t strategy) {
    char error[128] = "previous error";
    std::size_t required = 0;
    assert(strategy_capabilities_receipt(strategy, nullptr, 0, &required, error, sizeof(error)) ==
           PF_SETTINGS_BUFFER_TOO_SMALL);
    assert(required > 1 && error[0]);
    std::vector<char> output(required);
    assert(strategy_capabilities_receipt(strategy, output.data(), output.size(), &required,
                                        error, sizeof(error)) == PF_SETTINGS_OK);
    assert(error[0] == '\0' && output.back() == '\0');
    assert(std::strlen(output.data()) + 1 == required);
    return output.data();
}

void refused(const pineforge::live::Json& document, const std::string& name) {
    try {
        pineforge::live::require_close_only_capabilities(document.dump());
        assert(false);
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find(name) != std::string::npos);
    }
}
}

int main() {
    assert(strategy_capabilities_api_version() == PF_CAPABILITIES_API_VERSION);
    auto strategy = strategy_create(nullptr);
    assert(strategy);
    const auto initial = receipt(strategy);
    const auto document = pineforge::live::parse_json(initial);
    assert(document.at("version").value == "1");
    assert(document.at("declarations").members.size() == 11);
    assert(document.at("requests").items.empty());
    assert(document.at("requirements").members.size() == 6);
    assert(document.at("unresolved").items.empty());
    pineforge::live::require_close_only_capabilities(initial);
    std::size_t required = 999;
    char output[4] = "old";
    char error[8]{};
    assert(strategy_capabilities_receipt(nullptr, output, sizeof(output), &required,
                                         error, sizeof(error)) == PF_SETTINGS_INVALID_ARGUMENT);
    assert(required == 0 && error[sizeof(error) - 1] == '\0');
    assert(strategy_capabilities_receipt(strategy, output, sizeof(output), nullptr,
                                         error, sizeof(error)) == PF_SETTINGS_INVALID_ARGUMENT);
    assert(strategy_capabilities_receipt(strategy, output, sizeof(output), &required,
                                         error, sizeof(error)) == PF_SETTINGS_BUFFER_TOO_SMALL);
    assert(required == initial.size() + 1 && output[0] == '\0');
    assert(strategy_capabilities_receipt(strategy, nullptr, required, &required,
                                         nullptr, 0) == PF_SETTINGS_BUFFER_TOO_SMALL);
    assert(strategy_capabilities_receipt(strategy, output, 0, &required, error, 0) ==
           PF_SETTINGS_BUFFER_TOO_SMALL);
    strategy_set_input(strategy, "Enabled", "false");
    strategy_set_override(strategy, "process_orders_on_close", "true");
    assert(receipt(strategy) == initial);
    std::vector<pf_bar_t> bars;
    for (int index = 0; index < 10; ++index)
        bars.push_back({100, 101, 99, 100, 5, 1577836800000LL + index * 60000LL});
    for (int run = 0; run < 2; ++run) {
        pf_report_t report{};
        run_backtest_full(strategy, bars.data(), static_cast<int>(bars.size()), "1", "1",
                          0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
        report_free(&report);
        assert(receipt(strategy) == initial);
    }
    auto fresh = strategy_create(nullptr);
    assert(receipt(fresh) == initial);
    strategy_free(fresh);
    strategy_free(strategy);
    for (const auto& name : {"calc_on_every_tick", "calc_on_order_fills", "calc_on_every_history_tick", "process_orders_on_close", "use_bar_magnifier",
                             "fill_orders_on_standard_ohlc"}) {
        auto changed = document;
        changed.members.at("declarations").members.at(name) = pineforge::live::Json::boolean(true);
        refused(changed, name);
    }
    for (const auto& entry : document.at("requirements").members) {
        auto changed = document;
        changed.members.at("requirements").members.at(entry.first) = pineforge::live::Json::boolean(true);
        refused(changed, entry.first);
    }
    auto changed = document;
    changed.members.at("declarations").members.at("backtest_fill_limits_assumption") = pineforge::live::Json::number("1");
    refused(changed, "backtest_fill_limits_assumption");
    changed = document;
    changed.members.at("declarations").members.erase("calc_on_every_tick");
    refused(changed, "calc_on_every_tick");
    changed = document;
    changed.members.at("version") = pineforge::live::Json::number("2");
    refused(changed, "version");
    changed = document;
    changed.members.at("declarations").members.at("calc_on_every_tick") = pineforge::live::Json::boolean(true);
    changed.members.at("unresolved").items.push_back(pineforge::live::Json::string("calc_on_every_tick"));
    refused(changed, "calc_on_every_tick (unresolved at compilation)");
    for (const auto& name : {"barstate.islast", "barstate.islastconfirmedhistory", "last_bar_index", "last_bar_time"}) {
        changed = document;
        changed.members.at("unresolved").items.push_back(pineforge::live::Json::string(name));
        refused(changed, std::string(name) + " (unresolved at compilation)");
    }
    for (const auto& function : {"request.security", "request.security_lower_tf", "request.earnings", "request.footprint"}) {
        changed = document;
        changed.members.at("requests").items.push_back(pineforge::live::Json::object({
            {"function", pineforge::live::Json::string(function)}}));
        refused(changed, function);
        refused(changed, "the native stream does not yet reproduce the batch for requested series");
    }
    std::cout << "strategy capabilities: shape, buffer protocol, fresh/reused determinism and close-only policy PASS\n";
}
