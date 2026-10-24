#pragma once
#include "p2_a5strmap_fixture.hpp"
#include <pineforge/map.hpp>

#if __has_include(<pineforge/script_string.hpp>)
#include "p2_a5strmap_store_fixture.hpp"
#define PF_A5_EXTERNAL_MAP 1
#endif

namespace {
using namespace pineforge;
using Map = PineMap<int, std::string>;
constexpr int pairs = 50000;
constexpr std::size_t value_bytes = a5_fixture::page_bytes;

std::string payload(int key) {
    std::string value(value_bytes, static_cast<char>('a' + key % 26));
    for (unsigned i = 0; i < sizeof key; ++i)
        value[i] = static_cast<char>(static_cast<unsigned>(key) >> (8 * i));
    value[value.size() / 2] = '\0';
    return value;
}

class MapGrowthHost final : public source::PineStrategyHost {
public:
    MapGrowthHost() {
        a5_fixture::configure(*this);
        a5_heap::Window count;
#ifdef PF_A5_EXTERNAL_MAP
        session_ = std::make_unique<a5_fixture::Session>();
        auto holder = std::make_shared<detail::MapStoreHolder>(*session_->store);
        detail::MapStoreScope scope(holder);
#endif
        // The full scalar arena and hash domain must be prepared here,
        // before bar zero. No payload has been admitted at this checkpoint.
        map_ = Map::new_();
        before_ = a5_heap::live.load();
        a5_heap::reset_peak();
    }
    void on_source_bar(const Bar&) override {
        ++callbacks_;
        const int wanted = static_cast<int>(
            static_cast<std::int64_t>(callbacks_) * pairs / PF_A5_BARS);
        a5_heap::Window count;
        while (inserted_ < wanted) {
            (void)map_.put(inserted_, payload(inserted_));
            ++inserted_;
        }
    }
    void verify() {
        a5_fixture::require(callbacks_ == PF_A5_BARS, "map host did not calculate every bar");
        a5_fixture::require(inserted_ == pairs && map_.size() == pairs,
                            "map fixture did not reach the shipped 50000 pair cap");
        const auto retained = a5_heap::growth(a5_heap::live.load(), before_);
        const auto peak = a5_heap::growth(a5_heap::peak.load(), before_);
        std::printf("A5MAP bars=%d entries=%d payload=%zu live=%zu peak=%zu bound=%zu\n",
                    PF_A5_BARS, map_.size(), pairs * value_bytes, retained, peak,
                    a5_fixture::operation_ceiling);
#ifndef PF_A5_ENUMERATION_ROW
        a5_fixture::require(retained <= a5_fixture::operation_ceiling,
                            "map string payload grows retained C++ heap");
        a5_fixture::require(peak <= a5_fixture::operation_ceiling,
                            "map insertion materializes more than fixed operation scratch");
#endif
        for (int i = 0; i < pairs; ++i)
            a5_fixture::require(map_.get(i) == payload(i), "map payload bytes differ");

        const auto capture_before = a5_heap::live.load();
        a5_heap::reset_peak();
        {
            a5_heap::Window count;
            const auto keys = map_.keys();
            const auto values = map_.values();
            const auto snapshot = map_.snapshot();
            a5_fixture::require(keys.size() == pairs && values.size() == pairs,
                                "enumeration omitted entries");
            for (int i = 0; i < pairs; ++i) {
                a5_fixture::require(keys[static_cast<std::size_t>(i)] == i,
                                    "map insertion order differs");
                a5_fixture::require(values[static_cast<std::size_t>(i)] == payload(i),
                                    "map enumeration changed bytes");
            }
            (void)map_.put(0, "changed");
            (void)map_.remove(1);
            (void)map_.put(1, "reinserted");
            a5_fixture::require(keys[1] == 1 && values[0] == payload(0),
                                "enumeration did not preserve its version");
            map_.restore(snapshot);
            a5_fixture::require(map_.get(0) == payload(0) && map_.get(1) == payload(1),
                                "snapshot did not restore payloads");
        }
        const auto capture_peak = a5_heap::growth(a5_heap::peak.load(), capture_before);
        std::printf("A5MAP capture entries=%d peak=%zu bound=%zu\n",
                    pairs, capture_peak, a5_fixture::operation_ceiling);
        a5_fixture::require(capture_peak <= a5_fixture::operation_ceiling,
                            "enumeration or snapshot copied the whole map into C++ heap");
#ifdef PF_A5_EXTERNAL_MAP
        a5_fixture::require(session_->store->stats().bytes_live >= pairs * value_bytes,
                            "map fixture never externalized its payload");
#endif
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        a5_heap::Window count;
        sink.s("p2-a5strmap/map"); sink.i(callbacks_); sink.i(inserted_);
#ifdef PF_A5_EXTERNAL_MAP
        map_.hash_into(sink, [](auto& out, int key) { out.i(key); },
                      [](auto& out, const std::string& value) { out.s(value); });
#else
        sink.i(map_.size());
        const auto keys = map_.keys();
        for (auto key : keys) { sink.i(key); sink.s(map_.get(key)); }
#endif
    }
private:
#ifdef PF_A5_EXTERNAL_MAP
    std::unique_ptr<a5_fixture::Session> session_;
#endif
    Map map_;
    int callbacks_ = 0, inserted_ = 0;
    std::size_t before_ = 0;
};
} // namespace

int main() {
    try {
        const auto input = a5_fixture::bars(PF_A5_BARS);
        MapGrowthHost host;
        host.run(input.data(), PF_A5_BARS);
        a5_fixture::require(host.last_error().empty(), host.last_error().c_str());
        host.verify();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5MAP FAIL: %s\n", error.what());
        return 1;
    }
}
