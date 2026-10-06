#include "native_startup.hpp"
#include "report.hpp"
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>

using namespace pineforge::live;

template<class Function> Function require_symbol(void* library, const char* name) {
    auto function = dlsym(library, name);
    if (!function) throw std::runtime_error(std::string("missing oracle symbol: ") + name);
    return reinterpret_cast<Function>(function);
}

std::string read_text(const char* path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("cannot open oracle input: ") + path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

int main(int argc, char** argv) {
    if (argc < 5) return 1;
    try {
        bool confirmed = false, calendar_check = false;
        std::string input_tf = "1", stream_feed;
        int distribution = PF_MAGNIFIER_UNIFORM;
        Json config;
        for (int index = 5; index < argc; ++index) {
            const std::string option = argv[index];
            if (option == "--confirmed") { confirmed = true; continue; }
            if (option == "--calendar-check") { calendar_check = true; continue; }
            if (++index == argc) throw std::runtime_error("missing oracle option value");
            if (option == "--input-tf") input_tf = argv[index];
            else if (option == "--distribution") distribution = parse_json(argv[index]).integer<int>();
            else if (option == "--config") config = parse_json(read_text(argv[index]));
            else if (option == "--stream-feed") stream_feed = argv[index];
            else throw std::runtime_error("unknown oracle option: " + option);
        }
        if (distribution < 0 || distribution > 3) throw std::runtime_error("invalid oracle distribution");
        if (!stream_feed.empty() && distribution != PF_MAGNIFIER_ENDPOINTS)
            throw std::runtime_error("stream oracle requires ENDPOINTS (3)");
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
        if (config.kind == Json::Kind::Null) {
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
        }
        if (config.kind != Json::Kind::Null) {
            auto set_input = require_symbol<decltype(&strategy_set_input)>(library, "strategy_set_input");
            auto timezone = require_symbol<decltype(&strategy_set_syminfo_timezone)>(library, "strategy_set_syminfo_timezone");
            auto chart_timezone = require_symbol<decltype(&strategy_set_chart_timezone)>(library, "strategy_set_chart_timezone");
            auto session = require_symbol<decltype(&strategy_set_syminfo_session)>(library, "strategy_set_syminfo_session");
            timezone(state, config.at("timezone").text().c_str());
            chart_timezone(state, config.at("chart_timezone").text().c_str());
            session(state, config.at("session").text().c_str());
            const auto symbol = config.at("symbol").text();
            set_string(state, "tickerid", symbol.c_str());
            set_string(state, "ticker", symbol.substr(symbol.find(':') + 1).c_str());
            for (const auto& setting : config.at("inputs").items)
                set_input(state, setting.items.at(0).text().c_str(), setting.items.at(1).text().c_str());
            for (const auto& setting : config.at("overrides").items)
                set_override(state, setting.items.at(0).text().c_str(), setting.items.at(1).text().c_str());
            for (const auto& setting : config.at("syminfo").items) {
                const auto key = setting.items.at(0).text();
                const auto value = setting.items.at(1).text();
                if (key == "type" || key == "currency" || key == "basecurrency" ||
                    key == "description" || key == "volumetype") set_string(state, key.c_str(), value.c_str());
                else if (key == "mintick") set_tick(state, parse_json(value).real());
                else if (key == "pointvalue") set_point(state, parse_json(value).real());
                else set_metadata(state, key.c_str(), parse_json(value).real());
            }
        }
        if (retain(state)) throw std::runtime_error("oracle event retention failed");
        auto bars = history(read_text(argv[2]), input_tf != "1" || calendar_check);
        if (calendar_check) {
            NativeConfigValues clock;
            clock.input_tf = input_tf;
            clock.script_tf = argv[3];
            clock.timezone = config.at("timezone").text();
            clock.chart_timezone = config.at("chart_timezone").text();
            clock.session = config.at("session").text();
            require_native_warmup(clock, bars);
        }
        pf_report_t report{};
        auto check = [&](int result) {
            if (result) throw std::runtime_error(error(state) ? error(state) : "stream oracle refused input");
        };
        if (stream_feed.empty()) {
            run(state, bars.data(), static_cast<int>(bars.size()), input_tf.c_str(), argv[3], 0, 4,
                static_cast<pf_magnifier_distribution_t>(distribution), &report);
        } else {
            auto begin = require_symbol<decltype(&strategy_stream_begin)>(library, "strategy_stream_begin");
            auto push_bar = require_symbol<decltype(&strategy_stream_push_bar)>(library, "strategy_stream_push_bar");
            auto push_tick = require_symbol<decltype(&strategy_stream_push_tick)>(library, "strategy_stream_push_tick");
            auto advance = require_symbol<decltype(&strategy_stream_advance_time)>(library, "strategy_stream_advance_time");
            auto fill = require_symbol<decltype(&strategy_stream_fill_report)>(library, "strategy_stream_fill_report");
            check(begin(state, bars.data(), static_cast<int>(bars.size()), input_tf.c_str(), argv[3]));
            const auto consume = [&](const Json& event, const auto& self) -> void {
                const auto type = event.at("type").text();
                if (type == "batch") {
                    for (const auto& item : event.at("events").items) self(item, self);
                } else if (type == "bar") {
                    const auto& value = event.at("bar");
                    pf_bar_t bar{};
                    bar.timestamp = value.at("ts_open").integer<std::int64_t>();
                    bar.open = value.at("o").real(); bar.high = value.at("h").real();
                    bar.low = value.at("l").real(); bar.close = value.at("c").real();
                    bar.volume = value.at("v").real();
                    const auto result = push_bar(state, &bar);
                    if (result) throw std::runtime_error("confirmed chart bar " +
                        std::to_string(bar.timestamp) + ": " + error(state));
                } else if (type == "tick") {
                    pf_trade_tick_t tick{};
                    tick.timestamp = event.at("ts").integer<std::int64_t>();
                    tick.sequence = event.at("seq").integer<std::uint64_t>();
                    tick.price = event.at("price").real(); tick.quantity = event.at("qty").real();
                    check(push_tick(state, &tick));
                } else if (type == "time") check(advance(state, event.at("ts").integer<std::int64_t>()));
                else throw std::runtime_error("unknown stream oracle event");
            };
            std::ifstream feed(stream_feed);
            if (!feed) throw std::runtime_error("cannot open stream oracle feed");
            std::string line;
            while (std::getline(feed, line)) consume(parse_json(line), consume);
            if (feed.bad()) throw std::runtime_error("stream oracle feed read failed");
            check(fill(state, &report));
        }
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
