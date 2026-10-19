// CR-150 L19m. The unique-entry and auxiliary schedules are the native twins
// of tests/fixtures/flat_memory/{unique_entry_id,unique_auxiliary_ids}.pine.
// Storage is the sole difference between each pair of live Pine hosts.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/report_sink.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

bool same_bits(double a, double b) {
    std::uint64_t left = 0, right = 0;
    static_assert(sizeof left == sizeof a, "binary64 trade fields");
    std::memcpy(&left, &a, sizeof a);
    std::memcpy(&right, &b, sizeof b);
    return left == right;
}

struct Directory {
    std::string path = "/tmp/pineforge-phase-host-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create host scratch directory"); }
    ~Directory() {
        ::unlink((path + "/disk.jsonl").c_str());
        ::unlink((path + "/memory.jsonl").c_str());
        ::rmdir(path.c_str());
    }
};

std::string census_id(int ordinal, bool long_id) {
    char digits[32];
    std::snprintf(digits, sizeof digits, "%014d", ordinal);
    return std::string(long_id ? "census-entry-" : "E") + digits;
}

enum class Schedule { Unique, Auxiliary, Pyramiding, MixedPhases };

int stride(Schedule schedule) {
    if (schedule == Schedule::Unique) return 4;
    if (schedule == Schedule::Auxiliary) return 6;
    return 8;
}

class PhaseHost final : public source::PineStrategyHost {
public:
    PhaseHost(Schedule schedule, int cycles, int period, bool long_ids,
              bool memory, bool record_hashes, const std::string& directory)
        : schedule_(schedule), cycles_(cycles), period_(period), long_ids_(long_ids) {
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = schedule == Schedule::Pyramiding ? 4 : 1;
        config.process_orders_on_close = true;
        config.commission_value = 0.15;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(record_hashes);
        // This is L19l's merged single binding, extended by L19m. No
        // independent phase-directory setter or process-wide mode exists.
        adapter_.set_id_tables_directory(directory);
        if (memory) adapter_.fixture_use_memory_exit_phases();
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const int cycle = bar / stride(schedule_);
        if (cycle >= cycles_) return;
        const int step = bar % stride(schedule_);
        const int ordinal = period_ == 0 ? cycle : cycle % period_;
        const auto id = schedule_ == Schedule::Auxiliary
            ? std::string(long_ids_ ? "long-unique-entry-" : "E") + std::to_string(ordinal)
            : census_id(ordinal, long_ids_);
        if (schedule_ == Schedule::Unique) {
            if (step == 0) strategy_entry(id, true, kNa, kNa, 1.0);
            if (step == 1 && live_position_size() > 0.0) strategy_close(id);
        } else if (schedule_ == Schedule::Auxiliary) {
            const auto exit_id = cycle % 3 == 0 ? "shared-exit" : "half-" + id;
            if (step == 0) strategy_entry(id, true, kNa, kNa, 4.0);
            if (step == 1 || step == 3)
                strategy_exit(exit_id, id, 100.5, 99.5, kNa, kNa, kNa, 50.0);
            if (step == 4 && live_position_size() > 0.0) strategy_close(id);
        } else if (schedule_ == Schedule::Pyramiding) {
            if (step < 4) strategy_entry(id, true, kNa, kNa, 1.0);
            if (step == 6) strategy_close_all();
        } else {
            if (step == 0) strategy_entry(id, true, kNa, kNa, 2.0);
            if (step == 1) {
                // Both children close on the next bar: one at its gap open,
                // the other on its intrabar path. This reaches the host's
                // same-bar sort and gives different phase bytes to permute.
                strategy_exit("first", id, 102.0, kNa, kNa, kNa, kNa, 100.0, {}, 1.0);
                strategy_exit("second", id, 104.0, kNa, kNa, kNa, kNa, 100.0, {}, 1.0);
            }
            if (cycle == cycles_ - 1 && step == 4) {
                require(trade_count() >= 2, "mixed-phase fixture must close two children");
                const bool first = adapter_.is_open_phase_exit(0);
                const bool second = adapter_.is_open_phase_exit(1);
                require(first != second, "fixture must exercise distinct open/intrabar phases");
                adapter_.permute_exit_phases(0, std::vector<std::size_t>{1, 0});
                require(adapter_.is_open_phase_exit(0) == second
                        && adapter_.is_open_phase_exit(1) == first,
                        "historical permutation keeps each saved phase");
                rewrote_ = true; // Following bar seals/refolds the old prefix.
            }
        }
    }

    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    std::uint64_t disk_bytes() const { return adapter_.fixture_exit_phase_disk_bytes(); }
    bool phase(std::size_t index) const { return adapter_.is_open_phase_exit(index); }
    bool rewrote() const { return rewrote_; }
    void reset_observations() { rewrote_ = false; }

private:
    Schedule schedule_;
    int cycles_, period_;
    bool long_ids_, rewrote_ = false;
};

std::vector<Bar> tape(Schedule schedule, int cycles) {
    std::vector<Bar> bars(static_cast<std::size_t>(cycles * stride(schedule) + 8));
    for (std::size_t i = 0; i < bars.size(); ++i) {
        bars[i] = {100.0, 101.0, 99.0, 100.0, 1000.0,
                   kStart + static_cast<std::int64_t>(i) * 60000};
        if (schedule == Schedule::MixedPhases && i % 8 == 2) {
            bars[i].open = 103.0;
            bars[i].high = 105.0;
            bars[i].low = 102.0;
            bars[i].close = 104.0;
        }
    }
    return bars;
}

