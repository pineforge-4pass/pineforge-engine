#include "o1e3_allocation_probe.hpp"
#include "../src/broker_state_hash_internal.hpp"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
void run(std::size_t population) {
    std::unordered_map<std::string, double> values;
    std::unordered_map<std::uint64_t, std::unordered_map<std::string, double>> owned;
    std::unordered_set<std::string> names;
    std::map<std::string, double> reference;
    std::set<std::string> sorted_names;
    for (std::size_t i = 0; i < population; ++i) {
        const auto key = std::string(48, 'x') + std::to_string((i * 4051) % population)
            + std::string("\0\x80\xff", 3);
        const double value = static_cast<double>(i) / 7.0;
        values.emplace(key, value); reference.emplace(key, value);
        names.emplace(key); sorted_names.emplace(key);
        owned[i].emplace(key, value);
    }
    require(values.size() == population && names.size() == population && owned.size() == population,
            "E15 named populations not reached");
    pineforge::BrokerStateHashSink expected;
    expected.u(reference.size());
    for (const auto& row : reference) { expected.s(row.first); expected.d(row.second); }
    expected.u(owned.size());
    for (std::size_t i = 0; i < population; ++i) {
        expected.u(i); expected.u(owned.at(i).size());
        for (const auto& row : owned.at(i)) { expected.s(row.first); expected.d(row.second); }
    }
    expected.u(sorted_names.size()); for (const auto& name : sorted_names) expected.s(name);
    pineforge::BrokerStateHashSink actual;
    o1e3_test::AllocationProbe memory;
    pineforge::hash_str_double_map(actual, values);
    pineforge::hash_token_owned_map(actual, owned);
    pineforge::hash_str_set(actual, names);
    const auto peak = memory.stop();
    require(actual.h == expected.h, "E15 exact sorted fold differs");
    require(peak <= 4096, "E15 ordering workspace grew with key population");
#if defined(PINEFORGE_HAS_ORDERED_HASH_SCAN_V1)
    // Check every emitted leaf against a separately ordered std::map, not
    // only the final hash. The NUL/high bytes exercise string ordering and
    // encoding independently of numeric-key ordering.
    auto expected_row = reference.begin();
    std::size_t delivered = 0;
    pineforge::visit_ordered_hash_map(values, [&](const auto& row) {
        require(expected_row != reference.end() && row.first == expected_row->first,
                "E15 ordered key leaf differs");
        require(std::memcmp(&row.second, &expected_row->second, sizeof(double)) == 0,
                "E15 value leaf differs");
        if (delivered == 0 || delivered == population / 2 || delivered + 1 == population) {
            pineforge::BrokerStateHashSink nested;
            pineforge::hash_str_set(nested, names);
            require(row.first == expected_row->first, "E15 nested read invalidated row");
        }
        ++expected_row; ++delivered;
    });
    require(delivered == population && expected_row == reference.end(), "E15 full-consumption witness");
#endif
}
}
int main() {
    try { run(1024); run(4096); }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
    return 0;
}
