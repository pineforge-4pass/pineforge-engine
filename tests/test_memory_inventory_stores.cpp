// Inventory storage is checked for content and reuse, with current ignored-id
// retention pinned even after the corresponding control files are deleted.
// These are counts/content and reuse checks, not universal RAM ceilings.
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_file.hpp>
#include "../runner/service.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>
#include <unistd.h>

namespace {
using namespace pineforge;
using Host = source::PineStrategyHost;
using Adapter = source::PineExecutionAdapter;
using Table = source::PlacementTable;
using Scheduler = source::PineScheduler;
using Arena = detail::ScratchArena;
using File = detail::ScratchFile;
using Controls = live::ControlDirectory;
namespace fs = std::filesystem;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
#define MEMBER(tag, owner, field) \
    struct tag { friend auto access(tag); }; \
    template struct Access<tag, &owner::field>
MEMBER(HostAdapter, Host, adapter_);
MEMBER(Placements, Adapter, placement_);
MEMBER(Window, Table, window_);
MEMBER(Head, Table, head_);
MEMBER(Old, Table, old_);
MEMBER(Recent, Scheduler, confirmed_input_bars_);
MEMBER(Opens, Scheduler, confirmed_open_input_bars_);
MEMBER(OpenCount, Scheduler, confirmed_open_input_count_);
MEMBER(Region, File, region_);
MEMBER(Ignored, Controls, ignored_);
#undef MEMBER

using RecentType = std::remove_reference_t<decltype(std::declval<Scheduler&>().*access(Recent{}))>;
using OpensType = std::remove_reference_t<decltype(std::declval<Scheduler&>().*access(Opens{}))>;
static_assert(std::is_same_v<RecentType, std::array<Bar, 8>>,
              "pinescheduler_fixed_cadence_state_and_confirmed_input_rings: recent input ring has eight inline slots");
static_assert(std::is_same_v<OpensType, std::array<Bar, 2>>,
              "pinescheduler_fixed_cadence_state_and_confirmed_input_rings: opening input ring has two inline slots");

std::size_t checks = 0, failures = 0;
#define CHECK(row, expression) do { \
    ++checks; \
    if (!(expression)) { \
        ++failures; \
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s\n", row, __FILE__, __LINE__, #expression); \
    } \
} while (false)

template<class F> void scenario(const char* row, F&& f) {
    try { f(); }
    catch (const std::exception& e) {
        CHECK(row, false);
        std::fprintf(stderr, "  exception [%s]: %s\n", row, e.what());
    }
    catch (...) { CHECK(row, false); }
}

constexpr std::int64_t epoch = 1736121600000LL;
constexpr std::int64_t minute = 60000;
Bar bar_at(std::size_t index) {
    const double p = 100.0 + static_cast<double>(index % 7);
    return {p, p + 2, p - 2, p + 1, 1, epoch + static_cast<std::int64_t>(index) * minute};
}
bool same_bar(const Bar& a, const Bar& b) {
    return a.timestamp == b.timestamp && a.open == b.open && a.high == b.high
        && a.low == b.low && a.close == b.close && a.volume == b.volume;
}

// Observe the actual begin reserve before any input or source calculation.
class ReserveHost final : public Host {
public:
    std::size_t callbacks = 0, preparations = 0, begin_capacity = 0;
    void on_source_bar(const Bar&) override { ++callbacks; }
protected:
    void prepare_script_run(const Bar*, int n, bool) override {
        auto& adapter = this->*access(HostAdapter{});
        const auto& table = adapter.*access(Placements{});
        const auto& window = table.*access(Window{});
        ++preparations;
        CHECK("prepare_native_begin_placement_continuation", callbacks == 0);
        CHECK("prepare_native_begin_placement_continuation", window.empty());
        // pine_strategy_host.cpp reserves 2*n; PlacementTable::reserve clamps
        // the requested number of slots to PlacementTable::kWindowReserve=256.
        const auto requested = 2U * static_cast<std::size_t>(n);
        const auto ceiling = std::min<std::size_t>(requested, 256);
        CHECK("prepare_native_begin_placement_continuation", window.capacity() <= ceiling);
        begin_capacity = window.capacity();
    }
};

void placement_reserve_and_window() {
    std::array<std::size_t, 2> capacities{};
    std::size_t trial = 0;
    for (const int n : {25000, 100000}) {
        ReserveHost host;
        std::vector<Bar> bars;
        bars.reserve(n);
        for (int i = 0; i < n; ++i) bars.push_back(bar_at(i));
        host.run(bars.data(), n, "1", "1");
        CHECK("prepare_native_begin_placement_continuation", host.last_error().empty());
        CHECK("prepare_native_begin_placement_continuation", host.preparations == 1);
        CHECK("prepare_native_begin_placement_continuation", host.callbacks == static_cast<std::size_t>(n));
        capacities[trial++] = host.begin_capacity;
        auto& adapter = host.*access(HostAdapter{});
        adapter.reset_for_run();
        CHECK("prepare_native_begin_placement_continuation", (adapter.*access(Placements{}).*access(Window{})).empty());
    }
    CHECK("prepare_native_begin_placement_continuation", capacities[0] == capacities[1]);

    // A pinned old row plus a rolling 64-row tail exercises both the sparse
    // compaction path and its old_ insert. The model never uses table readers
    // to decide which keys should exist.
    for (const std::size_t rounds : {512U, 2048U}) {
        Table table;
        table.fixture_use_memory_auxiliary_tables();
        std::set<std::uint64_t> model;
        std::size_t inserted = 0;
        const auto add = [&](std::uint64_t key) {
            source::PlacementSnapshot row{};
            row.source_id = "rolling";
            row.command_sequence = key;
            const auto result = table.try_emplace(key, std::move(row));
            CHECK("prepare_native_begin_placement_continuation", result.second);
            model.insert(key);
            ++inserted;
        };
        add(1);
        bool saw_old = false;
        for (std::uint64_t key = 2; key < rounds + 2; ++key) {
            add(key);
            if (key > 65) {
                CHECK("prepare_native_begin_placement_continuation", table.erase(key - 64));
                model.erase(key - 64);
            }
            table.compact();
            const auto& window = table.*access(Window{});
            const auto head = table.*access(Head{});
            const auto& old = table.*access(Old{});
            saw_old = saw_old || !old.empty();
            // compact returns only at span<=64 or span<=4*window_rows_.
            // window_rows_ <= all live rows; old_ has only live rows.
            CHECK("prepare_native_begin_placement_continuation", window.size() - head <= std::max<std::size_t>(64, 4 * model.size()));
            CHECK("prepare_native_begin_placement_continuation", old.size() <= model.size());
            std::set<std::uint64_t> actual;
            table.visit_rows([&](std::uint64_t id, const auto&) {
                actual.insert(id);
                return false;
            });
            CHECK("prepare_native_begin_placement_continuation", actual == model);
        }
        CHECK("prepare_native_begin_placement_continuation", inserted == rounds + 1);
        CHECK("prepare_native_begin_placement_continuation", saw_old);
        CHECK("prepare_native_begin_placement_continuation", table.size() == 1 + 64);
        table.clear();
        CHECK("prepare_native_begin_placement_continuation", (table.*access(Window{})).empty());
        CHECK("prepare_native_begin_placement_continuation", (table.*access(Old{})).empty());
    }
}

// Two independent FIFO models, including replacement without FIFO advance.
void scheduler_rings() {
    for (const int intervals : {16, 64}) {
        Scheduler scheduler;
        NativeBeginArgs begin{};
        begin.is_stream = true;
        scheduler.capture_begin(begin);
        std::vector<Bar> recent, opens;
        const auto retain = [](std::vector<Bar>& model, const Bar& bar, std::size_t limit) {
            const auto found = std::find_if(model.begin(), model.end(), [&](const Bar& b) {
                return b.timestamp == bar.timestamp;
            });
            if (found != model.end()) *found = bar;
            else {
                model.push_back(bar);
                if (model.size() > limit) model.erase(model.begin());
            }
        };
        const auto compare = [&] {
            CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.confirmed_input_count() == recent.size());
            CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.*access(OpenCount{}) == opens.size());
            const auto compare_ring = [&](const auto& ring, const std::vector<Bar>& expected) {
                std::vector<Bar> actual(ring.begin(), ring.begin() + expected.size());
                std::sort(actual.begin(), actual.end(), [](const Bar& a, const Bar& b) {
                    return a.timestamp < b.timestamp;
                });
                auto sorted = expected;
                std::sort(sorted.begin(), sorted.end(), [](const Bar& a, const Bar& b) {
                    return a.timestamp < b.timestamp;
                });
                CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", std::equal(actual.begin(), actual.end(), sorted.begin(), same_bar));
            };
            compare_ring(scheduler.*access(Recent{}), recent);
            compare_ring(scheduler.*access(Opens{}), opens);
        };
        // Each interval is longer than the recent ring. Its anchor therefore
        // can be found ONLY in the opening ring by the end of that interval.
        for (int interval = 0; interval < intervals; ++interval) {
            for (int offset = 0; offset < 12; ++offset) {
                auto bar = bar_at(interval * 12 + offset);
                const auto open = bar_at(interval * 12).timestamp;
                scheduler.retain_confirmed_input(bar, open);
                retain(recent, bar, 8);
                if (offset == 0) retain(opens, bar, 2);
                compare();
                if (offset == 0) {
                    bar.high += 0.5; // duplicate anchor updates, never appends
                    scheduler.retain_confirmed_input(bar, open);
                    retain(recent, bar, 8);
                    retain(opens, bar, 2);
                    compare();
                }
            }
            for (int old = 0; old <= interval; ++old) {
                const auto stamp = bar_at(old * 12).timestamp;
                const bool retained = old >= interval - 1;
                CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.confirmed_input_bar_at(stamp).has_value() == retained);
            }
        }
        CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.*access(OpenCount{}) == 2);
        CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.confirmed_input_count() == 8);
        scheduler.capture_begin(begin);
        CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.*access(OpenCount{}) == 0);
        CHECK("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler.confirmed_input_count() == 0);
    }
}

