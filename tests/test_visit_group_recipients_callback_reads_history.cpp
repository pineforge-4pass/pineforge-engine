// B1-08: the public visitor contract permits reads of the core. Run this row
// in the sanitizers profile: a callback evicts the cause's cached page before
// the walk reaches its next recipient.
#include "p2_kernel_order_fixture.hpp"

#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
using p2_kernel_order_test::Fixture;
using p2_kernel_order_test::require;

no::Request grouped(std::uint64_t group, std::uint64_t cohort) {
    no::Request request{no::Transact{1.0}, "member", ""};
    request.group = no::Member{group, cohort, no::GroupEffect::Reduce};
    return request;
}

void callback_reads_history() {
    Fixture fixture("p2-b1-08-reentrant-reader");
    const auto source = fixture.submit(grouped(17, 1));
    std::vector<no::RequestHandle> expected;
    for (std::uint64_t cohort = 2; cohort != 5; ++cohort)
        expected.push_back(fixture.submit(grouped(17, cohort)));
    fixture.submit(grouped(17, 1)); // Same cohort: excluded.
    fixture.submit(grouped(18, 2)); // Other group: excluded.
    const auto cause = fixture.fill(source, 1.0);
    fixture.submit(grouped(17, 9)); // Accepted after the fill: excluded.
    require(fixture.core.history_end() < 32, "cause did not fit in the first journal chunk");

    // Pages hold at most kChunk=32 events; 2,048+8*32 later events force
    // more than five old pages out of the resident journal. Reads below are
    // separated by a whole chunk, so each necessarily loads a distinct page.
    constexpr std::size_t kResidentEvents = 2048;
    constexpr std::size_t kChunk = 32;
    for (std::size_t i = 0; i != kResidentEvents + 8 * kChunk; ++i) fixture.reject_small();
    require(fixture.core.history_end() - 6 * kChunk > kResidentEvents,
            "history positions used by the callback are still resident");
    require(fixture.core.group_recipients(cause) == expected,
            "non-reentrant recipient list differs from the fixture contract");

    struct Visitor {
        const no::WorkingRequestCore& core;
        std::vector<no::RequestHandle> received;
        std::size_t reads = 0;
        static void visit(void* context, const no::RequestHandle& handle) {
            auto& self = *static_cast<Visitor*>(context);
            self.received.push_back(handle);
            // Cause is in page/chunk zero, never one of these five. The
            // journal has four cache slots, so these reads evict its page.
            for (const std::size_t at : std::array<std::size_t, 5>{64, 96, 128, 160, 192}) {
                require(std::holds_alternative<no::RejectedEvent>(self.core.history_at(at)),
                        "callback did not read the expected old journal page");
                ++self.reads;
            }
        }
    } visitor{fixture.core, {}, 0};
    const auto before = fixture.core.last_ordinal();
    fixture.core.visit_group_recipients(cause, &visitor, Visitor::visit);
    require(visitor.received == expected, "callback history reads changed recipient order/content");
    require(visitor.reads == 5 * expected.size(), "callback did not evict pages between recipients");
    require(fixture.core.last_ordinal() == before, "read visitor changed the core");
}
} // namespace

int main() {
    try {
        callback_reads_history();
        std::puts("test_visit_group_recipients_callback_reads_history: PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_visit_group_recipients_callback_reads_history: FAIL: %s\n",
                     error.what());
        return 1;
    }
}
