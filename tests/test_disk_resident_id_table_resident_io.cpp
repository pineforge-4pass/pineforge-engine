// CR150-CX: real process I/O counters, not a ScratchFile wrapper or a mock.
// Round 1's rows have executed base/fix evidence. Round 2 adds the runtime
// budget API and a separate eager-sorted-flush mutation witness for the lab.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/resident_budget.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

class EnvironmentScope {
public:
    EnvironmentScope(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) previous_ = previous;
        require((value ? ::setenv(name, value, 1) : ::unsetenv(name)) == 0,
                "select resident test environment");
    }
    EnvironmentScope(const EnvironmentScope&) = delete;
    EnvironmentScope& operator=(const EnvironmentScope&) = delete;
    ~EnvironmentScope() {
        if ((previous_ ? ::setenv(name_.c_str(), previous_->c_str(), 1)
                       : ::unsetenv(name_.c_str())) != 0) {
            std::fputs("restore resident test environment failed\n", stderr);
            std::abort();
        }
    }
private:
    std::string name_;
    std::optional<std::string> previous_;
};

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
        const auto outer_before = sample();
        (void)sample();
        (void)sample();
        const auto outer_after = sample();
        const auto nested_control = difference(outer_before, outer_after, 2);
        require(nested_control.reads == 0 && nested_control.writes == 0,
                "nested /proc/self/io accounting control is not zero");
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
    static Io difference(Io before, Io after, std::uint64_t inner_samples = 0) {
#if defined(__linux__)
        require(after.reads > before.reads && after.writes >= before.writes,
                "process I/O counters moved backwards");
        // Linux accounts the pread after the proc snapshot is generated:
        // after includes the BEFORE snapshot's one read, not its own read.
        // The whole-bar row also encloses exactly two nested call-window
        // snapshots. Remove those real counter reads, never any host I/O.
        require(after.reads - before.reads > inner_samples,
                "counter window lacks its own snapshot reads");
        return {after.reads - before.reads - 1 - inner_samples, after.writes - before.writes};
#else
        (void)before;
        (void)after;
        (void)inner_samples;
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

constexpr std::size_t resident_rows = Table::kResidentRows;

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

void hash_walk_cycle(ProcessIo& counter) {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    const std::array<std::string, 5> ids{{"z", "a", "m", std::string("b\0x", 3), "q"}};
    for (std::size_t i = 0; i < ids.size(); ++i) {
        const auto placed = table.emplace(ids[i], facts(i, true));
        require(placed.second, "hash-cycle insert");
        placed.first->second.record.cycle += 71;
        placed.first->second.live = false;
    }
    std::vector<std::string> sorted(ids.begin(), ids.end());
    std::sort(sorted.begin(), sorted.end());
    require(ids.size() < table.resident_limit(), "hash cycle must fit below the resident limit");
    const auto before = counter.sample();
    for (int cycle = 0; cycle < 2'000; ++cycle) {
        std::size_t visited = 0;
        for (const auto& row : table.all_sorted()) {
            require(row.first == sorted.at(visited++), "hash cycle lost lexical order");
            const auto original = std::find(ids.begin(), ids.end(), row.first);
            require(original != ids.end(), "hash cycle invented a key");
            auto expected = facts(static_cast<std::size_t>(original - ids.begin())).record;
            expected.cycle += 71;
            require(!row.second.live && same(row.second.record, expected),
                    "hash cycle lost a dirty resident value");
        }
        require(visited == ids.size(), "hash cycle omitted a row");
        const auto found = table.find(ids[2]);
        require(found != table.end() && found->second.record.cycle == facts(2).record.cycle + 71,
                "resident find after a hash walk lost its value");
    }
    const auto after = counter.sample();
    // Protects Iterator<*, Sorted>::operator++'s spill_to(resident_limit_).
    // Replacing ONLY that call with spill() flushes unpinned non-first rows;
    // subsequent walk loads/finds do real file I/O and MUST fail this zero.
    no_io("unit-hash-walk-cycle", ProcessIo::difference(before, after), 2'000);
}

void sorted_walk_above_limit() {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    constexpr std::size_t limit = 2, count = 3 * limit;
    table.set_resident_limit(limit);
    std::vector<std::string> ids;
    std::vector<Facts::Record> records;
    for (std::size_t i = 0; i < count; ++i) {
        ids.push_back(key(i));
        const auto placed = table.emplace(ids.back(), facts(i, true));
        require(placed.second, "bounded sorted walk insert");
        placed.first->second.record.cycle += 83;
        placed.first->second.live = false;
        records.push_back(placed.first->second.record);
    }
    auto sorted = ids;
    std::sort(sorted.begin(), sorted.end());
    for (const bool keep_pin : {false, true}) {
        std::optional<Table::iterator> pin;
        if (keep_pin) pin = table.find(ids.back());
        std::size_t visited = 0;
        for (const auto& row : table.all_sorted()) {
            require(row.first == sorted.at(visited++), "bounded walk repeats or reorders rows");
            const auto at = std::find(ids.begin(), ids.end(), row.first);
            require(at != ids.end() && same(row.second.record, records.at(at - ids.begin())),
                    "bounded sorted walk lost a dirty value");
            // The range's first iterator and current iterator each pin one
            // row, in addition to the optional independent caller pin.
            require(table.resident_size() <= limit + 2 + (keep_pin ? 1 : 0),
                    "walk residency exceeds the budget plus possible pins");
        }
        require(visited == count && table.size() == count, "bounded walk lost logical rows");
        require(table.resident_size() <= limit + (keep_pin ? 1 : 0),
                "completed walk retained more than its limit plus caller pins");
        if (keep_pin) {
            const auto resident = table.find_resident(ids.back());
            require(resident != table.end() && &resident->second == &(*pin)->second,
                    "sorted walk evicted a caller-pinned row");
        }
    }
    require(table.find("not-an-id") == table.end() && table.resident_size() <= limit,
            "released walk pins did not trim at the next lookup");
}

void shrinking_limit_is_deferred(ProcessIo& counter) {
    Directory directory;
    Table table;
    table.set_directory(directory.path);
    constexpr std::size_t count = 8, limit = 2;
    table.set_resident_limit(count);
    std::array<std::string, count> ids;
    for (std::size_t i = 0; i < count; ++i) {
        ids[i] = key(i);
        table.emplace(ids[i], facts(i));
    }
    require(table.resident_size() == count, "shrink setup is not fully resident");
    const auto before = counter.sample();
    table.set_resident_limit(limit);
    const auto after = counter.sample();
    no_io("unit-shrink-setter", ProcessIo::difference(before, after), 1);
    require(table.resident_size() == count, "resident-limit setter evicted eagerly");
    {
        // This key is resident already: the deferred trim cannot rely on a miss.
        const auto found = table.find(ids.back());
        require(found != table.end() && same(found->second.record, facts(count - 1).record),
                "first lookup after shrink lost the resident value");
    }
    require(table.resident_size() <= limit && table.size() == count,
            "first lookup after shrink did not trim to budget");
    require(table.find_resident(ids.front()) == table.end(), "shrink did not evict oldest first");
    table.set_resident_limit(1);
    const Table& constant = table;
    require(constant.find(ids.back()) != constant.end() && table.resident_size() <= 1,
            "const lookup did not apply a deferred shrink");
}

void resident_budget_contract() {
    using namespace pineforge;
    using namespace pineforge::detail;
    const auto& row = resident_budget(ResidentKind::idtable_entries);
    EnvironmentScope ledger(kResidentBudgets[0].env_name, nullptr);
    EnvironmentScope idtable(row.env_name, nullptr);
    EnvironmentScope placement(kResidentBudgets[2].env_name, nullptr);
    EnvironmentScope journal(kResidentBudgets[3].env_name, nullptr);
    require(resident_entries(ResidentKind::idtable_entries) == Table::kResidentRows,
            "unset budget differs from the table's named default");
    const auto maximum = std::to_string(row.max_entries);
    const std::array<std::pair<const char*, std::uint64_t>, 3> valid_values{{
        {"1", 1}, {"0002", 2}, {maximum.c_str(), row.max_entries}
    }};
    for (const auto& valid : valid_values) {
        EnvironmentScope selected(row.env_name, valid.first);
        const auto entries = resident_entries(ResidentKind::idtable_entries);
        require(entries == valid.second, "valid decimal budget parsed incorrectly");
    }
    const auto too_large = std::to_string(row.max_entries + 1);
    struct InvalidBudget {
        std::string value;
        std::string json_value;
        const char* reason;
        int error;
    };
    const std::vector<InvalidBudget> invalid_values{
        {"", "", "nondecimal", EINVAL},
        {"0", "0", "nonpositive", EINVAL},
        {"000", "000", "nonpositive", EINVAL},
        {"-1", "-1", "nondecimal", EINVAL},
        {"+1", "+1", "nondecimal", EINVAL},
        {" 1", " 1", "nondecimal", EINVAL},
        {"1 ", "1 ", "nondecimal", EINVAL},
        {"1x", "1x", "nondecimal", EINVAL},
        {"x1", "x1", "nondecimal", EINVAL},
        {"1\n", "1\\u000a", "nondecimal", EINVAL},
        {"a\"b", "a\\\"b", "nondecimal", EINVAL},
        {"a\\b", "a\\\\b", "nondecimal", EINVAL},
        {"18446744073709551616", "18446744073709551616", "overflow", ERANGE},
        {too_large, too_large, "above_maximum", ERANGE},
        {std::string(1025, 'x'), std::string(1024, 'x'), "nondecimal", EINVAL},
        {std::string(1023, 'x') + "\xC3\xA9", std::string(1023, 'x'),
         "nondecimal", EINVAL},
        {std::string(1022, 'x') + "\xC3\xA9" + "y", std::string(1022, 'x') + "\xC3\xA9",
         "nondecimal", EINVAL},
    };
    for (const auto& invalid : invalid_values) {
        EnvironmentScope selected(row.env_name, invalid.value.c_str());
        bool refused = false;
        try { (void)resident_entries(ResidentKind::idtable_entries); }
        catch (const std::system_error& error) {
            const std::string message = error.what();
            const auto failure = classify_run_failure(error);
            const auto args = std::string("{\"kind\":\"idtable_entries\",\"max\":") + maximum
                + ",\"reason\":\"" + invalid.reason
                + "\",\"stage\":\"resident_budget\",\"value\":\"" + invalid.json_value + "\"}";
            refused = failure.code == RunFailureCode::scratch_configuration_invalid
                && failure.args && *failure.args == args
                && error.code() == std::error_code(invalid.error, std::generic_category())
                && message.find(row.name) != std::string::npos
                && message.find(std::string("value=\"") + invalid.value + "\"") != std::string::npos
                && message.find("max=" + maximum) != std::string::npos;
        }
        require(refused, "malformed/out-of-range budget lost its typed refusal or context");
    }
    // Synthetic widths exercise arithmetic; these are not measured cache sizes.
    constexpr ResidentBudgetDescriptor arithmetic{"arithmetic", "", 1, 1, 8, 16, true};
    static_assert(*resident_bytes(arithmetic, 1) == 24, "resident-byte product plus base");
    constexpr auto last = (std::numeric_limits<std::uint64_t>::max() - 16) / 8;
    static_assert(resident_bytes(arithmetic, last).has_value(), "last fitting byte product");
    static_assert(!resident_bytes(arithmetic, last + 1).has_value(), "overflow must be refused");
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < kResidentBudgets.size(); ++i) {
        const auto kind = static_cast<ResidentKind>(i);
        require(!kResidentBudgets[i].measured || kResidentBudgets[i].bytes_per_entry != 0,
                "a measured budget has a zero width");
        const auto bytes = resident_bytes(kind, resident_entries(kind));
        require(bytes.has_value(), "default resident-byte plan overflowed");
        expected += *bytes;
    }
    require(planned_resident_bytes() == expected, "resident-byte plan did not sum effective budgets");
    {
        EnvironmentScope invalid(kResidentBudgets[2].env_name, "abc");
        bool refused = false;
        try { (void)planned_resident_bytes(); }
        catch (const std::system_error& error) {
            const auto failure = classify_run_failure(error);
            const auto args = std::string("{\"kind\":\"placement_snapshots\",\"max\":")
                + std::to_string(kResidentBudgets[2].max_entries)
                + ",\"reason\":\"nondecimal\",\"stage\":\"resident_budget\",\"value\":\"abc\"}";
            refused = failure.code == RunFailureCode::scratch_configuration_invalid
                && failure.args && *failure.args == args
                && error.code() == std::error_code(EINVAL, std::generic_category());
        }
        require(refused, "byte planner ignored an invalid effective budget");
    }
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
        const auto start = bar >= warmup_bars ? counter_.sample() : Io{};
        if (bar > warmup_bars) {
            // Between successive starts there were exactly two counter-file
            // reads for the nested strategy-call window below. All output,
            // counter opens/closes and test fixture I/O stay outside the run.
            whole_bar_counts[static_cast<std::size_t>(bar - 1)] =
                ProcessIo::difference(previous_start_, start, 2);
            ++whole_bars_measured;
        }
        if (bar >= warmup_bars) previous_start_ = start;
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
    void print_cohort_layout() const {
        const auto layout = adapter_.fixture_cohort_resident_layout();
        // Real production CohortFacts, not the small Facts unit carrier.
        // These are value/container sizes, NOT allocator node-size estimates.
        std::printf("[idtable-layout] CohortFacts=%zu pair_string_facts=%zu shared_ptr_row=%zu "
                    "map_value=%zu key_index_value=%zu map=%zu key_index=%zu table=%zu string=%zu "
                    "bytes_per_entry=not-measured fixed_base_bytes=not-measured\n",
                    layout.value_bytes, layout.row_bytes, layout.shared_ptr_bytes,
                    layout.map_value_bytes, layout.key_index_value_bytes,
                    layout.map_bytes, layout.key_index_bytes, layout.table_bytes, sizeof(std::string));
    }
    int callbacks = 0, measured = 0, whole_bars_measured = 0;
    std::array<Io, host_bars> counts{};
    std::array<Io, host_bars> whole_bar_counts{};
private:
    ProcessIo& counter_;
    Io previous_start_{};
    const std::string entry_id_ = "L", exit_id_ = "X";
    const double missing_ = std::numeric_limits<double>::quiet_NaN();
};

void host_reissues(ProcessIo& counter) {
    EnvironmentScope default_budget("PF_RESIDENT_IDTABLE_ENTRIES", nullptr);
    ReissueHost host(counter);
    host.print_cohort_layout(); // Before either host counter window can begin.
    std::array<pineforge::Bar, host_bars> bars{};
    for (int i = 0; i < host_bars; ++i)
        bars[static_cast<std::size_t>(i)] =
            {100.0, 101.0, 99.0, 100.0, 1.0, 1'736'121'600'000LL + i * 60'000LL};
    host.run(bars.data(), host_bars, "1", "1", false, 4,
             pineforge::MagnifierDistribution::ENDPOINTS);
    require(host.last_error().empty(), host.last_error().c_str());
    require(host.callbacks == host_bars && host.measured == host_bars - warmup_bars,
            "host counter window did not execute every held bar");
    constexpr int whole_intervals = host_bars - warmup_bars - 1;
    require(host.whole_bars_measured == whole_intervals,
            "whole-bar counter window did not span every adjacent callback start");
    bool zero = true;
    if (ProcessIo::enabled()) {
        for (int i = warmup_bars; i < host_bars; ++i) {
            const auto delta = host.counts[static_cast<std::size_t>(i)];
            std::printf("[host-reissue] bar=%d syscr=%llu syscw=%llu\n", i,
                        static_cast<unsigned long long>(delta.reads),
                        static_cast<unsigned long long>(delta.writes));
            zero = zero && delta.reads == 0 && delta.writes == 0;
        }
        for (int i = warmup_bars; i < host_bars - 1; ++i) {
            const auto delta = host.whole_bar_counts[static_cast<std::size_t>(i)];
            std::printf("[host-whole-bar] bar=%d next=%d syscr=%llu syscw=%llu\n", i, i + 1,
                        static_cast<unsigned long long>(delta.reads),
                        static_cast<unsigned long long>(delta.writes));
        }
        constexpr int quarter = whole_intervals / 4;
        double first_reads = 0, first_writes = 0, last_reads = 0, last_writes = 0;
        for (int i = 0; i < quarter; ++i) {
            const auto first = host.whole_bar_counts[static_cast<std::size_t>(warmup_bars + i)];
            const auto last = host.whole_bar_counts[static_cast<std::size_t>(host_bars - 1 - quarter + i)];
            first_reads += static_cast<double>(first.reads);
            first_writes += static_cast<double>(first.writes);
            last_reads += static_cast<double>(last.reads);
            last_writes += static_cast<double>(last.writes);
        }
        first_reads /= quarter; first_writes /= quarter;
        last_reads /= quarter; last_writes /= quarter;
        // Full bars include genuine ingress/report/journal I/O. Allow four
        // extra syscalls per kind in the last-quarter mean for bounded page
        // boundary variation; this is a fixed additive allowance, never a
        // bar-index multiplier, and the whole-bar counts need not be zero.
        constexpr double allowance = 4.0;
        std::printf("[host-whole-bar-quarters] n=%d first_syscr=%.3f first_syscw=%.3f "
                    "last_syscr=%.3f last_syscw=%.3f allowance=%.0f\n",
                    quarter, first_reads, first_writes, last_reads, last_writes, allowance);
        require(last_reads <= first_reads + allowance && last_writes <= first_writes + allowance,
                "whole-bar I/O grows beyond the fixed quarter-mean allowance");
        // The nested strategy-call windows still require zero on every bar.
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
        run("unit-hash-walk-cycle", [&] { hash_walk_cycle(counter); });
        run("unit-sorted-bound", sorted_walk_above_limit);
        run("unit-shrink", [&] { shrinking_limit_is_deferred(counter); });
        run("unit-resident-budget", resident_budget_contract);
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
