// SPDX-License-Identifier: Apache-2.0
#include "json.hpp"
#include "capabilities.hpp"
#include "deployment_identity.hpp"
#include "native_startup.hpp"
#include "store.hpp"
#include "transport.hpp"
#include "delivery.hpp"
#include "report.hpp"
#include "service.hpp"
#include <pineforge/pineforge.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <poll.h>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace {
using namespace pineforge::live;
namespace fs = std::filesystem;
std::atomic<std::sig_atomic_t> stopped{0};
static_assert(std::atomic<std::sig_atomic_t>::is_always_lock_free);
void signal_stop(int) { stopped = 1; }
constexpr std::size_t MAX_FRAME = 1024 * 1024;

struct Config {
    std::string strategy, warmup, feed = "-", ledger, webhook, mode = "", input_tf = "1", script_tf,
                                  symbol, name = "strategy";
    std::string session = "24x7", timezone = "UTC", chart_timezone = "UTC", secret_env, feed_url,
                subscribe_path, native_config, routes_path;
    std::string status_file, control_dir;
    RoutingConfig routing;
    std::vector<std::pair<std::string, std::string>> inputs, overrides, syminfo;
    std::set<std::string> explicit_flags;
    NativeConfigValues native;
    std::uint64_t from_input = 0, max_events = 0, max_attempts = 8;
    std::uint64_t status_interval = 1, max_ledger_bytes = 0, feed_idle_timeout = 15, feed_message_timeout = 15;
    long poll_ms = 1000;
    bool check = false, allow_http = false, report_jsonl = false;
};

void help() {
    std::cout << "PineForge native live runner (C++17)\n"
                 "Usage: pineforge-live run --strategy strategy.so --warmup history.csv\n"
                 "       --script-tf 15 --mode ticks|bars --ledger orders.sqlite3\n"
                 "       --symbol EXCHANGE:SYMBOL [--webhook-url https://receiver.example/events]\n"
                 "       [--feed events.jsonl|- | --feed-url https://...|wss://...]\n"
                 "Options: --input-tf 1 --name NAME --session 24x7 --timezone UTC\n"
                 "         --input TITLE=VALUE --override KEY=VALUE (repeatable)\n"
                 "         --syminfo KEY=VALUE --chart-timezone UTC\n"
                 "         --subscribe subscription.json (WebSocket only)\n"
                 "         --webhook-secret-env NAME --allow-insecure-http\n"
                 "         --webhook-routes FILE (strict per-action routing; optional webhooks)\n"
                 "         --from-input N --max-events N --max-attempts 8\n"
                 "         --native-config FILE (strict native run specification)\n"
                 "         --check (one HTTP snapshot) --poll-ms 1000\n"
                 "         --report-jsonl (mirror committed cumulative reports to stdout)\n"
                 "         --status-file PATH --status-interval 1 (seconds, 1..300)\n"
                 "         --feed-idle-timeout 15 --feed-message-timeout 15 (seconds, 1..300)\n"
                 "         --control-dir PATH --max-ledger-bytes N (0 disables budget)\n"
                 "JSONL: {\"type\":\"tick\",\"ts\":60000,\"seq\":1,\"price\":100,\"qty\":1}\n"
                 "       "
                 "{\"type\":\"bar\",\"bar\":{\"ts_open\":60000,\"o\":100,\"h\":102,\"l\":99,\"c\":"
                 "101,\"v\":4}}\n"
                 "       {\"type\":\"time\",\"ts\":120000} (tick mode only)\n"
                 "Recovery replays immutable warmup + ledger inputs before any delivery.\n"
                 "File/HTTP input defaults to the full recorded prefix; --from-input declares\n"
                 "the zero-based start of a resumed tail. HTTP polls use full snapshots.\n"
                 "Usage: pineforge-live actions --ledger L --after N [--follow] [--deployment D]\n"
                 "       pineforge-live status --ledger L [--deployment D]\n"
                 "       pineforge-live report --ledger L [--deployment D] [--at-input N]\n"
                 "       pineforge-live probe --status-file PATH --max-age S [--ready]\n"
                 "       pineforge-live redeliver --ledger L --deployment D --target T [--from N] [--failed-only]\n"
                 "Add --control-dir PATH to redeliver to request resending from a running runner.\n"
                 "Without --control-dir redeliver is offline and requires the ledger lock.\n";
}
std::uint64_t unsigned_arg(const std::string &s) {
    return Json::number(s).integer<std::uint64_t>();
}
std::uint64_t seconds_arg(const std::string& value) {
    const auto seconds = unsigned_arg(value);
    if (!seconds || seconds > 300) throw std::runtime_error("timeout/interval must be 1..300 seconds");
    return seconds;
}
void validate_script_tf(const std::string &tf) {
    std::string digits = tf;
    std::uint64_t seconds = 60;
    if (!tf.empty() && (tf.back() == 'D' || tf.back() == 'W')) {
        seconds = tf.back() == 'D' ? 86400 : 604800;
        digits.pop_back();
        if (digits.empty())
            digits = "1";
    }
    auto count = unsigned_arg(digits);
    if (!count || count > static_cast<std::uint64_t>(INT_MAX) / seconds)
        throw std::runtime_error("script timeframe exceeds native integer range");
}
std::pair<std::string, std::string> pair_arg(const std::string &s) {
    auto n = s.find('=');
    if (n == 0 || n == std::string::npos)
        throw std::runtime_error("expected KEY=VALUE");
    return {s.substr(0, n), s.substr(n + 1)};
}
Config args(int argc, char **argv) {
    Config c;
    if (argc < 2 || std::string(argv[1]) != "run")
        throw std::runtime_error("expected run; use --help");
    std::set<std::string> seen;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--parser" || a == "--parser-config")
            throw std::runtime_error("in-runner parsers were removed; use an external feed adapter (runner/README.md#feed-format)");
        if (a != "--input" && a != "--override" && a != "--syminfo" && !seen.insert(a).second)
            throw std::runtime_error("duplicate option: " + a);
        c.explicit_flags.insert(a);
        if (a == "--report-jsonl") {
            c.report_jsonl = true;
            continue;
        }
        if (a == "--check") {
            c.check = true;
            continue;
        }
        if (a == "--allow-insecure-http") {
            c.allow_http = true;
            continue;
        }
        if (i + 1 == argc)
            throw std::runtime_error("missing option value: " + a);
        std::string v = argv[++i];
        if (a == "--strategy")
            c.strategy = v;
        else if (a == "--warmup")
            c.warmup = v;
        else if (a == "--feed")
            c.feed = v;
        else if (a == "--feed-url")
            c.feed_url = v;
        else if (a == "--ledger")
            c.ledger = v;
        else if (a == "--webhook-url")
            c.webhook = v;
        else if (a == "--webhook-routes")
            c.routes_path = v;
        else if (a == "--mode")
            c.mode = v;
        else if (a == "--input-tf")
            c.input_tf = v;
        else if (a == "--script-tf")
            c.script_tf = v;
        else if (a == "--symbol")
            c.symbol = v;
        else if (a == "--name")
            c.name = v;
        else if (a == "--session")
            c.session = v;
        else if (a == "--timezone")
            c.timezone = v;
        else if (a == "--webhook-secret-env")
            c.secret_env = v;
        else if (a == "--chart-timezone")
            c.chart_timezone = v;
        else if (a == "--syminfo")
            c.syminfo.push_back(pair_arg(v));
        else if (a == "--subscribe")
            c.subscribe_path = v;
        else if (a == "--input")
            c.inputs.push_back(pair_arg(v));
        else if (a == "--override")
            c.overrides.push_back(pair_arg(v));
        else if (a == "--from-input")
            c.from_input = unsigned_arg(v);
        else if (a == "--max-events")
            c.max_events = unsigned_arg(v);
        else if (a == "--max-attempts")
            c.max_attempts = unsigned_arg(v);
        else if (a == "--native-config")
            c.native_config = v;
        else if (a == "--status-file") c.status_file = v;
        else if (a == "--control-dir") c.control_dir = v;
        else if (a == "--status-interval") c.status_interval = seconds_arg(v);
        else if (a == "--feed-idle-timeout") c.feed_idle_timeout = seconds_arg(v);
        else if (a == "--feed-message-timeout") c.feed_message_timeout = seconds_arg(v);
        else if (a == "--max-ledger-bytes") {
            c.max_ledger_bytes = unsigned_arg(v);
            if (c.max_ledger_bytes > static_cast<std::uint64_t>(INT64_MAX))
                throw std::runtime_error("ledger budget exceeds supported range");
        }
        else if (a == "--poll-ms") {
            auto n = unsigned_arg(v);
            if (n < 100 || n > 3600000)
                throw std::runtime_error("poll-ms out of range");
            c.poll_ms = static_cast<long>(n);
        } else
            throw std::runtime_error("unknown option: " + a);
    }
    if (c.strategy.empty() || c.warmup.empty() || c.ledger.empty())
        throw std::runtime_error("strategy, warmup and ledger are required");
    if (c.mode != "bars" && c.mode != "ticks")
        throw std::runtime_error("mode must be bars or ticks");
    if (!c.native_config.empty()) {
        if (!c.inputs.empty() || !c.overrides.empty() || !c.syminfo.empty())
            throw std::runtime_error("native-config refuses --input, --override and --syminfo");
    } else {
        if (c.symbol.empty() || c.script_tf.empty())
            throw std::runtime_error("symbol and script-tf are required");
        if (c.input_tf != "1")
            throw std::runtime_error("native runner input-tf currently must be 1 minute");
        validate_script_tf(c.script_tf);
        if (c.chart_timezone.empty())
            c.chart_timezone = "UTC";
    }
    if (!c.feed_url.empty() && seen.count("--feed"))
        throw std::runtime_error("choose feed or feed-url");
    const bool websocket = c.feed_url.rfind("ws://", 0) == 0 || c.feed_url.rfind("wss://", 0) == 0;
    if (!c.feed_url.empty() && !websocket && c.from_input)
        throw std::runtime_error("HTTP snapshots require from-input 0");
    if (!c.subscribe_path.empty() && !websocket)
        throw std::runtime_error("subscribe requires WebSocket feed-url");
    if (!c.max_attempts || c.max_attempts > 1000)
        throw std::runtime_error("max-attempts must be 1..1000");
    if (!c.allow_http && (c.webhook.rfind("http://", 0) == 0 ||
                          c.feed_url.rfind("http://", 0) == 0 || c.feed_url.rfind("ws://", 0) == 0))
        throw std::runtime_error("HTTP/WS requires --allow-insecure-http; use TLS otherwise");
    for (const auto &list : {c.inputs, c.overrides, c.syminfo}) {
        std::set<std::string> keys;
        for (const auto &[k, v] : list)
            if (!keys.insert(k).second)
                throw std::runtime_error("duplicate strategy setting: " + k);
    }
    // These semantics are not supplied by the existing native stream engine.
    for (const auto &[k, v] : c.overrides)
        if ((k == "calc_on_every_tick" || k == "calc_on_order_fills") && v != "false" && v != "0")
            throw std::runtime_error("native runner supports close-only strategy calculation");
    auto ledger = fs::weakly_canonical(fs::absolute(c.ledger));
    if (!c.status_file.empty()) {
        const auto status = fs::weakly_canonical(fs::absolute(c.status_file));
        for (const auto& protected_path : {c.ledger, c.ledger + "-wal", c.ledger + "-shm", c.ledger + ".lock",
                c.strategy, c.warmup, c.feed == "-" ? std::string{} : c.feed, c.subscribe_path, c.routes_path, c.native_config})
            if (!protected_path.empty() && status == fs::weakly_canonical(fs::absolute(protected_path)))
                throw std::runtime_error("status-file must not replace a ledger or input artifact");
    }
    for (const auto &src : {c.strategy, c.warmup, c.feed == "-" ? std::string{} : c.feed,
                            c.subscribe_path, c.routes_path, c.native_config})
        if (!src.empty()) {
            auto path = fs::weakly_canonical(fs::absolute(src));
            for (const auto &suffix : {"", "-wal", "-shm", ".lock"})
                if (path == fs::path(ledger.string() + suffix))
                    throw std::runtime_error("ledger/sidecar overlaps an input artifact");
        }
    return c;
}
std::string read_file(const std::string &path, std::size_t limit) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw std::runtime_error("cannot read artifact: " + path);
    std::string out;
    char buffer[65536];
    while (in) {
        in.read(buffer, sizeof buffer);
        out.append(buffer, static_cast<std::size_t>(in.gcount()));
        if (out.size() > limit)
            throw std::runtime_error("artifact exceeds size limit");
    }
    if (!in.eof())
        throw std::runtime_error("artifact read failed");
    return out;
}
bool line(std::istream &in, std::string &out) {
    out.clear();
    char c;
    while (in.get(c)) {
        if (c == '\n')
            return true;
        out += c;
        if (out.size() > MAX_FRAME)
            throw std::runtime_error("input line exceeds 1 MiB");
    }
    if (in.bad())
        throw std::runtime_error("feed read failed");
    return !out.empty();
}
bool blank(const std::string &s) { return s.find_first_not_of(" \t\r\n") == std::string::npos; }

