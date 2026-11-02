#include <pineforge/cache_policy.hpp>
#include <pineforge/report_sink.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace feed_security_test {
using namespace pineforge;
using TapeRow = source::RecordedSeriesRow;
using Tape = pineforge::detail::FeedProjection<TapeRow>;
using CalendarColumn = pineforge::detail::FeedProjection<std::int64_t>;
constexpr std::int64_t origin = 1704067200000LL;
constexpr std::int64_t step = 6 * 3600000LL;
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::printf("FAIL line %d: %s\n", __LINE__, #condition); ++failures; \
} } while (false)

template<class T>
void append(std::string& bytes, const T& value) {
    bytes.append(reinterpret_cast<const char*>(&value), sizeof(value));
}

double tape_value(int i) {
    if (i % 7 == 2 || i % 31 == 5) return std::numeric_limits<double>::quiet_NaN();
    if (i % 29 == 0) return -0.0;
    return ((i * 37) % 257 - 128) * 0.125;
}

bool same_bits(double a, double b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

Bar bar_at(std::int64_t time, int i, double base) {
    const double price = base + ((i * 13) % 211) * 0.125;
    return {price, price + 0.75, price - 0.5, price + (i % 3 - 1) * 0.125,
            1.0 + i % 19, time};
}

// Serialize every public report field, excluding addresses and padding.
void trade_stats(std::string& bytes, const pf_trade_stats_t& s) {
#define FIELD(name) append(bytes, s.name)
    FIELD(num_trades); FIELD(num_wins); FIELD(num_losses); FIELD(num_even);
    FIELD(percent_profitable); FIELD(net_profit); FIELD(net_profit_pct);
    FIELD(gross_profit); FIELD(gross_profit_pct); FIELD(gross_loss); FIELD(gross_loss_pct);
    FIELD(profit_factor); FIELD(avg_trade); FIELD(avg_trade_pct); FIELD(avg_win);
    FIELD(avg_win_pct); FIELD(avg_loss); FIELD(avg_loss_pct); FIELD(ratio_avg_win_avg_loss);
    FIELD(largest_win); FIELD(largest_win_pct); FIELD(largest_loss); FIELD(largest_loss_pct);
    FIELD(commission_paid); FIELD(expectancy); FIELD(max_consecutive_wins);
    FIELD(max_consecutive_losses); FIELD(avg_bars_in_trade); FIELD(avg_bars_in_wins);
    FIELD(avg_bars_in_losses);
#undef FIELD
}

std::string report_bytes(const ReportC& r) {
    std::string bytes;
#define FIELD(name) append(bytes, r.name)
    FIELD(total_trades); FIELD(trades_len); FIELD(net_profit);
    FIELD(input_bars_processed); FIELD(script_bars_processed); FIELD(security_feeds_total);
    FIELD(security_eval_complete_total); FIELD(security_eval_partial_total);
    FIELD(magnifier_sub_bars_total); FIELD(magnifier_sample_ticks_total);
    FIELD(input_tf_seconds); FIELD(script_tf_seconds); FIELD(script_tf_ratio);
    FIELD(needs_aggregation); FIELD(bar_magnifier_enabled); FIELD(security_diag_len);
    FIELD(trace_len); FIELD(trace_names_len); FIELD(equity_curve_len); FIELD(broker_state_hash_len);
#undef FIELD
    for (int i = 0; i < r.trades_len; ++i) {
        const auto& t = r.trades[i];
#define FIELD(name) append(bytes, t.name)
        FIELD(entry_time); FIELD(exit_time); FIELD(entry_price); FIELD(exit_price);
        FIELD(pnl); FIELD(pnl_pct); FIELD(is_long); FIELD(max_runup); FIELD(max_drawdown);
        FIELD(qty); FIELD(commission); FIELD(entry_bar_index); FIELD(exit_bar_index);
        FIELD(open_at_end);
#undef FIELD
    }
    for (int i = 0; i < r.security_diag_len; ++i) {
        const auto& d = r.security_diag[i];
        append(bytes, d.sec_id); append(bytes, d.feed_count);
        append(bytes, d.eval_complete_count); append(bytes, d.eval_partial_count);
    }
    for (int i = 0; i < r.trace_names_len; ++i) {
        const std::string name(r.trace_names[i]);
        append(bytes, name.size());
        bytes += name;
    }
    for (int i = 0; i < r.trace_len; ++i) {
        const auto& t = r.trace[i];
        append(bytes, t.timestamp); append(bytes, t.bar_index);
        append(bytes, t.name_id); append(bytes, t.value);
    }
    trade_stats(bytes, r.metrics.all);
    trade_stats(bytes, r.metrics.longs);
    trade_stats(bytes, r.metrics.shorts);
    const auto& e = r.metrics.equity;
#define FIELD(name) append(bytes, e.name)
    FIELD(max_equity_drawdown); FIELD(max_equity_drawdown_pct); FIELD(max_equity_runup);
    FIELD(max_equity_runup_pct); FIELD(buy_hold_return); FIELD(buy_hold_return_pct);
    FIELD(sharpe_monthly); FIELD(sortino_monthly); FIELD(sharpe_bar); FIELD(sortino_bar);
    FIELD(cagr); FIELD(calmar); FIELD(recovery_factor); FIELD(time_in_market_pct); FIELD(open_pl);
#undef FIELD
    for (std::int64_t i = 0; i < r.equity_curve_len; ++i) {
        const auto& p = r.equity_curve[i];
        append(bytes, p.time_ms); append(bytes, p.equity); append(bytes, p.open_profit);
    }
    for (std::int64_t i = 0; i < r.broker_state_hash_len; ++i)
        append(bytes, r.broker_state_hash[i]);
    return bytes;
}

class Probe final : public source::PineStrategyHost {
public:
    Probe(std::size_t calendar_capacity, bool lookahead, bool gaps, bool coof)
        : calendar_capacity_(calendar_capacity), lookahead_(lookahead), gaps_(gaps) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_string("tickerid", "TEST:BASE");
        set_syminfo_mintick(0.01);
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.calc_on_order_fills = coof;
        configure_pine_strategy(config);
        set_broker_state_hash_recording(true);
        set_trace_enabled(true);
        declare_outputs(4, 1, 1);
        // Declaring slots leaves recording off; O/V need it enabled before run begin.
        CHECK(set_outputs_enabled(true));
    }
    std::string observations;
    std::size_t callbacks = 0;
    bool calendar_installed = false;
    std::array<std::uint64_t, 3> calendar_reads_before{};

    void configure_security_evaluators() override {
        observations.clear();
        callbacks = 0;
        calendar_installed = false;
        evaluations_.fill(0);
        security_eval_states_.clear();
        latest_.fill(std::numeric_limits<double>::quiet_NaN());
        register_security_eval(0, "D", input_tf_, lookahead_, gaps_);
        register_security_eval(1, "W", input_tf_, lookahead_, gaps_);
        register_security_eval(2, "M", input_tf_, lookahead_, gaps_);
        register_security_eval(3, "720", input_tf_, lookahead_, gaps_);
        register_security_eval(4, "TEST:OTHER", "360", input_tf_, false, false);
    }
    void evaluate_security(int id, const Bar& bar, bool) override {
        latest_.at(static_cast<std::size_t>(id)) = bar.close;
        ++evaluations_.at(static_cast<std::size_t>(id));
    }
    void clear_security(int id) override {
        latest_.at(static_cast<std::size_t>(id)) = std::numeric_limits<double>::quiet_NaN();
    }
    void on_source_bar(const Bar& bar) override {
        if (callbacks == 0) {
            calendar_installed = !chart_day_partition_.empty();
            chart_day_partition_.trade_day.set_capacity_bytes(calendar_capacity_);
            chart_day_partition_.week_open.set_capacity_bytes(calendar_capacity_);
            chart_day_partition_.month_open.set_capacity_bytes(calendar_capacity_);
            std::size_t column_index = 0;
            for (const auto* column : {&chart_day_partition_.trade_day,
                                      &chart_day_partition_.week_open,
                                      &chart_day_partition_.month_open}) {
                const auto disk = std::dynamic_pointer_cast<
                    const pineforge::detail::ScratchFeedReader<std::int64_t>>(column->reader());
                CHECK(disk != nullptr);
                calendar_reads_before[column_index++] = disk ? disk->read_calls() : 0;
            }
            output_run_begin();
            output_constant(0, -0.0);
        }
        const int i = static_cast<int>((bar.timestamp - origin) / step);
        const double value = recorded_series_value("tape");
        CHECK(same_bits(value, tape_value(i)));
        ++callbacks;
        append(observations, bar.timestamp);
        append(observations, value);
        for (double latest : latest_) append(observations, latest);
        for (auto period : {CalendarPeriod::DAY, CalendarPeriod::WEEK, CalendarPeriod::MONTH}) {
            append(observations, session_period_open_ms(bar.timestamp, "UTC", "24x7", period));
            append(observations, session_period_close_ms(bar.timestamp, "UTC", "24x7", period));
            append(observations,
                   session_period_last_traded_close_ms(bar.timestamp, "UTC", "24x7", period));
        }
        // Revisit both ends, forcing collisions in each one-page column.
        if (calendar_installed && i % 13 == 0) {
            const auto last = chart_day_partition_.stamps.size() - 1;
            for (auto index : {std::size_t{0}, last, std::size_t{0}}) {
                append(observations, chart_day_partition_.trade_day[index]);
                append(observations, chart_day_partition_.week_open[index]);
                append(observations, chart_day_partition_.month_open[index]);
            }
        }
        trace("tape", value);
        trace("daily", latest_[0]);
        output_bar(bar.timestamp, bar.timestamp + step);
        output_value(0, value);
        output_value(1, latest_[0]);
        output_value(2, latest_[1]);
        output_value(3, latest_[2]);
        if (i % 11 == 1 && std::isfinite(value) && std::isfinite(latest_[0])
            && std::isfinite(latest_[4])) {
            strategy_entry("E", value + latest_[0] - latest_[4] > 0);
            output_event(0, value, "entry");
        }
        if (i % 11 == 8) strategy_close_all();
    }
    const Tape& tape() const { return recorded_series_.at("tape").rows; }
    const NativeDayPartition& calendar() const { return chart_day_partition_; }
    const std::array<std::size_t, 5>& evaluations() const { return evaluations_; }
private:
    std::size_t calendar_capacity_;
    bool lookahead_, gaps_;
    std::array<double, 5> latest_{};
    std::array<std::size_t, 5> evaluations_{};
};

