// Covered TV controls exhaust six broker fills, then resume exactly at the
// declared trading-session day boundary (17:00 New York, with DST). Constant
// synthetic prices isolate the risk clock from strategy signals and PnL.
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <cstdio>
#include <string>
#include <vector>

using namespace pineforge;
namespace {
int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL %d %s\n", __LINE__, #x); } } while (0)
constexpr int64_t hour = 3600000;
constexpr int64_t minute = 60000;

class SessionOrders : public pineforge::source::PineStrategyHost {
public:
    bool market_close;
    explicit SessionOrders(const std::string& display_zone, bool close_command = false)
        : market_close(close_command) {
        initial_capital_ = 1000000;
        default_qty_type_ = QtyType::FIXED;
        default_qty_value_ = 1;
        adapter_.cap = 6;
        process_orders_on_close_ = true;
        commission_value_ = 0;
        slippage_ = 0;
        syminfo_mintick_ = 0.01;
        syminfo_.pointvalue = 1;
        set_syminfo_timezone("America/New_York");
        set_syminfo_session("1700-1700");
        set_chart_timezone(display_zone);
        set_syminfo_metadata("intraday_cap_count_pooc_full_close_fills", 1.0);
    }
    void on_source_bar(const Bar&) override {
        if (market_close && signed_position_size() > 0) strategy_close("L");
        const bool request = bar_index_ == 0 || bar_index_ == 2 || bar_index_ == 4
            || (bar_index_ >= 6 && bar_index_ <= 10)
            || bar_index_ == 12 || bar_index_ == 14 || bar_index_ == 16;
        if (request && signed_position_size() == 0) {
            strategy_entry("L", true);
            if (!market_close)
                strategy_exit("X", "L", 101.0, std::numeric_limits<double>::quiet_NaN());
        }
    }
    const std::vector<Trade>& rows() const { return trades_; }
};

std::vector<Bar> session_bars(int64_t day, int reset_hour) {
    const int64_t offsets[] = {
        6*hour, 6*hour+15*minute, 8*hour, 8*hour+15*minute,
        10*hour, 10*hour+15*minute, 11*hour, 15*hour+45*minute,
        16*hour, reset_hour*hour-15*minute, reset_hour*hour,
        reset_hour*hour+15*minute, reset_hour*hour+30*minute,
        reset_hour*hour+45*minute, reset_hour*hour+60*minute,
        reset_hour*hour+75*minute, 24*hour, 24*hour+15*minute,
    };
    std::vector<Bar> bars;
    for (int64_t offset : offsets) bars.push_back({100, 101, 100, 100, 1, day+offset});
    return bars;
}

void test_session_boundary_uses_exchange_clock_and_dst() {
    struct Date { int64_t day; int reset_hour; };
    for (const Date date : {Date{1744243200000LL,21}, Date{1762128000000LL,22},
                            Date{1741305600000LL,22}, Date{1741564800000LL,21}}) {
        const auto bars = session_bars(date.day, date.reset_hour);
        for (const char* chart_zone : {"", "UTC", "Asia/Taipei", "America/New_York"}) {
          for (bool market_close : {false, true}) {
            SessionOrders engine(chart_zone, market_close);
            engine.run(bars.data(), static_cast<int>(bars.size()));
            CHECK(engine.rows().size() == 6);
            if (engine.rows().size() != 6) continue;
            CHECK(engine.rows()[0].entry_time == date.day+6*hour);
            CHECK(engine.rows()[1].entry_time == date.day+8*hour);
            CHECK(engine.rows()[2].entry_time == date.day+10*hour);
            CHECK(engine.rows()[3].entry_time == date.day+date.reset_hour*hour);
            CHECK(engine.rows()[4].entry_time == date.day+date.reset_hour*hour+30*minute);
            CHECK(engine.rows()[5].entry_time == date.day+date.reset_hour*hour+60*minute);
          }
        }
    }
}

class LegacyClock : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {}
    void exhaust_at(int64_t time) {
        adapter_.cap = 6;
        current_bar_.timestamp = time;
        _intraday_cap_currently_latched();
        for (int i = 0; i < 6; ++i) {
            adapter_.cap.pre_dispatch(pine_cap_clock(),
                pine_cap_calculation(),
                {compat::pine::OrderKind::Market, 0, 0, true,
                 compat::pine::Side::Flat, 0, 0}, 0);
        }
        adapter_.cap.after_immediate_close_attempt();
    }
    bool latched_at(int64_t time) {
        current_bar_.timestamp = time;
        return _intraday_cap_currently_latched();
    }
};

