// Written before the admission fix, using the base public API.
// Generated Pine calls this host with std::string operands. Drive its public
// run entry directly so embedded NUL reaches the real command door unchanged;
// this test needs neither a Pine compiler nor a C-string transport.
#include <pineforge/bar.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/selected_window.h>
#include <pineforge/source/pine_strategy_host.hpp>

#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kT0 = 1704067200000LL;
constexpr std::int64_t kMinute = 60000LL;
int checks = 0;
int failures = 0;
std::string row;

#define CHECK(expr) do {                                                       \
    ++checks;                                                                 \
    if (!(expr)) {                                                            \
        ++failures;                                                           \
        std::printf("FAIL %s: %s\n", row.c_str(), #expr);                        \
    }                                                                         \
} while (false)

std::vector<Bar> feed() {
    return {
        {100, 101, 99, 100, 1000, kT0},
        {100, 101, 99, 100, 1000, kT0 + kMinute},
        {100, 111, 99, 110, 1000, kT0 + 2 * kMinute},
        {110, 111, 109, 110, 1000, kT0 + 3 * kMinute},
    };
}

class ConfiguredHost : public source::PineStrategyHost {
public:
    ConfiguredHost() {
        attach_pine_execution_adapter();
        source::PineStrategyConfig config;
        config.initial_capital = 10000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 2.0;
        config.pyramiding = 2;
        config.process_orders_on_close = false;
        configure_pine_strategy(config);
        syminfo_mintick_ = 0.01;
        set_syminfo_metadata("qty_step", 1.0);
    }
    int attempted = 0;
    int returned = 0;
};

void run(ConfiguredHost& host, bool retained = false) {
    const auto bars = feed();
    if (retained) {
        pf_selected_window_config_v1 window{};
        window.struct_size = static_cast<std::uint32_t>(sizeof window);
        window.version = 1;
        window.start_ms = kT0 + kMinute;
        window.end_ms = kT0 + 4 * kMinute;
        CHECK(strategy_set_selected_window_v1(
            static_cast<BacktestEngine*>(&host), &window) == 0);
    }
    host.run(bars.data(), static_cast<int>(bars.size()), "1", "1", false);
}

void refused(ConfiguredHost& host, const char* field) {
    CHECK(host.native_state().kind == NativeLifecycleKind::Failed);
    CHECK(!host.last_error().empty());
    CHECK(std::string(run_failure_code_of(host)) == "pine_invalid_argument");
    const std::string name(field);
    const auto dot = name.find('.');
    const std::string expected = std::string("{\"argument\":\"") + name.substr(dot + 1)
        + "\",\"function\":\"strategy." + name.substr(0, dot)
        + "\",\"rule\":\"unsupported_combination\"}";
    CHECK(std::string(run_failure_args_of(host)) == expected);
    CHECK(run_failure_code_class(run_failure_value_of(host).code)
          == RunFailureClass::strategy);
    CHECK(host.trade_count() == 0);
    CHECK(host.live_position_size() == 0.0);
}

class CollidingExits final : public ConfiguredHost {
public:
    CollidingExits(char separator, bool from_first)
        : separator_(separator), from_first_(from_first) {}

    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        // Two different pairs:
        //   ("a<sep>b", "c") and ("a", "b<sep>c").
        // For US the adapter's exit/replacement strings are identical.
        // For NUL source_key hashes identical left + NUL + right bytes.
        const std::string left = std::string("a") + separator_ + "b";
        const std::string right = std::string("b") + separator_ + "c";
        for (bool second : {from_first_, !from_first_}) {
            ++attempted;
            strategy_exit(second ? "a" : left, second ? right : "c",
                          110.0, kNaN, kNaN, kNaN, kNaN, 100.0);
            ++returned;
        }
    }

private:
    char separator_;
    bool from_first_;
};

enum class Door { EntryId, EntryOca, OrderId, OrderOca, ExitId, ExitFrom,
                  ExitOca, CloseId, CloseTokenId, BracketId, BracketFrom, CancelId };
struct DoorCase { Door door; const char* field; };
constexpr DoorCase doors[] = {
    {Door::EntryId, "entry.id"}, {Door::EntryOca, "entry.oca_name"},
    {Door::OrderId, "order.id"}, {Door::OrderOca, "order.oca_name"},
    {Door::ExitId, "exit.id"}, {Door::ExitFrom, "exit.from_entry"},
    {Door::ExitOca, "exit.oca_name"}, {Door::CloseId, "close.id"},
    {Door::CloseTokenId, "close.id"},
    {Door::BracketId, "exit_cancel_bracket.exit_id"},
    {Door::BracketFrom, "exit_cancel_bracket.from_entry"}, {Door::CancelId, "cancel.id"},
};

