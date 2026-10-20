// L8c differential: real Pine tapes with a sink, without a sink, and with
// retention forced. No expected hash, trade or metric is harvested here.
// Fail-before: the previous header has no closed_trade_prefix/seal API.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/report_sink_writer.hpp>
#include <pineforge/run_failure.hpp>
#include "../src/native_execution_consumer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
constexpr std::int64_t start_ms = INT64_C(1736121600000);
constexpr std::int64_t minute = 60000;
constexpr double missing = std::numeric_limits<double>::quiet_NaN();

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}
bool same(double left, double right) {
    return std::memcmp(&left, &right, sizeof left) == 0;
}
template<class Action>
void invariant(Action action) {
    bool caught = false;
    try { action(); }
    catch (const std::logic_error& error) {
        caught = classify_run_failure(error).code == RunFailureCode::engine_invariant;
    }
    require(caught, "internal access/rewind must throw engine_invariant");
}

struct Directory {
    std::string path;
    std::vector<std::string> files;
    Directory() {
        char pattern[] = "/tmp/pineforge-l8c-XXXXXX";
        const char* made = mkdtemp(pattern);
        if (!made) throw std::runtime_error("mkdtemp failed");
        path = made;
    }
    ~Directory() {
        for (const auto& file : files) std::remove(file.c_str());
        rmdir(path.c_str());
    }
    std::string file() {
        files.push_back(path + '/' + std::to_string(files.size()) + ".ndjson");
        return files.back();
    }
};
struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};
std::string read(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    require(file.good(), "sink file missing");
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
std::string records(const std::string& bytes, char tag) {
    const std::string prefix = std::string("[\"") + tag + "\",";
    std::string result;
    std::size_t begin = 0;
    while (begin < bytes.size()) {
        const auto end = bytes.find('\n', begin);
        require(end != std::string::npos, "sink record lacks LF");
        if (bytes.compare(begin, prefix.size(), prefix) == 0)
            result.append(bytes, begin, end - begin + 1);
        begin = end + 1;
    }
    return result;
}
// Names the first line two record streams disagree on, with both lines.
std::string first_difference(const std::string& left, const std::string& right) {
    const auto lines = [](const std::string& text) {
        std::vector<std::string> result;
        for (std::size_t begin = 0; begin < text.size();) {
            const auto end = text.find('\n', begin);
            const auto stop = end == std::string::npos ? text.size() : end;
            result.push_back(text.substr(begin, stop - begin));
            begin = stop + 1;
        }
        return result;
    };
    const auto a = lines(left), b = lines(right);
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i)
        if (a[i] != b[i])
            return "line " + std::to_string(i + 1) + " [" + a[i] + "] vs [" + b[i] + "]";
    return "line counts " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
}
pf_report_sink_receipt_v1 receipt(const BacktestEngine& host) {
    pf_report_sink_receipt_v1 result{};
    result.struct_size = sizeof result;
    result.version = 1;
    require(host.report_sink_receipt_v1(&result) == 0, "receipt query failed");
    return result;
}
void install(BacktestEngine& host, const std::string& path) {
    const pf_report_sink_v1 sink{sizeof sink, 1, path.c_str(), "L8c", nullptr};
    require(host.set_report_sink_v1(&sink) == 0, "sink installation failed");
}
std::vector<Bar> tape(int count) {
    std::vector<Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double price = 100.0 + (i % 13 - 6) * 0.25;
        bars.push_back({price, price + 1.5, price - 1.5, price + (i % 2 ? -0.25 : 0.25),
                        1.0, start_ms + i * minute});
    }
    return bars;
}

