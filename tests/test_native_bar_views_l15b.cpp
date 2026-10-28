// Borrowed symbol-feed export and host storage.
// The conditional declaration keeps the first witness linkable against the
// L15a tree: before the implementation, the missing runtime symbol is the
// intended red edge.
#include <pineforge/pineforge.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if !defined(PINEFORGE_HAS_SYMBOL_FEED_BORROWED_V1)
extern "C" int strategy_set_symbol_feed_borrowed_v1(
    pf_strategy_t, const char*, const char*, const pf_bar_t*,
    const std::int64_t*, int);
#endif

namespace {

int checks = 0;
int failures = 0;
const char* scenario = "initialization";

#define CHECK(expression)                                                        \
    do {                                                                         \
        ++checks;                                                                \
        if (!(expression)) {                                                     \
            ++failures;                                                          \
            std::printf("FAIL [%s] line %d: %s\n", scenario, __LINE__,         \
                        #expression);                                            \
        }                                                                        \
    } while (false)

constexpr std::int64_t kMinute = 60000;
constexpr std::int64_t kStart = 1744032600000LL;
const char* kSymbol = "SYN:BORROWED";
const char* kTimeframe = "1";

struct Row {
    double close = std::nan("");
    double time = std::nan("");
    double column = std::nan("");
};

struct Feed {
    std::vector<pf_bar_t> bars;
    std::vector<std::int64_t> close_ms;
    std::vector<double> column;
};

class Reader final : public pineforge::source::PineStrategyHost {
public:
    std::vector<Row> rows;
    Row value;

    void configure_security_evaluators() override {
        security_eval_states_.clear();
        value = Row{};
        register_security_eval(0, kSymbol, kTimeframe, input_tf_, false, false, false);
    }

    void evaluate_security(int, const pineforge::Bar& bar, bool) override {
        value.close = bar.close;
        value.time = static_cast<double>(bar.timestamp);
        value.column = security_column_value(0, "carry");
    }

    void clear_security(int) override { value = Row{}; }

    void on_source_bar(const pineforge::Bar&) override { rows.push_back(value); }
};

pf_strategy_t handle(pineforge::BacktestEngine& engine) {
    return static_cast<void*>(&engine);
}

void prime(Reader& reader) {
    reader.set_syminfo_timezone("Etc/UTC");
    reader.set_syminfo_session("24x7");
    reader.set_syminfo_type("crypto");
    reader.set_syminfo_string("tickerid", "SYN:CHART");
    reader.set_syminfo_string("ticker", "CHART");
}

std::vector<pineforge::Bar> chart() {
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < 6; ++i) {
        const double open = 100.0 + static_cast<double>(i);
        bars.push_back({open, open + 0.5, open - 0.5, open + 0.25,
                        1000.0 + static_cast<double>(i), kStart + i * kMinute});
    }
    return bars;
}

Feed feed(int shift = 0) {
    Feed result;
    for (int i = 0; i < 6; ++i) {
        const std::int64_t open = kStart + (i - 1) * kMinute;
        const double close = 40.0 + static_cast<double>(i + shift);
        result.bars.push_back({close - 0.25, close + 0.5, close - 0.5, close,
                               std::nan(""), open});
        result.close_ms.push_back(open + kMinute);
        result.column.push_back(10.0 + static_cast<double>(i + shift));
    }
    return result;
}

bool install_copied(Reader& reader, const Feed& tape) {
    const pf_strategy_t strategy = handle(reader);
    return strategy_set_symbol_feed(strategy, kSymbol, kTimeframe,
                                    tape.bars.data(), tape.close_ms.data(),
                                    static_cast<int>(tape.bars.size())) == 0
        && strategy_set_symbol_feed_column(strategy, kSymbol, kTimeframe, "carry",
                                           tape.column.data(),
                                           static_cast<int>(tape.column.size())) == 0;
}

bool install_borrowed(Reader& reader, const Feed& tape) {
    const pf_strategy_t strategy = handle(reader);
    return strategy_set_symbol_feed_borrowed_v1(
               strategy, kSymbol, kTimeframe, tape.bars.data(), tape.close_ms.data(),
               static_cast<int>(tape.bars.size())) == 0
        && strategy_set_symbol_feed_column(strategy, kSymbol, kTimeframe, "carry",
                                           tape.column.data(),
                                           static_cast<int>(tape.column.size())) == 0;
}

bool run(Reader& reader, const std::vector<pineforge::Bar>& bars) {
    reader.rows.clear();
    reader.run(bars.data(), static_cast<int>(bars.size()), kTimeframe, kTimeframe,
               false, 4, pineforge::MagnifierDistribution::ENDPOINTS);
    return reader.last_error().empty();
}

bool same(double left, double right) {
    if (std::isnan(left) || std::isnan(right)) return std::isnan(left) && std::isnan(right);
    return left == right;
}

void copied_and_borrowed_are_equal() {
    scenario = "copied versus borrowed";
    const Feed tape = feed();
    const std::vector<pineforge::Bar> bars = chart();
    Reader copied;
    Reader borrowed;
    prime(copied);
    prime(borrowed);
    CHECK(install_copied(copied, tape));
    CHECK(install_borrowed(borrowed, tape));
    CHECK(run(copied, bars));
    CHECK(run(borrowed, bars));
    CHECK(copied.rows.size() == borrowed.rows.size());
    for (std::size_t i = 0; i < copied.rows.size() && i < borrowed.rows.size(); ++i) {
        CHECK(same(copied.rows[i].close, borrowed.rows[i].close));
        CHECK(same(copied.rows[i].time, borrowed.rows[i].time));
        CHECK(same(copied.rows[i].column, borrowed.rows[i].column));
    }
    CHECK(copied.native_continuation_hash() == borrowed.native_continuation_hash());
    const auto copied_state = copied.native_state();
    const auto borrowed_state = borrowed.native_state();
    CHECK(copied_state.spec != nullptr && borrowed_state.spec != nullptr);
    if (copied_state.spec != nullptr && borrowed_state.spec != nullptr) {
        CHECK(copied_state.spec->instrument_feeds.size() == 1);
        CHECK(borrowed_state.spec->instrument_feeds.size() == 1);
        CHECK(copied_state.spec->instrument_feeds[0].bars.size() == tape.bars.size());
        CHECK(borrowed_state.spec->instrument_feeds[0].bars.empty());
        CHECK(borrowed_state.spec->instrument_feeds[0].close_ms.empty());
    }
}

void replacement_and_empty_feed_keep_lifetime_rules() {
    scenario = "replacement and empty borrowed feed";
    const Feed first = feed();
    const Feed replacement = feed(100);
    const std::vector<pineforge::Bar> bars = chart();
    Reader reader;
    prime(reader);
    CHECK(install_borrowed(reader, first));
    CHECK(run(reader, bars));
    CHECK(!reader.rows.empty() && same(reader.rows.front().close, first.bars[1].close));
    CHECK(install_borrowed(reader, replacement));
    CHECK(run(reader, bars));
    CHECK(!reader.rows.empty() && same(reader.rows.front().close, replacement.bars[1].close));
    CHECK(strategy_set_symbol_feed_borrowed_v1(handle(reader), kSymbol, kTimeframe,
                                               nullptr, nullptr, 0) == 0);
    CHECK(run(reader, bars));
    for (const Row& row : reader.rows) {
        CHECK(std::isnan(row.close));
        CHECK(std::isnan(row.column));
    }
}

void malformed_arguments_are_refused_before_install() {
    scenario = "borrowed argument refusals";
    Reader reader;
    prime(reader);
    const Feed tape = feed();
    const pf_strategy_t strategy = handle(reader);
    CHECK(strategy_set_symbol_feed_borrowed_v1(nullptr, kSymbol, kTimeframe,
                                               tape.bars.data(), tape.close_ms.data(),
                                               static_cast<int>(tape.bars.size())) == -1);
    CHECK(strategy_set_symbol_feed_borrowed_v1(strategy, kSymbol, kTimeframe,
                                               nullptr, tape.close_ms.data(), 1) == -1);
    CHECK(strategy_set_symbol_feed_borrowed_v1(strategy, kSymbol, kTimeframe,
                                               tape.bars.data(), nullptr, 1) == -1);
    CHECK(strategy_set_symbol_feed_borrowed_v1(strategy, kSymbol, kTimeframe,
                                               tape.bars.data(), tape.close_ms.data(), -1) == -1);
    pf_bar_t invalid = tape.bars.front();
    invalid.high = std::nan("");
    CHECK(strategy_set_symbol_feed_borrowed_v1(strategy, kSymbol, kTimeframe,
                                               &invalid, tape.close_ms.data(), 1) == -1);
    CHECK(std::string(reader.last_error()).find("strategy_set_symbol_feed_borrowed_v1")
          != std::string::npos);
}

}  // namespace

int main() {
    copied_and_borrowed_are_equal();
    replacement_and_empty_feed_keep_lifetime_rules();
    malformed_arguments_are_refused_before_install();
    std::printf("test_native_bar_views_l15b: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