class IdDoor final : public ConfiguredHost {
public:
    IdDoor(Door door, std::string text) : door_(door), text_(std::move(text)) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() != 0) return;
        ++attempted;
        switch (door_) {
            case Door::EntryId:
                strategy_entry(text_, true, kNaN, kNaN, 2.0); break;
            case Door::EntryOca:
                strategy_entry("L", true, kNaN, kNaN, 2.0, "", text_); break;
            case Door::OrderId:
                strategy_order(text_, true, 2.0, kNaN, kNaN); break;
            case Door::OrderOca:
                strategy_order("L", true, 2.0, kNaN, kNaN, text_); break;
            case Door::ExitId:
                strategy_exit(text_, "L", 110.0, kNaN, kNaN, kNaN, kNaN); break;
            case Door::ExitFrom:
                strategy_exit("X", text_, 110.0, kNaN, kNaN, kNaN, kNaN); break;
            case Door::ExitOca:
                strategy_exit("X", "L", 110.0, kNaN, kNaN, kNaN, kNaN,
                              100.0, "", kNaN, text_); break;
            case Door::CloseId: strategy_close(text_); break;
            case Door::CloseTokenId:
                strategy_close(text_, "", kNaN, kNaN, false, 17); break;
            case Door::BracketId: strategy_exit_cancel_bracket(text_, "L"); break;
            case Door::BracketFrom: strategy_exit_cancel_bracket("X", text_); break;
            case Door::CancelId: strategy_cancel(text_); break;
        }
        ++returned;
    }

private:
    Door door_;
    std::string text_;
};

class ValidIds final : public ConfiguredHost {
public:
    ValidIds(std::string entry_id, std::string exit_id, std::string oca,
             std::string comment = {})
        : entry_id_(std::move(entry_id)), exit_id_(std::move(exit_id)),
          oca_(std::move(oca)), comment_(std::move(comment)) {}
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() == 0)
            strategy_entry(entry_id_, true, kNaN, kNaN, 2.0, comment_, oca_);
        if (pine_bar_index() == 1) {
            // The repeated exit exercises replacement with the same valid ID.
            for (int reissue = 0; reissue < 2; ++reissue)
                strategy_exit(exit_id_, entry_id_, 110.0, kNaN, kNaN, kNaN,
                              kNaN, 100.0, comment_, kNaN, oca_);
        }
    }

private:
    std::string entry_id_, exit_id_, oca_, comment_;
};

void valid_trade(const std::string& entry, const std::string& exit,
                 const std::string& oca, const std::string& comment = {}) {
    ValidIds host(entry, exit, oca, comment);
    run(host);
    CHECK(host.native_state().kind == NativeLifecycleKind::Completed);
    CHECK(host.last_error().empty());
    CHECK(run_failure_value_of(host).code == RunFailureCode::none);
    CHECK(host.trade_count() == 1);
    CHECK(host.live_position_size() == 0.0);
    if (host.trade_count() != 1) return;
    const auto& trade = host.get_trade(0);
    // Hand arithmetic: buy 2 at the next open (100), sell 2 at the limit
    // (110) on bar 2; no commission/slippage, point value 1: 2*(110-100)=20.
    CHECK(trade.entry_id == entry);
    CHECK(trade.exit_id == exit);
    CHECK(trade.entry_comment == comment);
    CHECK(trade.exit_comment == comment);
    CHECK(trade.entry_time == kT0 + kMinute);
    CHECK(trade.exit_time == kT0 + 2 * kMinute);
    CHECK(trade.entry_price == 100.0);
    CHECK(trade.exit_price == 110.0);
    CHECK(trade.qty == 2.0);
    CHECK(trade.pnl == 20.0);
    CHECK(trade.commission == 0.0);
}

} // namespace

int main() {
    for (char separator : {'\x1f', '\0'}) {
        const std::string label = separator == '\0' ? "NUL" : "US";
        for (bool retained : {false, true}) {
            for (bool from_first : {false, true}) {
                row = label + "/colliding_exits/"
                    + (from_first ? "from_first/" : "id_first/")
                    + (retained ? "retained" : "ordinary");
                CollidingExits host(separator, from_first);
                run(host, retained);
                refused(host, from_first ? "exit.from_entry" : "exit.id");
                CHECK(host.attempted == (retained ? 2 : 1));
                CHECK(host.returned == (retained ? 2 : 0));
            }
            for (const auto& door : doors) {
                // Check both the length-aware interior and the end positions.
                const std::string values[] = {
                    std::string(1, separator) + "id",
                    std::string("i") + separator + "d",
                    std::string("id") + separator,
                };
                for (int index = 0; index < 3; ++index) {
                    row = label + "/" + door.field + "/" + std::to_string(index)
                        + (retained ? "/retained" : "/ordinary");
                    IdDoor host(door.door, values[index]);
                    run(host, retained);
                    refused(host, door.field);
                    CHECK(host.attempted == 1);
                    CHECK(host.returned == (retained ? 1 : 0));
                }
            }
        }
    }
    row = "plain_ids";
    valid_trade("Long", "TP", "Group");
    row = "spaces_utf8_quotes_punctuation";
    valid_trade(u8"Long α / \"quoted\" !", u8"TP β : 'quoted' ?",
                u8"Group γ , [one]");
    row = "comment_bytes_are_not_ids";
    valid_trade("Long", "TP", "Group", std::string("note") + '\0' + '\x1f' + "end");
    std::printf("test_pine_composite_id_admission: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
