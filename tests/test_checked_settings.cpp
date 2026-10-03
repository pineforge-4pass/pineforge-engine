#include <pineforge/pineforge.h>
#include <pineforge/checked_settings.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cassert>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int allocation_failure = 0;

class PreparationFailureProbe final : public pineforge::source::PineStrategyHost {
public:
    enum class Kind { Standard, Unknown, Latched };
    explicit PreparationFailureProbe(Kind kind) : kind_(kind) {}
    int callbacks = 0;
    void prepare_script_run(const pineforge::Bar*, int, bool) override {
        if (kind_ == Kind::Standard) throw std::runtime_error("literal preparation failure");
        if (kind_ == Kind::Unknown) throw 73;
        throw pineforge::checked_settings::LatchedSettingsFailure("strategy_set_override: original failure");
    }
    void on_source_bar(const pineforge::Bar&) override { ++callbacks; }
private:
    Kind kind_;
};

std::string receipt_field(const std::string& document, const std::string& name,
                          const std::string& field) {
    const auto row = document.find("{\"name\":\"" + name + "\"");
    assert(row != std::string::npos);
    const auto key = "\"" + field + "\":";
    const auto found = document.find(key, row);
    assert(found != std::string::npos && found < document.find('}', row));
    const auto begin = found + key.size();
    if (document[begin] == '"') {
        const auto end = document.find('"', begin + 1);
        return document.substr(begin + 1, end - begin - 1);
    }
    const auto end = document[begin] == '[' ? document.find(']', begin) + 1
                                           : document.find_first_of(",}", begin);
    return document.substr(begin, end - begin);
}
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
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new(size); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try { return ::operator new[](size); } catch (...) { return nullptr; }
}
void operator delete(void* allocation, const std::nothrow_t&) noexcept { ::operator delete(allocation); }
void operator delete[](void* allocation, const std::nothrow_t&) noexcept { ::operator delete[](allocation); }