class Strategy {
    void *library_ = nullptr;
    decltype(&strategy_stream_begin) begin_ = nullptr;
    decltype(&strategy_configure_native_v1) configure_native_ = nullptr;
    decltype(&strategy_set_input_checked) set_input_checked_ = nullptr;
    decltype(&strategy_set_override_checked) set_override_checked_ = nullptr;
    decltype(&strategy_get_effective_settings) settings_receipt_ = nullptr;
    decltype(&strategy_capabilities_receipt) capabilities_receipt_ = nullptr;
    decltype(&strategy_capabilities_receipt) confirmed_bar_receipt_ = nullptr;
    template <class T> T symbol(const char *name) {
        auto p = dlsym(library_, name);
        if (!p)
            throw std::runtime_error(std::string("compiled strategy lacks native ABI symbol: ") +
                                     name);
        return reinterpret_cast<T>(p);
    }
    template <class T> T optional_symbol(const char *name) {
        return reinterpret_cast<T>(dlsym(library_, name));
    }
    void release() {
        if (state && free)
            free(state);
        state = nullptr;
        if (library_)
            dlclose(library_);
        library_ = nullptr;
    }

  public:
    pf_strategy_t state = nullptr;
    decltype(&strategy_free) free = nullptr;
    decltype(&strategy_get_last_error) error = nullptr;
    decltype(&strategy_stream_push_tick) tick = nullptr;
    decltype(&strategy_stream_push_bar) bar = nullptr;
    decltype(&strategy_stream_advance_time) advance = nullptr;
    decltype(&strategy_stream_order_actions_len) count = nullptr;
    decltype(&strategy_stream_order_action_get) get = nullptr;
    decltype(&strategy_stream_order_actions_clear) clear = nullptr;
    decltype(&strategy_stream_state_hash) hash = nullptr;
    decltype(&strategy_stream_fill_report) fill_report = nullptr;
    decltype(&report_free) free_report = nullptr;
    int contract = 1;
    bool has_configure_native = false;
    Strategy() = default;
    ~Strategy() { release(); }
    Strategy(const Strategy &) = delete;
    Strategy &operator=(const Strategy &) = delete;
    void check(int rc) {
        if (rc != 0) {
            const char *e = error ? error(state) : nullptr;
            throw std::runtime_error(std::string("native stream refused: ") +
                                     (e ? e : "unknown error"));
        }
    }
    void load(const std::string &path) {
        library_ = dlopen(fs::absolute(path).c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library_)
            throw std::runtime_error("cannot load compiled strategy library");
        try {
            auto abi = symbol<decltype(&pf_abi_version)>("pf_abi_version");
            if (abi() != PF_ABI_VERSION)
                throw std::runtime_error("strategy ABI mismatch");
            if (symbol<decltype(&strategy_stream_api_version)>("strategy_stream_api_version")() !=
                1)
                throw std::runtime_error("native stream extension version mismatch");
            free = symbol<decltype(&strategy_free)>("strategy_free");
            error = symbol<decltype(error)>("strategy_get_last_error");
            tick = symbol<decltype(tick)>("strategy_stream_push_tick");
            bar = symbol<decltype(bar)>("strategy_stream_push_bar");
            advance = symbol<decltype(advance)>("strategy_stream_advance_time");
            count = symbol<decltype(count)>("strategy_stream_order_actions_len");
            get = symbol<decltype(get)>("strategy_stream_order_action_get");
            clear = symbol<decltype(clear)>("strategy_stream_order_actions_clear");
            hash = symbol<decltype(hash)>("strategy_stream_state_hash");
            fill_report = symbol<decltype(fill_report)>("strategy_stream_fill_report");
            free_report = optional_symbol<decltype(free_report)>("report_free");
            if (!free_report)
                free_report = symbol<decltype(free_report)>("strategy_native_report_free_v1");
            begin_ = symbol<decltype(&strategy_stream_begin)>("strategy_stream_begin");
            auto capabilities_version = optional_symbol<decltype(&strategy_capabilities_api_version)>(
                "strategy_capabilities_api_version");
            if (capabilities_version) {
                if (capabilities_version() != PF_CAPABILITIES_API_VERSION)
                    throw std::runtime_error("capabilities extension version mismatch");
                capabilities_receipt_ = symbol<decltype(capabilities_receipt_)>(
                    "strategy_capabilities_receipt");
                auto confirmed_version = optional_symbol<decltype(&strategy_capabilities_api_version)>(
                    "strategy_confirmed_bar_api_version");
                if (confirmed_version) {
                    if (confirmed_version() != 1u)
                        throw std::runtime_error("confirmed-bar capabilities extension version mismatch");
                    confirmed_bar_receipt_ = symbol<decltype(confirmed_bar_receipt_)>(
                        "strategy_confirmed_bar_receipt");
                } else if (optional_symbol<decltype(confirmed_bar_receipt_)>("strategy_confirmed_bar_receipt")) {
                    throw std::runtime_error("confirmed-bar capabilities extension lacks version");
                }
            } else {
                std::cerr << "pineforge-live: warning: compiled strategy lacks execution capabilities; "
                             "close-only eligibility cannot be proved (legacy behavior retained)\n";
            }
            auto settings_version = optional_symbol<decltype(&strategy_settings_api_version)>(
                "strategy_settings_api_version");
            if (settings_version) {
                if (settings_version() != PF_SETTINGS_API_VERSION)
                    throw std::runtime_error("settings extension version mismatch");
                set_input_checked_ = symbol<decltype(set_input_checked_)>("strategy_set_input_checked");
                set_override_checked_ = symbol<decltype(set_override_checked_)>("strategy_set_override_checked");
                settings_receipt_ = symbol<decltype(settings_receipt_)>("strategy_get_effective_settings");
                char message[512]{};
                auto create = symbol<decltype(&strategy_create_checked)>("strategy_create_checked");
                if (create(nullptr, &state, message, sizeof(message)) != PF_SETTINGS_OK)
                    throw std::runtime_error(std::string("strategy creation failed: ") + message);
            } else {
                std::cerr << "pineforge-live: warning: compiled strategy lacks checked settings; "
                             "legacy settings may be ignored or defaulted\n";
                state = symbol<decltype(&strategy_create)>("strategy_create")(nullptr);
            }
            if (!state)
                throw std::runtime_error("strategy creation failed");
            auto contract_fn = optional_symbol<decltype(&strategy_execution_contract)>(
                "strategy_execution_contract");
            contract = 1;
            if (contract_fn) {
                contract = contract_fn(state);
                if (contract != 1 && contract != 2)
                    throw std::runtime_error("unknown strategy execution contract");
            }
            configure_native_ = optional_symbol<decltype(&strategy_configure_native_v1)>(
                "strategy_configure_native_v1");
            has_configure_native = configure_native_ != nullptr;
        } catch (...) {
            release();
            throw;
        }
    }
    std::string effective_settings() const {
        if (!settings_receipt_)
            return {};
        char message[512]{};
        std::size_t required = 0;
        if (settings_receipt_(state, nullptr, 0, &required, message, sizeof(message)) !=
                PF_SETTINGS_BUFFER_TOO_SMALL || required == 0 || required > MAX_FRAME)
            throw std::runtime_error(std::string("settings receipt refused: ") + message);
        std::vector<char> receipt(required);
        if (settings_receipt_(state, receipt.data(), receipt.size(), &required, message,
                              sizeof(message)) != PF_SETTINGS_OK)
            throw std::runtime_error(std::string("settings receipt refused: ") + message);
        std::string document(receipt.data());
        parse_json(document);
        return document;
    }
    std::string capabilities(bool confirmed = false) const {
        auto receipt_function = confirmed ? confirmed_bar_receipt_ : capabilities_receipt_;
        if (!receipt_function)
            return {};
        char message[512]{};
        std::size_t required = 0;
        if (receipt_function(state, nullptr, 0, &required, message, sizeof(message)) !=
                PF_SETTINGS_BUFFER_TOO_SMALL || required == 0 || required > MAX_FRAME)
            throw std::runtime_error(std::string("capabilities receipt refused: ") + message);
        std::vector<char> receipt(required);
        const auto capacity = receipt.size();
        if (receipt_function(state, receipt.data(), capacity, &required, message,
                                  sizeof(message)) != PF_SETTINGS_OK || required != capacity ||
                receipt.back() != '\0' || std::strlen(receipt.data()) + 1 != capacity)
            throw std::runtime_error(std::string("capabilities receipt refused: ") + message);
        return std::string(receipt.data(), capacity - 1);
    }
    void require_contract(const Config &c) const {
        if (c.native.present) {
            if (contract != 2)
                throw std::runtime_error("native-config requires NativeMarketV1");
            if (!has_configure_native)
                throw std::runtime_error(
                    "compiled strategy lacks native ABI symbol: strategy_configure_native_v1");
        }
    }
    void configure(const Config &c) {
        if (c.native.present) {
            pf_native_run_spec_v1 spec{};
            spec.struct_size = sizeof(spec);
            spec.session_key = c.native.session_key.c_str();
            spec.run_number = c.native.run_number;
            spec.input_tf = c.native.input_tf.c_str();
            spec.script_tf = c.native.script_tf.c_str();
            spec.ticker = c.native.ticker.c_str();
            spec.tickerid = c.native.tickerid.c_str();
            spec.type = c.native.type.c_str();
            spec.currency = c.native.currency.c_str();
            spec.basecurrency = c.native.basecurrency.c_str();
            spec.description = c.native.description.c_str();
            spec.volumetype = c.native.volumetype.c_str();
            spec.timezone = c.native.timezone.c_str();
            spec.session = c.native.session.c_str();
            spec.chart_timezone = c.native.chart_timezone.c_str();
            spec.initial_capital = c.native.initial_capital;
            spec.point_value = c.native.point_value;
            spec.account_fx = c.native.account_fx;
            spec.price_tick = c.native.price_tick;
            spec.slippage_ticks = c.native.slippage_ticks;
            spec.fee_kind = c.native.fee_kind;
            spec.fee_value = c.native.fee_value;
            spec.optional_mask = c.native.optional_mask;
            spec.close_execution = c.native.close_execution;
            spec.allowed_open_directions = c.native.allowed_open_directions;
            spec.quantity_grid = c.native.quantity_grid;
            spec.max_abs_units = c.native.max_abs_units;
            spec.initial_margin_fraction = c.native.initial_margin_fraction;
            spec.max_open_lots = c.native.max_open_lots;
            if (configure_native_(state, &spec) != 0) {
                const char *e = error(state);
                throw std::runtime_error(std::string("native configure refused: ") +
                                         (e ? e : "unknown error"));
            }
            return;
        }
        auto set_input = symbol<decltype(&strategy_set_input)>("strategy_set_input");
        auto set_override = symbol<decltype(&strategy_set_override)>("strategy_set_override");
        for (const auto &[key, value] : c.inputs) {
            if (set_input_checked_) {
                char message[512]{};
                if (set_input_checked_(state, key.c_str(), value.c_str(), message,
                                       sizeof(message)) != PF_SETTINGS_OK)
                    throw std::runtime_error("input '" + key + "' refused: " + message);
            } else {
                set_input(state, key.c_str(), value.c_str());
            }
        }
        for (const auto &[key, value] : c.overrides) {
            if (set_override_checked_) {
                char message[512]{};
                if (set_override_checked_(state, key.c_str(), value.c_str(), message,
                                          sizeof(message)) != PF_SETTINGS_OK)
                    throw std::runtime_error("override '" + key + "' refused: " + message);
            } else {
                set_override(state, key.c_str(), value.c_str());
            }
        }
        symbol<decltype(&strategy_set_syminfo_timezone)>("strategy_set_syminfo_timezone")(
            state, c.timezone.c_str());
        symbol<decltype(&strategy_set_chart_timezone)>("strategy_set_chart_timezone")(
            state, c.chart_timezone.c_str());
        symbol<decltype(&strategy_set_syminfo_session)>("strategy_set_syminfo_session")(
            state, c.session.c_str());
        auto set_string =
            symbol<decltype(&strategy_set_syminfo_string)>("strategy_set_syminfo_string");
        if (set_string(state, "tickerid", c.symbol.c_str()) != 0)
            throw std::runtime_error("strategy rejected tickerid");
        auto ticker = c.symbol.substr(
            c.symbol.find(':') == std::string::npos ? 0 : c.symbol.find(':') + 1);
        if (set_string(state, "ticker", ticker.c_str()) != 0)
            throw std::runtime_error("strategy rejected ticker");
        for (const auto &[key, value] : c.syminfo) {
            if (key == "type" || key == "currency" || key == "basecurrency" ||
                key == "description" || key == "volumetype") {
                if (set_string(state, key.c_str(), value.c_str()) != 0)
                    throw std::runtime_error("strategy rejected string metadata");
            } else {
                double v = parse_json(value).real();
                if (key == "mintick" || key == "pointvalue" || key == "qty_step") {
                    if (v <= 0)
                        throw std::runtime_error(
                            "symbol tick/point/quantity steps must be positive");
                } else if (key == "margin_long" || key == "margin_short") {
                    if (v < 0)
                        throw std::runtime_error("symbol margins must be nonnegative");
                } else
                    throw std::runtime_error("unsupported syminfo key: " + key);
                if (key == "mintick")
                    symbol<decltype(&strategy_set_syminfo_mintick)>("strategy_set_syminfo_mintick")(
                        state, v);
                else if (key == "pointvalue")
                    symbol<decltype(&strategy_set_syminfo_pointvalue)>(
                        "strategy_set_syminfo_pointvalue")(state, v);
                else
                    symbol<decltype(&strategy_set_syminfo_metadata)>(
                        "strategy_set_syminfo_metadata")(state, key.c_str(), v);
            }
        }
    }
    void begin(const Config &c, const std::vector<pf_bar_t> &warmup) {
        check(begin_(state, warmup.data(), static_cast<int>(warmup.size()), c.input_tf.c_str(),
                     c.script_tf.c_str()));
        clear(state);
    }
};

