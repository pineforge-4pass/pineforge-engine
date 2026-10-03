// SPDX-License-Identifier: Apache-2.0
#include "json.hpp"
#include "native_startup.hpp"
#include "store.hpp"
#include "transport.hpp"
#include <pineforge/pineforge.h>

#include <algorithm>
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
volatile std::sig_atomic_t stopped = 0;
void signal_stop(int) { stopped = 1; }
constexpr std::size_t MAX_FRAME = 1024 * 1024;

struct Config {
    std::string strategy, warmup, feed = "-", ledger, webhook, mode = "", input_tf = "1", script_tf,
                                  symbol, name = "strategy";
    std::string session = "24x7", timezone = "UTC", chart_timezone = "UTC", secret_env, feed_url,
                subscribe_path, native_config;
    std::vector<std::pair<std::string, std::string>> inputs, overrides, syminfo;
    std::set<std::string> explicit_flags;
    NativeConfigValues native;
    std::uint64_t from_input = 0, max_events = 0, max_attempts = 8;
    long poll_ms = 1000;
    bool check = false, allow_http = false;
};

void help() {
    std::cout << "PineForge native live runner (C++17)\n"
                 "Usage: pineforge-live run --strategy strategy.so --warmup history.csv\n"
                 "       --script-tf 15 --mode ticks|bars --ledger orders.sqlite3\n"
                 "       --webhook-url https://receiver.example/events --symbol EXCHANGE:SYMBOL\n"
                 "       [--feed events.jsonl|- | --feed-url https://...|wss://...]\n"
                 "Options: --input-tf 1 --name NAME --session 24x7 --timezone UTC\n"
                 "         --input TITLE=VALUE --override KEY=VALUE (repeatable)\n"
                 "         --syminfo KEY=VALUE --chart-timezone UTC\n"
                 "         --subscribe subscription.json (WebSocket only)\n"
                 "         --webhook-secret-env NAME --allow-insecure-http\n"
                 "         --from-input N --max-events N --max-attempts 8\n"
                 "         --native-config FILE (strict native run specification)\n"
                 "         --check (one HTTP snapshot) --poll-ms 1000\n"
                 "JSONL: {\"type\":\"tick\",\"ts\":60000,\"seq\":1,\"price\":100,\"qty\":1}\n"
                 "       "
                 "{\"type\":\"bar\",\"bar\":{\"ts_open\":60000,\"o\":100,\"h\":102,\"l\":99,\"c\":"
                 "101,\"v\":4}}\n"
                 "       {\"type\":\"time\",\"ts\":120000} (tick mode only)\n"
                 "Recovery replays immutable warmup + ledger inputs before any delivery.\n"
                 "File/HTTP input defaults to the full recorded prefix; --from-input declares\n"
                 "the zero-based start of a resumed tail. HTTP polls use full snapshots.\n";
}
std::uint64_t unsigned_arg(const std::string &s) {
    return Json::number(s).integer<std::uint64_t>();
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
        else if (a == "--poll-ms") {
            auto n = unsigned_arg(v);
            if (n < 100 || n > 3600000)
                throw std::runtime_error("poll-ms out of range");
            c.poll_ms = static_cast<long>(n);
        } else
            throw std::runtime_error("unknown option: " + a);
    }
    if (c.strategy.empty() || c.warmup.empty() || c.ledger.empty() || c.webhook.empty())
        throw std::runtime_error("strategy, warmup, ledger and webhook-url are required");
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
    for (const auto &src : {c.strategy, c.warmup, c.feed == "-" ? std::string{} : c.feed,
                            c.subscribe_path})
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
            begin_ = symbol<decltype(&strategy_stream_begin)>("strategy_stream_begin");
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
            throw std::runtime_error("tick sequence gap or regression");
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
        out.push_back({std::move(id), payload.dump()});
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
bool drain(Ledger &ledger, const HttpOptions &options, const Config &c, std::uint64_t &delivered) {
    while (!stopped) {
        auto e = ledger.pending_event();
        if (!e)
            return true;
        if (e->attempts >= c.max_attempts)
            throw std::runtime_error("webhook retry limit reached; queued event remains in ledger");
        ledger.begin_delivery(e->id);
        auto result = post_webhook(options, *e);
        if (result.success) {
            ledger.acknowledge(e->id);
            ++delivered;
            continue;
        }
        ledger.record_delivery_failure(e->id, result.error);
        if (result.status >= 300 && result.status < 500 && result.status != 408 &&
            result.status != 429)
            throw std::runtime_error("webhook receiver refused event; event remains queued");
        const auto delay =
            std::min<std::uint64_t>(5000, 100ULL << std::min<std::uint32_t>(e->attempts, 5));
        for (std::uint64_t n = 0; n < delay && !stopped; n += 100)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}
LegacyIdentityFields legacy_fields(const Config &c) {
    return {c.mode,     c.input_tf, c.script_tf, c.session, c.timezone, c.chart_timezone,
            c.symbol,   c.name,     c.webhook,   c.inputs,  c.overrides, c.syminfo};
}

int run(Config c) {
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
    std::string deployment =
        c.native.present
            ? native_identity(c.native, c.mode, c.name, c.webhook, original, library)
            : identity(legacy_fields(c), original, library);
    if (!settings_receipt.empty())
        deployment = sha256_hex(deployment + ":settings-v1:" + settings_receipt);
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
    Ledger ledger(c.ledger, deployment);
    Cursor cursor;
    auto recorded = ledger.input_count();
    for (std::uint64_t i = 0; i < recorded; ++i) {
        if (stopped)
            return 130;
        auto row = ledger.input(i);
        if (!row)
            throw std::runtime_error("ledger input hole");
        apply_record(strategy, c, cursor, parse_json(row->canonical_json));
        auto events = actions(strategy, c, deployment);
        if (row->state_hash != std::to_string(strategy.hash(strategy.state)) ||
            events.size() != row->events.size())
            throw std::runtime_error("native replay state/action count mismatch");
        for (std::size_t k = 0; k < events.size(); ++k)
            if (events[k].id != row->events[k].id || events[k].payload != row->events[k].payload)
                throw std::runtime_error("native replay order-action mismatch");
        strategy.clear(strategy.state);
    }
    HttpOptions webhook;
    webhook.url = c.webhook;
    webhook.allow_insecure_http = c.allow_http;
    if (!c.secret_env.empty()) {
        const char *value = std::getenv(c.secret_env.c_str());
        if (!value || !*value)
            throw std::runtime_error("webhook secret environment variable is missing or empty");
        webhook.hmac_secret = value;
    }
    if (c.from_input > recorded)
        throw std::runtime_error("from-input skips unrecorded inputs");
    std::uint64_t delivered = 0, processed = 0, replayed_prefix = 0;
    drain(ledger, webhook, c, delivered);
    auto consume_message = [&](const std::string &message, std::uint64_t &index) {
        auto frame = feed_record(message);
        auto canonical = frame.dump();
        if (auto previous = ledger.input(index)) {
            if (previous->canonical_json != canonical)
                throw std::runtime_error("input conflicts with committed prefix");
            ++replayed_prefix;
        } else {
            if (index != ledger.input_count())
                throw std::runtime_error("input sequence is not contiguous");
            // The entire feed message advances in memory before one
            // input/state/outbox transaction. Failure discards this instance;
            // interruption and delivery begin only after every event commits.
            apply_record(strategy, c, cursor, frame);
            auto events = actions(strategy, c, deployment);
            ledger.commit_input(index, canonical, strategy.hash(strategy.state), events);
            strategy.clear(strategy.state);
            ++processed;
            if (!drain(ledger, webhook, c, delivered))
                return;
        }
        ++index;
    };
    auto consume = [&](std::istream &in, std::uint64_t start, bool full_snapshot) {
        std::string row;
        std::uint64_t index = start;
        while (!stopped && line(in, row)) {
            if (blank(row))
                continue;
            consume_message(row, index);
            if (c.max_events && processed >= c.max_events)
                break;
        }
        if (full_snapshot && !stopped && !(c.max_events && processed >= c.max_events) &&
            index < recorded)
            throw std::runtime_error("input snapshot omits committed prefix");
    };
    if (c.feed_url.empty()) {
        if (c.feed == "-") {
            std::uint64_t index = c.from_input;
            std::string pending;
            char buffer[65536];
            while (!stopped && !(c.max_events && processed >= c.max_events)) {
                pollfd fd{STDIN_FILENO, POLLIN, 0};
                int rc = poll(&fd, 1, 100);
                if (rc < 0) {
                    if (errno == EINTR)
                        continue;
                    throw std::runtime_error("stdin poll failed");
                }
                if (!rc)
                    continue;
                auto n = read(STDIN_FILENO, buffer, sizeof buffer);
                if (n < 0) {
                    if (errno == EINTR)
                        continue;
                    throw std::runtime_error("stdin read failed");
                }
                if (!n) {
                    if (!blank(pending))
                        consume_message(pending, index);
                    break;
                }
                pending.append(buffer, static_cast<std::size_t>(n));
                for (;;) {
                    auto end = pending.find('\n');
                    if (end == std::string::npos)
                        break;
                    if (end > MAX_FRAME)
                        throw std::runtime_error("input line exceeds 1 MiB");
                    auto message = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!blank(message))
                        consume_message(message, index);
                    if (stopped || (c.max_events && processed >= c.max_events))
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
        std::string subscription =
            c.subscribe_path.empty() ? "" : read_file(c.subscribe_path, MAX_FRAME);
        std::uint64_t index = c.from_input;
        receive_websocket(
            feed, subscription,
            [&](std::string_view bytes) {
                consume_message(std::string(bytes), index);
                return !stopped && !(c.max_events && processed >= c.max_events);
            },
            [] { return stopped != 0; });
    } else {
        HttpOptions feed;
        feed.url = c.feed_url;
        feed.allow_insecure_http = c.allow_http;
        do {
            auto snapshot = get_feed_snapshot(feed);
            std::istringstream input(snapshot);
            consume(input, 0, true);
            recorded = ledger.input_count();
            if (c.check || stopped || (c.max_events && processed >= c.max_events))
                break;
            for (long n = 0; n < c.poll_ms && !stopped; n += 100)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        } while (!stopped);
    }
    auto pending = ledger.pending_count();
    std::cout << Json::object(
                     {{"deployment", Json::string(deployment)},
                      {"effective_settings", settings_receipt.empty() ? Json{} : parse_json(settings_receipt)},
                      {"inputs_committed", num(ledger.input_count())},
                      {"inputs_processed", num(processed)},
                      {"prefix_skipped", num(replayed_prefix)},
                      {"webhooks_delivered", num(delivered)},
                      {"webhooks_pending", num(pending)},
                      {"last_tick_sequence", cursor.seen_tick ? num(cursor.tick_seq) : Json{}}})
                     .dump()
              << '\n';
    return stopped ? 130 : pending ? 2 : 0;
}
} // namespace
int main(int argc, char **argv) {
    std::locale::global(std::locale::classic());
    std::signal(SIGINT, signal_stop);
    std::signal(SIGTERM, signal_stop);
    try {
        if (argc == 1 ||
            (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "help"))) {
            help();
            return 0;
        }
        return run(args(argc, argv));
    } catch (const std::exception &e) {
        std::cerr << "pineforge-live: " << e.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "pineforge-live: unknown C++ exception\n";
        return 1;
    }
}