struct Inputs {
    std::vector<Bar> chart, daily, other;
    std::vector<std::int64_t> closes, opens;
    std::vector<double> values;
    std::shared_ptr<const FeedReader<TapeRow>> reader;
    Inputs() {
        constexpr int days = 1100;
        pineforge::detail::FeedProjectionBuilder<TapeRow> builder;
        for (int i = 0; i < 4 * days; ++i) {
            const auto time = origin + static_cast<std::int64_t>(i) * step;
            chart.push_back(bar_at(time, i, 100.0));
            other.push_back(bar_at(time, i + 17, 80.0));
            closes.push_back(time + step);
            if (i % 4 == 0) daily.push_back(bar_at(time, i / 4, 200.0));
            // Absent chart opens and installed NaNs are distinct paths.
            if (i % 7 != 2) {
                opens.push_back(time);
                values.push_back(tape_value(i));
                builder.append(TapeRow{time, tape_value(i)});
            }
        }
        reader = builder.finish(0).reader();
    }
};

struct Snapshot {
    std::string report, sink, observations;
    std::uint64_t final_hash = 0;
    bool operator==(const Snapshot& other) const {
        return report == other.report && sink == other.sink
            && observations == other.observations && final_hash == other.final_hash;
    }
};

Snapshot run(const Inputs& input, std::size_t calendar_capacity, std::size_t tape_capacity,
             bool lookahead, bool gaps, bool coof, bool reader, bool sink) {
    Probe host(calendar_capacity, lookahead, gaps, coof);
    CHECK(host.set_native_security_feed("D", input.daily.data(),
                                        static_cast<int>(input.daily.size())));
    CHECK(host.set_symbol_feed_borrowed("TEST:OTHER", "360", input.other.data(),
                                        input.closes.data(), static_cast<int>(input.other.size())));
    if (reader) {
        CHECK(host.set_recorded_series_reader("tape", input.reader, tape_capacity));
    } else {
        CHECK(host.set_recorded_series("tape", input.opens.data(), input.values.data(),
                                       static_cast<int>(input.opens.size()), tape_capacity));
    }
    CHECK(host.tape().capacity_bytes() == tape_capacity);
    const auto disk = std::dynamic_pointer_cast<const pineforge::detail::ScratchFeedReader<TapeRow>>(
        host.tape().reader());
    CHECK(disk != nullptr);
    const auto physical_reads_before_run = disk ? disk->read_calls() : 0;
    std::string path;
    if (sink) {
        std::string refusal;
        int directory_error = 0;
        path = pineforge::detail::resolve_scratch_directory({}, refusal, directory_error);
        if (path.empty()) throw std::runtime_error("fixture scratch directory: " + refusal);
        if (path.back() != '/') path += '/';
        path += "pineforge-feed-security-XXXXXX";
        const int fd = ::mkstemp(path.data());
        CHECK(fd >= 0);
        if (fd >= 0) ::close(fd);
        const pf_report_sink_v1 descriptor{
            sizeof(descriptor), 1, path.c_str(), "feed-projections", nullptr};
        CHECK(host.set_report_sink_v1(&descriptor) == 0);
    }
    host.run(input.chart.data(), static_cast<int>(input.chart.size()), "360", "360",
             false, 4, MagnifierDistribution::ENDPOINTS);
    if (!host.last_error().empty())
        std::printf("run error: %s\n", host.last_error().c_str());
    CHECK(host.last_error().empty());
    CHECK(host.callbacks >= input.chart.size());
    if (coof) CHECK(host.callbacks > input.chart.size());
    for (auto calls : host.evaluations()) CHECK(calls > 0);
    CHECK(host.calendar_installed);
    CHECK(host.tape().capacity_bytes() == tape_capacity);
    CHECK(host.tape().stats().resident_bytes <= tape_capacity);
    CHECK(host.calendar().trade_day.stats().resident_bytes <= calendar_capacity);
    if (tape_capacity == Tape::page_bytes()) {
        CHECK(host.tape().stats().evictions > 0);
        CHECK(host.tape().cache_stats().evictions > 0);
    }
    if (calendar_capacity == CalendarColumn::page_bytes()) {
        CHECK(host.calendar().trade_day.stats().evictions > 0);
        CHECK(host.calendar().week_open.stats().evictions > 0);
        CHECK(host.calendar().month_open.stats().evictions > 0);
    }
    CHECK(disk && disk->read_calls() > 1);
    if (tape_capacity <= Tape::page_bytes())
        CHECK(disk && disk->read_calls() > physical_reads_before_run);
    CHECK(host.tape().cache_stats().get_calls > 0);
    std::size_t column_index = 0;
    for (const auto* column : {&host.calendar().trade_day, &host.calendar().week_open,
                              &host.calendar().month_open}) {
        const auto calendar_disk = std::dynamic_pointer_cast<
            const pineforge::detail::ScratchFeedReader<std::int64_t>>(column->reader());
        CHECK(calendar_disk != nullptr);
        CHECK(calendar_disk
              && calendar_disk->read_calls() > host.calendar_reads_before[column_index]);
        CHECK(column->cache_stats().get_calls > 0);
        ++column_index;
        if (calendar_disk && calendar_capacity <= CalendarColumn::page_bytes()) {
            const auto first = (*column)[0];
            const auto before_last = calendar_disk->read_calls();
            (void)(*column)[column->size() - 1];
            CHECK(calendar_disk->read_calls() == before_last + 1);
            const auto before_first = calendar_disk->read_calls();
            CHECK((*column)[0] == first);
            CHECK(calendar_disk->read_calls() == before_first + 1);
        }
    }
    ReportC report{};
    host.fill_report(&report);
    CHECK(report.total_trades > 0);
    Snapshot result;
    result.report = report_bytes(report);
    result.observations = host.observations;
    result.final_hash = host.broker_state_hash();
    if (sink) {
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof(receipt);
        receipt.version = 1;
        CHECK(host.report_sink_receipt_v1(&receipt) == 0);
        CHECK(receipt.complete == 1);
        CHECK(receipt.count_t > 0 && receipt.count_e > 0 && receipt.count_h > 0);
        CHECK(receipt.count_o > 0 && receipt.count_v > 0 && receipt.count_x > 0);
        // Recalculations replace the current row; each distinct chart open emits one O.
        CHECK(receipt.count_o == static_cast<std::int64_t>(input.chart.size()));
        CHECK(receipt.count_k == 1); // One run-constant record at successful finalization.
        std::ifstream file(path, std::ios::binary);
        result.sink.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        CHECK(!result.sink.empty());
        ::unlink(path.c_str());
    }
    BacktestEngine::free_report(&report);
    return result;
}

