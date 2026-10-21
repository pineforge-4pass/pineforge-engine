// LB-446: not built/run here. Frozen-base first diagnostic must be the absent
// exit_payload_readback.hpp; with headers transplanted, the provider must link.
#include <pineforge/source/exit_payload_readback.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include "global_allocation_replacement.hpp"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace {
namespace pf = pineforge;
namespace no = pineforge::native_order;
namespace sd = pineforge::source::detail;
using namespace pineforge::detail;
using pineforge::source::PlacementSnapshot;
unsigned checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::printf("FAIL %d: %s\n", __LINE__, #x); } } while (0)
#define REQUIRE(x) do { const bool ok = bool(x); CHECK(ok); if (!ok) throw std::runtime_error(#x); } while (0)

struct NoAlloc {
    std::size_t before = global_allocation::allocations;
    ~NoAlloc() { CHECK(before == global_allocation::allocations); }
};
alignas(64) std::array<std::byte, 64 * 1024 * 1024> storage;

bool equal_text(const ScratchByteSource& actual, std::string_view expected) {
    if (actual.size() != expected.size()) return false;
    std::array<std::byte, 73> buffer{};
    for (std::size_t offset = 0; offset < expected.size();) {
        const auto count = std::min(buffer.size(), expected.size() - offset);
        if (actual.read(offset, {buffer.data(), count}) != ScratchStatus::ok
            || std::memcmp(buffer.data(), expected.data() + offset, count)) return false;
        offset += count;
    }
    return actual.read(actual.size(), {nullptr, 0}) == ScratchStatus::ok;
}
bool field_name(const PayloadField& field, std::string_view name) {
    return equal_text(field.name, name);
}

void request_placement_pending() {
    for (auto n : {17U, 129U, 513U}) {
        FixedMemoryScratchBacking backing({storage.data(), storage.size()}, 512);
        ScratchStore store({2, 512, 1, 1, sd::exit_payload_key_order(), ScratchOpenMode::create_empty}, &backing);
        REQUIRE(store.status() == ScratchStatus::ok);
        sd::ExitScratchContext context{store, {"o1s2-payload", 1}};
        sd::ExitScratchVersions versions(context);
        context.versions = &versions;
        std::string text(n * 129, '\x82');
        text[128] = '\0';
        PlacementSnapshot placement;
        placement.source_id = placement.from_entry = placement.comment = text;
        placement.oca_name = placement.close_first_id = text;
        placement.command_sequence = 991;
        placement.requested_qty = -0.0;
        placement.sizing.frozen_units = 23.25;
        placement.birth = pf::OrderBirth::fill_evaluation(4, 60000,
            pf::BirthCursor::point(pf::BirthCursorDomain::HistoricalPath, 2, 4),
            99.75, 7, 9, 3);
        placement.legs.attach(77, 4);
        placement.legs.set_prices({110, 90, 2, 101, 3, 8, 9});
        placement.reservation_growth_source.assign_capture(77, 4);
        auto admission = std::make_shared<pf::admission::CommandObservation>();
        admission->id = admission->oca_name = text;
        admission->command = 912;
        placement.market_admission.bind(admission);
        placement.market_admission.reviewed({93, pf::admission::Checkpoint::ExplicitPair, 4, 912});

        no::Request request{no::Reduce{no::ScopeFraction{0.5, no::ScopeClaim::NetOfSiblings,
                                                      no::ScopeBasis::AtAcceptance}}, text, text};
        request.trigger = no::Trail{2, 101, no::TrailTicks{3}, 111};
        request.capacity = no::PointBudget{5};
        no::BindOpenings owner{{}, 4};
        for (unsigned i = 0; i != n; ++i) owner.openings.push_back({context.run, i + 1});
        request.owner = std::move(owner);
        request.group = no::Member{12, 8, no::GroupEffect::Reduce};
        request.anchor = no::FromOwnerFill{-3, true, no::NativeAnchorRounding::Directional};
        ScratchHandle request_version = 0, placement_version = 0, pending_version = 0;
        {
            NoAlloc guard;
            CHECK(sd::freeze_request(context, 1, request, request_version) == ScratchStatus::ok);
            CHECK(sd::freeze_placement(context, 1, placement, placement_version) == ScratchStatus::ok);
        }
        REQUIRE(request_version && placement_version);
        const auto request_view = versions.request(request_version);
        const auto placement_view = versions.placement(placement_version);
        PayloadField label;
        REQUIRE(request_view.label(label) == ScratchStatus::ok);
        // The pending key deliberately reads the SAME store. The production
        // codec must stage fixed chunks before put, never from its source callback.
        {
            NoAlloc guard;
            CHECK(sd::freeze_pending(context, 2, request, placement, label.bytes, pending_version)
                == ScratchStatus::ok);
        }
        REQUIRE(pending_version);
        const auto pending = versions.pending(pending_version);
        request = {};
        placement = {};
        admission.reset();
        {
            NoAlloc guard;
            PayloadField value;
            CHECK(request_view.label(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(request_view.comment(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            PayloadReadback owner_variant, owner_value;
            CHECK(request_view.subobject("owner", owner_variant) == ScratchStatus::ok);
            CHECK(owner_variant.subobject("value", owner_value) == ScratchStatus::ok);
            CHECK(owner_value.find("count", value) == ScratchStatus::ok);
            std::uint64_t owners = 0;
            CHECK(value.unsigned_value(owners) == ScratchStatus::ok && owners == n);
            struct Owners {
                std::uint64_t next = 1;
                static bool visit(void* context, const PayloadField& field) {
                    auto& call = *static_cast<Owners*>(context);
                    if (field_name(field, "incarnation")) {
                        std::uint64_t incarnation = 0;
                        CHECK(field.unsigned_value(incarnation) == ScratchStatus::ok);
                        CHECK(incarnation == call.next++);
                    }
                    return true;
                }
            } owner_rows;
            CHECK(owner_value.visit(&owner_rows, Owners::visit) == ScratchStatus::ok);
            CHECK(owner_rows.next == n + 1);
            PayloadReadback intent, reduce, size, fraction;
            CHECK(request_view.subobject("intent", intent) == ScratchStatus::ok);
            CHECK(intent.subobject("value", reduce) == ScratchStatus::ok);
            CHECK(reduce.subobject("size", size) == ScratchStatus::ok);
            CHECK(size.subobject("value", fraction) == ScratchStatus::ok);
            CHECK(fraction.find("fraction", value) == ScratchStatus::ok);
            double amount = 0;
            CHECK(value.real_value(amount) == ScratchStatus::ok && amount == 0.5);
            for (auto name : {"source_id", "from_entry", "comment", "oca_name", "close_first_id"}) {
                CHECK(placement_view.find(name, value) == ScratchStatus::ok);
                CHECK(equal_text(value.bytes, text));
            }
            CHECK(placement_view.find("command_sequence", value) == ScratchStatus::ok);
            std::uint64_t sequence = 0;
            CHECK(value.unsigned_value(sequence) == ScratchStatus::ok && sequence == 991);
            CHECK(placement_view.find("requested_qty", value) == ScratchStatus::ok);
            double quantity = 1;
            CHECK(value.real_value(quantity) == ScratchStatus::ok);
            CHECK(quantity == 0 && std::signbit(quantity));
            CHECK(placement_view.find("qty_type", value) == ScratchStatus::ok);
            std::int64_t signed_value = 88;
            CHECK(value.signed_value(signed_value) == ScratchStatus::ok && signed_value == -1);
            CHECK(placement_view.find("is_long", value) == ScratchStatus::ok);
            bool direction = false;
            CHECK(value.boolean_value(direction) == ScratchStatus::ok && direction);
            CHECK(value.signed_value(signed_value) == ScratchStatus::corrupt && signed_value == -1);
            PayloadChunkBytes key;
            CHECK(pending.replacement_key(key) == ScratchStatus::ok);
            CHECK(equal_text(key, text));
            no::RequestReadback pending_request;
            pf::source::PlacementReadback pending_placement;
            CHECK(pending.request(pending_request) == ScratchStatus::ok);
            CHECK(pending.placement(pending_placement) == ScratchStatus::ok);
            CHECK(pending_request.label(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(pending_placement.source_id(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(pending_placement.from_entry(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(pending_placement.comment(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(pending_placement.oca_name(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            CHECK(pending_placement.close_first_id(value) == ScratchStatus::ok);
            CHECK(equal_text(value.bytes, text));
            std::array<std::byte, 257> cross{};
            CHECK(key.read(63, {cross.data(), cross.size()}) == ScratchStatus::ok);
            CHECK(std::memcmp(cross.data(), text.data() + 63, cross.size()) == 0);
            CHECK(key.read(key.size() + 1, {nullptr, 0}) == ScratchStatus::programming_error);
            std::uint64_t field_count = 0;
            CHECK(placement_view.field_count(field_count) == ScratchStatus::ok && field_count > 128);
            struct Nested {
                const no::RequestReadback& request;
                const std::string& text;
                std::size_t fields = 0, ids = 0;
                static bool visit(void* context, const PayloadField& field) {
                    auto& call = *static_cast<Nested*>(context);
                    ++call.fields;
                    if (field.type == PayloadType::text && field_name(field, "id")) {
                        ++call.ids;
                        CHECK(equal_text(field.bytes, call.text));
                    }
                    if (call.fields % 257 == 1) {
                        PayloadField nested;
                        CHECK(call.request.label(nested) == ScratchStatus::ok);
                        CHECK(equal_text(nested.bytes, call.text));
                    }
                    return true;
                }
            } nested{request_view, text};
            CHECK(pending.visit(&nested, Nested::visit) == ScratchStatus::ok);
            CHECK(nested.fields > field_count && nested.ids == 1);
            std::size_t count = 0;
            CHECK(request_view.visit(&count, [](void* context, const PayloadField&) {
                ++*static_cast<std::size_t*>(context); return false;
            }) == ScratchStatus::cancelled);
            CHECK(count == 1);
        }
        // Failure after many writes must restore every previous record and
        // keep the caller's output unchanged.
        struct Broken final : ScratchByteSource {
            std::uint64_t size() const noexcept override { return 256; }
            ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
                if (offset >= 128) return ScratchStatus::io_error;
                std::memset(out.data, 'q', out.size);
                return ScratchStatus::ok;
            }
        } broken;
        std::array<std::byte, 512> page{};
        std::uint64_t before = 0, after = 0;
        REQUIRE(store.ordered_digest({page.data(), page.size()}, before) == ScratchStatus::ok);
        ScratchHandle unchanged = 777;
        {
            NoAlloc guard;
            CHECK(sd::freeze_pending(context, 3, request, placement, broken, unchanged) == ScratchStatus::io_error);
            CHECK(unchanged == 777 && !store.transaction_active());
            CHECK(store.ordered_digest({page.data(), page.size()}, after) == ScratchStatus::ok);
            CHECK(before == after);
            CHECK(versions.release_scope(1) == ScratchStatus::ok);
            PayloadChunkBytes key;
            CHECK(pending.replacement_key(key) == ScratchStatus::ok);
            CHECK(equal_text(key, text));
            CHECK(request_view.label(label) == ScratchStatus::not_found);
            CHECK(versions.release_scope(2) == ScratchStatus::ok);
            CHECK(store.record_count() == 0);
        }
    }
}
} // namespace

int main() {
    try { request_placement_pending(); }
    catch (const std::exception& error) { ++failures; std::printf("FAIL: %s\n", error.what()); }
    std::printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