struct Cursor {
    std::uint64_t tick_seq = 0;
    bool seen_tick = false;
};
struct InputGap : std::runtime_error { using std::runtime_error::runtime_error; };
void apply(Strategy &s, const Config &c, Cursor &cursor, const Json &frame) {
    auto type = frame.at("type").text();
    if (type == "tick") {
        only_fields(frame, {"type", "ts", "seq", "price", "qty"});
        if (c.mode != "ticks")
            throw std::runtime_error("bars mode refuses ticks");
        pf_trade_tick_t t{};
        t.timestamp = frame.at("ts").integer<std::int64_t>();
        t.sequence = frame.at("seq").integer<std::uint64_t>();
        t.price = frame.at("price").real();
        t.quantity = frame.at("qty").real();
        if (t.timestamp < 0 || !t.sequence || t.quantity <= 0 || t.price <= 0)
            throw std::runtime_error("invalid trade tick");
        if (cursor.seen_tick &&
            (cursor.tick_seq == UINT64_MAX || t.sequence != cursor.tick_seq + 1))
            throw InputGap("tick sequence gap or regression");
        s.check(s.tick(s.state, &t));
        cursor.tick_seq = t.sequence;
        cursor.seen_tick = true;
    } else if (type == "bar") {
        only_fields(frame, {"type", "bar"});
        if (c.mode != "bars")
            throw std::runtime_error("ticks mode uses trade ticks and explicit time boundaries");
        const auto &j = frame.at("bar");
        only_fields(j, {"ts_open", "o", "h", "l", "c", "v", "trade_count"});
        if (j.find("trade_count"))
            j.at("trade_count").integer<std::uint64_t>();
        pf_bar_t b{};
        b.timestamp = j.at("ts_open").integer<std::int64_t>();
        b.open = j.at("o").real();
        b.high = j.at("h").real();
        b.low = j.at("l").real();
        b.close = j.at("c").real();
        b.volume = j.at("v").real();
        s.check(s.bar(s.state, &b));
    } else if (type == "time") {
        only_fields(frame, {"type", "ts"});
        if (c.mode != "ticks")
            throw std::runtime_error("time boundaries are for tick mode");
        auto ts = frame.at("ts").integer<std::int64_t>();
        if (ts < 0)
            throw std::runtime_error("negative time boundary");
        s.check(s.advance(s.state, ts));
    } else
        throw std::runtime_error("unknown input event type");
}
Json num(std::uint64_t value) { return Json::number(std::to_string(value)); }
Json real(double value) {
    if (!std::isfinite(value))
        throw std::runtime_error("nonfinite native action");
    std::ostringstream s;
    s.imbue(std::locale::classic());
    s << std::setprecision(17) << value;
    return Json::number(s.str());
}
std::string cumulative_report(Strategy& strategy, const std::string& deployment,
                              std::uint64_t cursor, ReportDeltas& deltas) {
    pf_report_t report{};
    struct ReleaseReport {
        Strategy& strategy;
        pf_report_t& report;
        ~ReleaseReport() { strategy.free_report(&report); }
    } release{strategy, report};
    strategy.check(strategy.fill_report(strategy.state, &report));
    return deltas.update(report, deployment, cursor, strategy.hash(strategy.state));
}
std::vector<Event> actions(Strategy &s, const Config &c, const std::string &deployment) {
    std::vector<Event> out;
    int n = s.count(s.state);
    if (n < 0 || n > 1000000)
        throw std::runtime_error("invalid native action count");
    for (int i = 0; i < n; ++i) {
        pf_stream_order_action_t a{};
        s.check(s.get(s.state, i, &a));
        if (!a.sequence || a.quantity <= 0 || a.timestamp_ms < 0)
            throw std::runtime_error("invalid native order action");
        std::string id = sha256_hex(deployment + ":" + std::to_string(a.sequence));
        Json payload = Json::object(
            {{"schema_version", Json::string("pineforge-native-order-action/v1")},
             {"event", Json::string("order_action")},
             {"event_id", Json::string(id)},
             {"deployment", Json::string(deployment)},
             {"strategy", Json::string(c.name)},
             {"symbol", Json::string(c.symbol)},
             {"timeframe", Json::string(c.script_tf)},
             {"sequence", num(a.sequence)},
             {"timestamp", Json::number(std::to_string(a.timestamp_ms))},
             {"bar_index", Json::number(std::to_string(a.bar_index))},
             {"order",
              Json::object(
                  {{"id", Json::string(a.order_id ? a.order_id : "")},
                   {"comment", Json::string(a.comment ? a.comment : "")},
                   {"action", Json::string((a.is_entry ? a.is_long : !a.is_long) ? "buy" : "sell")},
                   {"leg", Json::string(a.is_entry ? "entry" : "exit")},
                   {"contracts", real(a.quantity)},
                   {"price", real(a.price)},
                   {"reduce_only", Json::boolean(!a.is_entry)},
                   {"entry_incarnation", num(a.entry_incarnation)}})}});
        const std::string kind = a.is_entry ? "entry" : "exit";
        const std::string side = a.is_long ? "long" : "short";
        const auto target = c.routing.select(a.order_id ? a.order_id : "", kind, side);
        const auto delivery_id = c.routing.routed ? delivery_identity(id, target) : id;
        if (c.routing.routed) {
            payload.members["schema_version"] = Json::string("pineforge-native-order-action/v2");
            payload.members["target_id"] = target ? Json::string(*target) : Json{};
            payload.members["delivery_id"] = Json::string(delivery_id);
            payload.members["order"].members["kind"] = Json::string(kind);
            payload.members["order"].members["side"] = Json::string(side);
        }
        out.push_back({std::move(id), payload.dump(), target, delivery_id});
    }
    return out;
}
void apply_record(Strategy &strategy, const Config &c, Cursor &cursor, const Json &record) {
    if (record.at("type").text() != "batch") {
        apply(strategy, c, cursor, record);
        return;
    }
    only_fields(record, {"type", "events"});
    const auto &events = record.at("events");
    if (events.kind != Json::Kind::Array || events.items.empty() || events.items.size() > 1024)
        throw std::runtime_error("normalized batch requires 1..1024 events");
    for (const auto &event : events.items)
        apply(strategy, c, cursor, event);
}
void require_feed_event(const Json &event) {
    const auto type = event.at("type").text();
    if (type == "tick") {
        only_fields(event, {"type", "ts", "seq", "price", "qty"});
        event.at("ts").integer<std::int64_t>();
        event.at("seq").integer<std::uint64_t>();
        event.at("price").real();
        event.at("qty").real();
    } else if (type == "time") {
        only_fields(event, {"type", "ts"});
        event.at("ts").integer<std::int64_t>();
    } else if (type == "bar") {
        only_fields(event, {"type", "bar"});
        const auto &bar = event.at("bar");
        only_fields(bar, {"ts_open", "o", "h", "l", "c", "v", "trade_count"});
        if (bar.find("trade_count"))
            bar.at("trade_count").integer<std::uint64_t>();
        bar.at("ts_open").integer<std::int64_t>();
        for (const auto *field : {"o", "h", "l", "c", "v"})
            bar.at(field).real();
    } else
        throw std::runtime_error("expected tick, time or confirmed bar event");
}
Json feed_record(const std::string &message) {
    try {
        auto record = parse_json(message);
        if (record.kind == Json::Kind::Array)
            record = Json::object({{"type", Json::string("batch")}, {"events", std::move(record)}});
        if (record.at("type").text() == "batch") {
            only_fields(record, {"type", "events"});
            const auto &events = record.at("events");
            if (events.kind != Json::Kind::Array || events.items.empty() || events.items.size() > 1024)
                throw std::runtime_error("normalized batch requires 1..1024 events");
            for (const auto &event : events.items)
                require_feed_event(event);
        } else
            require_feed_event(record);
        return record;
    } catch (const std::exception &error) {
        throw std::runtime_error(
            std::string("invalid feed message: ") + error.what() +
            "; PineForge feed events required; use an external feed adapter (runner/README.md#feed-format)");
    }
}
LegacyIdentityFields legacy_fields(const Config &c) {
    return {c.mode,     c.input_tf, c.script_tf, c.session, c.timezone, c.chart_timezone,
            c.symbol,   c.name,     c.webhook,   c.inputs,  c.overrides, c.syminfo};
}

