// CR150-CX: real process I/O counters, not a ScratchFile wrapper or a mock.
// Keep this unit buildable on c82fd00e8d5de30fd5d959c3e3fbcca0f0c48e9b:
// the lab must run the SAME source on that base before testing the fix.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

struct Directory {
    std::string path = "/tmp/pineforge-resident-io-XXXXXX";
    Directory() { require(::mkdtemp(path.data()) != nullptr, "create scratch directory"); }
    ~Directory() { ::rmdir(path.c_str()); }
};

struct Io {
    std::uint64_t reads = 0, writes = 0;
};

class ProcessIo {
public:
    ProcessIo() {
#if defined(__linux__)
        fd_ = ::open("/proc/self/io", O_RDONLY | O_CLOEXEC);
        require(fd_ >= 0, "open /proc/self/io");
        // Verify the accounting assumption on the actual kernel. No output,
        // file creation, EOF probe, or second read is hidden in a snapshot.
        const auto before = sample();
        const auto after = sample();
        const auto control = difference(before, after);
        require(control.reads == 0 && control.writes == 0,
                "back-to-back /proc/self/io control is not zero");
#else
        std::puts("SKIP resident syscall counters: /proc/self/io requires Linux");
#endif
    }
    ProcessIo(const ProcessIo&) = delete;
    ProcessIo& operator=(const ProcessIo&) = delete;
    ~ProcessIo() {
#if defined(__linux__)
        if (fd_ >= 0) ::close(fd_);
#endif
    }
    static constexpr bool enabled() noexcept {
#if defined(__linux__)
        return true;
#else
        return false;
#endif
    }
    Io sample() const {
#if defined(__linux__)
        char bytes[1024];
        // A single successful pread, without stdio buffering or an EOF read.
        // An interrupted/short snapshot fails the test instead of changing
        // the number of counter reads to subtract.
        const auto length = ::pread(fd_, bytes, sizeof(bytes) - 1, 0);
        require(length > 0 && static_cast<std::size_t>(length) < sizeof(bytes) - 1,
                "read /proc/self/io in one syscall");
        bytes[length] = '\0';
        const char* reads = std::strstr(bytes, "syscr: ");
        const char* writes = std::strstr(bytes, "syscw: ");
        require(reads && writes, "/proc/self/io lacks syscall counters");
        return {std::strtoull(reads + 7, nullptr, 10),
                std::strtoull(writes + 7, nullptr, 10)};
#else
        return {};
#endif
    }
    static Io difference(Io before, Io after) {
#if defined(__linux__)
        require(after.reads > before.reads && after.writes >= before.writes,
                "process I/O counters moved backwards");
        // Linux accounts the pread after the proc snapshot is generated:
        // after includes the BEFORE snapshot's one read, not its own read.
        // Subtract exactly that read; no test I/O remains in the delta.
        return {after.reads - before.reads - 1, after.writes - before.writes};
#else
        (void)before;
        (void)after;
        return {};
#endif
    }
private:
#if defined(__linux__)
    int fd_ = -1;
#endif
};

// The five-word retirement shape of CohortFacts, with a small live marker.
// Every field is preserved by retirement once the row has no live members.
struct Facts {
    struct Record {
        std::uint64_t handle;
        std::int64_t cycle;
        std::uint64_t count, digest, sides;
    };
    Record record{};
    bool live = false;
    Record retired_record() const noexcept { return record; }
    static Facts restore_record(const Record& record) { return {record, false}; }
    bool spillable() const noexcept { return !live; }
};
static_assert(sizeof(Facts::Record) == 40, "the cohort retirement width");
using Table = pineforge::detail::DiskResidentIdTable<Facts>;

// Deliberately independent of the new member so the base compiles and fails
// on real syscalls, rather than failing on a missing kResidentRows symbol.
constexpr std::size_t resident_rows = 512;

