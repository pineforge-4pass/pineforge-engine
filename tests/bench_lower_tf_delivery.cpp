// Standalone comparison for release archives, including ones predating the
// caller catalog. No timers or benchmark thresholds belong to the CTest row.
#include <pineforge/source/pine_strategy_host.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

namespace {
using namespace pineforge;

class Host final : public source::PineStrategyHost {
public:
    Host(bool request, bool audit) : request_(request), audit_(audit) {
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_type("crypto");
        set_syminfo_mintick(0.125);
        configure_pine_strategy(source::PineStrategyConfig{});
    }
    void configure_security_evaluators() override {
        security_eval_states_.clear();
        if (request_) register_security_lower_tf_eval(0, "1", input_tf_);
    }
    void evaluate_security(int id, const Bar& bar, bool publish) override {
        if (security_lower_tf_sub_bar_index(id) == 0) closes_.clear();
        closes_.push_back(bar.close);
        valid = valid && id == 0 && publish;
        ++callbacks;
    }
    void on_source_bar(const Bar& bar) override {
        if (request_)
            valid = valid && closes_.size() == 240 && closes_.back() == bar.close;
        if (audit_) {
            std::printf("bar %d %lld %zu", charts, static_cast<long long>(bar.timestamp),
                        closes_.size());
            for (double value : closes_) std::printf(" %a", value);
            std::putchar('\n');
        }
        if (valid && charts % 32 == 0) strategy_entry("a", true);
        if (charts % 32 == 13) strategy_close("a");
        ++charts;
    }
    int charts = 0, callbacks = 0;
    bool valid = true;
private:
    bool request_, audit_;
    std::vector<double> closes_;
};
}

int main(int argc, char** argv) {
    if (argc != 3 || (std::string(argv[1]) != "request" && std::string(argv[1]) != "control")
        || (std::string(argv[2]) != "time" && std::string(argv[2]) != "audit")) {
        std::fprintf(stderr, "usage: bench_lower_tf_delivery control|request time|audit\n");
        return 2;
    }
    const bool request = std::string(argv[1]) == "request", audit = std::string(argv[2]) == "audit";
    const int count = audit ? 128 : 12'288;
    std::vector<pineforge::Bar> bars;
    for (int i = 0; i < count; ++i) {
        const double open = 43.0 + (i * 11 % 47) * 0.125;
        bars.push_back({open, open + 2.75, open - 1.5, open + (i % 5 - 2) * 0.375,
                        731.0 + i % 19, (1440LL + 240LL * i) * 60'000});
    }
    Host host(request, audit);
    const auto start = std::chrono::steady_clock::now();
    host.run(bars.data(), count, "240", "240");
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    const int expected = request ? 240 * count : 0;
    if (!host.last_error().empty() || !host.valid || host.charts != count
        || host.callbacks != expected || host.trade_count() != count / 32) {
        std::fprintf(stderr, "NOT COMPARED: valid %d/1, charts %d/%d, callbacks %d/%d,"
                     " trades %d/%d, error '%s'\n", host.valid, host.charts, count,
                     host.callbacks, expected, host.trade_count(), count / 32, host.last_error().c_str());
        return 1;
    }
    for (int i = 0; i < host.trade_count(); ++i) {
        const auto t = host.get_trade(i);
        std::printf("trade %d %lld %lld %a %a %a %a %a %d %d %d %s %s %s %s %d %a %a %a %llu %d %d\n",
                    i, static_cast<long long>(t.entry_time), static_cast<long long>(t.exit_time),
                    t.entry_price, t.exit_price, t.qty, t.pnl, t.pnl_pct, t.is_long,
                    t.entry_bar_index, t.exit_bar_index, t.entry_id.c_str(), t.exit_id.c_str(),
                    t.entry_comment.c_str(), t.exit_comment.c_str(), t.exit_from_bracket,
                    t.max_runup, t.max_drawdown, t.commission,
                    static_cast<unsigned long long>(t.entry_incarnation), t.open_at_end,
                    static_cast<int>(t.close_cause));
    }
    std::printf("charts %d callbacks %d trades %d\n", host.charts, host.callbacks, host.trade_count());
    if (!audit) std::fprintf(stderr, "%s %.6f seconds\n", argv[1], elapsed.count());
    return std::fflush(stdout) == 0 && !std::ferror(stdout) ? 0 : 1;
}
