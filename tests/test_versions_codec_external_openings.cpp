// S4E A-6: red on the base because payload_fields(BindOpenings) omits
// external_openings. Compile/run, including the mutation control, on the lab box.
#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
namespace no = pineforge::native_order;
namespace pd = pineforge::detail;
namespace sd = pineforge::source::detail;
using pineforge::source::PlacementSnapshot;
using Status = pd::ScratchStatus;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Fixed backing is independent of the two resident cache slots. The largest
// roster below spans both 512-byte payload input windows and backing pages.
std::array<std::byte, 8 * 1024 * 1024> storage{};

no::RunIdentity opening_run() {
    return {std::string("codec\0openings", 14), UINT64_C(0xfedcba9876543210)};
}

std::uint64_t incarnation(std::uint64_t index) {
    return (UINT64_C(1) << 40) + 3 * index + 1;
}

class Openings final : public no::OpeningReadback {
public:
    explicit Openings(std::uint64_t count) : run_(opening_run()), count_(count) {}
    bool poisoned = false;
    std::uint64_t count() const noexcept override { return count_; }
    Status opening_at(std::uint64_t index, no::RequestHandle& out) const noexcept override {
        if (poisoned) return Status::io_error;
        if (index >= count_) return Status::not_found;
        out = {run_, incarnation(index)};
        return Status::ok;
    }
private:
    no::RunIdentity run_;
    std::uint64_t count_;
};

no::Request request_value(std::shared_ptr<const no::OpeningReadback> external = {}) {
    no::Request request{no::Reduce{no::ExplicitUnits{3}}, "codec exit", "frozen"};
    request.trigger = no::Limit{104.25};
    request.capacity = no::PointBudget{9.5};
    // Keep an inline member too: external membership takes precedence, but
    // archiving the additional field must not overwrite the inline vector.
    request.owner = no::BindOpenings{{{opening_run(), 7}}, 37, std::move(external)};
    request.group = no::Member{19, 23, no::GroupEffect::Reduce};
    return request;
}

void check_request(const no::Request& request, std::uint64_t count) {
    const auto* reduce = std::get_if<no::Reduce>(&request.intent);
    require(reduce && std::holds_alternative<no::ExplicitUnits>(reduce->size),
            "retain the reduction kind");
    require(std::get<no::ExplicitUnits>(reduce->size).units == 3, "retain reduction units");
    require(request.label == "codec exit" && request.comment == "frozen", "retain request text");
    const auto* limit = std::get_if<no::Limit>(&request.trigger);
    require(limit && limit->price == 104.25 && !limit->fill_through, "retain the limit");
    const auto* capacity = std::get_if<no::PointBudget>(&request.capacity);
    require(capacity && capacity->units == 9.5, "retain capacity");
    const auto* group = std::get_if<no::Member>(&request.group);
    require(group && group->group == 19 && group->cohort == 23
                && group->effect == no::GroupEffect::Reduce, "retain fields after the owner");
    require(std::holds_alternative<no::Absolute>(request.anchor), "retain the anchor");
    const auto* owner = std::get_if<no::BindOpenings>(&request.owner);
    require(owner && owner->cycle == 37, "retain BindOpenings and its cycle");
    require(owner->openings == std::vector<no::RequestHandle>{{opening_run(), 7}},
            "retain the inline vector independently of the external roster");
    require(owner->external_openings != nullptr, "A-6: retain the external opening readback");
    require(owner->external_openings->count() == count, "retain the complete external count");
    require(no::opening_count(owner->openings, owner->external_openings.get()) == count,
            "external membership must retain precedence over the inline vector");
    for (std::uint64_t i = 0; i < count; ++i) {
        no::RequestHandle got;
        require(no::opening_at(owner->openings, owner->external_openings.get(), i, got)
                    == Status::ok, "read every external handle");
        require(got == no::RequestHandle{opening_run(), incarnation(i)},
                "retain every incarnation and full run identity, including embedded NUL");
    }
    no::RequestHandle unchanged{opening_run(), 999};
    require(owner->external_openings->opening_at(count, unchanged) == Status::not_found,
            "external readback must refuse an out-of-range index");
    require(unchanged == no::RequestHandle{opening_run(), 999},
            "a refused read must leave its output unchanged");
}

std::uint64_t schema_word(const pd::CommandScratchStore& store, pd::ScratchHandle version) {
    std::array<std::byte, 8> bytes{};
    require(store.read(version, pd::ScratchField::value, 0, {bytes.data(), bytes.size()})
                == Status::ok, "read the payload schema");
    std::uint64_t word = 0;
    for (unsigned i = 0; i != bytes.size(); ++i)
        word |= std::uint64_t(std::to_integer<unsigned char>(bytes[i])) << (8 * i);
    return word;
}

