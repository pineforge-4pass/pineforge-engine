// One live adapter/strategy, differing only in the storage
// backend of its auxiliary id tables. Fail-before on b3c39238e: the explicit
// fixture storage seam is absent. CORE-TOP owns executed fail-before proof.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/detail/id_pair_store.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
using namespace pineforge;
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
bool same_bits(double left, double right) {
    std::uint64_t a = 0, b = 0;
    std::memcpy(&a, &left, sizeof(a));
    std::memcpy(&b, &right, sizeof(b));
    return a == b;
}
struct ScratchDirectory {
    std::string path = "/tmp/pineforge-auxiliary-test-XXXXXX";
    ScratchDirectory() {
        require(::mkdtemp(path.data()) != nullptr, "create adapter test directory");
    }
    ~ScratchDirectory() { ::rmdir(path.c_str()); }
};

// Keep the old nested in-memory minima as the reference, independently of
// the store's optional fixture backend. Query retained rows by scanning.
class PlacementReference {
    struct Minima {
        std::uint64_t any = std::numeric_limits<std::uint64_t>::max();
        std::unordered_map<std::string, std::uint64_t> by_from_entry;
    };
    using Index = std::unordered_map<std::string, Minima>;

public:
    void insert(std::uint64_t incarnation, const source::PlacementSnapshot& row) {
        rows_.emplace(incarnation, row);
    }
    void erase(std::uint64_t incarnation) {
        const auto found = rows_.find(incarnation);
        require(found != rows_.end(), "reference row exists before erase");
        fold(erased_, found->second);
        rows_.erase(found);
    }
    void replace(std::uint64_t incarnation, const source::PlacementSnapshot& row) {
        rows_.at(incarnation) = row;
    }
    std::uint64_t sequence(const std::string& id, const std::string& from) const {
        Index index = erased_;
        for (const auto& row : rows_) fold(index, row.second);
        const auto found = index.find(id);
        if (found == index.end()) return std::numeric_limits<std::uint64_t>::max();
        const auto paired = found->second.by_from_entry.find(from);
        if (paired != found->second.by_from_entry.end()
            && paired->second != std::numeric_limits<std::uint64_t>::max())
            return paired->second;
        return found->second.any;
    }

private:
    static void fold(Index& index, const source::PlacementSnapshot& row) {
        auto& minima = index[row.source_id];
        minima.any = std::min(minima.any, row.command_sequence);
        const auto inserted = minima.by_from_entry.emplace(row.from_entry, row.command_sequence);
        if (!inserted.second)
            inserted.first->second = std::min(inserted.first->second, row.command_sequence);
    }
    std::map<std::uint64_t, source::PlacementSnapshot> rows_;
    Index erased_;
};

void placement_minima_match_old_nested_reference() {
    ScratchDirectory scratch;
    source::PlacementTable table;
    table.set_auxiliary_tables_directory(scratch.path);
    PlacementReference reference;
    const auto query = [&](const std::string& id, const std::string& from) {
        require(table.command_sequence_for(id, from) == reference.sequence(id, from),
                "placement minima match retained scan plus old erased nested index");
    };
    for (std::uint64_t i = 1; i <= 512; ++i) {
        source::PlacementSnapshot row;
        row.source_id = "exit-" + std::to_string(i % 31);
        row.from_entry = "unique-entry-" + std::to_string(i);
        row.command_sequence = 10000 - i;
        table.try_emplace(i, row);
        reference.insert(i, row);
        if (i % 2 != 0) query(row.source_id, row.from_entry); // scan before erase
        require(table.erase(i), "erase a real placement row");
        reference.erase(i);
        table.compact();
        query(row.source_id, row.from_entry);
        query(row.source_id, "absent-from-entry");
    }
    source::PlacementSnapshot row;
    row.source_id = "replace-id";
    row.from_entry = "";
    row.command_sequence = 3;
    table.try_emplace(513, row);
    reference.insert(513, row);
    query(row.source_id, row.from_entry);
    const auto erased_bytes = table.auxiliary_disk_bytes();
    row.source_id = "replacement-id";
    row.from_entry = "new-from-entry";
    row.command_sequence = std::numeric_limits<std::uint64_t>::max();
    reference.replace(513, row);
    table.replace(513, std::move(row));
    query("replace-id", "");
    query("replacement-id", "new-from-entry"); // present UINT64_MAX
    query("exit-1", "unique-entry-1"); // erased minima survive replacement
    require(table.auxiliary_disk_bytes() == erased_bytes,
            "retained scans and replacement create no q data or index");
    require(table.erase(513), "erase replacement row");
    reference.erase(513);
    row = {};
    row.source_id = "replacement-id";
    row.from_entry = "other-entry";
    row.command_sequence = 7;
    table.try_emplace(514, row);
    reference.insert(514, row);
    query("replacement-id", "new-from-entry"); // max pair falls back to any
    query("replacement-id", "other-entry");
    query("replacement-id", "unknown");
    require(table.auxiliary_disk_bytes() > 0, "placement witness uses real scratch files");
    table.clear();
    require(table.auxiliary_disk_bytes() == 0
            && table.command_sequence_for("exit-1", "unique-entry-1")
                == std::numeric_limits<std::uint64_t>::max(), "clear forgets all old minima");
}

