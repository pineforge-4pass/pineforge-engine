#pragma once

// The syscall observer and independent image reader are shared with the
// existing id-cache tests. No cache counter certifies a cold read.
#include "p2cx_sc412_io.hpp"

#include <pineforge/detail/disk_id_table.hpp>
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/id_pair_store.hpp>

#include <map>
#include <optional>
#include <utility>

namespace id_table_cache_test {
using sc412::require;
using namespace pineforge::detail;
using Tree = DiskIdTable<std::uint64_t>;
using Minima = IdPairStore<std::uint64_t>;
using Cycles = IdPairStore<std::int64_t, true>;
struct Facts {
    using Record = std::uint64_t;
    std::uint64_t value = 0;
    Record retired_record() const noexcept { return value; }
    static Facts restore_record(Record record) { return {record}; }
    bool spillable() const noexcept { return true; }
};
using Cohorts = DiskResidentIdTable<Facts>;
using PairKey = std::pair<std::string, std::optional<std::string>>;

// Named namespace: explicit Access instantiations must have external linkage.
// Read the current owners rather than a registry of closed/reused rehash fds.
template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct TreeFile { friend auto member(TreeFile); };
struct TreeCache { friend auto member(TreeCache); };
struct CohortIndex { friend auto member(CohortIndex); };
struct MinimaData { friend auto member(MinimaData); };
struct MinimaIndex { friend auto member(MinimaIndex); };
struct MinimaCache { friend auto member(MinimaCache); };
struct CycleData { friend auto member(CycleData); };
struct CycleIndex { friend auto member(CycleIndex); };
struct CycleCache { friend auto member(CycleCache); };
struct PairFile { friend auto member(PairFile); };
template struct Access<TreeFile, &Tree::file_>;
template struct Access<TreeCache, &Tree::read_cache_>;
template struct Access<CohortIndex, &Cohorts::index_>;
template struct Access<MinimaData, &Minima::data_>;
template struct Access<MinimaIndex, &Minima::index_>;
template struct Access<MinimaCache, &Minima::read_cache_>;
template struct Access<CycleData, &Cycles::data_>;
template struct Access<CycleIndex, &Cycles::index_>;
template struct Access<CycleCache, &Cycles::read_cache_>;
template struct Access<PairFile, &IdPairScratchFile::file_>;

inline int descriptor(const Tree& tree) {
    return (tree.*member(TreeFile{})).descriptor();
}
inline int descriptor(const IdPairScratchFile& file) {
    return (file.*member(PairFile{})).descriptor();
}
inline std::string key_for(std::size_t n) {
    auto key = "cohort/" + std::to_string(n) + std::string("\0tail", 5);
    key.resize(192, static_cast<char>('a' + n % 26));
    return key;
}
inline std::string from_for(std::size_t n) {
    return "entry/" + std::to_string(n % 19) + std::string("\0from", 5);
}

struct Bytes {
    std::vector<unsigned char> value;
    void word(std::uint64_t n) {
        for (unsigned shift = 0; shift < 64; shift += 8)
            value.push_back(static_cast<unsigned char>(n >> shift));
    }
    void text(const std::string& text) {
        word(text.size());
        value.insert(value.end(), text.begin(), text.end());
    }
    void pair_key(const PairKey& key) {
        text(key.first);
        word(key.second.has_value());
        if (key.second) text(*key.second);
    }
};
struct Result {
    Bytes output;
    std::vector<std::vector<unsigned char>> images;
    sc412::Traffic cold_ids, cold_pairs;
};
struct ByteCapacity {
    std::size_t tree = 0, pair = 0;
    template<class Table> Table make(bool paired) const {
        return Table(paired ? pair : tree);
    }
};
struct DefaultCapacity {
    template<class Table> Table make(bool) const { return Table(); }
};
inline void identical(const Result& expected, const Result& actual) {
    require(!expected.output.value.empty()
            && expected.output.value == actual.output.value,
            "complete id/cohort/minima/cycle outputs are byte identical");
    require(!expected.images.empty() && expected.images == actual.images,
            "complete tree and pair data/index file images are byte identical");
}

template<class Capacity>
Result workload(Capacity capacity) {
    auto tree = capacity.template make<Tree>(false);
    auto cohorts = capacity.template make<Cohorts>(false);
    auto minima = capacity.template make<Minima>(true);
    auto cycles = capacity.template make<Cycles>(true);
    tree.set_directory("."); cohorts.set_directory(".");
    minima.set_directory("."); cycles.set_directory(".");
    std::map<std::string, std::uint64_t> tree_rows, cohort_rows;
    std::map<PairKey, std::uint64_t> minima_rows;
    std::map<std::string, std::int64_t> cycle_rows;
    std::vector<std::string> inserted_ids;
    Result result;
    auto& out = result.output;
    const auto dump = [&] {
        out.word(tree.size()); out.word(tree.file_bytes());
        std::map<std::string, std::uint64_t> observed_tree, observed_cohorts;
        auto sorted_tree = tree_rows.begin();
        tree.for_each_sorted([&](const auto& key, auto value) {
            require(sorted_tree != tree_rows.end() && key == sorted_tree->first,
                    "tree sorted order matches independent map");
            ++sorted_tree;
            out.text(key); out.word(value);
            require(observed_tree.emplace(key, value).second, "tree walk has no duplicates");
        });
        require(observed_tree == tree_rows, "full tree rows match independent map");
        std::vector<std::string> ids;
        tree.for_each_inserted([&](const auto& key, auto value) {
            out.text(key); out.word(value); ids.push_back(key);
        });
        require(ids == inserted_ids, "tree first-insertion order is complete");
        ids.clear();
        cohorts.for_each_id([&](const auto& key) { out.text(key); ids.push_back(key); });
        require(ids == inserted_ids, "cohort ids preserve first-insertion order");
        auto sorted_cohort = cohort_rows.begin();
        cohorts.for_each_sorted_id([&](const auto& key) {
            require(sorted_cohort != cohort_rows.end() && key == sorted_cohort->first,
                    "cohort sorted ids match independent map order");
            ++sorted_cohort;
            out.text(key);
        });
        require(sorted_cohort == cohort_rows.end(), "sorted cohort id walk is complete");
        out.word(cohorts.size());
        for (const auto& row : cohorts.all_sorted()) {
            out.text(row.first); out.word(row.second.value);
            require(observed_cohorts.emplace(row.first, row.second.value).second,
                    "cohort walk has no duplicates");
        }
        require(observed_cohorts == cohort_rows, "full cohort facts match independent map");
        cohorts.spill();
        require(cohorts.resident_size() == 0, "decoded test facts really leave residency");
        out.word(minima.size());
        std::map<PairKey, std::uint64_t> observed_minima;
        for (const auto& row : minima) {
            const PairKey key{row.first, row.from_entry};
            out.pair_key(key); out.word(row.second);
            require(observed_minima.emplace(key, row.second).second, "minima walk has no duplicates");
        }
        require(observed_minima == minima_rows, "all minima domains match independent map");
        for (const auto& row : minima_rows) {
            const auto value = row.first.second
                ? minima.lookup(row.first.first, *row.first.second) : minima.lookup(row.first.first);
            out.pair_key(row.first); out.word(value.has_value());
            require(value == row.second, "every retained minimum is readable");
            out.word(*value);
        }
        out.word(cycles.size());
        std::map<std::string, std::int64_t> observed_cycles;
        for (const auto& row : cycles) {
            require(!row.from_entry, "consumed cycles keep the single-key domain");
            out.text(row.first); out.word(static_cast<std::uint64_t>(row.second));
            require(observed_cycles.emplace(row.first, row.second).second,
                    "cycle walk has no duplicates");
        }
        require(observed_cycles == cycle_rows, "all consumed cycles match independent map");
        pineforge::BrokerStateHashSink actual, reference;
        cycles.fold_sorted(actual);
        reference.u(cycle_rows.size());
        for (const auto& row : cycle_rows) { reference.s(row.first); reference.i(row.second); }
        require(actual.h == reference.h, "consumed fold keeps its exact ordered spelling");
        out.word(actual.h);
        for (const auto& row : cycle_rows) {
            const auto value = cycles.lookup(row.first);
            require(value == row.second, "every consumed cycle is readable");
            out.text(row.first); out.word(static_cast<std::uint64_t>(*value));
        }
        for (int fd : {descriptor(tree), descriptor(cohorts.*member(CohortIndex{})),
                       descriptor(minima.*member(MinimaData{})),
                       descriptor(minima.*member(MinimaIndex{})),
                       descriptor(cycles.*member(CycleData{})),
                       descriptor(cycles.*member(CycleIndex{}))})
            result.images.push_back(sc412::image(fd));
    };

    // 320 * (56 + 8 + 192) + 1 = 81,921 bytes per AVL: 21 legacy pages,
    // exceeding 16 slots. Both pair stores also exceed their 8-slot defaults.
    constexpr std::size_t count = 320;
    for (std::size_t i = 0; i < count; ++i) {
        const auto n = (i * 197) % count;
        const auto key = key_for(n), from = from_for(n);
        const auto placed = tree.insert(key, 3 * n + 7);
        require(placed.second, "new tree id inserted");
        out.word(placed.first); out.word(placed.second);
        tree_rows[key] = 3 * n + 7;
        require(cohorts.emplace(key, Facts{5 * n + 11}).second, "new cohort id inserted");
        cohort_rows[key] = 5 * n + 11;
        inserted_ids.push_back(key);
        minima.min_assign(key, 1000 + n); minima.min_assign(key, from, 2000 + n);
        minima_rows[{key, std::nullopt}] = 1000 + n;
        minima_rows[{key, from}] = 2000 + n;
        const auto joined = key + '\x1f' + from;
        const auto cycle = -7 * static_cast<std::int64_t>(n + 1);
        require(cycles.insert_or_assign(joined, cycle), "new consumed cycle inserted");
        cycle_rows[joined] = cycle;
    }
    require(tree.file_bytes() == 1 + count * (56 + 8 + 192),
            "workload covers more pages than the legacy large cache");
    dump();
    // Empty the physical page tags explicitly; spill already removed decoded
    // facts. The following observations come from the real pread interposer.
    (tree.*member(TreeCache{})).invalidate();
    ((cohorts.*member(CohortIndex{})).*member(TreeCache{})).invalidate();
    (minima.*member(MinimaCache{})).invalidate();
    (cycles.*member(CycleCache{})).invalidate();
    {
        sc412::Measure reads;
        const auto key = key_for(0), from = from_for(0), joined = key + '\x1f' + from;
        const auto at = tree.find(key);
        require(at && tree.get(*at) == tree_rows.at(key), "cold valid tree key reads");
        const auto cohort = cohorts.find(key);
        require(cohort != cohorts.end() && cohort->second.value == cohort_rows.at(key),
                "cold valid cohort key restores");
        require(minima.lookup(key, from) == minima_rows.at({key, from}),
                "cold valid minimum pair reads");
        require(cycles.lookup(joined) == cycle_rows.at(joined), "cold valid consumed cycle reads");
        const auto traffic = reads.finish();
        result.cold_ids = traffic[sc412::ids];
        result.cold_pairs = traffic[sc412::pairs];
        require(traffic[sc412::ids].reads >= 2 && traffic[sc412::pairs].reads >= 2
                && traffic[sc412::ids].past_end == 0 && traffic[sc412::pairs].past_end == 0,
                "cold tree/cohort/pair probes really read backing within its extent");
        std::printf("ID_CACHE_COLD ids_reads=%llu ids_bytes=%llu pair_reads=%llu pair_bytes=%llu\n",
            static_cast<unsigned long long>(result.cold_ids.reads),
            static_cast<unsigned long long>(result.cold_ids.read_bytes),
            static_cast<unsigned long long>(result.cold_pairs.reads),
            static_cast<unsigned long long>(result.cold_pairs.read_bytes));
    }
    for (std::size_t n = 0; n < count; ++n) {
        const auto key = key_for(n), from = from_for(n), joined = key + '\x1f' + from;
        if (n % 3 == 0) {
            const auto at = tree.find(key);
            require(at && !tree.insert(key, 0).second, "duplicate keeps its original id");
            tree.set(*at, 10000 + n); tree_rows[key] = 10000 + n;
            const auto cohort = cohorts.find(key);
            require(cohort != cohorts.end(), "updated cohort id is present");
            cohort->second.value = 20000 + n; cohort_rows[key] = 20000 + n;
            minima.min_assign(key, n); minima.min_assign(key, from, n + 1);
            minima_rows[{key, std::nullopt}] = n; minima_rows[{key, from}] = n + 1;
        }
        if (n % 5 == 0) {
            require(minima.erase(key, from), "erase existing pair");
            minima_rows.erase({key, from});
            require(!minima.lookup(key, from) && !minima.erase(key, from),
                    "erased pair stays absent in the transcript");
            out.text(key); out.word(minima.lookup(key, from).has_value());
            if (n % 10 == 0) {
                require(minima.insert_or_assign(key, from, 30000 + n), "resurrect erased pair");
                minima_rows[{key, from}] = 30000 + n;
            }
        }
        if (n % 7 == 0) {
            require(cycles.erase(joined) && !cycles.lookup(joined), "erase consumed cycle");
            cycle_rows.erase(joined);
            out.text(joined); out.word(cycles.lookup(joined).has_value());
            if (n % 14 == 0) {
                require(cycles.insert_or_assign(joined, static_cast<std::int64_t>(n)),
                        "resurrect consumed cycle");
                cycle_rows[joined] = static_cast<std::int64_t>(n);
            }
        }
    }
    dump();
    // All four owners use their ordinary invalidating moves.
    tree = Tree(std::move(tree));
    cohorts = Cohorts(std::move(cohorts));
    minima = Minima(std::move(minima));
    cycles = Cycles(std::move(cycles));
    dump();
    tree.clear(); cohorts.clear(); minima.clear(); cycles.clear();
    require(!tree.find(key_for(0)) && !cohorts.contains(key_for(0))
            && !minima.lookup(key_for(0)) && !cycles.lookup(key_for(0) + '\x1f' + from_for(0)),
            "clear removes every old logical key");
    tree_rows.clear(); cohort_rows.clear(); minima_rows.clear(); cycle_rows.clear();
    inserted_ids = {"fresh"};
    tree.insert("fresh", 31); tree_rows["fresh"] = 31;
    cohorts.emplace("fresh", Facts{37}); cohort_rows["fresh"] = 37;
    minima.insert_or_assign("fresh", 41); minima_rows[{"fresh", std::nullopt}] = 41;
    cycles.insert_or_assign("fresh", -43); cycle_rows["fresh"] = -43;
    dump();
    return result;
}
} // namespace id_table_cache_test