class FaultReader final : public FeedReader<TapeRow> {
public:
    explicit FaultReader(std::shared_ptr<const FeedReader<TapeRow>> source)
        : source_(std::move(source)) {}
    mutable std::size_t calls = 0;
    std::size_t fail_on_call = 0;
    std::size_t size() const noexcept override { return source_->size(); }
    void read(std::size_t first, TapeRow* destination, std::size_t count) const override {
        if (++calls == fail_on_call)
            throw pineforge::detail::AuxiliaryScratchIoError(ENOSPC, "fixture feed backing");
        source_->read(first, destination, count);
    }
private:
    std::shared_ptr<const FeedReader<TapeRow>> source_;
};

class DoorProbe final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    double read(const std::string& key, std::int64_t time) {
        current_bar_.timestamp = time;
        return recorded_series_value(key);
    }
    std::uint64_t digest() const { return symbol_data_digest_; }
};

void doors_and_failure_atomicity(const Inputs& input) {
    DoorProbe host;
    const std::int64_t opens[] = {1000, 2000};
    const double values[] = {-0.0, 7.5};
    CHECK(host.set_recorded_series("tape", opens, values, 2));
    const auto before = host.digest();
    auto fault = std::make_shared<FaultReader>(input.reader);
    // Validation reads every page once. Fail on the next physical read,
    // which starts the staged digest from the beginning with one cache page.
    fault->fail_on_call = input.reader->size() / Tape::rows_per_page()
        + (input.reader->size() % Tape::rows_per_page() != 0) + 1;
    bool failed = false;
    try { (void)host.set_recorded_series_reader("tape", fault, Tape::page_bytes()); }
    catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        failed = error.code().value() == ENOSPC;
    }
    CHECK(failed);
    CHECK(fault->calls == fault->fail_on_call);
    CHECK(host.digest() == before);
    CHECK(same_bits(host.read("tape", 1000), -0.0));
    CHECK(same_bits(host.read("tape", 2000), 7.5));
    fault->fail_on_call = 0;
    CHECK(host.set_recorded_series_reader("tape", fault, 0));
    CHECK(same_bits(host.read("tape", input.opens.front()), input.values.front()));
    const auto installed = host.digest();
    pineforge::detail::FeedProjectionBuilder<TapeRow> unordered;
    unordered.append(TapeRow{2000, 1.0});
    unordered.append(TapeRow{1000, 2.0});
    CHECK(!host.set_recorded_series_reader("tape", unordered.finish(0).reader(), 0));
    CHECK(host.digest() == installed);
    CHECK(!host.set_recorded_series_reader("tape", nullptr));
    CHECK(host.digest() == installed);

    // Insertion before/between/after keys and replacement reproduce map order.
    DoorProbe a, b;
    for (const auto* key : {"z", "a", "m"}) CHECK(a.set_recorded_series(key, opens, values, 2, 0));
    for (const auto* key : {"a", "m", "z"}) CHECK(b.set_recorded_series(key, opens, values, 2));
    CHECK(a.digest() == b.digest());
    CHECK(a.set_recorded_series("m", input.opens.data(), input.values.data(),
                                static_cast<int>(input.opens.size()), Tape::page_bytes()));
    CHECK(b.set_recorded_series_reader("m", input.reader));
    CHECK(a.digest() == b.digest());
    CHECK(a.set_symbol_facts("TEST:OTHER", "type", "crypto"));
    CHECK(b.set_symbol_facts("TEST:OTHER", "type", "crypto"));
    CHECK(a.digest() == b.digest()); // later refresh folds the same paged bytes

    std::weak_ptr<const FeedReader<TapeRow>> lifetime;
    {
        pineforge::detail::FeedProjectionBuilder<TapeRow> builder;
        builder.append(TapeRow{1000, -0.0});
        builder.append(TapeRow{2000, 9.5});
        const auto projection = builder.finish(0);
        lifetime = projection.reader();
        CHECK(host.set_recorded_series_reader("owned", projection.reader(), 0));
    }
    CHECK(!lifetime.expired());
    CHECK(same_bits(host.read("owned", 2000), 9.5));
    CHECK(host.set_recorded_series("owned", nullptr, nullptr, 0, 0));
    CHECK(lifetime.expired());
}