void compare_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause, "disk/reference trade metadata match");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl) && same_bits(a.pnl_pct, b.pnl_pct)
            && same_bits(a.commission, b.commission) && same_bits(a.max_runup, b.max_runup)
            && same_bits(a.max_drawdown, b.max_drawdown),
            "every disk/reference trade double matches bit for bit");
}

void differential(Schedule schedule, int cycles, int period, bool long_ids) {
    Directory directory;
    PhaseHost disk(schedule, cycles, period, long_ids, false, true, directory.path);
    PhaseHost memory(schedule, cycles, period, long_ids, true, true, directory.path);
    const auto bars = tape(schedule, cycles);
    const int trades_per_cycle = schedule == Schedule::Unique ? 1
        : (schedule == Schedule::Pyramiding ? 4 : 2);
    for (int run = 0; run < 2; ++run) {
        disk.reset_observations();
        memory.reset_observations();
        disk.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        memory.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(disk.last_error().empty() && memory.last_error().empty(),
                "both live adapter runs complete");
        require(disk.trade_count() == cycles * trades_per_cycle
                && memory.trade_count() == disk.trade_count(), "fixture closes its complete population");
        for (int i = 0; i < disk.trade_count(); ++i) {
            compare_trade(disk.get_trade(i), memory.get_trade(i));
            require(disk.phase(static_cast<std::size_t>(i))
                    == memory.phase(static_cast<std::size_t>(i)),
                    "historical open-phase queries agree after many other trades");
            if (schedule == Schedule::Unique) {
                require(disk.get_trade(i).entry_id == census_id(period == 0 ? i : i % period, long_ids),
                        "unique/reused/recurring entry identity follows the L0b schedule");
            }
        }
        require(!disk.hashes().empty() && disk.hashes() == memory.hashes(),
                "all recorded broker hash columns match the old vector backend");
        require(disk.broker_state_hash() == memory.broker_state_hash(),
                "complete final broker state matches the old vector backend");
        require(disk.disk_bytes() == static_cast<std::uint64_t>(disk.trade_count())
                && memory.disk_bytes() == 0, "differential reaches disk and old RAM implementations");
        if (schedule == Schedule::MixedPhases)
            require(disk.rewrote() && memory.rewrote(), "old finalized phase prefix was actually rewritten");
    }
}

std::string read_all(const std::string& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "open completed report sink");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Report {
    ReportC value{};
    ~Report() { BacktestEngine::free_report(&value); }
};

void sink_bytes_match_old_vector(bool streaming) {
    Directory directory;
    const auto schedule = Schedule::Auxiliary;
    const int cycles = 40;
    PhaseHost disk(schedule, cycles, 31, true, false, true, directory.path);
    PhaseHost memory(schedule, cycles, 31, true, true, true, directory.path);
    const auto disk_path = directory.path + "/disk.jsonl";
    const auto memory_path = directory.path + "/memory.jsonl";
    const pf_report_sink_v1 disk_sink{sizeof(pf_report_sink_v1), 1, disk_path.c_str(),
                                    "L19m", directory.path.c_str()};
    const pf_report_sink_v1 memory_sink{sizeof(pf_report_sink_v1), 1, memory_path.c_str(),
                                      "L19m", directory.path.c_str()};
    disk.set_trade_history_streamable(streaming);
    memory.set_trade_history_streamable(streaming);
    require(disk.set_report_sink_v1(&disk_sink) == 0
            && memory.set_report_sink_v1(&memory_sink) == 0, "install both report sinks");
    const auto bars = tape(schedule, cycles);
    disk.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    memory.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(disk.last_error().empty() && memory.last_error().empty(), "both sink runs complete");
    Report disk_report, memory_report;
    disk.fill_report(&disk_report.value);
    memory.fill_report(&memory_report.value);
    pf_report_sink_receipt_v1 disk_receipt{}, memory_receipt{};
    disk_receipt.struct_size = sizeof disk_receipt;
    memory_receipt.struct_size = sizeof memory_receipt;
    disk_receipt.version = memory_receipt.version = 1;
    require(disk.report_sink_receipt_v1(&disk_receipt) == 0
            && memory.report_sink_receipt_v1(&memory_receipt) == 0
            && disk_receipt.complete == 1 && memory_receipt.complete == 1,
            "sink reports completed trailers");
    require(disk_receipt.count_t == cycles * 2 && disk_receipt.count_h > 0,
            "sink differential emits real trades and broker hashes");
    require(read_all(disk_path) == read_all(memory_path),
            "every report byte, hash column, count and trailer matches the old vector");
    require(disk.disk_bytes() == static_cast<std::uint64_t>(cycles * 2)
            && memory.disk_bytes() == 0, "streaming also exercises the disk phase store");
}
} // namespace

int main() {
    try {
        for (const bool long_ids : {false, true}) {
            for (const int period : {0, 1, 31}) {
                differential(Schedule::Unique, 96, period, long_ids);
                differential(Schedule::Auxiliary, 96, period, long_ids);
            }
        }
        differential(Schedule::Pyramiding, 32, 1, false);
        differential(Schedule::MixedPhases, 32, 31, true);
        sink_bytes_match_old_vector(false);
        sink_bytes_match_old_vector(true);
        std::puts("test_adapter_phase_disk: complete trades, hashes, report bytes and resets agree");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_phase_disk: %s\n", error.what());
        return 1;
    }
}
