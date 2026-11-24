#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace {
using namespace pineforge;

struct Period {
    const char* literal;
    std::int64_t seconds;
};

constexpr Period month{"M", -1};
constexpr Period long_week{"3551W", 3551LL * 7 * 86400};
constexpr Bar original{10, 14, 8, 12, 30, 0};
constexpr Bar replacement{20, 25, 19, 21, 40, 86400000};

class Host final : public NativeStrategyHost {
public:
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {}

    bool holds_original(Period period) const {
        if (native_security_feeds_.size() != 1) return false;
        const auto& feed = native_security_feeds_.front();
        if (feed.tf != period.literal || feed.seconds != period.seconds || feed.bars.size() != 1)
            return false;
        const Bar& bar = feed.bars.front();
        return bar.timestamp == original.timestamp && bar.open == original.open
            && bar.high == original.high && bar.low == original.low
            && bar.close == original.close && bar.volume == original.volume;
    }
};

bool check_refusal(const char* literal, Period seed, bool clear) {
    Host host;
    const char* operation = clear ? "clear" : "install";
    if (!host.set_native_security_feed(seed.literal, &original, 1) || !host.holds_original(seed)) {
        std::printf("NOT COMPARED %s '%s': could not seed '%s'\n", operation, literal, seed.literal);
        return false;
    }
    const bool accepted = host.set_native_security_feed(
        literal, clear ? nullptr : &replacement, clear ? 0 : 1);
    const char* code = run_failure_code_of(host);
    const char* args = run_failure_args_of(host);
    const bool intact = host.holds_original(seed);
    const bool ok = !accepted && intact && std::strcmp(code, "security_feed_refused") == 0
        && std::strcmp(args, R"({"reason":"native_feed_timeframe"})") == 0;
    std::printf("%s %s '%s': accepted=%d code='%s' args='%s' intact=%d\n",
                ok ? "PASS" : "FAIL", operation, literal, accepted, code, args, intact);
    return ok;
}

bool check_valid(Period period) {
    Host host;
    if (!host.set_native_security_feed(period.literal, &original, 1) || !host.holds_original(period)) {
        std::printf("FAIL install '%s': expected original bar under its duration key\n", period.literal);
        std::printf("NOT COMPARED clear '%s': install failed\n", period.literal);
        return false;
    }
    const bool ok = host.set_native_security_feed(period.literal, nullptr, 0)
        && !host.native_security_feed_enabled();
    std::printf("%s valid '%s': installed original bar, cleared=%d\n",
                ok ? "PASS" : "FAIL", period.literal, ok);
    return ok;
}
}  // namespace

int main() {
    bool ok = true;
    for (bool clear : {false, true}) {
        if (!check_refusal("badM", month, clear)) ok = false;
        if (!check_refusal("3551xW", long_week, clear)) ok = false;
    }
    const char* const refused[] = {
        "0M", "01M", "2147483648M", "-1S", "+1W", " 1W", "1xD", "15xS", "1H",
        "S", "35791395"  // Bare S and a minute duration past int already refuse.
    };
    for (const char* literal : refused)
        if (!check_refusal(literal, month, false)) ok = false;

    const Period valid[] = {
        month, {"1M", -1}, {"3M", -1}, {"12M", -1},
        {"W", 7 * 86400}, long_week, {"24856D", 24856LL * 86400},
        {"1S", 1}, {"15S", 15}, {"2147483647S", 2147483647},
        {"35791394", 35791394LL * 60}
    };
    for (Period period : valid)
        if (!check_valid(period)) ok = false;
    return ok ? 0 : 1;
}