struct Options {
    bool coof = false;
    bool pooc = false;
    bool brackets = false;
    bool rewind = false;
    bool zero_fee = false;
    bool percent_fee = false;
    bool selected = false;
    bool observed = false;
    bool quote = false;
    bool empty = false;
    bool fail = false;
    bool abort = false;
    bool magnifier = false;
    bool variable = false;
    std::string script_tf = "1";
};
// The case an assertion belongs to: every failure below names it, so a lab log
// says which option set broke without a rerun.
std::string describe(const Options& options, int count) {
    const std::pair<const char*, bool> flags[] = {
        {"coof", options.coof}, {"pooc", options.pooc}, {"brackets", options.brackets},
        {"rewind", options.rewind}, {"zero_fee", options.zero_fee},
        {"percent_fee", options.percent_fee}, {"selected", options.selected},
        {"observed", options.observed}, {"quote", options.quote}, {"empty", options.empty},
        {"fail", options.fail}, {"abort", options.abort}, {"magnifier", options.magnifier},
        {"variable", options.variable}};
    std::string text = "case{bars=" + std::to_string(count) + " script_tf=" + options.script_tf;
    for (const auto& flag : flags)
        if (flag.second) text += std::string(" ") + flag.first;
    return text + "}";
}
// What a run latched, from the engine's own failure record.
std::string latched_error(const BacktestEngine& host) {
    return "last_error=\"" + host.last_error() + "\" code=" + run_failure_code_of(host)
        + " args=" + run_failure_args_of(host);
}
struct Observation {
    std::uint64_t hash = 0, extension = 0;
    std::array<double, 3> averages{};
};
class Strategy final : public source::PineStrategyHost {
public:
    explicit Strategy(Options options, bool streamable = true) : options_(std::move(options)) {
        // "UTC", not "UTC+0": a selected window's source-bar count compares the
        // chart zone's clock token with this one (selected_window_plan.cpp
        // admit_clock, clock_token), and "UTC+0" is a token of its own. The
        // mismatch makes the count throw engine_invariant at the run's begin
        // (pine_scheduler_native.cpp selected_executed_source_bars); the Pine
        // host latches it, skips every source bar and the capture finds none.
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(
            options_.variable ? QtyType::PERCENT_OF_EQUITY : QtyType::FIXED);
        config.default_qty_value = options_.variable ? 10.0 : 1.25;
        config.pyramiding = 3;
        config.commission_type = static_cast<int>(
            options_.percent_fee ? CommissionType::PERCENT : CommissionType::CASH_PER_ORDER);
        config.commission_value = options_.zero_fee ? 0.0 : (options_.percent_fee ? 0.0137 : 0.125);
        config.calc_on_order_fills = options_.coof;
        config.process_orders_on_close = options_.pooc;
        configure_pine_strategy(config);
        set_trade_history_streamable(streamable);
        set_broker_state_hash_recording(true);
        set_trace_enabled(true);
    }
    TradeLedger& ledger() { return trades_; }
    const TradeLedger& ledger() const { return trades_; }
    NativeExecutionConsumer& consumer() const { return NativeExecutionConsumer::bound(*this); }
    // How far the run got and where the ledger stood: printed with a latched
    // error, so the log shows the seal state at the failure, not only its text.
    std::string progress() const {
        return "callbacks=" + std::to_string(observations.size())
            + " ledger_size=" + std::to_string(trades_.size())
            + " ledger_base=" + std::to_string(trades_.base())
            + " closed_rows_final=" + std::to_string(consumer().closed_rows_final())
            + " sealed_base=" + std::to_string(consumer().closed_trade_prefix().base);
    }
    std::uint64_t extension_hash() const {
        BrokerStateHashSink f;
        hash_host_extension(f);
        return f.h;
    }
    void clear_phase(std::size_t global) {
        adapter_.permute_exit_phases(global, {std::numeric_limits<std::size_t>::max()});
    }
    std::vector<Observation> observations;
    std::size_t maximum_resident = 0;
    bool rewound = false;
    void execute(const std::vector<Bar>& bars, const std::string& script_tf = "1",
                 bool magnifier = false) {
        observations.clear();
        maximum_resident = 0;
        rewound = false;
        commanded_ = -1;
        run(bars.data(), static_cast<int>(bars.size()), "1", script_tf, magnifier);
    }
    void on_source_bar(const Bar& bar) override {
        const int index = pine_bar_index();
        maximum_resident = std::max(maximum_resident, trades_.size() - trades_.base());
        if (options_.rewind && index == 32 && !rewound && trades_.size() > trades_.base() + 1) {
            trades_.resize(trades_.size() - 1);
            native_closed_rows_amended(trades_.size());
            rewound = true;
        }
        observations.push_back({broker_state_hash(), extension_hash(),
            {avg_trade_percent(), avg_winning_trade_percent(), avg_losing_trade_percent()}});
        if (commanded_ == index) return;
        commanded_ = index;
        trace("seal", bar.close);
        if (options_.fail && index == 44) throw std::runtime_error("L8c intentional failure");
        if (options_.abort && index == 44) request_abort();
        if (options_.empty) return;
        const bool long_side = (index / 6) % 2 == 0;
        if (index % 6 == 0) {
            strategy_entry("entry-a", long_side, missing, missing,
                           options_.variable ? missing : 1.25, "a");
            strategy_entry("entry-b", long_side, missing, missing,
                           options_.variable ? missing : 2.5, "b");
        }
        if (options_.brackets && index % 6 == 1) {
            // Fixed-unit partial exits avoid relying on L18's parallel
            // qty_percent spill. Same-time rows still exercise tail grouping.
            strategy_exit("exit-b", "entry-b", bar.close + 0.5, bar.close - 0.5,
                          missing, missing, missing, 100.0, "bracket-b", 1.25);
            strategy_exit("exit-a", "entry-a", bar.close + 0.5, bar.close - 0.5,
                          missing, missing, missing, 100.0, "bracket-a", 1.25);
        }
        if (index % 6 == 3) strategy_close_all();
    }
private:
    Options options_;
    int commanded_ = -1;
};

