#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
using namespace pineforge;
constexpr std::int64_t minute = 60000;

class Host final : public source::PineStrategyHost {
public:
    void configure_security_evaluators() override {
        register_security_eval(0, "1", "1", true, false);
    }
    void evaluate_security(int id, const Bar& bar, bool publish) override {
        // A conditional EMA first used when the previous chart bar is published.
        if (publish && current_request_delivery(id).replay) {
            closes.push_back(bar.close);
            values.push_back(security_series_slot_is_new(id) ? ema.compute(bar.close)
                                                            : ema.recompute(bar.close));
        } else if (!closes.empty()) {
            subsequent.push_back(ema.compute(bar.close));
        }
    }
    void on_source_bar(const Bar&) override {}
    ta::EMA ema{3};
    std::vector<double> closes, values, subsequent;
};
} // namespace

int main() {
    Host host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    host.set_syminfo_metadata("series_origin_ms", 3 * minute);
    host.set_syminfo_metadata("chart_ema_na_warmup", 0);
    // Minute 5 is absent. The chart bar opening at minute 3 can only be
    // sealed at the next boundary; its last available close, 6, is replayed.
    const Bar bars[] = {{1,1,1,1,1,3*minute}, {6,6,6,6,1,4*minute},
                        {12,12,12,12,1,6*minute}, {30,30,30,30,1,7*minute},
                        {8,8,8,8,1,8*minute}};
    host.run(bars, 5, "1", "3");
    if (!host.last_error().empty() || host.closes != std::vector<double>{6} ||
        host.values.size() != 1 || host.subsequent.size() != 3) {
        std::fprintf(stderr, "expected one replay of 6 then three updates; got %zu/%zu: %s\n",
                     host.values.size(), host.subsequent.size(), host.last_error().c_str());
        return 1;
    }
    // On the third call (6+12+30)/3 = 16; the next step is 16+(8-16)/2 = 12.
    if (!std::isnan(host.values[0]) || !std::isnan(host.subsequent[0]) ||
        host.subsequent[1] != 16 || host.subsequent[2] != 12) {
        std::fprintf(stderr, "requested EMA: got %g,%g,%g,%g; expected na,na,16,12\n",
                     host.values[0], host.subsequent[0], host.subsequent[1], host.subsequent[2]);
        return 1;
    }
    return 0;
}
