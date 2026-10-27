#pragma once
#include "p2_a5strmap_fixture.hpp"
#include <pineforge/str_utils.hpp>

#if __has_include(<pineforge/script_string.hpp>)
#include "p2_a5strmap_store_fixture.hpp"
#define PF_A5_EXTERNAL_STRING 1
#endif

namespace {
using namespace pineforge;
#ifdef PF_A5_EXTERNAL_STRING
using Text = ScriptString;
#else
// RED baseline: the accumulating std::string and str_tostring concatenation
// idioms emitted by frozen2 generated code; no missing-module failure.
using Text = std::string;
#endif

class StringGrowthHost final : public source::PineStrategyHost {
public:
    explicit StringGrowthHost(bool formatted) : formatted_(formatted) {
        a5_fixture::configure(*this);
#ifdef PF_A5_EXTERNAL_STRING
        a5_heap::Window count;
        session_ = std::make_unique<a5_fixture::Session>();
        text_ = Text(*session_->store);
#endif
        before_ = a5_heap::live.load();
        a5_heap::reset_peak();
    }
    void on_source_bar(const Bar& bar) override {
        a5_heap::Window count;
        ++callbacks_;
        if (formatted_) {
            const auto oracle = str_tostring(bar.close);
#ifdef PF_A5_EXTERNAL_STRING
            text_ += str_tostring(*session_->store, bar.close);
#else
            text_ += oracle;
#endif
            expected_bytes_ += oracle.size();
        } else {
            const char ch = static_cast<char>('a' + pine_bar_index() % 26);
            text_ += ch;
            ++expected_bytes_;
        }
    }
    void verify(const std::vector<Bar>& input) const {
        a5_fixture::require(callbacks_ == PF_A5_BARS, "string host did not calculate every bar");
        a5_fixture::require(text_.size() == expected_bytes_, "string accumulation lost bytes");
        const auto live = a5_heap::growth(a5_heap::live.load(), before_);
        const auto peak = a5_heap::growth(a5_heap::peak.load(), before_);
        std::printf("A5STR bars=%d kind=%s bytes=%llu live=%zu peak=%zu bound=%zu\n",
                    PF_A5_BARS, formatted_ ? "formatted" : "byte",
                    static_cast<unsigned long long>(expected_bytes_), live, peak,
                    a5_fixture::operation_ceiling);
        a5_fixture::require(live <= a5_fixture::operation_ceiling,
                            "accumulated script string retains a growing heap payload");
        a5_fixture::require(peak <= a5_fixture::operation_ceiling,
                            "string operation materializes accumulated payload");
#ifdef PF_A5_EXTERNAL_STRING
        a5_fixture::require(session_->store->stats().bytes_live >= expected_bytes_,
                            "string never grew its external payload");
        const auto resident = session_->store->stats().bytes_resident;
        a5_fixture::require(resident <= a5_fixture::store_ceiling(session_->shape),
                            "provider workspace exceeds its geometry-derived bound");
        a5_fixture::require(session_->store->stats().bytes_backing > resident,
                            "string fixture never exceeded the resident window");
#endif
        // Verify ALL accumulated bytes, not only the reported length.
        std::uint64_t offset = 0;
#ifdef PF_A5_EXTERNAL_STRING
        std::array<char, 4096> page{};
        std::uint64_t first = ScriptString::npos;
        std::size_t available = 0;
        const auto byte_at = [&](std::uint64_t index) {
            if (first == ScriptString::npos || index - first >= available) {
                first = index;
                available = text_.read(first, page.data(), page.size());
            }
            return page[static_cast<std::size_t>(index - first)];
        };
#endif
        for (int i = 0; i < PF_A5_BARS; ++i) {
            const auto expected = formatted_ ? str_tostring(input[static_cast<std::size_t>(i)].close)
                                            : std::string(1, static_cast<char>('a' + i % 26));
            for (const char ch : expected)
#ifdef PF_A5_EXTERNAL_STRING
                a5_fixture::require(byte_at(offset++) == ch, "accumulated string byte differs");
#else
                a5_fixture::require(text_[offset++] == ch, "accumulated string byte differs");
#endif
        }
    }
    void hash_host_extension(BrokerStateHashSink& sink) const override {
        source::PineStrategyHost::hash_host_extension(sink);
        sink.s("p2-a5strmap/string"); sink.b(formatted_);
        sink.i(callbacks_); sink.u(expected_bytes_); sink.u(text_.size());
#ifdef PF_A5_EXTERNAL_STRING
        // The length has already been folded above, so stream just the bytes.
        std::array<char, 4096> buffer{};
        for (std::uint64_t offset = 0; offset < text_.size();) {
            const auto n = text_.read(offset, buffer.data(), buffer.size());
            sink.bytes(buffer.data(), n); offset += n;
        }
#else
        sink.bytes(text_.data(), text_.size());
#endif
    }
private:
    bool formatted_;
    int callbacks_ = 0;
    std::uint64_t expected_bytes_ = 0;
    std::size_t before_ = 0;
#ifdef PF_A5_EXTERNAL_STRING
    std::unique_ptr<a5_fixture::Session> session_;
#endif
    Text text_;
};
} // namespace

int main() {
    try {
        const auto input = a5_fixture::bars(PF_A5_BARS);
        for (bool formatted : {false, true}) {
            StringGrowthHost host(formatted);
            host.run(input.data(), PF_A5_BARS);
            a5_fixture::require(host.last_error().empty(), host.last_error().c_str());
            host.verify(input);
        }
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "A5STR FAIL: %s\n", error.what());
        return 1;
    }
}