void compare_observations(const Strategy& reference, const Strategy& actual) {
    require(reference.observations.size() == actual.observations.size(),
            "callback count changed: " + std::to_string(reference.observations.size()) + " vs "
                + std::to_string(actual.observations.size()));
    for (std::size_t i = 0; i < reference.observations.size(); ++i) {
        const auto& a = reference.observations[i];
        const auto& b = actual.observations[i];
        const std::string at = " at callback " + std::to_string(i);
        require(a.hash == b.hash, "live broker-state hash changed with the sink" + at);
        require(a.extension == b.extension, "live source-extension hash changed with the sink" + at);
        require(std::memcmp(a.averages.data(), b.averages.data(), sizeof a.averages) == 0,
                "sealed/unsealed percentage accessor bits changed" + at);
    }
}
void compare_summary(const ReportC& reference, const ReportC& actual) {
    require(reference.total_trades == actual.total_trades, "logical trade count changed");
    require(same(reference.net_profit, actual.net_profit), "net-profit fold bits changed");
    require(std::memcmp(&reference.metrics, &actual.metrics, sizeof reference.metrics) == 0,
            "768-byte metrics differ from sink-off report");
    require(reference.input_bars_processed == actual.input_bars_processed
            && reference.script_bars_processed == actual.script_bars_processed,
            "report diagnostics changed");
}
void expected_records(const Strategy& host, const ReportC& report, const std::string& path) {
    report_sink::LineWriter writer({sizeof(pf_report_sink_v1), 1, path.c_str(), "L8c", nullptr});
    writer.write_header({false, false, false, 0, 0});
    for (std::int64_t i = 0; i < report.equity_curve_len; ++i) writer.write_e(report.equity_curve[i]);
    for (int i = 0; i < report.trades_len; ++i) {
        static_assert(sizeof(TradeC) == sizeof(pf_trade_t), "public trade projection size");
        static_assert(offsetof(TradeC, open_at_end) == offsetof(pf_trade_t, open_at_end),
                      "public trade projection tail");
        pf_trade_t wire{};
        std::memcpy(&wire, &report.trades[i], sizeof wire);
        const auto& identity = host.get_report_trade(i);
        writer.write_t(i + INT64_C(1), wire, identity.entry_incarnation,
            host.closed_trade_close_cause(i), identity.entry_id, identity.exit_id,
            identity.exit_comment);
    }
    (void)writer.write_trailer();
}
void configure_observation(Strategy& host, const Options& options, int count) {
    if (options.observed) {
        const pf_execution_observer_v1 observer{sizeof observer, 1, nullptr,
            [](void*, const pf_execution_boundary_v1*, pf_boundary_receipt_v1* out) -> int {
                out->frame_bytes = 0;
                return 0;
            }};
        require(NativeExecutionConsumer::set_execution_observer_v1(&host, &observer) == 0,
                "observer installation failed");
    }
    if (options.selected) {
        const auto start = options.empty ? start_ms + count * minute : start_ms;
        const pf_selected_window_config_v1 window{sizeof window, 1, start,
                                                  start_ms + (count + 1) * minute};
        require(NativeExecutionConsumer::set_selected_window_v1(&host, &window) == 0,
                "selected-window installation failed");
    }
    if (options.quote)
        require(host.set_report_terminal_quote(start_ms + (count / 5 * 5) * minute, 117.25),
                "terminal quote installation failed");
}