void test_continuous_and_unconfigured_sessions_keep_chart_clock() {
    for (const char* session : {"", "24x7", "regular"}) {
        LegacyClock utc;
        utc.set_syminfo_session(session);
        utc.exhaust_at(1744243200000LL+15*hour);
        CHECK(utc.latched_at(1744243200000LL+21*hour));
        CHECK(!utc.latched_at(1744243200000LL+24*hour));
        LegacyClock shifted;
        shifted.set_syminfo_session(session);
        shifted.set_chart_timezone("Asia/Taipei");
        shifted.exhaust_at(1744243200000LL+15*hour);
        CHECK(shifted.latched_at(1744243200000LL+15*hour+45*minute));
        CHECK(!shifted.latched_at(1744243200000LL+16*hour));
    }
}

// Covered ES holiday oracle: the May26 17:00 Chicago reopen accepts a fresh
// six-fill budget although time("D") still returns the May25 daily stamp.
// A broker counter must not inherit the optional merged indicator calendar.
void test_native_holiday_merge_does_not_hold_broker_limit() {
    constexpr int64_t sunday_open = 1748210400000LL;
    constexpr int64_t monday_open = sunday_open + 24*hour;
    constexpr int64_t tuesday_open = sunday_open + 48*hour;
    constexpr int64_t wednesday_open = sunday_open + 72*hour;
    NativeDayPartition partition;
    partition.tz = "America/Chicago";
    partition.session = "1700-1600";
    partition.stamps = {sunday_open, tuesday_open};
    partition.trade_day = {
        session_day_index(tuesday_open-hour, partition.tz, partition.session),
        session_day_index(wednesday_open-hour, partition.tz, partition.session),
    };
    partition.last_bound = wednesday_open-hour;
    NativeDayPartitionScope scope(&partition);
    const auto indicator_day = session_day_index(sunday_open, partition.tz, partition.session);
    CHECK(session_day_index(monday_open, partition.tz, partition.session) == indicator_day);
    for (const char* display_zone : {"UTC", "Asia/Taipei"}) {
        LegacyClock engine;
        engine.set_syminfo_timezone(partition.tz);
        engine.set_syminfo_session(partition.session);
        engine.set_chart_timezone(display_zone);
        engine.exhaust_at(sunday_open+hour);
        CHECK(engine.latched_at(monday_open-minute));
        CHECK(!engine.latched_at(monday_open));
        engine.exhaust_at(monday_open+hour);
        CHECK(engine.latched_at(tuesday_open-minute));
        CHECK(!engine.latched_at(tuesday_open));
    }
    CHECK(active_native_day_partition() == &partition);
    CHECK(session_day_index(monday_open, partition.tz, partition.session) == indicator_day);
}

void test_other_timed_sessions_resume_on_the_next_open() {
    struct Market {
        const char* timezone;
        const char* session;
        int64_t open;
        int64_t before_reopen;
    };
    constexpr int64_t day = 1748304000000LL; // May27 UTC
    for (const Market market : {
            Market{"America/New_York", "0930-1600", day+13*hour+30*minute, minute},
            Market{"America/Chicago", "1700-1600", day+22*hour, minute},
            // The metal market's daily stamp is17:00, in its closed hour;
            // inspect its last trading hour and its actual18:00 reopen.
            Market{"America/New_York", "1800-1700", day+22*hour, 2*hour}}) {
        LegacyClock engine;
        engine.set_syminfo_timezone(market.timezone);
        engine.set_syminfo_session(market.session);
        engine.set_chart_timezone("Asia/Taipei");
        engine.exhaust_at(market.open+hour);
        CHECK(engine.latched_at(market.open+24*hour-market.before_reopen));
        CHECK(!engine.latched_at(market.open+24*hour));
    }
}

