#pragma once

#include <pineforge/engine.hpp>

#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#define PINEFORGE_HAS_ORDERED_HASH_SCAN_V1 1

namespace pineforge {
inline namespace engine_script_run_v19 {

// BrokerStateHashSink itself is public (engine.hpp): a host folds its own
// state through it. What stays here are the kernel's own fold helpers.
// Unordered maps/sets have unique keys. Select their next key by scanning,
// retaining only three iterators (previous, candidate, current), instead of
// copying the key population into a sort workspace. Keeping iterators rather
// than pointers also supports external ranges whose iterator owns its row.
// The range must not be mutated by emit. Key comparison is deliberately the
// same operator< the former std::sort used, including for byte strings.
template<class Range, class Key, class Emit>
inline void visit_ordered_hash_rows(const Range& rows, Key key, Emit emit) {
    const auto end = rows.end();
    auto previous = end;
    for (std::size_t delivered = 0; delivered < rows.size(); ++delivered) {
        auto next = end;
        for (auto at = rows.begin(); at != end; ++at) {
            if (previous != end && !(key(*previous) < key(*at))) continue;
            if (next == end || key(*at) < key(*next)) next = at;
        }
        if (next == end) throw std::logic_error("hash range has nonunique keys");
        emit(*next);
        previous = std::move(next);
    }
}

template<class Map, class Emit>
inline void visit_ordered_hash_map(const Map& rows, Emit emit) {
    visit_ordered_hash_rows(rows, [](const auto& row) -> const auto& { return row.first; }, emit);
}

template<class Set, class Emit>
inline void visit_ordered_hash_set(const Set& rows, Emit emit) {
    visit_ordered_hash_rows(rows, [](const auto& key) -> const auto& { return key; }, emit);
}

inline void hash_str_double_map(
        BrokerStateHashSink& f, const std::unordered_map<std::string, double>& m) {
    f.u(m.size());
    visit_ordered_hash_map(m, [&](const auto& kv) { f.s(kv.first); f.d(kv.second); });
}

inline void hash_token_owned_map(
        BrokerStateHashSink& f,
        const std::unordered_map<uint64_t, std::unordered_map<std::string, double>>& m) {
    f.u(m.size());
    visit_ordered_hash_map(m, [&](const auto& kv) {
        f.u(kv.first);
        hash_str_double_map(f, kv.second);
    });
}

inline void hash_str_set(BrokerStateHashSink& f, const std::unordered_set<std::string>& s) {
    f.u(s.size());
    visit_ordered_hash_set(s, [&](const auto& x) { f.s(x); });
}

} // inline namespace engine_script_run_v19
} // namespace pineforge