void differential_checks(Directory& directory, const Options& options, int count) {
    Strategy baseline(options), streamed(options), retained(options, false);
    const auto stream_path = directory.file(), retain_path = directory.file();
    install(streamed, stream_path);
    install(retained, retain_path);
    const auto bars = tape(count);
    struct Role {
        const char* name;
        Strategy* host;
    };
    const Role roles[] = {{"baseline (sink off)", &baseline},
                          {"streamed (sink on, streamable)", &streamed},
                          {"retained (sink on, retaining)", &retained}};
    // All three hosts run before any verdict, so one lab log shows which of
    // them latched an error and where each stood.
    std::string run_errors;
    for (const Role& role : roles) {
        Strategy* const host = role.host;
        configure_observation(*host, options, count);
        host->execute(bars, options.script_tf, options.magnifier);
        if (!host->last_error().empty() && !options.fail && !options.abort)
            run_errors += std::string(role.name) + " run latched an engine error: "
                + latched_error(*host) + " [" + host->progress() + "]; ";
    }
    require(run_errors.empty(), run_errors);
    for (const Role& role : roles)
        require(role.host->consumer().closed_rows_digest_holds(role.host->ledger()),
                std::string(role.name) + ": Debug resident re-fold does not match the seeded digest");
    if (!options.empty && options.script_tf == "1") {
        require(streamed.ledger().base() > 0, "test did not exercise production sealing");
        require(streamed.maximum_resident < baseline.maximum_resident,
                "production sealing did not bound the resident row window");
    }
    require(baseline.ledger().base() == 0 && retained.ledger().base() == 0,
            "sink-off or non-streamable policy retired rows");
    require(receipt(streamed).count_t == static_cast<std::int64_t>(streamed.ledger().base()),
            "T records were not written exactly at seal");
    require(receipt(retained).count_t == 0, "retaining path wrote provisional rows");
    require(receipt(streamed).complete == 0, "sink completed before fill");
    compare_observations(baseline, streamed);
    compare_observations(baseline, retained);
    if (options.rewind) require(streamed.rewound && baseline.rewound, "money rewind was not reached");
    Report expected, actual, kept, again;
    baseline.fill_report(&expected.value);
    if (options.quote) {
        bool remarked = false;
        for (int i = 0; i < expected.value.trades_len; ++i) {
            if (expected.value.trades[i].open_at_end) {
                require(expected.value.trades[i].exit_price == 117.25,
                        "terminal quote did not re-mark the range-end trade");
                remarked = true;
            }
        }
        require(remarked, "terminal-quote fixture lacks an open range-end row");
    }
    streamed.fill_report(&actual.value);
    retained.fill_report(&kept.value);
    compare_summary(expected.value, actual.value);
    compare_summary(expected.value, kept.value);
    const auto bytes = read(stream_path);
    const auto retained_bytes = read(retain_path);
    streamed.fill_report(&again.value);
    compare_summary(actual.value, again.value);
    require(read(stream_path) == bytes, "repeated fill appended records or a second trailer");
    if (options.fail || options.abort) {
        require(receipt(streamed).complete == 0, "failed/aborted sink acquired a trailer");
        require(bytes.find("\"trailer\"") == std::string::npos, "failed sink has trailer bytes");
    } else {
        require(receipt(streamed).complete == 1, "successful sink lacks trailer");
        require(receipt(streamed).count_t == expected.value.total_trades,
                "sealed + tail + range-end T count differs");
        for (char tag : {'E', 'T', 'H', 'X'}) {
            const auto streamed_records = records(bytes, tag);
            const auto retained_records = records(retained_bytes, tag);
            require(streamed_records == retained_records,
                    std::string("streamed ") + tag + " records differ from retaining sink records: "
                        + first_difference(streamed_records, retained_records));
        }
        const auto reference_path = directory.file();
        expected_records(baseline, expected.value, reference_path);
        const auto reference = read(reference_path);
        for (char tag : {'E', 'T'}) {
            const auto streamed_records = records(bytes, tag);
            const auto reference_records = records(reference, tag);
            require(streamed_records == reference_records,
                    std::string("sink ") + tag + " record fields differ from sink-off report arrays: "
                        + first_difference(streamed_records, reference_records));
        }
    }
}