// Create a small ordinary arena backing without an external fixture.
// Regular-file backing is a public ScratchArena backend, including on Linux.
struct ArenaBacking {
    std::FILE* backing = nullptr;
    bool attached = false;
    ArenaBacking() {
        if (Arena::attached()) throw std::runtime_error("inventory storage requires its own arena");
        backing = std::tmpfile();
        if (!backing) throw std::runtime_error("tmpfile failed");
        if (ftruncate(fileno(backing), 16 * Arena::kChunkBytes) != 0) {
            std::fclose(backing);
            backing = nullptr;
            throw std::runtime_error("ftruncate failed");
        }
        try { Arena::attach(fileno(backing), 16 * Arena::kChunkBytes); attached = true; }
        catch (...) { std::fclose(backing); backing = nullptr; throw; }
    }
    ~ArenaBacking() {
        if (attached) {
            try { Arena::detach(); } catch (...) { CHECK("scratcharena_region_chunks", false); }
        }
        if (backing) std::fclose(backing);
    }
};

void region_chunks() {
    ArenaBacking backing;
    constexpr auto chunk = Arena::kChunkBytes;
    File bounded, unbounded;
    const auto inspect = [&](std::uint64_t bounded_size, std::uint64_t unbounded_size) {
        const auto& a = bounded.*access(Region{});
        const auto& b = unbounded.*access(Region{});
        const auto count = [](std::uint64_t bytes) { return bytes / chunk + (bytes % chunk != 0); };
        CHECK("scratcharena_region_chunks", a.size == bounded_size);
        CHECK("scratcharena_region_chunks", b.size == unbounded_size);
        CHECK("scratcharena_region_chunks", a.chunks.size() == count(bounded_size));
        CHECK("scratcharena_region_chunks", b.chunks.size() == count(unbounded_size));
        const auto stats = Arena::attached()->stats();
        CHECK("scratcharena_region_chunks", a.chunks.size() + b.chunks.size() == stats.chunks_total - stats.chunks_free);
        if (bounded.opened()) CHECK("scratcharena_region_chunks", a.chunks.capacity() <= 4);
        // Every logical chunk has distinct physical ownership, across regions.
        std::set<std::uint32_t> physical(a.chunks.begin(), a.chunks.end());
        physical.insert(b.chunks.begin(), b.chunks.end());
        CHECK("scratcharena_region_chunks", physical.size() == a.chunks.size() + b.chunks.size());
    };
    std::array<std::size_t, 2> retained{};
    std::size_t trial = 0;
    for (const int cycles : {8, 32}) {
        for (int cycle = 0; cycle < cycles; ++cycle) {
            bounded.open_bounded("mem", 4 * chunk);
            unbounded.open("mem");
            inspect(0, 0);
            bounded.ensure_size(chunk + 7);
            unbounded.ensure_size(2 * chunk + 1);
            inspect(chunk + 7, 2 * chunk + 1);
            const unsigned char value = static_cast<unsigned char>(cycle);
            bounded.write(3 * chunk + 4, &value, 1);
            inspect(3 * chunk + 5, 2 * chunk + 1);
            bounded.release_prefix(chunk);
            unbounded.release_prefix(2 * chunk);
            inspect(2 * chunk + 5, 1);
            unsigned char read = 0;
            bounded.read(2 * chunk + 4, &read, 1);
            CHECK("scratcharena_region_chunks", read == value);
            bounded.truncate(chunk - 1);
            unbounded.truncate(0);
            inspect(chunk - 1, 0);
            bounded.clear();
            unbounded.clear();
            inspect(0, 0);
            CHECK("scratcharena_region_chunks", (bounded.*access(Region{})).chunks.capacity() == 0);
            CHECK("scratcharena_region_chunks", (unbounded.*access(Region{})).chunks.capacity() == 0);
        }
        retained[trial++] = (bounded.*access(Region{})).chunks.capacity()
            + (unbounded.*access(Region{})).chunks.capacity();
    }
    CHECK("scratcharena_region_chunks", retained[0] == retained[1]);
}