void round_trip(std::uint64_t count) {
    no::Request restored, pending;
    {
        pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, 512);
        pd::CommandScratchStore store(
            {2, 512, 1, 1, sd::exit_scratch_key_order(), pd::ScratchOpenMode::create_empty},
            &backing);
        require(store.status() == Status::ok, "open the external roster fixture");
        sd::ExitScratchVersionsProduction versions(store);
        auto source = std::make_shared<Openings>(count);
        auto request = request_value(source);
        PlacementSnapshot placement;
        placement.source_id = "snapshot";
        placement.requested_qty = 4.25;
        placement.command_sequence = 61;
        const std::string key("pending\0key", 11);
        const auto request_version = versions.request(7, request);
        const auto pending_version = versions.pending(7, request, placement, key);
        require(request_version != 0 && pending_version != 0, "publish both payload versions");
        request = {};
        placement = {};
        source->poisoned = true;

        restored = versions.request(request_version);
        check_request(restored, count); // First defect assertion on the unfixed head.
        PlacementSnapshot saved;
        std::string saved_key;
        versions.pending(pending_version, pending, saved, saved_key);
        check_request(pending, count);
        require(saved.source_id == "snapshot" && saved.requested_qty == 4.25
                    && saved.command_sequence == 61, "retain the pending placement");
        require(saved_key == key, "retain the pending key after the extended owner");
        require(std::get<no::BindOpenings>(restored.owner).external_openings.get() != source.get()
                    && std::get<no::BindOpenings>(pending.owner).external_openings.get()
                        != source.get(),
                "decode frozen membership rather than retaining the caller's readback");
        require(schema_word(store, request_version) == 2
                    && schema_word(store, pending_version) == 2,
                "the added field must have an explicit schema version");
        source.reset();
        versions.release_scope(7);
        versions.check_teardown_failure();
        require(store.record_count() == 0, "release all payload and counter rows");
        check_request(restored, count);
        check_request(pending, count);
    }
    check_request(restored, count);
    check_request(pending, count);
}

// Independent schema-1 bytes, derived from the documented LE64 archive and
// native variant declarations, never from the production serializer's output.
struct LegacyBytes {
    std::vector<std::byte> bytes;
    void word(std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i)
            bytes.push_back(static_cast<std::byte>((value >> (8 * i)) & 0xff));
    }
    void real(double value) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        word(bits);
    }
    void text(const std::string& value) {
        word(value.size());
        for (const auto byte : value) bytes.push_back(static_cast<std::byte>(byte));
    }
    LegacyBytes() {
        word(1);                     // schema
        word(1); word(0); real(3);    // Reduce / ExplicitUnits
        text("codec exit"); text("frozen");
        word(1); real(104.25); word(0); // Limit / fill_through
        word(1); real(9.5);          // PointBudget
        word(3); word(1);            // BindOpenings / inline count
        text(std::string("codec\0openings", 14));
        word(UINT64_C(0xfedcba9876543210)); word(7);
        word(37);                    // cycle; schema 1 has no external field
        word(1); word(19); word(23); word(1); // Member / Reduce
        word(0);                     // Absolute
    }
};

pd::ScratchHandle put_raw(pd::CommandScratchStore& store, std::uint64_t scope,
                         const std::vector<std::byte>& bytes) {
    const sd::ExitScratchKey key(scope, 4, 1); // request_kind, sequence 1
    const pd::ScratchSpanSource payload({bytes.data(), bytes.size()});
    pd::ScratchHandle handle = 0;
    require(store.begin() == Status::ok, "begin a raw legacy row");
    require(store.put(key, payload, handle) == Status::ok, "write a raw legacy row");
    require(store.commit(pd::ScratchDurability::buffered) == Status::ok, "commit a raw legacy row");
    return handle;
}

void legacy_layout() {
    pd::FixedMemoryScratchBacking backing({storage.data(), storage.size()}, 512);
    pd::CommandScratchStore store(
        {2, 512, 1, 1, sd::exit_scratch_key_order(), pd::ScratchOpenMode::create_empty},
        &backing);
    require(store.status() == Status::ok, "open the legacy layout fixture");
    sd::ExitScratchVersionsProduction versions(store);
    LegacyBytes expected;
    const auto legacy = versions.request(put_raw(store, 9, expected.bytes));
    require(legacy.label == "codec exit" && legacy.comment == "frozen",
            "decode independently constructed schema-1 bytes");
    const auto& owner = std::get<no::BindOpenings>(legacy.owner);
    require(owner.cycle == 37 && !owner.external_openings
                && owner.openings == std::vector<no::RequestHandle>{{opening_run(), 7}},
            "schema 1 must retain inline ownership and default the absent field");
    require(std::get<no::Member>(legacy.group).group == 19,
            "schema 1 must not consume group bytes as an external field");

    const auto written = versions.request(10, request_value());
    pd::ScratchRecordInfo info{};
    require(store.point(written, info) == Status::ok, "inspect the legacy writer");
    require(info.value_bytes == expected.bytes.size(), "keep the entire schema-1 byte length");
    std::vector<std::byte> actual(expected.bytes.size());
    require(store.read(written, pd::ScratchField::value, 0, {actual.data(), actual.size()})
                == Status::ok, "read all legacy bytes");
    require(actual == expected.bytes, "unchanged inline requests must keep every existing byte");

    PlacementSnapshot placement;
    const auto placement_version = versions.placement(10, placement);
    const auto pending_version = versions.pending(10, request_value(), placement, "legacy");
    require(schema_word(store, placement_version) == 1 && schema_word(store, pending_version) == 1,
            "placement and pending rows without external membership must keep schema 1");
    expected.bytes[0] = std::byte{99};
    const auto unknown = put_raw(store, 11, expected.bytes);
    bool refused = false;
    try {
        (void)versions.request(unknown);
    } catch (const sd::ExitScratchError& error) {
        refused = error.status() == Status::corrupt;
    }
    require(refused, "an unknown payload schema must still be corrupt");
}
} // namespace

int main() {
    try {
        for (const auto count : {UINT64_C(1), UINT64_C(17), UINT64_C(129), UINT64_C(513),
                                 UINT64_C(0)})
            round_trip(count);
        legacy_layout();
        std::puts("PASS test_versions_codec_external_openings");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL test_versions_codec_external_openings: %s\n", error.what());
        return 1;
    }
}