void differential(Directory& directory, const Options& options, int count = 146) {
    const std::string where = describe(options, count);
    // Printed before the case runs, so even a crash (no exception) names it.
    std::fprintf(stderr, "L8c: running %s\n", where.c_str());
    try {
        differential_checks(directory, options, count);
    } catch (const std::exception& error) {
        throw std::runtime_error(where + ": " + error.what());
    }
}

// Reconcile L17c/d with L8c: exercise both the captured selected read and the
// ordinary sink read after real publication has retired a nonempty prefix.
// The sink-off engine supplies every expected row, identity and metric byte.
void sealed_sink_capture_and_read(Directory& directory) {
    const auto bars = tape(146); // ends with real open lots after many round trips
    for (const bool selected : {false, true}) {
        Options options;
        options.selected = selected;
        Strategy baseline(options), streamed(options);
        const auto path = directory.file();
        install(streamed, path);
        for (auto* host : {&baseline, &streamed}) {
            configure_observation(*host, options, static_cast<int>(bars.size()));
            host->execute(bars);
            require(host->last_error().empty(),
                    "reconcile fixture failed: " + latched_error(*host));
        }
        const auto base = streamed.ledger().base();
        require(base > 0 && base < streamed.ledger().size(),
                "reconcile fixture must seal rows and retain a closed suffix");
        require(streamed.consumer().closed_trade_prefix().base == base,
                "ledger and report prefix disagree");
        require(baseline.ledger().base() == 0, "sink-off reference retired rows");
        const auto before = receipt(streamed);
        require(before.complete == 0 && before.count_t == static_cast<std::int64_t>(base),
                "pre-read T cursor must equal the sealed global base");

        Report expected, actual, again;
        baseline.fill_report(&expected.value);
        require(expected.value.trades != nullptr && expected.value.trades_len > 0,
                "sink-off reference has no report rows");
        const int open_count = static_cast<int>(std::count_if(
            expected.value.trades, expected.value.trades + expected.value.trades_len,
            [](const TradeC& row) { return row.open_at_end != 0; }));
        require(open_count > 0, "reconcile fixture must present a real open-trade tail");
        if (selected) {
            const TradeC* open_rows = nullptr;
            int captured_count = -1;
            require(NativeExecutionConsumer::report_sink_open_trades(
                        streamed, open_rows, captured_count),
                    "selected sink did not capture its open-lot view");
            require(open_rows != nullptr && captured_count == open_count,
                    "selected sink capture copied closed rows into its open-lot view");
        }
        const auto reference_path = directory.file();
        expected_records(baseline, expected.value, reference_path);
        const auto expected_trades = records(read(reference_path), 'T');
        std::size_t prefix_end = 0;
        for (std::size_t global = 0; global < base; ++global) {
            const auto end = expected_trades.find('\n', prefix_end);
            require(end != std::string::npos, "sealed base exceeds the sink-off row count");
            prefix_end = end + 1;
        }
        require(records(read(path), 'T') == expected_trades.substr(0, prefix_end),
                "seal must write exactly the sink-off prefix, with global row numbers");
        streamed.fill_report(&actual.value);
        compare_summary(expected.value, actual.value);
        require(actual.value.trades == nullptr && actual.value.trades_len == 0,
                "sink summary exposed an owned trade-history array");
        const auto bytes = read(path);
        require(records(bytes, 'T') == expected_trades,
                "sealed + resident + open T rows differ in values, global order or multiplicity");
        require(receipt(streamed).complete == 1
                && receipt(streamed).count_t == expected.value.total_trades,
                "final sink row count differs from the unsealed engine");
        streamed.fill_report(&again.value);
        compare_summary(actual.value, again.value);
        require(read(path) == bytes, "repeated read duplicated the sealed prefix or suffix");
        require(streamed.set_report_sink_v1(nullptr) == 0, "sink removal failed");
        Report unavailable;
        invariant([&] { streamed.fill_report(&unavailable.value); });
        require(unavailable.value.trades == nullptr,
                "array refusal allocated rows before noticing the sealed base");
    }
}

