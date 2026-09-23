#pragma once

#include <pineforge/engine.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pineforge {
inline namespace engine_script_run_v19 {

// BrokerStateHashSink itself is public (engine.hpp): a host folds its own
// state through it. What stays here are the kernel's own fold helpers.

inline void hash_str_double_map(
        BrokerStateHashSink& f, const std::unordered_map<std::string, double>& m) {
    std::vector<std::pair<std::string, double>> v(m.begin(), m.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    f.u(v.size());
    for (const auto& kv : v) { f.s(kv.first); f.d(kv.second); }
}

inline void hash_token_owned_map(
        BrokerStateHashSink& f,
        const std::unordered_map<uint64_t, std::unordered_map<std::string, double>>& m) {
    std::vector<uint64_t> keys;
    keys.reserve(m.size());
    for (const auto& kv : m) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    f.u(keys.size());
    for (uint64_t k : keys) {
        f.u(k);
        hash_str_double_map(f, m.at(k));
    }
}

inline void hash_str_set(BrokerStateHashSink& f, const std::unordered_set<std::string>& s) {
    std::vector<std::string> v(s.begin(), s.end());
    std::sort(v.begin(), v.end());
    f.u(v.size());
    for (const auto& x : v) f.s(x);
}

} // inline namespace engine_script_run_v19
} // namespace pineforge
