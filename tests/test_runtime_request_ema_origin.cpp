#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/ta.hpp>

#include <cmath>
#include <cstdio>
#include <optional>
#include <vector>

namespace {
using namespace pineforge;
constexpr std::int64_t minute = 60000;

struct Sample {
    std::int64_t open;
    double ema;
};

class Host final : public source::PineStrategyHost {
public:
    void on_source_bar(const Bar& bar) override {
        if (!cursor) cursor.emplace(make_runtime_security_cursor("2", false, false, false));
        const auto index = static_cast<std::uint64_t>(pine_bar_index());
        cursor->feed({bar, index, index + 1}, [&](const Bar& requested, bool) {
            source::PineRuntimeSecurityScope context(*this, *cursor);
            samples.push_back({requested.timestamp, ema.compute(requested.close)});
        }, [] {});
        chart.push_back(chart_ema.compute(bar.close));
    }

    std::optional<source::PineRuntimeSecurityCursor> cursor;
    ta::EMA ema{3}, chart_ema{3};
    std::vector<Sample> samples;
    std::vector<double> chart;
};

bool check(bool declared) {
    Host host;
    host.set_syminfo_timezone("UTC");
    host.set_syminfo_session("24x7");
    if (declared) host.set_syminfo_metadata("series_origin_ms", minute);
    host.set_syminfo_metadata("chart_ema_na_warmup", declared ? 0.0 : 1.0);
    // The first 2-minute bucket opened at 0, before the first chart input.
    // Its sole close is 120. The three whole buckets close at 6, 12, 30.
    const double closes[] = {120, 4, 6, 10, 12, 20, 30};
    std::vector<Bar> bars;
    for (double c : closes)
        bars.push_back({c,c,c,c,1,static_cast<std::int64_t>(bars.size()+1)*minute});
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    if (!host.last_error().empty() || host.chart.size() != bars.size()) {
        std::fprintf(stderr, "expected seven chart bars, got %zu: %s\n",
                     host.chart.size(), host.last_error().c_str());
        return false;
    }
    if (declared ? host.chart.front() != 120 : !std::isnan(host.chart.front())) {
        std::fprintf(stderr, "wrong chart EMA after requested callback\n");
        return false;
    }
    // A declared origin gives (6+12+30)/3 = 16. With only chart warmup
    // enabled the independent requested EMA starts at 120 and halves each gap.
    const std::vector<Sample> expected = declared
        ? std::vector<Sample>{{2*minute, na<double>()}, {4*minute, na<double>()},
                              {6*minute, 16}}
        : std::vector<Sample>{{0, 120}, {2*minute, 63}, {4*minute, 37.5},
                              {6*minute, 33.75}};
    if (host.samples.size() != expected.size()) {
        std::fprintf(stderr, "declared=%d: got %zu requested bars, expected %zu\n",
                     declared, host.samples.size(), expected.size());
        return false;
    }
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const auto got = host.samples[i], want = expected[i];
        if (got.open != want.open ||
            !(got.ema == want.ema || (std::isnan(got.ema) && std::isnan(want.ema)))) {
            std::fprintf(stderr, "declared=%d row %zu: got (%lld,%g), expected (%lld,%g)\n",
                         declared, i, static_cast<long long>(got.open), got.ema,
                         static_cast<long long>(want.open), want.ema);
            return false;
        }
    }
    return true;
}
} // namespace

int main() { return check(true) && check(false) ? 0 : 1; }