void seal_conditions() {
    Strategy host(Options{});
    const auto bars = tape(80);
    host.execute(bars);
    require(host.last_error().empty(),
            "seal-condition fixture run latched an engine error: " + latched_error(host)
                + " [" + host.progress() + "]");
    const auto final = host.consumer().closed_rows_final();
    require(final > 5, "seal-condition fixture lacks closed notifications");
    invariant([&] { host.consumer().seal_closed_rows(host, 1, 0, 0.0, 0.0); });
    TradeLedger rows;
    for (std::size_t i = 0; i < final + 5; ++i) {
        Trade row{};
        row.exit_time = start_ms + static_cast<std::int64_t>(i);
        row.entry_incarnation = i + 1;
        rows.push_back(row);
    }
    const auto open = start_ms + 100000;
    const auto choose = [&](std::size_t phases, std::uint64_t money,
                            std::optional<std::uint64_t> pinned = std::nullopt) {
        return host.consumer().closed_rows_sealable(rows, phases, money, open, pinned);
    };
    require(choose(rows.size(), rows.size()) == final, "condition 1: notification finality ignored");
    require(choose(2, rows.size()) == 2, "exit-phase finality ignored");
    require(choose(rows.size(), 1) == 1, "condition 5: money-fold mark ignored");
    require(choose(rows.size(), rows.size(), rows[1].entry_incarnation) == 1,
            "condition 4: mutable short-seed incarnation was retired");
    const auto old_time = rows[1].exit_time;
    rows[1].exit_time = open;
    require(choose(rows.size(), rows.size()) == 1, "condition 3: equal bar-open time was retired");
    rows[1].exit_time = open + 1;
    require(choose(rows.size(), rows.size()) == 1, "condition 3: future row was retired");
    rows[1].exit_time = old_time;
    rows.resize(final);
    for (std::size_t i = final - 3; i < final; ++i) rows[i].exit_time = open - 1;
    require(choose(rows.size(), rows.size()) == final - 4,
            "condition 2: exit-time tail or predecessor was retired");
    rows.seal(1);
    require(choose(rows.size(), rows.size()) == final - 4, "global indices rebased after retirement");
    for (auto& row : rows) row.exit_time = open - 1;
    require(choose(rows.size(), rows.size()) == rows.base(),
            "single exit-time block must remain resident");
    rows.clear();
    require(choose(0, 0) == 0, "empty ledger produced a seal");
    for (int i = 0; i < 2; ++i) {
        Trade row{};
        row.exit_time = start_ms + i;
        rows.push_back(row);
        require(choose(rows.size(), rows.size()) == 0, "short tail lost its predecessor");
    }
}

void phase_transform() {
    NativeExecutionConsumer consumer;
    std::vector<std::uint8_t> phases;
    std::uint64_t random = UINT64_C(0x3971);
    for (int step = 0; step < 97; ++step) {
        if (step != 0) {
            const auto phase = static_cast<std::uint8_t>((step * 3) % 5);
            phases.push_back(phase);
            consumer.fold_sealed_exit_phase(phase);
        }
        for (int trial = 0; trial < 64; ++trial) {
            random = random * UINT64_C(6364136223846793005) + 1;
            const std::uint64_t seed = trial < 8 ? static_cast<std::uint64_t>(trial) : random;
            BrokerStateHashSink reference;
            reference.h = seed;
            for (const auto phase : phases) reference.u(phase);
            require(consumer.replay_sealed_exit_phases(seed) == reference.h,
                    "bounded phase-prefix transform differs from full byte fold");
        }
    }
}

