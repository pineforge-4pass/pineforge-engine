#include "native_startup.hpp"
#include "report.hpp"
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace pineforge::live;

template<class Function> Function require_symbol(void* library, const char* name) {
    auto function = dlsym(library, name);
    if (!function) throw std::runtime_error(std::string("missing oracle symbol: ") + name);
    return reinterpret_cast<Function>(function);
}

int main(int argc, char** argv) {
    if (argc != 5 && argc != 6) return 1;
    try {
        void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!library) throw std::runtime_error("cannot load oracle strategy");
        auto create = require_symbol<decltype(&strategy_create)>(library, "strategy_create");
        auto destroy = require_symbol<decltype(&strategy_free)>(library, "strategy_free");
        auto run = require_symbol<decltype(&run_backtest_full)>(library, "run_backtest_full");
        auto release = require_symbol<decltype(&report_free)>(library, "report_free");
        auto set_string = require_symbol<decltype(&strategy_set_syminfo_string)>(library, "strategy_set_syminfo_string");
        auto set_tick = require_symbol<decltype(&strategy_set_syminfo_mintick)>(library, "strategy_set_syminfo_mintick");
        auto set_point = require_symbol<decltype(&strategy_set_syminfo_pointvalue)>(library, "strategy_set_syminfo_pointvalue");
        auto set_metadata = require_symbol<decltype(&strategy_set_syminfo_metadata)>(library, "strategy_set_syminfo_metadata");
        auto set_override = require_symbol<decltype(&strategy_set_override)>(library, "strategy_set_override");
        auto retain = require_symbol<int(*)(void*)>(library, "equivalence_retain_events");
        auto export_actions = require_symbol<int(*)(void*, const char*)>(library, "equivalence_export_actions");
        auto error = require_symbol<decltype(&strategy_get_last_error)>(library, "strategy_get_last_error");
        auto state = create(nullptr);
        if (!state) throw std::runtime_error("oracle creation failed");
        const bool confirmed = argc == 6 && std::string(argv[5]) == "--confirmed";
        set_string(state, "tickerid", confirmed ? "BINANCE:ETHUSDT.P" : "TEST:EXAMPLE");
        set_string(state, "ticker", confirmed ? "ETHUSDT.P" : "EXAMPLE");
        set_tick(state, confirmed ? 0.01 : 0.25);
        set_point(state, confirmed ? 1.0 : 2.5);
        set_metadata(state, "qty_step", 0.001);
        if (confirmed) {
            set_string(state, "type", "crypto");
            set_string(state, "currency", "USDT");
            set_string(state, "basecurrency", "ETH");
        } else {
            set_override(state, "commission_value", "0.1");
            set_override(state, "slippage", "1");
        }
        if (retain(state)) throw std::runtime_error("oracle event retention failed");
        std::ifstream csv(argv[2]);
        auto bars = history(std::string(std::istreambuf_iterator<char>(csv), {}));
        pf_report_t report{};
        run(state, bars.data(), static_cast<int>(bars.size()), "1", argv[3], 0, 4,
            static_cast<pf_magnifier_distribution_t>(0), &report);
        if (const auto* message = error(state); message && *message)
            throw std::runtime_error(message);
        if (export_actions(state, argv[4])) throw std::runtime_error("oracle action export failed");
        std::cout << native_report_json(report).dump() << '\n';
        release(&report);
        destroy(state);
        dlclose(library);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
