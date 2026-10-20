// CX-D-P1: the live Flip/reversal shape from test_adapter_reversal_storage,
// with unchanged production eviction and file versus arena backing.
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "dport_id_arena_fixture.hpp"

#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace pineforge;
using namespace dport_id_fixture;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;
constexpr int kBars = 96;

int triangle(int index, int period) {
    const int phase = index % (2 * period);
    return phase < period ? phase : 2 * period - phase;
}
double price(int index) {
    return 0.25 * (400 + 2 * triangle(index, 9)
                  + 3 * triangle(index, 23) - triangle(index, 4));
}
std::vector<Bar> tape() {
    std::vector<Bar> bars;
    for (int i = 0; i < kBars; ++i) {
        const double open = price(i), close = price(i + 1);
        bars.push_back({open, std::max(open, close) + 0.5,
                        std::min(open, close) - 0.5, close, 1.0,
                        kStart + static_cast<std::int64_t>(i) * 60000});
    }
    return bars;
}

#if defined(__linux__)
std::uint64_t reversal_arena_bytes() {
    // All source entry/order ids the script can submit. Each independent
    // logical file rounds separately; rank keys here stay below its cap.
    const std::array<std::string, 5> ids{{"L", "S", "L2", "B", "o"}};
    std::uint64_t cohorts = 1, ranks = 1, longest = 0;
    for (const auto& id : ids) {
        cohorts += 56 + 40 + id.size();
        ranks += 56 + sizeof(std::uint32_t) + id.size();
        longest = std::max(longest, static_cast<std::uint64_t>(id.size()));
    }
    // The real adapter also exercises P2/P3/P4. Budget their regions from
    // the script's ten submission sites per bar, allowing a single and a
    // paired auxiliary key per submission, plus old/new hash files during
    // rehash. This is an input-derived upper bound, not a harvested peak.
    const auto submissions = static_cast<std::uint64_t>(kBars) * (5 + 2 + 1 + 2);
    const auto auxiliary_keys = 2 * submissions;
    std::uint64_t slots = 64;
    while (slots / 2 < auxiliary_keys) slots *= 2;
    const auto auxiliary = 3 * (rounded(auxiliary_keys * (40 + 2 * longest))
        + rounded(slots * 16) + rounded((slots / 2) * 16));
    const auto successor_roots = rounded(submissions * 16);
    const auto position_tail = rounded(2 * submissions * (30 + longest));
    return rounded(cohorts) + rounded(ranks) + auxiliary + successor_roots + position_tail;
}
#endif

class ReversalHost final : public source::PineStrategyHost {
public:
    explicit ReversalHost(bool recording, bool census = false) {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 2;
        config.commission_type = static_cast<int>(CommissionType::CASH_PER_ORDER);
        config.commission_value = 0.5;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(recording);
#if defined(__linux__)
        if (census) files_ = std::make_unique<RegularFiles>();
#else
        require(!census, "regular-file census requires Linux");
#endif
    }
    void on_source_bar(const Bar& bar) override {
        const int i = pine_bar_index();
        const double position = live_position_size();
        if (position != 0.0) {
            const int side = position > 0.0 ? 1 : -1;
            if (last_side_ != 0 && side != last_side_) ++reversals;
            last_side_ = side;
        }
        if (i % 6 == 1) strategy_entry("L", true);
        if (i % 6 == 4) strategy_entry("S", false);
        if (i % 12 == 2 && position > 0.0) strategy_entry("L2", true);
        if (i % 18 == 9) strategy_entry("B", true);
        if (i % 18 == 15) strategy_entry("B", false);
        if (position > 0.0)
            strategy_exit("xl", "L", bar.close + 2.0, bar.close - 2.0);
        if (position < 0.0)
            strategy_exit("xs", "S", bar.close - 2.0, bar.close + 2.0,
                          kNa, kNa, kNa, 100.0, {}, 1.0);
        if (i % 10 == 7) strategy_close("L", {}, 1.0);
        if (i % 14 == 11) strategy_order("o", true, 1.0);
        if (i % 14 == 12) strategy_order("o", false, 1.0);
        reads.push_back(broker_state_hash());
#if defined(__linux__)
        if (files_) {
            // In arena mode every adapter table's binding is path-agnostic,
            // even after rows have spilled; P2 owns its auxiliary wrappers.
            if (i == kBars / 2) adapter_.set_id_tables_directory("/dev/null/ignored-after-spill");
            files_->check();
        }
#endif
    }
    void clear_observations() {
        reads.clear();
        reversals = 0;
        last_side_ = 0;
    }
    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    std::vector<std::uint64_t> reads;
    std::size_t reversals = 0;
private:
    int last_side_ = 0;
#if defined(__linux__)
    std::unique_ptr<RegularFiles> files_;
#endif
};

struct Result {
    std::vector<Trade> trades;
    std::vector<std::uint64_t> hashes, reads;
    std::uint64_t final_hash = 0;
    std::size_t reversals = 0;
};