int run(Config c) {
    c.routing = c.routes_path.empty()
        ? single_target(c.webhook, c.secret_env, c.allow_http)
        : parse_routes(read_file(c.routes_path, MAX_FRAME), c.allow_http, c.webhook, c.secret_env);
    auto targets = c.routing.load_secrets();
    if (c.routing.routed)
        c.webhook = c.routing.default_target ? c.routing.targets.at(*c.routing.default_target).url : "";
    else if (c.explicit_flags.count("--max-attempts"))
        c.routing.delivery.transport_retries = static_cast<unsigned>(std::min<std::uint64_t>(2, c.max_attempts - 1));
    if (c.feed_url.rfind("ws://", 0) == 0 || c.feed_url.rfind("wss://", 0) == 0) {
        HttpOptions feed;
        feed.url = c.feed_url;
        feed.allow_insecure_http = c.allow_http;
        validate_websocket(feed);
    }
    if (!c.native_config.empty()) {
        c.native = parse_native_config(read_file(c.native_config, MAX_FRAME));
        NativeClockBindings clock{c.input_tf, c.script_tf, c.timezone, c.session,
                                  c.chart_timezone, c.symbol, c.explicit_flags};
        apply_native_config(clock, c.native);
        c.input_tf = clock.input_tf;
        c.script_tf = clock.script_tf;
        c.timezone = clock.timezone;
        c.session = clock.session;
        c.chart_timezone = clock.chart_timezone;
        c.symbol = clock.symbol;
        validate_native_config(c.native);
        if (c.input_tf != "1")
            throw std::runtime_error("native runner input-tf currently must be 1 minute");
    }
    auto original = read_file(c.warmup, 512ULL * 1024 * 1024);
    auto library = read_file(c.strategy, 512ULL * 1024 * 1024);
    Strategy strategy;
    strategy.load(c.strategy);
    if (sha256_hex(read_file(c.strategy, 512ULL * 1024 * 1024)) != sha256_hex(library))
        throw std::runtime_error("strategy library changed during initialization");
    strategy.require_contract(c);
    auto warmup = history(original, c.native.present);
    strategy.configure(c);
    const auto settings_receipt = strategy.effective_settings();
    const auto capabilities_receipt = strategy.capabilities();
    const auto confirmed_bar_receipt = strategy.capabilities(true);
    if (!capabilities_receipt.empty()) {
        require_close_only_capabilities(capabilities_receipt, confirmed_bar_receipt,
                                       c.mode, c.input_tf, c.script_tf,
                                       !c.native.present && c.session == "24x7" &&
                                       c.timezone == "UTC" && c.chart_timezone == "UTC");
        for (const auto& [name, value] : c.overrides)
            require_close_only_boolean(name, value == "true" || value == "1");
    }
    std::string deployment =
        c.native.present
            ? native_identity(c.native, c.mode, c.name, c.webhook, original, library)
            : identity(legacy_fields(c), original, library);
    deployment = bind_deployment_identity(deployment, settings_receipt, capabilities_receipt,
                                          c.routing.routed, c.routing.file_identity, confirmed_bar_receipt);
    try {
        // A switched PineStrategyHost is native-bound but owns its run spec
        // through prepare_native_begin.  Let that provider admit the stream
        // when the CLI uses ordinary source settings; strict native modules
        // still refuse at begin if no external configuration was supplied.
        strategy.begin(c, warmup);
    } catch (const std::runtime_error& error) {
        const std::string text = error.what();
        if (!c.native.present
            && text.find("native stream_begin requires Ready") != std::string::npos) {
            throw std::runtime_error("native strategy requires --native-config");
        }
        if (c.native.present
            && (text.find("Pine native adapter failed to configure projected run spec")
                    != std::string::npos
                || text.find("configure refused while ready") != std::string::npos
                || text.find("native host already failed") != std::string::npos)) {
            throw std::runtime_error("native-config requires NativeMarketV1");
        }
        throw;
    }
    if (c.native.present)
        require_native_warmup(c.native, warmup);
    ControlDirectory controls(c.control_dir);
    Ledger ledger(c.ledger, deployment);
    ServiceFile service(c.status_file, c.status_interval * 1000);
    ledger.bind_routing(c.routing.stored_document());
    ReportDeltas report_deltas;
    ledger.verify_report(0, cumulative_report(strategy, deployment, 0, report_deltas), [&] { return report_deltas.json(); });
    Cursor cursor;
    auto recorded = ledger.input_count();
    auto recovering = Json::object({{"deployment", Json::string(deployment)}, {"state", Json::string("recovering")},
        {"ready", Json::boolean(false)}, {"readiness", Json::object({
            {"validated_strategy_warmup", Json::boolean(true)}, {"recovered_ledger", Json::boolean(false)},
            {"verified_source_prefix", Json::boolean(false)}, {"no_unhealed_input_gap", Json::boolean(true)},
            {"storage_below_budget", Json::boolean(!c.max_ledger_bytes || ledger_bytes(c.ledger) <= c.max_ledger_bytes)}})},
        {"metrics", Json::object({{"committed_input", num(recorded)}, {"last_seq", Json{}},
            {"source_timestamp_ms", Json{}}, {"source_lag_ms", Json{}}, {"queue_bytes", num(0)},
            {"ledger_bytes", num(ledger_bytes(c.ledger))}, {"report_cursor", num(ledger.report_cursor())}, {"targets", Json::object({})}})}});
    service.publish(recovering, true);
    for (std::uint64_t i = 0; i < recorded; ++i) {
        if (stopped)
            return 0;
        auto row = ledger.input(i);
        if (!row)
            throw std::runtime_error("ledger input hole");
        apply_record(strategy, c, cursor, parse_json(row->canonical_json));
        auto events = actions(strategy, c, deployment);
        if (row->state_hash != std::to_string(strategy.hash(strategy.state)) ||
            events.size() != row->events.size())
            throw std::runtime_error("native replay state/action count mismatch");
        for (std::size_t k = 0; k < events.size(); ++k)
            if (events[k].id != row->events[k].id || events[k].payload != row->events[k].payload ||
                events[k].target_id != row->events[k].target_id || events[k].delivery_id != row->events[k].delivery_id)
                throw std::runtime_error("native replay order-action mismatch");
        ledger.verify_report(i + 1, cumulative_report(strategy, deployment, i + 1, report_deltas), [&] { return report_deltas.json(); });
        strategy.clear(strategy.state);
        recovering.members["metrics"].members["report_cursor"] = num(ledger.report_cursor());
        service.publish(recovering);
    }
    if (c.from_input > recorded)
        throw std::runtime_error("from-input skips unrecorded inputs");
    std::uint64_t processed = 0, replayed_prefix = 0;
    DeliveryWorker delivery(ledger, c.routing.delivery, std::move(targets), std::nullopt,
                            [] { return stopped != 0; }, c.control_dir, deployment);
    bool storage_stop = false;
    bool prefix_verified = recorded == 0 || c.from_input == recorded;
    std::uint64_t source_timestamp = static_cast<std::uint64_t>(warmup.back().timestamp + 60000);
    std::uint64_t intake_bytes = 0;
    const auto source_time = [&](const Json& record, const auto& self) -> std::uint64_t {
        const auto type = record.at("type").text();
        if (type == "batch") {
            std::uint64_t newest = 0;
            for (const auto& event : record.at("events").items) newest = std::max(newest, self(event, self));
            return newest;
        }
        if (type == "bar") return record.at("bar").at("ts_open").integer<std::uint64_t>() + 60000;
        return record.at("ts").integer<std::uint64_t>();
    };
    if (recorded) source_timestamp = source_time(parse_json(ledger.input(recorded - 1)->canonical_json), source_time);
    auto last_pulse = std::chrono::steady_clock::time_point{};
    std::string last_state;
    bool last_ready = false;
    const auto pulse = [&](bool force = false) {
        if (!service.enabled() && !c.max_ledger_bytes) return;
        service.heartbeat();
        const auto clock = std::chrono::steady_clock::now();
        const auto bytes = c.max_ledger_bytes ? ledger_bytes(c.ledger) : 0;
        if (c.max_ledger_bytes && bytes > c.max_ledger_bytes) storage_stop = true;
        if (!service.enabled()) return;
        const std::string state = storage_stop ? "storage_budget" : stopped ? "draining" : "running";
        const bool ready = prefix_verified && !storage_stop && !stopped;
        const bool changed = state != last_state || ready != last_ready;
        if (!force && !changed && clock - last_pulse < std::chrono::seconds(c.status_interval)) return;
        last_pulse = clock;
        last_state = state;
        last_ready = ready;
        const auto now = wall_time_ms();
        auto pending = parse_json(ledger.delivery_metrics_json(now));
        for (const auto& [name, target] : c.routing.targets) {
            (void)target;
            if (!pending.members.count(name)) pending.members[name] = Json::object({
                {"pending_count", num(0)}, {"oldest_age_ms", Json{}}});
        }
        service.publish(Json::object({{"deployment", Json::string(deployment)},
            {"state", Json::string(state)},
            {"ready", Json::boolean(ready)},
            {"readiness", Json::object({{"validated_strategy_warmup", Json::boolean(true)},
                {"recovered_ledger", Json::boolean(true)}, {"verified_source_prefix", Json::boolean(prefix_verified)},
                {"no_unhealed_input_gap", Json::boolean(true)}, {"storage_below_budget", Json::boolean(!storage_stop)}})},
            {"metrics", Json::object({{"committed_input", num(ledger.input_count())},
                {"last_seq", cursor.seen_tick ? num(cursor.tick_seq) : Json{}},
                {"source_timestamp_ms", num(source_timestamp)}, {"source_lag_ms", num(now > source_timestamp ? now - source_timestamp : 0)},
                {"queue_bytes", num(intake_bytes + delivery.queue_bytes())}, {"ledger_bytes", num(c.max_ledger_bytes ? bytes : ledger_bytes(c.ledger))},
                {"control_errors", num(delivery.control_errors())},
                {"report_cursor", num(ledger.report_cursor())}, {"targets", std::move(pending)}})}}), force || changed);
    };
    pulse(true);
    auto consume_message = [&](const std::string &message, std::uint64_t &index) {
        delivery.check();
        auto frame = feed_record(message);
        auto canonical = frame.dump();
        if (auto previous = ledger.input(index)) {
            if (previous->canonical_json != canonical)
                throw std::runtime_error("input conflicts with committed prefix");
            ++replayed_prefix;
        } else {
            if (index != ledger.input_count()) {
                service.input_gap();
                throw InputGap("input sequence is not contiguous");
            }
            // The entire feed message advances in memory before one
            // input/state/outbox transaction. Failure discards this instance;
            // interruption and delivery begin only after every event commits.
            try { apply_record(strategy, c, cursor, frame); }
            catch (const InputGap&) { service.input_gap(); throw; }
            auto events = actions(strategy, c, deployment);
            auto report = cumulative_report(strategy, deployment, index + 1, report_deltas);
            ledger.commit_input(index, canonical, strategy.hash(strategy.state), events, report);
            strategy.clear(strategy.state);
            ++processed;
            source_timestamp = source_time(frame, source_time);
            if (c.report_jsonl) {
                std::cout << report_deltas.json() << '\n' << std::flush;
                if (!std::cout) throw std::runtime_error("report-jsonl stdout write failed; committed input remains durable");
            }
        }
        ++index;
        if (index >= recorded) prefix_verified = true;
        pulse();
    };
    auto consume = [&](std::istream &in, std::uint64_t start, bool full_snapshot) {
        std::string row;
        std::uint64_t index = start;
        while (!stopped && !storage_stop && line(in, row)) {
            if (blank(row))
                continue;
            consume_message(row, index);
            if (c.max_events && processed >= c.max_events)
                break;
        }
        if (full_snapshot && !stopped && !storage_stop && !(c.max_events && processed >= c.max_events) &&
            index < recorded)
            throw std::runtime_error("input snapshot omits committed prefix");
    };
    try {
        if (c.feed_url.empty()) {
            if (c.feed == "-") {
                std::uint64_t index = c.from_input;
                std::string pending;
                char buffer[65536];
                auto last_data = std::chrono::steady_clock::now();
                auto message_started = last_data;
                while (!stopped && !storage_stop && !(c.max_events && processed >= c.max_events)) {
                    delivery.check();
                    intake_bytes = pending.size();
                    pulse();
                    if (storage_stop) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (now - last_data >= std::chrono::seconds(c.feed_idle_timeout) ||
                        (!pending.empty() && now - message_started >= std::chrono::seconds(c.feed_message_timeout)))
                        throw std::runtime_error("native stdin idle or message timeout");
                    pollfd fd{STDIN_FILENO, POLLIN, 0};
                    int rc = poll(&fd, 1, 100);
                    if (rc < 0) {
                        if (errno == EINTR)
                            continue;
                        throw std::runtime_error("stdin poll failed");
                    }
                    if (!rc)
                        continue;
                    if (stopped) break;
                    auto n = read(STDIN_FILENO, buffer, sizeof buffer);
                    if (n < 0) {
                        if (errno == EINTR)
                            continue;
                        throw std::runtime_error("stdin read failed");
                    }
                    if (stopped) break;
                    if (!n) {
                        if (!blank(pending))
                            consume_message(pending, index);
                        break;
                    }
                    last_data = std::chrono::steady_clock::now();
                    if (pending.empty()) message_started = last_data;
                    pending.append(buffer, static_cast<std::size_t>(n));
                    for (;;) {
                        auto end = pending.find('\n');
                        if (end == std::string::npos)
                            break;
                        if (end > MAX_FRAME)
                            throw std::runtime_error("input line exceeds 1 MiB");
                        auto message = pending.substr(0, end);
                        pending.erase(0, end + 1);
                        intake_bytes = pending.size();
                        message_started = std::chrono::steady_clock::now();
                        if (!blank(message))
                            consume_message(message, index);
                        if (stopped || storage_stop || (c.max_events && processed >= c.max_events))
                            break;
                    }
                    if (pending.size() > MAX_FRAME)
                        throw std::runtime_error("input line exceeds 1 MiB");
                }
            } else {
                std::ifstream input(c.feed);
                if (!input)
                    throw std::runtime_error("cannot open feed");
                consume(input, c.from_input, true);
            }
        } else if (c.feed_url.rfind("ws://", 0) == 0 || c.feed_url.rfind("wss://", 0) == 0) {
            HttpOptions feed;
            feed.url = c.feed_url;
            feed.allow_insecure_http = c.allow_http;
            feed.total_timeout_ms = static_cast<long>(c.feed_message_timeout * 1000);
            feed.connect_timeout_ms = std::min<long>(5000, feed.total_timeout_ms);
            feed.idle_timeout_ms = static_cast<long>(c.feed_idle_timeout * 1000);
            feed.message_timeout_ms = static_cast<long>(c.feed_message_timeout * 1000);
            std::string subscription =
                c.subscribe_path.empty() ? "" : read_file(c.subscribe_path, MAX_FRAME);
            std::uint64_t index = c.from_input;
            receive_websocket(
                feed, subscription,
                [&](std::string_view bytes) {
                    consume_message(std::string(bytes), index);
                    return !stopped && !storage_stop && !(c.max_events && processed >= c.max_events);
                },
                [&] { delivery.check(); pulse(); return stopped != 0 || storage_stop; },
                [&](std::size_t bytes) { intake_bytes = bytes; });
        } else {
            HttpOptions feed;
            feed.url = c.feed_url;
            feed.allow_insecure_http = c.allow_http;
            feed.total_timeout_ms = static_cast<long>(c.feed_message_timeout * 1000);
            feed.connect_timeout_ms = std::min<long>(5000, feed.total_timeout_ms);
            feed.idle_timeout_ms = static_cast<long>(c.feed_idle_timeout * 1000);
            do {
                delivery.check();
                pulse();
                if (storage_stop || stopped) break;
                auto snapshot = get_feed_snapshot(feed, [&] { pulse(); return stopped != 0 || storage_stop; });
                std::istringstream input(snapshot);
                consume(input, 0, true);
                recorded = ledger.input_count();
                if (c.check || stopped || storage_stop || (c.max_events && processed >= c.max_events))
                    break;
                for (long n = 0; n < c.poll_ms && !stopped; n += 100) {
                    delivery.check();
                    pulse();
                    if (storage_stop) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } while (!stopped && !storage_stop);
        }
        if (stopped || storage_stop) delivery.limit_drain();
        delivery.finish(false);
    } catch (const std::exception& error) {
        try { service.stop("failed"); } catch (...) {}
        delivery.limit_drain();
        try { delivery.finish(false); } catch (...) {}
        const auto pending = ledger.unsent_count();
        const auto status = parse_json(LedgerView(c.ledger).status_json());
        std::string guidance;
        for (const auto& [target, value] : status.at("targets").members)
            if (value.at("unsent").integer<std::uint64_t>())
                guidance += "; run `pineforge-live redeliver --ledger " + c.ledger +
                    " --deployment " + deployment + " --target " + target + "`";
        throw std::runtime_error(std::string(error.what()) + "; " + std::to_string(pending) +
                                 " actions not sent" + guidance);
    }
    auto pending = ledger.unsent_count();
    pulse(true);
    service.stop(storage_stop ? "storage_budget" : "stopped");
    std::cout << Json::object(
                     {{"deployment", Json::string(deployment)},
                      {"effective_settings", settings_receipt.empty() ? Json{} : parse_json(settings_receipt)},
                      {"execution_capabilities", capabilities_receipt.empty() ? Json{} : parse_json(capabilities_receipt)},
                      {"confirmed_bar_capabilities", confirmed_bar_receipt.empty() ? Json{} : parse_json(confirmed_bar_receipt)},
                      {"inputs_committed", num(ledger.input_count())},
                      {"inputs_processed", num(processed)},
                      {"prefix_skipped", num(replayed_prefix)},
                      {"webhooks_delivered", num(delivery.delivered())},
                      {"webhook_failures", num(delivery.failed())},
                      {"webhooks_pending", num(pending)},
                      {"last_tick_sequence", cursor.seen_tick ? num(cursor.tick_seq) : Json{}}})
                     .dump()
              << '\n';
    return storage_stop ? 3 : 0;
}

int probe_command(int argc, char** argv) {
    std::string path;
    std::uint64_t max_age = 0;
    bool ready = false;
    std::set<std::string> seen;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (!seen.insert(option).second) throw std::runtime_error("duplicate probe option");
        if (option == "--ready") { ready = true; continue; }
        if (index + 1 == argc) throw std::runtime_error("missing probe option value");
        const std::string value = argv[++index];
        if (option == "--status-file") path = value;
        else if (option == "--max-age") {
            max_age = unsigned_arg(value);
            if (!max_age || max_age > 86400) throw std::runtime_error("max-age must be 1..86400 seconds");
        } else throw std::runtime_error("unknown probe option");
    }
    if (path.empty() || !max_age) throw std::runtime_error("probe requires status-file and max-age");
    return probe_status(parse_json(read_file(path, MAX_FRAME)), max_age * 1000, ready) ? 0 : 1;
}

