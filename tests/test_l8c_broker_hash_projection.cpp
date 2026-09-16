// A39 P0-13: a source host may extend the generic broker projection, but it
// must not replace the native request-core continuation hash.
#include <pineforge/source/pine_native_host.hpp>

#include <cstdint>
#include <cstdio>
#include <string>

using namespace pineforge;
namespace no = pineforge::native_order;

namespace {

int failures = 0;

#define CHECK(expr) do {                                                        \
    if (!(expr)) {                                                              \
        ++failures;                                                             \
        std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #expr);           \
    }                                                                           \
} while (false)

class HashProjectionProbe final : public source::PineNativeHost {
public:
    explicit HashProjectionProbe(std::string label) : label_(std::move(label)) {}

    void on_source_bar(const Bar&) override {
        no::Request request;
        request.intent = no::Transact{1.0};
        request.trigger = no::Limit{90.0};
        request.label = label_;
        const auto result = submit(request);
        CHECK(result.status == no::SubmitStatus::Accepted);
    }

private:
    std::string label_;
};

} // namespace

int main() {
    const Bar bars[] = {{100.0, 100.0, 100.0, 100.0, 1.0, 60'000}};
    HashProjectionProbe first("kernel-request-A");
    HashProjectionProbe second("kernel-request-B");
    first.run(bars, 1, "1", "1");
    second.run(bars, 1, "1", "1");
    CHECK(first.last_error().empty());
    CHECK(second.last_error().empty());
    CHECK(first.native_continuation_hash() != second.native_continuation_hash());
    CHECK(first.broker_state_hash() != second.broker_state_hash());
    std::printf("L8c broker hash projection: %d failures\n", failures);
    return failures == 0 ? 0 : 1;
}
