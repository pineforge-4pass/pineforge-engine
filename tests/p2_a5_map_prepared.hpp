#pragma once
#include "p2_a5strmap_fixture.hpp"
#include <pineforge/map.hpp>
#if __has_include(<pineforge/script_string.hpp>)
#include "p2_a5strmap_store_fixture.hpp"
#define PF_A5_EXTERNAL_MAP 1
#endif

namespace {
using namespace pineforge;
class PreparedMapHost final : public source::PineStrategyHost {
public:
    PreparedMapHost() {
        a5_fixture::configure(*this);
#ifdef PF_A5_EXTERNAL_MAP
        session_ = std::make_unique<a5_fixture::Session>();
        auto holder = std::make_shared<detail::MapStoreHolder>(*session_->store);
        detail::MapStoreScope scope(holder);
#endif
        map_ = PineMap<int, double>::new_();
        calls_before_ = a5_heap::calls.load();
    }
    void on_source_bar(const Bar& bar) override {
        a5_heap::Window count;
        (void)map_.put(pine_bar_index() % 50000, bar.close);
        ++callbacks_;
    }
    void verify(const std::vector<Bar>& input) const {
        a5_fixture::require(callbacks_ == PF_A5_BARS && map_.size() == 50000,
                            "prepared scalar fixture did not reach its full domain");
        for (int key = 0; key < 50000; ++key) {
            const auto last = PF_A5_BARS - 50000 + key;
            a5_fixture::require(map_.get(key) == input[static_cast<std::size_t>(last)].close,
                                "prepared scalar map changed exact values");
        }
        const auto calls = a5_heap::calls.load() - calls_before_;
        std::printf("A5MAP prepared bars=%d entries=%d allocation_calls=%zu\n",
                    PF_A5_BARS, map_.size(), calls);
        a5_fixture::require(calls == 0, "scalar put allocated after preparation");
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("p2-a5strmap/prepared"); sink.i(callbacks_);
#ifdef PF_A5_EXTERNAL_MAP
        map_.hash_into(sink, [](auto& out, int key) { out.i(key); },
                      [](auto& out, double value) { out.d(value); });
#else
        const auto keys = map_.keys();
        sink.u(keys.size());
        for (auto key : keys) { sink.i(key); sink.d(map_.get(key)); }
#endif
    }
private:
#ifdef PF_A5_EXTERNAL_MAP
    std::unique_ptr<a5_fixture::Session> session_;
#endif
    PineMap<int, double> map_;
    int callbacks_ = 0;
    std::size_t calls_before_ = 0;
};
} // namespace

int main() {
    try {
        const auto input = a5_fixture::bars(PF_A5_BARS);
        PreparedMapHost host;
        host.run(input.data(), PF_A5_BARS);
        a5_fixture::require(host.last_error().empty(), host.last_error().c_str());
        host.verify(input);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5MAP prepared FAIL: %s\n", error.what());
        return 1;
    }
}
