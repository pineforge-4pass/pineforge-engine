#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include "../runner/json.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int allocation_failure = 0;
}

void* operator new(std::size_t size) {
    if (allocation_failure) {
        const int failure = allocation_failure;
        allocation_failure = 0;
        if (failure == 1) throw std::bad_alloc();
        throw 73;
    }
    if (void* allocation = std::malloc(size ? size : 1)) return allocation;
    throw std::bad_alloc();
}
void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete[](void* allocation) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, std::size_t) noexcept { ::operator delete(allocation); }

int main() {
    using namespace pineforge::checked_settings;
    using pineforge::live::parse_json;
    assert(number(0xffff0000LL) == "4294901760");
    assert(strategy_settings_api_version() == PF_SETTINGS_API_VERSION);
    char error[256]{};
    pf_strategy_t strategy = nullptr;
    assert(strategy_create_checked(nullptr, nullptr, error, sizeof(error)) == PF_SETTINGS_INVALID_ARGUMENT);
    assert(strategy_create_checked("{\"Length\":7}", &strategy, error, sizeof(error)) == PF_SETTINGS_UNSUPPORTED);
    assert(strategy == nullptr && error[0]);
    for (int failure : {1, 2}) {
        allocation_failure = failure;
        assert(strategy_create_checked(nullptr, &strategy, error, sizeof(error)) == PF_SETTINGS_EXCEPTION);
        assert(strategy == nullptr && error[0]);
        allocation_failure = failure;
        assert(strategy_create(nullptr) == nullptr);
    }
    assert(strategy_create_checked(nullptr, &strategy, error, sizeof(error)) == PF_SETTINGS_OK);
    assert(strategy && !error[0]);
    auto input = [&](const char* key, const char* value) {
        return strategy_set_input_checked(strategy, key, value, error, sizeof(error));
    };
    auto override_setting = [&](const char* key, const char* value) {
        return strategy_set_override_checked(strategy, key, value, error, sizeof(error));
    };
    auto receipt = [&] {
        std::size_t required = 0;
        assert(strategy_get_effective_settings(strategy, nullptr, 0, &required, error, sizeof(error)) == PF_SETTINGS_BUFFER_TOO_SMALL);
        assert(required > 1);
        char tiny[2] = {'x', 'x'};
        assert(strategy_get_effective_settings(strategy, tiny, sizeof(tiny), &required, error, sizeof(error)) == PF_SETTINGS_BUFFER_TOO_SMALL);
        assert(tiny[0] == '\0');
        std::vector<char> json(required);
        assert(strategy_get_effective_settings(strategy, json.data(), json.size(), &required, error, sizeof(error)) == PF_SETTINGS_OK);
        assert(!error[0] && required == std::strlen(json.data()) + 1);
        return std::string(json.data());
    };
    const auto defaults = receipt();
    for (const auto& setting : std::vector<std::pair<const char*, const char*>>{
             {"Lenght", "7"}, {"undeclared", "5"}, {"Length", "14suffix"},
             {"Length", "2.0"}, {"Length", "0"}, {"Length", "51"},
             {"Length", "2147483648"}, {"Threshold", "nan"}, {"Threshold", "NaN"},
             {"Threshold", "inf"}, {"Threshold", "-Inf"}, {"Threshold", "1e999"},
             {"Threshold", "1e-999"}, {"Threshold", "1e-310"}, {"Length", "+-1"},
             {"Threshold", "2.5 "}, {"Threshold", "0x1p2"}, {"Threshold", "2x"},
             {"Enabled", "TRUE"}, {"Enabled", "yes"}, {"Mode", "typo"},
             {"Side", "Side.neutral"}, {"Side", "-1"}, {"Side", "3"}, {"Source", "no-such-series"}}) {
        assert(input(setting.first, setting.second) == PF_SETTINGS_INVALID_ARGUMENT);
        assert(error[0]);
        assert(receipt() == defaults);
    }
    for (const auto& setting : std::vector<std::pair<const char*, const char*>>{
             {"default_qty_typo", "cash"}, {"default_qty_type", "typo"},
             {"commission_type", "3"}, {"process_orders_on_close", "falsex"},
             {"calc_on_order_fills", "2"}, {"close_entries_rule", "random"},
             {"slippage", "-1"}, {"pyramiding", "2.0"}, {"initial_capital", "1suffix"},
             {"commission_value", "NaN"}, {"default_qty_value", "Inf"}}) {
        assert(override_setting(setting.first, setting.second) == PF_SETTINGS_INVALID_ARGUMENT);
        assert(error[0]);
        assert(receipt() == defaults);
    }
    assert(input("Length", "4") == PF_SETTINGS_OK);
    assert(input("Threshold", "3.125") == PF_SETTINGS_OK);
    assert(input("Enabled", "0") == PF_SETTINGS_OK);
    assert(input("Mode", "slow") == PF_SETTINGS_OK);
    assert(input("Side", "Side.short") == PF_SETTINGS_OK);
    assert(input("Source", "open") == PF_SETTINGS_OK);
    assert(input("Stamp", "1700000000000") == PF_SETTINGS_OK);
    assert(override_setting("default_qty_type", "strategy.cash") == PF_SETTINGS_OK);
    assert(override_setting("default_qty_value", "200") == PF_SETTINGS_OK);
    assert(override_setting("commission_type", "2") == PF_SETTINGS_OK);
    assert(override_setting("close_entries_rule", "any") == PF_SETTINGS_OK);
    assert(override_setting("process_orders_on_close", "1") == PF_SETTINGS_OK);
    const auto document = parse_json(receipt());
    assert(document.members.at("version").value == "1");
    const auto& inputs = document.members.at("inputs").items;
    assert(inputs.size() == 8);
    assert(inputs[0].members.at("name").value == "Length");
    assert(inputs[0].members.at("type").value == "int");
    assert(inputs[0].members.at("min").value == "1");
    assert(inputs[0].members.at("max").value == "50");
    assert(inputs[0].members.at("step").value == "2");
    assert(inputs[0].members.at("effective_value").value == "4");
    assert(inputs[1].members.at("effective_value").value == "3.125");
    assert(inputs[2].members.at("effective_value").value == "false");
    assert(inputs[3].members.at("options").items.size() == 2);
    assert(inputs[4].members.at("effective_value").value == "2");
    assert(inputs[5].members.at("effective_value").value == "open");
    assert(inputs[6].members.at("effective_value").value == "1700000000000");
    const auto& overrides = document.members.at("overrides").items;
    assert(overrides.size() == 10);
    assert(overrides[2].members.at("effective_value").value == "200");
    assert(overrides[5].members.at("effective_value").value == "true");
    assert(overrides[7].members.at("effective_value").value == "ANY");
    assert(overrides[8].members.at("effective_value").value == "cash");
    assert(overrides[9].members.at("effective_value").value == "cash_per_contract");
    const std::string allocation_value(512, '1');
    for (int failure : {1, 2}) {
        allocation_failure = failure;
        assert(input("Length", "9") == PF_SETTINGS_EXCEPTION);
        assert(error[0]);
        allocation_failure = failure;
        assert(override_setting("pyramiding", "9") == PF_SETTINGS_EXCEPTION);
        allocation_failure = failure;
        strategy_set_input(strategy, "Length", allocation_value.c_str());
        assert(allocation_failure == 0);
        allocation_failure = failure;
        strategy_set_override(strategy, "initial_capital", allocation_value.c_str());
        assert(allocation_failure == 0);
    }
    pf_report_t report{};
    assert(run_backtest_full_checked(strategy, nullptr, -1, "", "", 0, 4,
           PF_MAGNIFIER_ENDPOINTS, &report, error, sizeof(error)) == PF_SETTINGS_INVALID_ARGUMENT);
    const std::string long_timeframe(256, '9');
    for (int failure : {1, 2}) {
        allocation_failure = failure;
        assert(run_backtest_full_checked(strategy, nullptr, 0, long_timeframe.c_str(), "",
               0, 4, PF_MAGNIFIER_ENDPOINTS, &report, error, sizeof(error)) == PF_SETTINGS_EXCEPTION);
        allocation_failure = failure;
        run_backtest_full(strategy, nullptr, 0, long_timeframe.c_str(), "", 0, 4,
                          PF_MAGNIFIER_ENDPOINTS, &report);
        assert(allocation_failure == 0);
    }
    assert(boundary(error, sizeof(error), [] { throw std::runtime_error("standard failure"); }) == PF_SETTINGS_EXCEPTION);
    assert(std::string(error) == "standard failure");
    char short_error[4]{};
    assert(boundary(short_error, sizeof(short_error), [] { throw 73; }) == PF_SETTINGS_EXCEPTION);
    assert(short_error[3] == '\0');
    assert(boundary(nullptr, 0, [] { throw 73; }) == PF_SETTINGS_EXCEPTION);
    pf_bar_t bars[8]{};
    for (int index = 0; index < 8; ++index) {
        bars[index] = {100, 102, 99, 101, 5,
                       static_cast<std::int64_t>(index) * 60000};
    }
    assert(run_backtest_full_checked(strategy, bars, 8, "1", "1", 0, 4,
           PF_MAGNIFIER_ENDPOINTS, &report, error, sizeof(error)) == PF_SETTINGS_OK);
    assert(report.total_trades == 0);
    assert(input("Enabled", "true") == PF_SETTINGS_UNSUPPORTED);
    report_free(&report);
    pf_strategy_t configured = nullptr;
    assert(strategy_create_checked(nullptr, &configured, error, sizeof(error)) == PF_SETTINGS_OK);
    assert(strategy_set_input_checked(configured, "Threshold", "3.125", error, sizeof(error)) == PF_SETTINGS_OK);
    assert(run_backtest_full_checked(configured, bars, 8, "1", "1", 0, 4,
           PF_MAGNIFIER_ENDPOINTS, &report, error, sizeof(error)) == PF_SETTINGS_OK);
    assert(report.total_trades > 0);
    for (int index = 0; index < report.trades_len; ++index)
        assert(report.trades[index].qty == 3.125);
    report_free(&report);
    strategy_free(configured);
    strategy_free(strategy);
    std::cout << "checked settings: validation, receipt and exception containment PASS\n";
}
