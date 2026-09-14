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
inline namespace engine_script_run_v17 {

class BrokerStateHashSink {
public:
    uint64_t h = 1469598103934665603ULL;

    void bytes(const void* p, size_t n) {
        const unsigned char* c = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= c[i]; h *= 1099511628211ULL; }
    }

    void d(double v) {
        if (v == 0.0) v = 0.0;
        if (v != v) v = std::numeric_limits<double>::quiet_NaN();
        bytes(&v, sizeof v);
    }

    void i(int64_t v) { bytes(&v, sizeof v); }
    void u(uint64_t v) { bytes(&v, sizeof v); }
    void b(bool v) { const unsigned char c = v ? 1 : 0; bytes(&c, 1); }
    void s(const std::string& v) { u(v.size()); bytes(v.data(), v.size()); }
};

inline void hash_admission_field(BrokerStateHashSink& f, const admission::Field& field) {
    f.s(field.path); f.u(field.value.index());
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, uint64_t>) f.u(value);
        else if constexpr (std::is_same_v<T, int64_t>) f.i(value);
        else if constexpr (std::is_same_v<T, double>) f.d(value);
        else f.s(value);
    }, field.value);
}

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

} // inline namespace engine_script_run_v17
} // namespace pineforge