Facts facts(std::size_t n, bool live = false) {
    return {{n + 1, static_cast<std::int64_t>(n + 11), n * 3,
             1469598103934665603ULL ^ n, n % 4}, live};
}
bool same(const Facts::Record& a, const Facts::Record& b) {
    return a.handle == b.handle && a.cycle == b.cycle && a.count == b.count
        && a.digest == b.digest && a.sides == b.sides;
}
std::string key(std::size_t n) {
    return "resident-cohort-with-a-long-id/" + std::to_string(n) + std::string("\0tail", 5);
}
void no_io(const char* row, Io delta, std::size_t calls) {
    if (!ProcessIo::enabled()) return;
    std::printf("[%s] calls=%zu syscr=%llu syscw=%llu\n", row, calls,
                static_cast<unsigned long long>(delta.reads),
                static_cast<unsigned long long>(delta.writes));
    require(delta.reads == 0 && delta.writes == 0, "resident operation issued file I/O");
}

void resident_hits(ProcessIo& counter) {
    Directory directory;
    Table table;
    table.set_directory(directory.path); // No slab or ScratchArena: real file mode.
    const std::array<std::string, 3> ids{{key(0), key(1), key(2)}};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto placed = table.emplace(ids[i], facts(i, true));
        require(placed.second, "insert a fresh resident key");
        placed.first->second.record.cycle += 100; // Dirty after initial disk insertion.
        placed.first->second.live = false;
    }
    const auto expected = facts(0).record.cycle + 100;
    const auto before = counter.sample();
    for (int i = 0; i < 10'000; ++i) {
        const auto found = table.find(ids[0]);
        require(found != table.end() && found->second.record.cycle == expected,
                "repeated find lost the latest value");
    }
    const auto after = counter.sample();
    no_io("unit-find", ProcessIo::difference(before, after), 10'000);

    const Table& constant = table;
    const auto before_other = counter.sample();
    for (int i = 0; i < 10'000; ++i) {
        const auto found = constant.find(ids[0]);
        const auto resident = table.find_resident(ids[0]);
        const auto const_resident = constant.find_resident(ids[0]);
        const auto placed = table.emplace(ids[0], facts(999));
        require(found != constant.end() && resident != table.end()
                && const_resident != constant.end() && !placed.second,
                "resident key lookup or duplicate insertion failed");
        require(&found->second == &resident->second && &found->second == &const_resident->second
                && &found->second == &placed.first->second
                && found->second.record.cycle == expected,
                "resident index lost row identity or overwrote a duplicate");
    }
    const auto after_other = counter.sample();
    no_io("unit-const-resident-emplace", ProcessIo::difference(before_other, after_other), 10'000);
}

void bounded_round_trip(ProcessIo& counter) {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    constexpr std::size_t count = resident_rows + 17;
    std::vector<std::string> ids;
    std::vector<Facts::Record> expected;
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(key(i));
        auto value = facts(i, true);
        const auto placed = table.emplace(ids.back(), value);
        require(placed.second, "insert beyond resident bound");
        placed.first->second.record.cycle += 1000;
        placed.first->second.live = false;
        expected.push_back(placed.first->second.retired_record());
        require(table.resident_size() <= resident_rows, "emplace exceeded resident row budget");
    }
    require(table.size() == count, "physical eviction changed logical count");
    require(table.find_resident(ids.front()) == table.end(), "oldest eligible row did not spill");
    require(table.find_resident(ids.back()) != table.end(), "newest row did not stay resident");
    const auto before = counter.sample();
    {
        const auto restored = table.find(ids.front());
        require(restored != table.end() && same(restored->second.record, expected.front()),
                "first evicted row did not restore its dirty record");
    }
    const auto after = counter.sample();
    if (ProcessIo::enabled()) {
        const auto delta = ProcessIo::difference(before, after);
        std::printf("[unit-spilled] syscr=%llu syscw=%llu\n",
                    static_cast<unsigned long long>(delta.reads),
                    static_cast<unsigned long long>(delta.writes));
        // Positive control: a real spill/restore must register actual disk
        // reads and eviction writes, with the same counter as the zero rows.
        require(delta.reads > 0 && delta.writes > 0, "spill/restore bypassed process I/O counters");
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto found = table.find(ids[i]);
        require(found != table.end() && !found->second.live && same(found->second.record, expected[i]),
                "spilled row did not round-trip its dirty record");
        require(table.resident_size() <= resident_rows, "lookup exceeded resident row budget");
    }
    // Mutating a pinned row and nesting a miss must not evict that row.
    {
        const auto pin = table.find(ids.front());
        pin->second.record.cycle += 7;
        expected.front() = pin->second.retired_record();
        require(table.find("not-an-id") == table.end(), "true miss became a logical row");
        table.spill();
        const auto again = table.find(ids.front());
        require(&again->second == &pin->second, "nested lookup broke the shared-row pin");
        require(table.resident_size() == 1, "explicit spill retained unpinned empty rows");
    }
    table.spill();
    require(table.resident_size() == 0, "explicit spill is no longer eager");
    std::vector<std::string> insertion, sorted;
    table.for_each_id([&](const auto& id) { insertion.push_back(id); });
    table.for_each_sorted_id([&](const auto& id) { sorted.push_back(id); });
    auto expected_sorted = ids;
    std::sort(expected_sorted.begin(), expected_sorted.end());
    require(insertion == ids && sorted == expected_sorted, "historical key walk changed order");
    require(table.resident_size() == 0, "key walks restored physical rows");
    std::size_t visited = 0;
    for (const auto& row : table.all_sorted()) {
        require(row.first == expected_sorted.at(visited++), "all_sorted changed logical order");
        const auto at = std::find(ids.begin(), ids.end(), row.first);
        require(at != ids.end() && same(row.second.record, expected.at(at - ids.begin())),
                "all_sorted used a stale disk copy");
    }
    require(visited == count, "all_sorted omitted logical rows");
}