class UniqueEntryHost final : public source::PineStrategyHost {
public:
    UniqueEntryHost(int cycles, int period, bool long_ids, bool memory, const std::string& directory)
        : cycles_(cycles), period_(period), long_ids_(long_ids) {
        adapter_.reset_for_run();
        adapter_.set_auxiliary_tables_directory(directory);
        if (memory) adapter_.fixture_use_memory_auxiliary_tables();
        source::PineStrategyConfig config;
        config.initial_capital = 1000000.0;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 4.0;
        config.pyramiding = 1;
        config.process_orders_on_close = true;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
        set_broker_state_hash_recording(true);
    }

    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        const int cycle = bar / 6;
        if (cycle >= cycles_) return;
        const auto ordinal = period_ == 0 ? cycle : cycle % period_;
        const auto id = std::string(long_ids_ ? "long-unique-entry-" : "E")
            + std::to_string(ordinal);
        const auto exit_id = cycle % 3 == 0 ? "shared-exit" : "half-" + id;
        if (bar % 6 == 0) strategy_entry(id, true, kNa, kNa, 4.0);
        if (bar % 6 == 1 || bar % 6 == 3)
            strategy_exit(exit_id, id, 100.5, 99.5, kNa, kNa, kNa, 50.0);
        if (bar % 6 == 4 && live_position_size() > 0.0) strategy_close(id);
        // Exercise retained-row scans as well as erased minima.
        // Observations never steer the strategy or add a broker hash field.
        sequences_.push_back(adapter_.command_sequence_for_exit(exit_id, id));
        fallbacks_.push_back(adapter_.command_sequence_for_exit(exit_id, "unbound"));
    }

    const std::vector<std::uint64_t>& hashes() const { return broker_state_hashes_; }
    const std::vector<std::uint64_t>& sequences() const { return sequences_; }
    const std::vector<std::uint64_t>& fallbacks() const { return fallbacks_; }
    std::uint64_t auxiliary_disk_bytes() const { return adapter_.fixture_auxiliary_disk_bytes(); }
    std::uint64_t sequence(const std::string& id, const std::string& from) const {
        return adapter_.command_sequence_for_exit(id, from);
    }
    void clear_observations() { sequences_.clear(); fallbacks_.clear(); }
    void reset_to_memory_auxiliary_tables() {
        adapter_.reset_for_run();
        adapter_.fixture_use_memory_auxiliary_tables();
        clear_observations();
    }

private:
    int cycles_;
    int period_;
    bool long_ids_;
    std::vector<std::uint64_t> sequences_, fallbacks_;
};

std::vector<Bar> tape(int cycles) {
    std::vector<Bar> bars;
    bars.reserve(static_cast<std::size_t>(cycles * 6 + 8));
    for (int i = 0; i < cycles * 6 + 8; ++i)
        bars.push_back({100.0, 101.0, 99.0, 100.0, 1000.0, kStart + i * 60000LL});
    return bars;
}