class CapacityProbe final : public source::PineStrategyHost {
public:
    CapacityProbe() {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        configure_pine_strategy(config);
    }
    std::string observations;
    std::size_t callbacks = 0;
    void on_source_bar(const Bar& bar) override {
        const int i = static_cast<int>((bar.timestamp - origin) / step);
        for (const auto& tape : recorded_series_) {
            const auto value = recorded_series_value(tape.first);
            CHECK(same_bits(value, tape_value(i)));
            append(observations, value);
        }
        ++callbacks;
    }
    const Tape& tape(const char* key) const { return recorded_series_.at(key).rows; }
};

void check_tape_capacity(const Tape& tape, std::size_t capacity) {
    const auto line = Tape::page_bytes();
    const auto payload = Tape::page_payload_bytes();
    const auto pages = tape.size() / Tape::rows_per_page()
        + (tape.size() % Tape::rows_per_page() != 0);
    const auto slots = std::min(capacity / line, pages);
    CHECK(tape.capacity_bytes() == capacity);
    CHECK(tape.stats().resident_bytes <= capacity);
    CHECK(tape.cache_stats().current_bytes == slots * line);
    CHECK(tape.cache_stats().current_payload_bytes == slots * payload);
    CHECK(tape.cache_stats().current_metadata_bytes == slots * (line - payload));
    CHECK(tape.cache_stats().get_calls > 0);
    const auto disk = std::dynamic_pointer_cast<
        const pineforge::detail::ScratchFeedReader<TapeRow>>(tape.reader());
    CHECK(disk != nullptr);
    if (!disk) return;
    const auto first = tape[0];
    const auto before_repeat = disk->read_calls();
    CHECK(same_bits(tape[0].value, first.value));
    CHECK(disk->read_calls() == before_repeat + (slots == 0 ? 1 : 0));
    if (slots < pages) {
        // Page number == slot count collides with page zero. In bypass mode,
        // use page one; both reads must still reach the physical provider.
        const auto collision = std::max(std::size_t{1}, slots) * Tape::rows_per_page();
        const auto before_collision = disk->read_calls();
        (void)tape[collision];
        CHECK(disk->read_calls() == before_collision + 1);
        const auto before_first = disk->read_calls();
        const auto reread = tape[0];
        CHECK(reread.open_ms == first.open_ms);
        CHECK(same_bits(reread.value, first.value));
        CHECK(disk->read_calls() == before_first + 1);
        if (slots) CHECK(tape.cache_stats().evictions > 0);
        else CHECK(tape.cache_stats().bypass_reads > 0);
    }
}