void pins_and_live_rows_survive_pressure() {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    for (std::size_t i = 0; i < resident_rows + 3; ++i) table.emplace(key(i), facts(i, true));
    require(table.resident_size() == resident_rows + 3, "cache pressure evicted live members");
    {
        const auto pin = table.find(key(0));
        for (auto& row : table) row.second.live = false;
        require(table.find("not-an-id") == table.end(), "pressure lookup invented an id");
        require(table.resident_size() == resident_rows, "released live rows did not trim to budget");
        const auto again = table.find_resident(key(0));
        require(again != table.end() && &again->second == &pin->second,
                "oldest caller-pinned row was evicted under pressure");
        require(table.find_resident(key(1)) == table.end(), "oldest unpinned row did not spill");
        table.spill();
        require(table.resident_size() == 1, "explicit spill ignored a pin or retained eligible rows");
    }
    table.spill();
    require(table.resident_size() == 0, "released pin could not be evicted");
}

void resident_order_moves_and_dirty_hash_walk() {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    // Live rows retain the original newest-logical-insertion ordering.
    const std::array<std::string, 3> ids{{"z", "a", "m"}};
    for (std::size_t i = 0; i < ids.size(); ++i) table.emplace(ids[i], facts(i, true));
    std::vector<std::string> newest, insertion;
    for (const auto& row : table) newest.push_back(row.first);
    for (const auto& id : table.resident_ids()) insertion.push_back(id);
    require(newest == std::vector<std::string>({"m", "a", "z"})
            && insertion == std::vector<std::string>({"z", "a", "m"}),
            "resident iteration order changed");
    Table moved(std::move(table));
    Table assigned;
    assigned = std::move(moved);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto found = assigned.find(ids[i]);
        require(found != assigned.end(), "move lost the resident key index");
        found->second.record.cycle += 99;
        found->second.live = false;
    }
    // No explicit spill first: a hash walk must see current resident values,
    // including the dirty first lexical row, rather than the insertion copy.
    const Table& constant = assigned;
    std::size_t visited = 0;
    for (const auto& row : constant.all_sorted()) {
        const auto at = std::find(ids.begin(), ids.end(), row.first);
        require(at != ids.end(), "hash walk invented an id");
        const auto original = facts(static_cast<std::size_t>(at - ids.begin()));
        require(row.second.record.cycle == original.record.cycle + 99,
                "resident/spilled hash walk lost a mutation");
        ++visited;
    }
    require(visited == ids.size(), "hash walk lost a resident row");
    assigned.clear();
    require(assigned.size() == 0 && assigned.resident_size() == 0
            && assigned.find_resident("a") == assigned.end(), "clear retained a cached key");
    require(assigned.emplace("a", facts(100)).second
            && assigned.find("a")->second.record.handle == 101, "clear/reuse used a stale row");
}

