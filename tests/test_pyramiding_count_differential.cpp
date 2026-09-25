/*
 * test_pyramiding_count_differential.cpp — H-MEASURE row M13 / G2-15.
 *
 * Pine pyramiding is adapter command policy: PineExecutionAdapter::entry()
 * counts `accepted_in_cycle` (the cycle's opened origins that no close path or
 * owned bracket has drained, plus every live same-side opening order) when the
 * strategy.entry command is written, and project() leaves the kernel's
 * NativeRunSpec::max_open_lots unset (ADR-0001 ruling row `max_open_lots`,
 * design row MG3). This row measures that policy three ways on the same bars
 * and the same commands:
 *
 *   ADAPTER  a PineStrategyHost with pyramiding = 2;
 *   KERNEL   a bare NativeStrategyHost with max_open_lots = 2 (a surviving +
 *            new physical-lot cap checked at the match);
 *   TAPE     TradingView's own trades (`lab tv`, NYSE:F 15m, ws-report-v1,
 *            rangeProof covered; tests/fixtures/pyramiding_count).
 *
 * Nine scenarios, each on its own session, flat at its start, closed by
 * strategy.close_all. Every outcome is one canonical string (rows sorted by
 * entry bar, exit bar, id), pinned for the adapter and the kernel, and each
 * scenario asserts which of the two TradingView agrees with:
 *
 *   P1  close(qty) closes lot 1 whole, re-add         adapter = kernel = TV
 *   P2  close(qty) inside a lot, re-add               adapter = kernel = TV
 *   P3  close one id of two, third id                 adapter = kernel = TV
 *   P4  resting limit entry, then a market add        adapter != TV = kernel
 *   P4b the resting limit fills after the market add  adapter != TV != kernel
 *   P6  three market entries on one flat bar          adapter != TV = kernel
 *   P8  exit(qty) from B drains lot A (FIFO), third   adapter != TV = kernel
 *   P9  exit(qty) from A drains lot A, third id       adapter = kernel = TV
 *   P10 exit from B, default quantity, two lots open  adapter = kernel = TV
 *
 * P10 is not a pyramiding question: it is the quantity a default
 * strategy.exit(from_entry) closes under FIFO with two lots open, which the
 * tape's scenario exposed (TradingView closes B's 100, the adapter closed the
 * book until R5 lane H-THIN reserved the entry's own quantity).
 */

#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

using namespace pineforge;
namespace no = pineforge::native_order;

static int tests_passed = 0;
static int tests_failed = 0;

#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #expr);     \
            ++tests_failed;                                                    \
        } else {                                                               \
            ++tests_passed;                                                    \
        }                                                                      \
    } while (0)

#ifndef PINEFORGE_PYRAMIDING_FIXTURE_DIR
#error "PINEFORGE_PYRAMIDING_FIXTURE_DIR must name tests/fixtures/pyramiding_count"
#endif

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr int kBars = 12;
constexpr std::int64_t kBarMs = 900'000;

struct FeedBar {
    std::int64_t ts;
    double open, high, low, close;
};

#include "fixtures/pyramiding_count/bars.inc"

// ── one canonical outcome ──────────────────────────────────────────────

struct Row {
    std::string id;
    double qty;
    int entry_bar;
    double entry_price;
    int exit_bar;
    double exit_price;
};

std::string canonical(std::vector<Row> rows) {
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.entry_bar != b.entry_bar) return a.entry_bar < b.entry_bar;
        if (a.exit_bar != b.exit_bar) return a.exit_bar < b.exit_bar;
        if (a.id != b.id) return a.id < b.id;
        return a.qty < b.qty;
    });
    std::string out;
    for (const auto& r : rows) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s %g @%d:%.2f->%d:%.2f; ", r.id.c_str(), r.qty,
                      r.entry_bar, r.entry_price, r.exit_bar, r.exit_price);
        out += buf;
    }
    return out;
}

std::vector<Row> engine_rows(const BacktestEngine& e, std::int64_t s0) {
    std::vector<Row> rows;
    for (int i = 0; i < e.report_trade_count(); ++i) {
        const Trade& t = e.get_report_trade(i);
        rows.push_back({t.entry_id, t.qty, static_cast<int>((t.entry_time - s0) / kBarMs),
                        t.entry_price, static_cast<int>((t.exit_time - s0) / kBarMs),
                        t.exit_price});
    }
    return rows;
}

// ── the commands, per signal-bar offset ────────────────────────────────

enum class Op { Entry, LimitEntry, StopEntry, Close, CloseAll, Cancel, StopExit };

struct Cmd {
    int bar;
    Op op;
    const char* id;
    double qty = kNaN;      // Entry/Close/StopExit quantity (NaN: default)
    double level = kNaN;    // LimitEntry limit / StopExit stop
    const char* from = "";  // StopExit from_entry
};