int ledger_command(int argc, char** argv) {
    const std::string command = argv[1];
    std::string path, target, deployment, control_dir;
    std::uint64_t after = 0, from = 1;
    std::optional<std::uint64_t> at_input;
    bool follow = false, failed_only = false;
    std::set<std::string> seen;
    for (int index = 2; index < argc; ++index) {
        const std::string option = argv[index];
        if (!seen.insert(option).second) throw std::runtime_error("duplicate option: " + option);
        if (command == "actions" && option == "--follow") { follow = true; continue; }
        if (command == "redeliver" && option == "--failed-only") { failed_only = true; continue; }
        if (index + 1 == argc) throw std::runtime_error("missing option value: " + option);
        const std::string value = argv[++index];
        if (option == "--ledger") path = value;
        else if (option == "--deployment") deployment = value;
        else if (command == "actions" && option == "--after") after = unsigned_arg(value);
        else if (command == "report" && option == "--at-input") at_input = unsigned_arg(value);
        else if (command == "redeliver" && option == "--from") from = unsigned_arg(value);
        else if (command == "redeliver" && option == "--target") target = value;
        else if (command == "redeliver" && option == "--control-dir") control_dir = value;
        else throw std::runtime_error("unknown option: " + option);
    }
    if (path.empty()) throw std::runtime_error("ledger is required");
    if (command == "redeliver" && deployment.empty())
        throw std::runtime_error("redeliver requires --deployment <id>");
    LedgerView view(path);
    if (!deployment.empty() && deployment != view.identity())
        throw std::runtime_error("ledger deployment identity mismatch");
    if (command == "report") {
        std::cout << view.report_json(at_input) << '\n';
        return 0;
    }
    if (command == "status") {
        std::cout << view.status_json() << '\n';
        return 0;
    }
    if (command == "actions") {
        do {
            const auto events = view.actions_after(after);
            for (const auto& event : events) {
                std::cout << event.payload << '\n' << std::flush;
                after = event.ordinal;
            }
            if (events.size() == 256) continue;
            if (!follow) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        } while (!stopped);
        return stopped ? 130 : 0;
    }
    if (target.empty()) throw std::runtime_error("redeliver requires target");
    const auto document = view.routing_document();
    if (document.empty()) throw std::runtime_error("resume this phase-A ledger with run before redelivering");
    auto routing = restore_routes(document);
    if (!routing.targets.count(target)) throw std::runtime_error("undefined webhook target: " + target);
    if (!from || from > static_cast<std::uint64_t>(INT64_MAX))
        throw std::runtime_error("redeliver from must be 1..INT64_MAX");
    if (!control_dir.empty()) {
        const auto identifier = sha256_hex(deployment + ":" + std::to_string(getpid()) + ":" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        ControlDirectory controls(control_dir, false);
        if (!ledger_running(path)) throw std::runtime_error("no runner owns this ledger; use offline redeliver");
        controls.submit(Json::object({{"schema_version", Json::string("pineforge-redelivery-request/v1")},
            {"deployment", Json::string(deployment)}, {"request_id", Json::string(identifier)},
            {"target", Json::string(target)}, {"from", num(from)}, {"failed_only", Json::boolean(failed_only)}}));
        std::cout << Json::object({{"request_id", Json::string(identifier)}, {"queued", Json::boolean(true)}}).dump() << '\n';
        return 0;
    }
    for (auto position = routing.targets.begin(); position != routing.targets.end();) {
        if (position->first != target) position = routing.targets.erase(position);
        else ++position;
    }
    auto targets = routing.load_secrets();
    Ledger ledger(path, view.identity());
    auto events = view.redelivery_events(target, from, failed_only);
    const auto selected = events.size();
    std::set<std::string> selected_ids;
    for (const auto& event : events) selected_ids.insert(event.id);
    DeliveryWorker delivery(ledger, routing.delivery, std::move(targets), std::move(events),
                            [] { return stopped != 0; });
    delivery.finish();
    std::uint64_t failed = 0, pending = 0;
    for (const auto& event : view.redelivery_events(target, from, true))
        if (selected_ids.count(event.id)) ++failed;
    auto low = from ? from - 1 : 0;
    while (const auto next = ledger.next_delivery_event(low)) {
        low = next->event.ordinal;
        if (next->unsent && selected_ids.count(next->event.id)) ++pending;
    }
    std::cout << Json::object({{"selected", num(selected)}, {"delivered", num(delivery.delivered())},
        {"failed", num(failed)}, {"pending", num(pending)}}).dump() << '\n';
    return stopped ? 0 : (failed || pending ? 2 : 0);
}
} // namespace
int main(int argc, char **argv) {
    std::locale::global(std::locale::classic());
    capture_proxy_environment();
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    std::signal(SIGPIPE, SIG_IGN);
    try {
        if (argc == 1 ||
            (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "help"))) {
            help();
            return 0;
        }
        const std::string command = argv[1];
        if (command == "probe") return probe_command(argc, argv);
        if (command == "actions" || command == "status" || command == "redeliver" || command == "report")
            return ledger_command(argc, argv);
        return run(args(argc, argv));
    } catch (const std::exception &e) {
        std::cerr << "pineforge-live: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "pineforge-live: unknown C++ exception\n";
        return 1;
    }
}
