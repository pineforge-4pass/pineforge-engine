#include <pineforge/bar.hpp>
#include <pineforge/na.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <iostream>
#include <tuple>
#include <vector>

namespace {

using pineforge::Bar;
using Group = std::tuple<std::int64_t, double, double, bool>;  // first child, open, close, complete
constexpr std::int64_t hour = 3'600'000;
constexpr std::int64_t day = 24 * hour;

class DailyReader final : public pineforge::source::PineStrategyHost {
public:
    void configure_security_evaluators() override {
        register_security_eval(0, "D", input_tf_, true, false, false, true);
    }

    void evaluate_security(int, const Bar& bar, bool complete) override {
        groups.emplace_back(bar.timestamp, bar.open, bar.close, complete);
        visible = bar.close;
    }
    void clear_security(int) override { visible = pineforge::na<double>(); }
    void on_source_bar(const Bar& bar) override {
        times.push_back(bar.timestamp);
        reads.push_back(visible);
    }

    std::vector<Group> groups;
    std::vector<std::int64_t> times;
    std::vector<double> reads;
    double visible = pineforge::na<double>();
};

int check(const char* row, const char* zone, const std::vector<std::int64_t>& times,
          const std::vector<Group>& groups, const std::vector<double>& reads,
          const std::vector<Bar>& native = {}, bool forming = false) {
    DailyReader host;
    host.set_syminfo_session("24x7");
    host.set_syminfo_timezone(zone);
    host.set_syminfo_type("crypto");
    std::vector<Bar> bars;
    for (const auto time : times) {
        const double open = 10.0 * (1 + bars.size());
        bars.push_back({open, open + 2, open - 1, open + 1, 1, time});
    }
    if (bars.empty() || groups.empty() || reads.size() != bars.size()) {
        std::cout << row << " NOT COMPARED: incomplete fixture\n";
        return 1;
    }
    if (!native.empty()
        && !host.set_native_security_feed("D", native.data(), static_cast<int>(native.size()))) {
        std::cout << row << " NOT COMPARED: native feed refused: " << host.last_error() << '\n';
        return 1;
    }
    const int count = static_cast<int>(bars.size());
    if (forming && !host.set_realtime_tail(true, count)) {
        std::cout << row << " NOT COMPARED: forming tail refused: " << host.last_error() << '\n';
        return 1;
    }
    host.run(bars.data(), count, "60", "60");
    if (!host.last_error().empty()) {
        std::cout << row << " NOT COMPARED: " << host.last_error() << '\n';
        return 1;
    }
    const bool ok = host.groups == groups && host.times == times && host.reads == reads;
    std::cout << row << (ok ? " PASS\n" : " FAIL\n");
    if (!ok) {
        for (const auto& [time, open, close, complete] : host.groups)
            std::cout << "  group " << time << " open=" << open << " close=" << close
                      << " complete=" << complete << '\n';
        for (std::size_t i = 0; i < host.reads.size(); ++i)
            std::cout << "  read " << host.times[i] << " close=" << host.reads[i] << '\n';
    }
    return !ok;
}

}  // namespace

int main() {
    // Bar opens are 10, 20, 30 in input order; closes are 11, 21, 31.
    // Jan 2 2024 is 54 ordinary years + 13 leap days + 1 day after Jan 1 1970.
    constexpr auto jan2 = (54 * 365 + 13 + 1) * day;
    int failures = 0;
    failures += check("utc-epoch", "UTC", {-hour, 0, hour},
                      {{-hour, 10, 11, true}, {0, 20, 31, false}}, {11, 31, 31});
    // Raw chart labels can carry milliseconds: 23:59:59.999 is still December.
    failures += check("utc-final-millisecond", "UTC", {-1, 0},
                      {{-1, 10, 11, true}, {0, 20, 21, false}}, {11, 21});
    failures += check("utc-2024", "UTC", {jan2 - hour, jan2, jan2 + hour},
                      {{jan2 - hour, 10, 11, true}, {jan2, 20, 31, false}}, {11, 31, 31});

    // Etc/GMT+1 is UTC minus one hour. The same three bars open at 22:00,
    // 23:00 and 00:00 local; UTC midnight is still in the first group.
    constexpr auto west = "Etc/GMT+1";
    failures += check("west-epoch", west, {-hour, 0, hour},
                      {{-hour, 10, 21, true}, {hour, 30, 31, false}}, {21, 21, 31});
    failures += check("west-2024", west, {jan2 - hour, jan2, jan2 + hour},
                      {{jan2 - hour, 10, 21, true}, {jan2 + hour, 30, 31, false}}, {21, 21, 31});
    // With a gap in the hourly tape these are Dec 30 23:00,
    // Dec 31 23:00 and Jan 1 00:00. The middle group starts at Unix zero.
    failures += check("west-sparse-epoch", west, {-day, 0, hour},
                      {{-day, 10, 11, true}, {0, 20, 21, true}, {hour, 30, 31, false}},
                      {11, 21, 31});

    failures += check("utc-starts-at-zero", "UTC", {0, hour},
                      {{0, 10, 21, false}}, {21, 21});
    failures += check("utc-ends-at-zero", "UTC", {-hour, 0},
                      {{-hour, 10, 11, true}, {0, 20, 21, false}}, {11, 21});
    failures += check("utc-last-hour-closes-at-zero", "UTC", {-2 * hour, -hour},
                      {{-2 * hour, 10, 21, true}}, {21, 21});
    failures += check("utc-forming-at-zero", "UTC", {-hour, 0},
                      {{-hour, 10, 11, true}, {0, 20, 21, false}}, {11, 21}, {}, true);

    // The second supplied day starts at 01:00 UTC. Its predecessor includes
    // the 00:00 child. Neither daily price can come from the hourly bars.
    failures += check("native-day-spans-zero", "UTC", {-hour, 0, hour},
                      {{-hour, 80, 83, true}, {hour, 90, 94, false}}, {83, 83, 94},
                      {{80, 84, 79, 83, 7, -day}, {90, 95, 89, 94, 8, hour}});
    return failures != 0;
}