struct PrivateDirectory {
    fs::path path;
    PrivateDirectory() {
        auto pattern = (fs::temp_directory_path() / "pineforge-mem-XXXXXX").string();
        const auto* result = mkdtemp(pattern.data());
        if (!result) throw std::runtime_error("mkdtemp failed");
        path = result;
    }
    ~PrivateDirectory() { std::error_code error; fs::remove_all(path, error); }
};

void ignored_control_ids() {
    for (const int n : {5, 300}) {
        PrivateDirectory directory;
        Controls controls(directory.path.string(), false);
        std::set<std::string> expected;
        std::vector<fs::path> paths;
        for (int i = 0; i < n; ++i) {
            // Fixed-width lowercase hex is the real request-id grammar.
            char tail[17];
            std::snprintf(tail, sizeof tail, "%016llx", static_cast<unsigned long long>(i));
            const std::string id = std::string(48, '0') + tail;
            expected.insert(id);
            paths.push_back(directory.path / (id + ".request.json"));
            std::ofstream output(paths.back());
            output << '{'; // syntactically invalid JSON enters ignore()
            output.close();
            CHECK("controldirectory_ignored", !output.fail());
        }
        std::size_t accepted = 0;
        const auto poll = [&] {
            controls.poll("mem", [&](const live::Json&) { ++accepted; return std::uint64_t{0}; });
        };
        poll();
        CHECK("controldirectory_ignored", expected.size() == static_cast<std::size_t>(n));
        CHECK("controldirectory_ignored", (controls.*access(Ignored{})) == expected);
        CHECK("controldirectory_ignored", accepted == 0);
        poll();
        CHECK("controldirectory_ignored", (controls.*access(Ignored{})) == expected);
        // Removing the files does NOT prune remembered IDs in today's code.
        for (const auto& path : paths) CHECK("controldirectory_ignored", fs::remove(path));
        poll();
        CHECK("controldirectory_ignored", (controls.*access(Ignored{})) == expected);
        std::ofstream(paths.front()) << '{'; // same bad ID after disappearance
        poll();
        CHECK("controldirectory_ignored", (controls.*access(Ignored{})) == expected);
    }
}
} // namespace

int main() {
    scenario("prepare_native_begin_placement_continuation", placement_reserve_and_window);
    scenario("pinescheduler_fixed_cadence_state_and_confirmed_input_rings", scheduler_rings);
    scenario("scratcharena_region_chunks", region_chunks);
    scenario("controldirectory_ignored", ignored_control_ids);
    std::printf("memory proof inventory_stores: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