std::array<Result, 2> replay(bool magnifier, bool recording, bool arena) {
    const auto bars = tape();
    ReversalHost host(recording, arena);
    std::array<Result, 2> results;
    for (auto& result : results) {
        host.clear_observations();
        host.run(bars.data(), static_cast<int>(bars.size()), "", "", magnifier, 4,
                 MagnifierDistribution::ENDPOINTS);
        require(host.last_error().empty(), host.last_error().c_str());
        for (int i = 0; i < host.trade_count(); ++i)
            result.trades.push_back(host.get_trade(i));
        result.hashes = host.hashes();
        result.reads = host.reads;
        result.final_hash = host.broker_state_hash();
        result.reversals = host.reversals;
        require(!result.trades.empty() && result.reversals > 1,
                "fixture executes real closes and repeated live reversals");
        require(result.reads.size() == bars.size()
                && (recording ? !result.hashes.empty() : result.hashes.empty()),
                "fixture observes every callback and the selected recording mode");
        if (arena) {
            const auto stats = ScratchArena::attached()->stats();
            require(stats.bytes_used >= 2 * kChunk && stats.high_water <= stats.bytes_total,
                    "live adapter actually claims arena regions within computed capacity");
        }
    }
    return results;
}

bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }
void compare_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause, "file/arena reversal trade metadata agrees");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl)
            && same_bits(a.pnl_pct, b.pnl_pct) && same_bits(a.commission, b.commission)
            && same_bits(a.max_runup, b.max_runup) && same_bits(a.max_drawdown, b.max_drawdown),
            "file/arena reversal trade doubles agree bit for bit");
}
void compare(const std::array<Result, 2>& actual, const std::array<Result, 2>& expected) {
    for (std::size_t run = 0; run < actual.size(); ++run) {
        const auto& a = actual[run];
        const auto& b = expected[run];
        require(a.hashes == b.hashes && a.reads == b.reads && a.final_hash == b.final_hash
                && a.reversals == b.reversals && a.trades.size() == b.trades.size(),
                "every recorded/callback/final hash and reversal count matches file mode");
        for (std::size_t i = 0; i < a.trades.size(); ++i) compare_trade(a.trades[i], b.trades[i]);
    }
}

#if defined(__linux__)
// Inspect the public OS descriptors, without a private production fd seam.
void require_id_files_in(const std::string& directory) {
    DIR* descriptors = ::opendir("/proc/self/fd");
    require(descriptors != nullptr, "open live id-file census");
    std::size_t count = 0;
    bool correct = true;
    while (const auto* entry = ::readdir(descriptors)) {
        const std::string path = std::string("/proc/self/fd/") + entry->d_name;
        std::array<char, 4096> target{};
        const auto length = ::readlink(path.c_str(), target.data(), target.size());
        if (length < 0) continue;
        const std::string name(target.data(), static_cast<std::size_t>(length));
        if (name.find("/pineforge-ids-") == std::string::npos) continue;
        ++count;
        correct = correct && name.compare(0, directory.size() + 1, directory + "/") == 0
            && name.find(" (deleted)") != std::string::npos;
    }
    ::closedir(descriptors);
    require(correct && count == 2, "both anonymous id files use the run scratch_dir");
}
#endif

class DirectoryHost final : public source::PineStrategyHost {
public:
    DirectoryHost() {
        source::PineStrategyConfig config;
        config.initial_capital = 100000.0;
        config.default_qty_value = 1.0;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        if (pine_bar_index() % 4 == 0) strategy_entry("directory-entry", true);
        if (pine_bar_index() % 4 == 2) strategy_close("directory-entry");
    }
};

void production_spill_directory() {
    Directory first, second, output;
    const std::string sink_path = output.path + "/report.sink";
    struct RemoveSink {
        const std::string& path;
        ~RemoveSink() { ::unlink(path.c_str()); }
    } remove{sink_path};
    DirectoryHost host;
    auto bars = tape();
    bars.resize(12);
    Environment invalid_default("TMPDIR", "/dev/null/production-must-bind-spill-dir");
    for (const auto* directory : {&first.path, &second.path}) {
        const pf_report_sink_v1 sink{sizeof(pf_report_sink_v1), 1,
                                    sink_path.c_str(), "dport-directory", directory->c_str()};
        require(host.set_report_sink_v1(&sink) == 0, "install run report sink");
        host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(host.last_error().empty(), host.last_error().c_str());
        require(host.trade_count() > 0, "directory witness executes real fills");
#if defined(__linux__)
        require_id_files_in(*directory);
#endif
    }
}
} // namespace

int main() {
    try {
        require(!ScratchArena::attached(), "adapter file reference starts without an arena");
        production_spill_directory();
        for (const bool magnifier : {false, true}) {
            for (const bool recording : {false, true}) {
                const auto expected = replay(magnifier, recording, false);
#if defined(__linux__)
                const Backing backing(reversal_arena_bytes());
                child([&] {
                    Environment window("PF_SCRATCH_WINDOW_BYTES", "65536");
                    Environment invalid_default("TMPDIR", "/dev/null/no-arena-fallback");
                    Attachment attached(backing);
                    RegularFiles files;
                    compare(replay(magnifier, recording, true), expected);
                    files.check();
                    require(ScratchArena::attached()->stats().bytes_used == 0,
                            "adapter destruction releases all arena regions");
                });
#else
                compare(replay(magnifier, recording, false), expected);
#endif
            }
        }
#if !defined(__linux__)
        std::puts("SKIP: sealed-memfd adapter arena/jail checks require Linux");
#endif
        std::puts("test_adapter_id_tables_arena: production spill-dir and reversal differential");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_id_tables_arena: %s\n", error.what());
        return 1;
    }
}