void restarts_and_reuse(Directory& directory) {
    Strategy baseline(Options{}), streamed(Options{});
    const auto path = directory.file();
    install(streamed, path);
    const auto bars = tape(146);
    for (auto* host : {&baseline, &streamed}) {
        host->execute(bars);
        require(host->last_error().empty(),
                std::string(host == &baseline ? "restart baseline" : "restart streamed")
                    + " fixture run latched an engine error: " + latched_error(*host)
                    + " [" + host->progress() + "]");
    }
    const auto base = streamed.ledger().base();
    require(base > 0 && base < streamed.consumer().closed_rows_final(),
            "restart fixture did not retain a final row at nonzero base");
    // Force the real adapter's reset-marker branch through its existing
    // public permutation operation; no private-member access or test switch.
    for (auto* host : {&baseline, &streamed}) host->clear_phase(base);
    require(streamed.consumer().exit_phases_restarted(), "exit-phase restart not exercised");
    require(baseline.extension_hash() == streamed.extension_hash(),
            "pre-refold exit-phase hash changed with storage split");
    const auto old = streamed.ledger()[base].pnl;
    for (auto* host : {&baseline, &streamed}) {
        (void)host->consumer().closed_rows_digest(host->ledger());
        host->ledger()[base].pnl += 0.125;
        require(!host->consumer().closed_rows_digest_holds(host->ledger()),
                "Debug witness missed an unannounced resident amendment");
        host->native_closed_rows_amended(base);
    }
    require(baseline.consumer().closed_rows_digest(baseline.ledger())
                == streamed.consumer().closed_rows_digest(streamed.ledger()),
            "closed-row amendment did not restart from its immutable prefix");
    for (auto* host : {&baseline, &streamed}) {
        host->ledger()[base].pnl = old;
        host->native_closed_rows_amended(base);
        (void)host->consumer().closed_rows_digest(host->ledger());
    }
    invariant([&] { streamed.native_closed_rows_amended(base - 1); });
    invariant([&] { streamed.clear_phase(base - 1); });
    invariant([&] { (void)streamed.ledger()[base - 1]; });
    invariant([&] { streamed.ledger().resize(base - 1); });
    invariant([&] { streamed.consumer().seal_closed_rows(streamed, base - 1, 0, 0.0, 0.0); });
    // Shrink to the base itself: the running digest must become the sealed
    // seed, and the Debug re-walk must accept an empty resident interval.
    streamed.ledger().resize(base);
    streamed.native_closed_rows_amended(base);
    require(streamed.consumer().closed_rows_digest(streamed.ledger())
                == streamed.consumer().closed_trade_prefix().rows_digest,
            "digest rewind to base did not restore the seed");
    require(streamed.consumer().closed_rows_digest_holds(streamed.ledger()),
            "Debug empty-tail re-fold failed");
    // A new sink run on the same host resets every prefix and report fold.
    Strategy fresh(Options{});
    install(fresh, directory.file());
    for (auto* host : {&streamed, &fresh}) {
        host->execute(bars);
        require(host->last_error().empty(),
                std::string(host == &streamed ? "reused streamed host" : "fresh host")
                    + " rerun latched an engine error: " + latched_error(*host)
                    + " [" + host->progress() + "]");
    }
    compare_observations(fresh, streamed);
    Report reused, expected;
    streamed.fill_report(&reused.value);
    fresh.fill_report(&expected.value);
    compare_summary(expected.value, reused.value);
    require(records(read(path), 'T') == records(read(directory.files.back()), 'T'),
            "reused host leaked its previous trade prefix");
}
} // namespace

int main() {
    int failures = 0;
    // Each step reports on its own and the next one still runs, so one lab
    // round names every broken case instead of only the first.
    const auto attempt = [&failures](const char* step, const std::function<void()>& action) {
        // Printed first, so a crash in any step (not only a differential case) names it.
        std::fprintf(stderr, "L8c: step %s\n", step);
        try {
            action();
        } catch (const std::exception& error) {
            ++failures;
            std::fprintf(stderr, "L8c: FAILED %s: %s\n", step, error.what());
        }
    };
    try {
        Directory directory;
        Options options;
        const auto run = [&](int count = 146) {
            attempt("differential", [&] { differential(directory, options, count); });
        };
        attempt("seal_conditions", seal_conditions);
        attempt("phase_transform", phase_transform);
        attempt("sealed_sink_capture_and_read", [&] { sealed_sink_capture_and_read(directory); });
        run();
        options.coof = options.pooc = options.brackets = true;
        run();
        options = {};
        options.zero_fee = options.rewind = true;
        run();
        options = {};
        options.variable = true;
        options.percent_fee = true;
        run();
        options = {};
        options.script_tf = "5";
        options.magnifier = true;
        run(432);
        options = {};
        options.observed = true;
        run();
        options = {};
        options.selected = true;
        run();
        options.empty = true;
        run(12);
        options = {};
        options.empty = true;
        run(0);
        options = {};
        options.script_tf = "5";
        options.magnifier = options.quote = true;
        run(432);
        options = {};
        options.fail = true;
        run();
        options = {};
        options.abort = true;
        run();
        attempt("restarts_and_reuse", [&] { restarts_and_reuse(directory); });
    } catch (const std::exception& error) {
        std::fprintf(stderr, "L8c: %s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "L8c: %d step(s) failed\n", failures);
        return 1;
    }
    std::puts("L8c seal, fold-restart and sink differential checks passed");
    return 0;
}
