#include <pineforge/timeframe.hpp>

#include <cstdint>
#include <cstdio>

int main() {
    // 11:00 in Hong Kong on 2024-01-15 is 03:00 UTC.
    constexpr std::int64_t minute = 60'000;
    constexpr std::int64_t hour_open = 1'705'276'800'000LL + 3 * 60 * minute;
    pineforge::TimeframeAggregator hour("60", "7", "Asia/Hong_Kong", "0930-1200,1300-1600");
    int publications = 0;
    // Start at 09:30. The 11:00 cell receives 11:01, :08, :15, :22,
    // :29, :36, :43, :50 and :57. Their prices and volumes are 14..22.
    for (int i = 0; i < 22; ++i) {
        const int offset = -90 + 7 * i;
        const double value = i + 1;
        const auto step = hour.feed({value, value, value, value, value,
                                     hour_open + offset * minute});
        if (offset < 0 || !step.is_complete) continue;
        ++publications;
        if (offset != 57 || step.bar.timestamp != hour_open || step.sub_bar_count != 9
            || step.bar.close != 22 || step.bar.volume != 162) {
            std::fprintf(stderr, "11:%02d: open %lld, inputs %d, close %.0f, volume %.0f; "
                         "expected 11:57: open %lld, inputs 9, close 22, volume 162\n",
                         offset, static_cast<long long>(step.bar.timestamp), step.sub_bar_count,
                         step.bar.close, step.bar.volume, static_cast<long long>(hour_open));
            return 1;
        }
    }
    if (publications != 1) {
        std::fprintf(stderr, "expected one completed 11:00 bar, got %d\n", publications);
        return 1;
    }
    return 0;
}