int main() {
    using namespace pineforge::checked_settings;
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
             {"Length", "5.5"}, {"Length", "5.0000000000000001"}, {"Length", "0"}, {"Length", "51"},
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
             {"slippage", "-1"}, {"pyramiding", "2.5"}, {"initial_capital", "1suffix"},
             {"commission_value", "NaN"}, {"default_qty_value", "Inf"}}) {
        assert(override_setting(setting.first, setting.second) == PF_SETTINGS_INVALID_ARGUMENT);
        assert(error[0]);
        assert(receipt() == defaults);
    }
    assert(input("Length", "5.0") == PF_SETTINGS_OK);
    assert(receipt_field(receipt(), "Length", "effective_value") == "5");
    assert(input("Length", "5e0") == PF_SETTINGS_OK);
    assert(receipt_field(receipt(), "Length", "effective_value") == "5");
    assert(integer("9223372036854775807.0", 64) == std::numeric_limits<std::int64_t>::max());
    assert(integer("-9223372036854775808e0", 64) == std::numeric_limits<std::int64_t>::min());
    assert(input("Side", "short") == PF_SETTINGS_INVALID_ARGUMENT);
    assert(std::string(error) == "invalid enum option");
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
    const auto document = receipt();
    assert(document.find("{\"version\":1,\"inputs\":[") == 0);
    const auto split = document.find("],\"overrides\":[");
    assert(split != std::string::npos);
    assert(std::count(document.begin(), document.begin() + split, '{') == 9);
    assert(std::count(document.begin() + split, document.end(), '{') == 10);
    assert(receipt_field(document, "Length", "type") == "int");
    assert(receipt_field(document, "Length", "kind") == "int");
    assert(receipt_field(document, "Tint", "type") == "int");
    assert(receipt_field(document, "Tint", "kind") == "string");
    assert(receipt_field(document, "Length", "min") == "1");
    assert(receipt_field(document, "Length", "max") == "50");
    assert(receipt_field(document, "Length", "step") == "2");
    assert(receipt_field(document, "Length", "effective_value") == "4");
    assert(receipt_field(document, "Threshold", "effective_value") == "3.125");
    assert(receipt_field(document, "Enabled", "effective_value") == "false");
    assert(receipt_field(document, "Mode", "options") == "[\"fast\",\"slow\"]");
    assert(receipt_field(document, "Side", "effective_value") == "2");
    assert(receipt_field(document, "Source", "effective_value") == "open");
    assert(receipt_field(document, "Stamp", "effective_value") == "1700000000000");
    assert(receipt_field(document, "default_qty_value", "default") == "1");
    assert(receipt_field(document, "default_qty_value", "effective_value") == "200");
    assert(receipt_field(document, "process_orders_on_close", "effective_value") == "true");
    assert(receipt_field(document, "close_entries_rule", "effective_value") == "ANY");
    assert(receipt_field(document, "default_qty_type", "effective_value") == "cash");
    assert(receipt_field(document, "commission_type", "effective_value") == "cash_per_contract");
    const std::string allocation_value(512, '1');
    for (int failure : {1, 2}) {
        allocation_failure = failure;
        assert(input("Length", "9") == PF_SETTINGS_EXCEPTION);
        assert(error[0]);
        allocation_failure = failure;
        assert(override_setting("pyramiding", "9") == PF_SETTINGS_EXCEPTION);
        pf_strategy_t failed = strategy_create(nullptr);
        allocation_failure = failure;
        strategy_set_input(failed, "Length", allocation_value.c_str());
        assert(allocation_failure == 0);
        assert(std::string(strategy_get_last_error(failed)).find("strategy_set_input:") == 0);
        const std::string first_failure = strategy_get_last_error(failed);
        allocation_failure = failure;
        strategy_set_override(failed, "initial_capital", allocation_value.c_str());
        assert(allocation_failure == 0);
        assert(std::string(strategy_get_last_error(failed)) == first_failure);
        strategy_free(failed);
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
        assert(std::string(strategy_get_last_error(strategy)).find("run_backtest_full:") == 0);
        if (failure == 2)
            assert(std::string(strategy_get_last_error(strategy)).find("unknown C++ exception") != std::string::npos);
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
    const pineforge::Bar source_bars[]{{100, 102, 99, 101, 5, 0},
                                     {100, 102, 99, 101, 5, 60000}};
    for (const auto kind : {PreparationFailureProbe::Kind::Standard,
                           PreparationFailureProbe::Kind::Unknown,
                           PreparationFailureProbe::Kind::Latched}) {
        const bool latched = kind == PreparationFailureProbe::Kind::Latched;
        const std::string expected = latched ? "strategy_set_override: original failure"
            : kind == PreparationFailureProbe::Kind::Standard ? "literal preparation failure"
            : "unknown error during Pine script preparation";
        PreparationFailureProbe batch_probe(kind);
        batch_probe.run(source_bars, 2);
        assert(batch_probe.last_error() == expected && batch_probe.callbacks == 0);
        PreparationFailureProbe stream_probe(kind);
        assert(stream_probe.stream_begin(source_bars, 2, "1", "1") == !latched);
        assert(stream_probe.last_error() == expected && stream_probe.callbacks == 0);
        assert(stream_probe.stream_is_realtime() == !latched);
        if (!latched) assert(stream_probe.stream_end());
    }
    std::cout << "source preparation: latched batch/stream refusal and ordinary exception compatibility PASS\n";
    pf_strategy_t poisoned = strategy_create(nullptr);
    strategy_set_override(poisoned, "pyramiding", "abc");
    const auto setter_failure = std::string(strategy_get_last_error(poisoned));
    assert(setter_failure.find("strategy_set_override:") == 0 && setter_failure.size() > 23);
    assert(strategy_set_aux_security_feed(poisoned, nullptr, 0, "1") == 0);
    assert(std::string(strategy_get_last_error(poisoned)).empty());
    strategy_set_input(poisoned, "Length", "4");
    report.total_trades = 99;
    run_backtest_full(poisoned, bars, 8, "1", "1", 0, 4, PF_MAGNIFIER_ENDPOINTS, &report);
    assert(report.total_trades == 0 && std::string(strategy_get_last_error(poisoned)) == setter_failure);
    assert(run_backtest_full_checked(poisoned, bars, 8, "1", "1", 0, 4,
           PF_MAGNIFIER_ENDPOINTS, &report, error, sizeof(error)) == PF_SETTINGS_RUN_FAILED);
    assert(std::string(error) == setter_failure);
    report.total_trades = 99;
    run_backtest(poisoned, bars, 8, &report);
    assert(report.total_trades == 0 && std::string(strategy_get_last_error(poisoned)) == setter_failure);
    assert(strategy_set_aux_security_feed(poisoned, nullptr, 0, "1") == 0);
    assert(std::string(strategy_get_last_error(poisoned)).empty());
    assert(strategy_stream_begin(poisoned, bars, 8, "1", "1") == -1);
    assert(std::string(strategy_get_last_error(poisoned)) == setter_failure);
    run_backtest(poisoned, bars, 8, &report);
    assert(report.total_trades == 0 && std::string(strategy_get_last_error(poisoned)) == setter_failure);
    strategy_free(poisoned);
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