struct Scenario {
    const char* key;
    const char* tape;
    std::vector<Cmd> cmds;
    bool adapter_matches_tv;
    bool kernel_matches_tv;
    const char* adapter;  // pinned outcome
    const char* kernel;   // pinned outcome
};

// ── ADAPTER ────────────────────────────────────────────────────────────

class PineProbe final : public source::PineStrategyHost {
public:
    explicit PineProbe(const std::vector<Cmd>& cmds) : cmds_(cmds) {
        source::PineStrategyConfig c;
        c.initial_capital = 100'000.0;
        c.default_qty_type = static_cast<int>(QtyType::FIXED);
        c.default_qty_value = 100.0;
        c.commission_value = 0.0;
        c.commission_type = static_cast<int>(CommissionType::PERCENT);
        c.slippage = 0;
        c.pyramiding = 2;
        c.process_orders_on_close = false;
        configure_pine_strategy(c);
        syminfo_mintick_ = 0.01;
    }
    void on_source_bar(const Bar&) override {
        for (const auto& c : cmds_) {
            if (c.bar != bar_index_) continue;
            switch (c.op) {
            case Op::Entry: strategy_entry(c.id, true, kNaN, kNaN, c.qty); break;
            case Op::LimitEntry: strategy_entry(c.id, true, c.level); break;
            case Op::StopEntry: strategy_entry(c.id, true, kNaN, c.level); break;
            case Op::Close: strategy_close(c.id, "", c.qty); break;
            case Op::CloseAll: strategy_close_all(); break;
            case Op::Cancel: strategy_cancel(c.id); break;
            case Op::StopExit:
                strategy_exit(c.id, c.from, kNaN, c.level, kNaN, kNaN, kNaN, 100.0, "", c.qty);
                break;
            }
        }
    }
private:
    std::vector<Cmd> cmds_;
};

// ── KERNEL ─────────────────────────────────────────────────────────────

class KernelProbe final : public NativeStrategyHost {
public:
    explicit KernelProbe(const std::vector<Cmd>& cmds) : cmds_(cmds) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        const int b = bar_++;
        for (const auto& c : cmds_) {
            if (c.bar != b) continue;
            const double qty = std::isnan(c.qty) ? 100.0 : c.qty;
            no::Request r;
            r.label = c.id;
            switch (c.op) {
            case Op::Entry: r.intent = no::Transact{qty}; break;
            case Op::LimitEntry: r.intent = no::Transact{100.0}; r.trigger = no::Limit{c.level}; break;
            case Op::StopEntry: r.intent = no::Transact{100.0}; r.trigger = no::Stop{c.level}; break;
            case Op::Close: r.intent = no::Reduce{no::ExplicitUnits{qty}}; break;
            case Op::CloseAll: r.intent = no::Flatten{}; break;
            case Op::StopExit:
                // the exit's own entry's quantity, FIFO from the oldest lot
                r.intent = no::Reduce{no::ExplicitUnits{qty}};
                r.trigger = no::Stop{c.level};
                break;
            case Op::Cancel: {
                const auto h = handles_.find(c.id);
                if (h != handles_.end()) (void)cancel(h->second);
                continue;
            }
            }
            const auto res = submit(r);
            if (res.handle) handles_[c.id] = *res.handle;
        }
    }
    int max_open_lots_rejects() const {
        int n = 0;
        for (const auto& row : native_events(0)) {
            if (!row.command) continue;
            if (const auto* e = std::get_if<no::MatchRejectedEvent>(&*row.command))
                if (e->reason == no::MatchRejectReason::MaxOpenLots) ++n;
        }
        return n;
    }
private:
    std::vector<Cmd> cmds_;
    std::map<std::string, no::RequestHandle> handles_;
    int bar_ = 0;
};

NativeRunSpec kernel_spec(const char* key) {
    NativeRunSpec s;
    s.identity = {key, 1};
    s.event_retention = NativeEventRetention::Full;
    s.input_tf = "15";
    s.script_tf = "15";
    s.tickerid = "NYSE:F";
    s.timezone = "UTC";
    s.session = "24x7";
    s.initial_capital = 100'000.0;
    s.point_value = 1.0;
    s.account_fx = 1.0;
    s.price_tick = 0.01;
    s.fee_kind = NativeFeeKind::CashPerExecution;
    s.fee_value = 0.0;
    s.max_open_lots = 2;
    return s;
}

// ── TAPE ───────────────────────────────────────────────────────────────

std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "YYYY-MM-DD HH:MM" in the tape's UTC+8 -> UTC milliseconds.
std::int64_t tape_ms(const std::string& text) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0;
    if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5) return -1;
    const std::int64_t days = days_from_civil(y, static_cast<unsigned>(mo),
                                              static_cast<unsigned>(d));
    return ((days * 24 + h - 8) * 60 + mi) * 60'000;
}