void compare_trade(const Trade& a, const Trade& b) {
    require(a.entry_time == b.entry_time && a.exit_time == b.exit_time
            && a.entry_bar_index == b.entry_bar_index && a.exit_bar_index == b.exit_bar_index
            && a.entry_id == b.entry_id && a.exit_id == b.exit_id
            && a.entry_comment == b.entry_comment && a.exit_comment == b.exit_comment
            && a.entry_incarnation == b.entry_incarnation && a.is_long == b.is_long
            && a.open_at_end == b.open_at_end && a.exit_from_bracket == b.exit_from_bracket
            && a.close_cause == b.close_cause,
            "disk/reference trade identity and metadata match");
    require(same_bits(a.entry_price, b.entry_price) && same_bits(a.exit_price, b.exit_price)
            && same_bits(a.qty, b.qty) && same_bits(a.pnl, b.pnl) && same_bits(a.pnl_pct, b.pnl_pct)
            && same_bits(a.commission, b.commission) && same_bits(a.max_runup, b.max_runup)
            && same_bits(a.max_drawdown, b.max_drawdown),
            "disk/reference trade doubles match bit for bit");
}

void differential(int cycles, int period, bool long_ids) {
    ScratchDirectory scratch;
    UniqueEntryHost disk(cycles, period, long_ids, false, scratch.path);
    UniqueEntryHost memory(cycles, period, long_ids, true, scratch.path);
    const auto bars = tape(cycles);
    for (int run = 0; run < 2; ++run) {
        disk.clear_observations();
        memory.clear_observations();
        disk.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        memory.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
        require(disk.last_error().empty() && memory.last_error().empty(),
                "both adapter runs complete");
        require(disk.trade_count() == cycles * 2 && disk.trade_count() == memory.trade_count(),
                "each cycle closes one partial exit and the remainder");
        for (int i = 0; i < disk.trade_count(); ++i) compare_trade(disk.get_trade(i), memory.get_trade(i));
        require(!disk.hashes().empty() && disk.hashes() == memory.hashes(),
                "every recorded broker hash is identical");
        require(disk.broker_state_hash() == memory.broker_state_hash(),
                "final complete broker hash is identical");
        require(disk.sequences() == memory.sequences() && disk.fallbacks() == memory.fallbacks(),
                "paired and fallback sequence minima agree throughout the run");
        require(disk.auxiliary_disk_bytes() > 0 && memory.auxiliary_disk_bytes() == 0,
                "differential actually compares disk and in-memory tables");
        const auto first = long_ids ? "long-unique-entry-0" : "E0";
        require(disk.sequence("shared-exit", first) == memory.sequence("shared-exit", first)
                && disk.sequence("shared-exit", first) != std::numeric_limits<std::uint64_t>::max(),
                "first exit/entry pair still finds its old row after many other ids");
    }
}

void directory_failure_is_a_typed_run_failure() {
    ScratchDirectory scratch;
    UniqueEntryHost disk(4, 0, true, false, scratch.path + "/missing");
    const auto bars = tape(4);
    disk.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(!disk.last_error().empty(), "configured scratch failure ends the run");
    require(run_failure_value_of(disk).code == RunFailureCode::scratch_io_failed,
            "scratch IO failure is scratch_io_failed at the public run boundary");
}

void fixture_backend_requires_reset_after_use() {
    ScratchDirectory scratch;
    source::PlacementTable table;
    table.set_auxiliary_tables_directory(scratch.path);
    source::PlacementSnapshot row;
    row.source_id = "fixture-exit";
    row.from_entry = "fixture-entry";
    row.command_sequence = 7;
    table.try_emplace(1, row);
    require(table.command_sequence_for(row.source_id, row.from_entry) == row.command_sequence,
            "scan retained evidence before switching backends");
    require(table.erase(1), "erase the fixture placement while keeping its history");
    bool refused = false;
    try {
        table.fixture_use_memory_auxiliary_tables();
    } catch (const std::logic_error& error) {
        refused = classify_run_failure(error).code == RunFailureCode::engine_invariant;
    }
    require(refused, "even an empty used placement table requires reset before backend selection");
    require(table.command_sequence_for(row.source_id, row.from_entry) == row.command_sequence
            && table.auxiliary_disk_bytes() > 0,
            "refused backend selection preserves the disk minima and erased history");
    table.clear();
    table.fixture_use_memory_auxiliary_tables();
    row.command_sequence = 11;
    table.try_emplace(1, row);
    require(table.command_sequence_for(row.source_id, row.from_entry) == row.command_sequence
            && table.auxiliary_disk_bytes() == 0,
            "clear permits backend selection and removes the previous run's minima");
}