constexpr int host_bars = 200;
constexpr int warmup_bars = 16;

class ReissueHost final : public pineforge::source::PineStrategyHost {
public:
    explicit ReissueHost(ProcessIo& counter) : counter_(counter) {
        attach_pine_execution_adapter();
        pineforge::source::PineStrategyConfig config;
        config.initial_capital = 100'000.0;
        config.default_qty_type = static_cast<int>(pineforge::QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = 1;
        config.margin_long = 100.0;
        config.margin_short = 100.0;
        config.commission_value = 0.0;
        config.slippage = 0;
        config.process_orders_on_close = false;
        config.calc_on_order_fills = false;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(1.0);
    }
    void on_source_bar(const pineforge::Bar&) override {
        const auto bar = pine_bar_index();
        require(bar >= 0 && bar < host_bars, "unexpected host callback coordinate");
        ++callbacks;
        if (bar >= warmup_bars) {
            require(physical_position().signed_units == 1.0
                    && physical_position().lot_count == 1,
                    "host witness is not holding its fixed-id lot");
        }
        const auto before = bar >= warmup_bars ? counter_.sample() : Io{};
        // A real unfilled limit exit is amended every held bar. Same-side
        // entry reissues hit the pyramiding cap. Only these two public calls
        // are inside the counter window: no printing, chart/report reads,
        // broker hash walks, bar ingress, or counter-file open/close.
        strategy_exit(exit_id_, entry_id_, 1000.0, missing_);
        strategy_entry(entry_id_, true);
        const auto after = bar >= warmup_bars ? counter_.sample() : Io{};
        if (bar >= warmup_bars) {
            counts[static_cast<std::size_t>(bar)] = ProcessIo::difference(before, after);
            ++measured;
        }
    }
    int callbacks = 0, measured = 0;
    std::array<Io, host_bars> counts{};
private:
    ProcessIo& counter_;
    const std::string entry_id_ = "L", exit_id_ = "X";
    const double missing_ = std::numeric_limits<double>::quiet_NaN();
};

void host_reissues(ProcessIo& counter) {
    ReissueHost host(counter);
    std::array<pineforge::Bar, host_bars> bars{};
    for (int i = 0; i < host_bars; ++i)
        bars[static_cast<std::size_t>(i)] =
            {100.0, 101.0, 99.0, 100.0, 1.0, 1'736'121'600'000LL + i * 60'000LL};
    host.run(bars.data(), host_bars, "1", "1", false, 4,
             pineforge::MagnifierDistribution::ENDPOINTS);
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks == host_bars && host.measured == host_bars - warmup_bars,
            "host counter window did not execute every held bar");
    bool zero = true;
    if (ProcessIo::enabled()) {
        for (int i = warmup_bars; i < host_bars; ++i) {
            const auto delta = host.counts[static_cast<std::size_t>(i)];
            std::printf("[host-reissue] bar=%d syscr=%llu syscw=%llu\n", i,
                        static_cast<unsigned long long>(delta.reads),
                        static_cast<unsigned long long>(delta.writes));
            zero = zero && delta.reads == 0 && delta.writes == 0;
        }
        // No average or N-dependent tolerance can hide per-bar growth.
        require(zero, "host strategy reissues issued file I/O after warmup");
    }
}
} // namespace

int main() {
    try {
        require(pineforge::detail::ScratchArena::attached() == nullptr,
                "resident I/O witnesses require real file-backed scratch");
        ProcessIo counter;
        int failures = 0;
        const auto run = [&](const char* row, auto&& body) {
            try {
                body();
            } catch (const std::exception& error) {
                std::fprintf(stderr, "FAIL [%s]: %s\n", row, error.what());
                ++failures;
            }
        };
        // Keep running after UNIT fails, so the base also executes and prints
        // the independent HOST counter witness.
        run("unit-resident", [&] { resident_hits(counter); });
        run("unit-bound", [&] { bounded_round_trip(counter); });
        run("unit-pins-live", pins_and_live_rows_survive_pressure);
        run("unit-order-hash", resident_order_moves_and_dirty_hash_walk);
        run("host-reissue", [&] { host_reissues(counter); });
        return failures == 0 ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL resident I/O setup: %s\n", error.what());
        return 1;
    }
}
