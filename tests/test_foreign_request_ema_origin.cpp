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
        register_security_eval(0, "TEST:OTHER", "1", "1", false, false, false);
    }
    void evaluate_security(int, const Bar& bar, bool) override {
        values.push_back(ema.compute(bar.close));
    }
    void on_source_bar(const Bar&) override {}

    ta::EMA ema{3};
    std::vector<double> values;
};

bool check(bool origin) {
    Host host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    if (origin) {
        host.set_syminfo_metadata("series_origin_ms", minute);
        host.set_syminfo_metadata("chart_ema_na_warmup", 0);
    }
    const Bar chart[] = {{1,1,1,1,1,minute}, {1,1,1,1,1,2*minute},
                         {1,1,1,1,1,3*minute}, {1,1,1,1,1,4*minute}};
    const Bar feed[] = {{6,6,6,6,1,minute}, {12,12,12,12,1,2*minute},
                        {30,30,30,30,1,3*minute}};
    const std::int64_t closes[] = {2*minute, 3*minute, 4*minute};
    if (!host.set_symbol_feed("TEST:OTHER", "1", feed, closes, 3)) {
        std::fprintf(stderr, "symbol feed refused: %s\n", host.last_error().c_str());
        return false;
    }
    host.run(chart, 4, "1", "1");
    if (!host.last_error().empty() || host.values.size() != 3) {
        std::fprintf(stderr, "expected three requested bars, got %zu: %s\n",
                     host.values.size(), host.last_error().c_str());
        return false;
    }
    // (6+12+30)/3 = 16. With a first-price seed, alpha=1/2 gives 6, 9, 19.5.
    const double expected[] = {origin ? na<double>() : 6, origin ? na<double>() : 9,
                               origin ? 16.0 : 19.5};
    for (int i = 0; i < 3; ++i) {
        const double got = host.values[i];
        if (!(got == expected[i] || (std::isnan(got) && std::isnan(expected[i])))) {
            std::fprintf(stderr, "origin=%d requested bar %d: got %g, expected %g\n",
                         origin, i, got, expected[i]);
            return false;
        }
    }
    return true;
}
} // namespace

int main() { return check(false) && check(true) ? 0 : 1; }