// The spent slot belongs to one close, risk day, source bar, and exact pending
// order incarnation. None of those identities may independently bypass a
// latched day after the close has consumed its final slot.
void test_close_quota_transfer_requires_all_owners_and_consumes_once() {
    using compat::pine::OrderRiskDay;
    using compat::pine::QuotaAdmission;
    const OrderRiskDay first_day{101}, next_day{102};
    compat::pine::IntradayOrderBudget budget;
    CHECK(budget.admit_matched_attempt(first_day, 2, 7, 100, 0)
          == QuotaAdmission::BelowLimit);
    budget.count_committed_close(first_day, 2, 2, 8, 200);
    CHECK(budget.charged_slots() == 2);
    CHECK(budget.latched());
    CHECK(budget.can_inherit(first_day, 8, 200, 2));
    CHECK(!budget.can_inherit(first_day, 8, 201, 2));
    CHECK(!budget.can_inherit(first_day, 9, 200, 2));
    CHECK(!budget.can_inherit(next_day, 8, 200, 2));
    CHECK(!budget.can_inherit(first_day, 8, 200, 3));

    struct Attempt { int bar; uint64_t incarnation; uint64_t latest_fill; };
    for (const auto attempt : {Attempt{8, 201, 2}, Attempt{9, 200, 2},
                               Attempt{8, 200, 3}}) {
        auto wrong_owner = budget;
        CHECK(wrong_owner.admit_matched_attempt(first_day, 2, attempt.bar,
                  attempt.incarnation, attempt.latest_fill)
              == QuotaAdmission::Blocked);
        CHECK(wrong_owner.charged_slots() == 2);
        CHECK(wrong_owner.latched());
    }

    auto continued = budget;
    CHECK(continued.admit_matched_attempt(first_day, 2, 8, 200, 2)
          == QuotaAdmission::ReachedLimit);
    CHECK(continued.charged_slots() == 2);
    CHECK(!continued.transfer());
    CHECK(continued.admit_matched_attempt(first_day, 2, 8, 200, 2)
          == QuotaAdmission::Blocked);

    auto declined = budget;
    declined.decline(201); // Another declined attempt cannot spend this slot.
    CHECK(declined.can_inherit(first_day, 8, 200, 2));
    declined.decline(200);
    CHECK(!declined.transfer());
    CHECK(declined.charged_slots() == 2);
    CHECK(declined.latched());

    auto expired_batch = budget;
    expired_batch.expire_transfer();
    CHECK(expired_batch.admit_matched_attempt(first_day, 2, 8, 200, 2)
          == QuotaAdmission::Blocked);

    // A new close while already latched receives no debit. It therefore cannot
    // mint a new transfer or keep the earlier close's continuation alive.
    auto uncounted_close = budget;
    uncounted_close.count_committed_close(first_day, 2, 3, 8, 201);
    CHECK(uncounted_close.charged_slots() == 2);
    CHECK(!uncounted_close.transfer());
    CHECK(uncounted_close.admit_matched_attempt(first_day, 2, 8, 201, 3)
          == QuotaAdmission::Blocked);

    // Observing the same risk day leaves ownership intact. Renewing quota
    // retires the old transfer; its incarnation must spend a fresh slot.
    budget.enter_day(first_day);
    CHECK(budget.can_inherit(first_day, 8, 200, 2));
    budget.enter_day(next_day);
    CHECK(budget.charged_slots() == 0);
    CHECK(!budget.latched());
    CHECK(!budget.transfer());
    CHECK(budget.admit_matched_attempt(next_day, 2, 8, 200, 2)
          == QuotaAdmission::BelowLimit);
    CHECK(budget.charged_slots() == 1);
}
}

int main() {
    test_session_boundary_uses_exchange_clock_and_dst();
    test_continuous_and_unconfigured_sessions_keep_chart_clock();
    test_native_holiday_merge_does_not_hold_broker_limit();
    test_other_timed_sessions_resume_on_the_next_open();
    test_close_quota_transfer_requires_all_owners_and_consumes_once();
    std::printf("%d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