void host_reset_permits_backend_selection() {
    ScratchDirectory scratch;
    UniqueEntryHost reused(8, 0, true, false, scratch.path);
    UniqueEntryHost reference(8, 0, true, true, scratch.path);
    const auto bars = tape(8);
    reused.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(reused.last_error().empty() && reused.trade_count() > 0
            && reused.auxiliary_disk_bytes() > 0,
            "use a real host's disk tables before resetting to the memory backend");
    reused.reset_to_memory_auxiliary_tables();
    require(reused.auxiliary_disk_bytes() == 0,
            "reset closes every auxiliary scratch file before fixture backend selection");
    reused.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    reference.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
    require(reused.last_error().empty() && reference.last_error().empty()
            && reused.trade_count() > 0 && reused.trade_count() == reference.trade_count(),
            "reset host and fresh memory host both complete the same trades");
    for (int i = 0; i < reused.trade_count(); ++i)
        compare_trade(reused.get_trade(i), reference.get_trade(i));
    require(!reused.hashes().empty() && reused.hashes() == reference.hashes()
            && reused.broker_state_hash() == reference.broker_state_hash()
            && reused.sequences() == reference.sequences()
            && reused.fallbacks() == reference.fallbacks()
            && reused.auxiliary_disk_bytes() == 0 && reference.auxiliary_disk_bytes() == 0,
            "backend selection after reset preserves all trades, hashes and sequence minima");
}

void placement_directory_failure_is_part_c_error() {
    ScratchDirectory scratch;
    source::PlacementTable table;
    table.set_auxiliary_tables_directory(scratch.path + "/missing");
    source::PlacementSnapshot row;
    row.source_id = "part-c-exit";
    row.from_entry = "part-c-entry";
    row.command_sequence = 1;
    table.try_emplace(1, row);
    const auto before_digest = table.erased_digest();
    require(table.command_sequence_for(row.source_id, row.from_entry) == row.command_sequence
            && table.auxiliary_disk_bytes() == 0,
            "retained q queries succeed without touching even a missing directory");
    bool failed = false;
    try {
        (void)table.erase(1); // Only the retained e store performs disk I/O.
    } catch (const detail::IdPairStoreError& error) {
        failed = error.code().value() == ENOENT
            && classify_run_failure(error).code == RunFailureCode::scratch_io_failed;
    }
    require(failed, "part C's placement minima preserve the typed missing-directory error");
    require(table.size() == 1 && table.contains(1) && table.erased_rows() == 0
            && table.erased_digest() == before_digest && table.auxiliary_disk_bytes() == 0,
            "failed erasure retains its row, digest, counters and unpublished e files");
    table.set_auxiliary_tables_directory(scratch.path);
    require(table.erase(1) && table.erased_rows() == 1 && table.auxiliary_disk_bytes() > 0
            && table.command_sequence_for(row.source_id, row.from_entry) == row.command_sequence,
            "e erasure retries after directory repair and keeps the same query answer");
}
} // namespace

int main() {
    try {
        placement_minima_match_old_nested_reference();
        placement_directory_failure_is_part_c_error();
        fixture_backend_requires_reset_after_use();
        host_reset_permits_backend_selection();
        differential(256, 0, false);
        differential(256, 0, true);
        differential(320, 31, true);
        differential(64, 1, false);
        directory_failure_is_a_typed_run_failure();
        std::puts("test_adapter_auxiliary_disk: trades, hashes, recurrence and reset agree");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_adapter_auxiliary_disk: %s\n", error.what());
        return 1;
    }
}