struct TapeRow {
    std::string id;
    double qty = 0.0;
    std::int64_t entry_ms = 0, exit_ms = 0;
    double entry_price = kNaN, exit_price = kNaN;
};

std::vector<TapeRow> read_tape(const std::string& slug) {
    std::ifstream in(std::string(PINEFORGE_PYRAMIDING_FIXTURE_DIR) + "/" + slug + "/tv_trades.csv");
    std::map<int, TapeRow> trades;
    std::string line;
    bool header = true;
    while (std::getline(in, line)) {
        if (header) { header = false; continue; }
        std::vector<std::string> cell;
        std::stringstream fields(line);
        std::string field;
        while (std::getline(fields, field, ',')) cell.push_back(field);
        if (cell.size() < 6) continue;
        TapeRow& t = trades[std::stoi(cell[0])];
        const bool entry = cell[1].rfind("Entry", 0) == 0;
        if (entry) {
            t.entry_ms = tape_ms(cell[2]);
            t.entry_price = std::stod(cell[4]);
            t.id = cell[3];
            t.qty = std::stod(cell[5]);
        } else {
            t.exit_ms = tape_ms(cell[2]);
            t.exit_price = std::stod(cell[4]);
        }
    }
    std::vector<TapeRow> out;
    for (auto& kv : trades) out.push_back(kv.second);
    return out;
}

std::string tape_outcome(const std::vector<TapeRow>& tape, std::int64_t s0) {
    std::vector<Row> rows;
    for (const auto& t : tape) {
        if (t.entry_ms < s0 || t.entry_ms >= s0 + kBars * kBarMs) continue;
        rows.push_back({t.id, t.qty, static_cast<int>((t.entry_ms - s0) / kBarMs), t.entry_price,
                        static_cast<int>((t.exit_ms - s0) / kBarMs), t.exit_price});
    }
    return canonical(rows);
}

#include "fixtures/pyramiding_count/expectations.inc"

}  // namespace

int main() {
    const bool dumping = std::getenv("PF_DUMP") != nullptr;
    const std::map<std::string, std::size_t> tape_trades = {
        {"hm-orders-m13-f-pyramiding", 24},
        {"hm-orders-m13-f-pyramiding-rule", 9},
        {"hm-orders-m13-f-pyramiding-cap-orders", 4},
    };
    std::map<std::string, std::vector<TapeRow>> tapes;
    for (const auto& kv : tape_trades) {
        tapes[kv.first] = read_tape(kv.first);
        CHECK(tapes[kv.first].size() == kv.second);
    }
    int index = 0;
    for (const auto& s : kScenarios) {
        const FeedBar* fb = kPyramidBars[index++];
        std::vector<Bar> bars;
        for (int i = 0; i < kBars; ++i) {
            Bar b{};
            b.timestamp = fb[i].ts; b.open = fb[i].open; b.high = fb[i].high;
            b.low = fb[i].low; b.close = fb[i].close; b.volume = 1000.0;
            bars.push_back(b);
        }
        const std::int64_t s0 = bars[0].timestamp;

        PineProbe pine(s.cmds);
        pine.run(bars.data(), kBars);
        CHECK(pine.last_error().empty());
        const std::string adapter = canonical(engine_rows(pine, s0));

        KernelProbe kernel(s.cmds);
        CHECK(kernel.configure_native(kernel_spec(s.key)).status == NativeSetupStatus::Applied);
        kernel.run(bars.data(), kBars);
        CHECK(kernel.last_error().empty());
        const std::string native = canonical(engine_rows(kernel, s0));

        const std::string tv = tape_outcome(tapes[s.tape], s0);
        std::printf("-- %s\n   tape    %s\n   adapter %s%s\n   kernel  %s%s  (MaxOpenLots refusals %d)\n",
                    s.key, tv.c_str(), adapter.c_str(), adapter == tv ? " [= tape]" : " [!= tape]",
                    native.c_str(), native == tv ? " [= tape]" : " [!= tape]",
                    kernel.max_open_lots_rejects());
        if (dumping) continue;
        CHECK(pine.physical_position().signed_units == 0.0);
        CHECK(kernel.physical_position().signed_units == 0.0);
        CHECK(adapter == s.adapter);
        CHECK(native == s.kernel);
        CHECK((adapter == tv) == s.adapter_matches_tv);
        CHECK((native == tv) == s.kernel_matches_tv);
    }
    std::printf("\n%s pyramiding count differential: %d checks, %d failures\n",
                tests_failed == 0 ? "PASS" : "FAIL", tests_passed + tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