void tape_capacity_admission(const Inputs& input) {
    const auto line = Tape::page_bytes();
    const auto normal = pineforge::detail::kFeedProjectionCacheBytes;
    const std::array<const char*, 4> keys{
        "copy-explicit", "copy-inherited", "reader-explicit", "reader-inherited"};
    // The existing security sweep retains its full input. This extra fixture
    // uses a chart prefix; collision reads still address the complete tape.
    const auto count = 3 * Tape::rows_per_page() + 1;
    CHECK(input.chart.size() >= count);
    std::string expected;
    for (std::size_t i = 0; i < count; ++i)
        for (std::size_t k = 0; k < keys.size(); ++k)
            append(expected, tape_value(static_cast<int>(i)));
    for (const auto capacity : std::array<std::size_t, 7>{
            0, line - 1, line, 2 * line - 1, 2 * line, normal, normal + line}) {
        CapacityProbe host;
        const auto rows = static_cast<int>(input.opens.size());
        CHECK(host.set_recorded_series(keys[0], input.opens.data(), input.values.data(),
                                       rows, capacity));
        CHECK(host.set_recorded_series(keys[1], input.opens.data(), input.values.data(), rows));
        CHECK(host.set_recorded_series_reader(keys[2], input.reader, capacity));
        CHECK(host.set_recorded_series_reader(keys[3], input.reader));
        const auto exercise = [&](std::size_t run_capacity,
                                  const std::array<std::size_t, 4>& budgets) {
            std::array<std::size_t, 4> previous_capacity{}, previous_resident{};
            for (std::size_t k = 0; k < keys.size(); ++k) {
                previous_capacity[k] = host.tape(keys[k]).capacity_bytes();
                previous_resident[k] = host.tape(keys[k]).stats().resident_bytes;
            }
            CacheDefaults policy;
            policy.recorded_series_pages = {run_capacity};
            host.set_cache_policy(policy);
            for (std::size_t k = 0; k < keys.size(); ++k) {
                CHECK(host.tape(keys[k]).capacity_bytes() == previous_capacity[k]);
                CHECK(host.tape(keys[k]).stats().resident_bytes == previous_resident[k]);
            }
            host.observations.clear();
            host.callbacks = 0;
            host.run(input.chart.data(), static_cast<int>(count), "360", "360",
                     false, 4, MagnifierDistribution::ENDPOINTS);
            CHECK(host.last_error().empty());
            CHECK(host.callbacks == count);
            CHECK(host.observations == expected);
            for (std::size_t k = 0; k < keys.size(); ++k)
                check_tape_capacity(host.tape(keys[k]), budgets[k]);
        };
        for (const auto run_capacity : {normal, std::size_t{0}, 3 * line - 1})
            exercise(run_capacity, {capacity, run_capacity, capacity, run_capacity});

        // Rejected replacements cannot change either capacity selection.
        const std::int64_t unordered[] = {2000, 1000};
        const double values[] = {1.0, 2.0};
        CHECK(!host.set_recorded_series(keys[0], unordered, values, 2));
        CHECK(!host.set_recorded_series(keys[1], unordered, values, 2, normal));
        CHECK(!host.set_recorded_series_reader(keys[2], nullptr));
        CHECK(!host.set_recorded_series_reader(keys[3], nullptr, 0));
        exercise(line, {capacity, line, capacity, line});

        // Replacing without bytes restores inheritance; supplying bytes opts
        // out, even when the explicit value equals the internal default.
        CHECK(host.set_recorded_series(keys[0], input.opens.data(), input.values.data(), rows));
        CHECK(host.set_recorded_series(keys[1], input.opens.data(), input.values.data(),
                                       rows, normal));
        CHECK(host.set_recorded_series_reader(keys[2], input.reader));
        CHECK(host.set_recorded_series_reader(keys[3], input.reader, 0));
        exercise(2 * line, {2 * line, normal, 2 * line, 0});
    }
}
} // namespace feed_security_test

int main() {
    using namespace feed_security_test;
    const Inputs input;
    doors_and_failure_atomicity(input);
    tape_capacity_admission(input);
    // One axis at a time, then all tiny/bypassed. Each row compares complete
    // report scalars/arrays, per-callback reads, hashes and (when enabled)
    // the complete E/T/O/V/K/X/H stream including its trailer.
    for (bool sink : {false, true}) {
        for (int mode = 0; mode < 3; ++mode) {
            const bool lookahead = mode == 1, gaps = mode == 2, coof = mode == 2;
            const auto normal = pineforge::detail::kFeedProjectionCacheBytes;
            const auto reference = run(input, normal, normal, lookahead, gaps, coof, false, sink);
            CHECK(run(input, CalendarColumn::page_bytes(), normal,
                      lookahead, gaps, coof, false, sink) == reference);
            CHECK(run(input, normal, Tape::page_bytes(),
                      lookahead, gaps, coof, true, sink) == reference);
            CHECK(run(input, CalendarColumn::page_bytes(), Tape::page_bytes(),
                      lookahead, gaps, coof, false, sink) == reference);
            CHECK(run(input, 0, 0, lookahead, gaps, coof, true, sink) == reference);
        }
    }
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
