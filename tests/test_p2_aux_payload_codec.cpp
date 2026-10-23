// A-5: the field-stream placement payload must preserve the filled-exit fact.
#include <pineforge/payload_readback.hpp>
#include <pineforge/detail/scratch_backing.hpp>
#include <pineforge/source/exit_payload_readback.hpp>
#include <pineforge/source/pine_adapter.hpp>
#include "../src/source/exit_scratch_versions_production.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace {
namespace pf = pineforge;
namespace sd = pineforge::source::detail;
using pf::detail::CommandScratchStore;
using pf::detail::FixedMemoryScratchBacking;
using pf::detail::ScratchHandle;
using pf::detail::ScratchOpenMode;
using pf::detail::ScratchStatus;
using pf::detail::PayloadField;
using pf::source::PlacementReadback;
using pf::source::PlacementSnapshot;

struct StoreFixture {
    std::vector<std::byte> bytes = std::vector<std::byte>(64 * 1024 * 1024);
    FixedMemoryScratchBacking backing({bytes.data(), bytes.size()}, 512);
    CommandScratchStore store;

    explicit StoreFixture(pf::detail::ScratchKeyOrder order)
        : store({2, 512, 1, 1, order, ScratchOpenMode::create_empty}, &backing) {}
};
}

int main() {
    StoreFixture production(sd::exit_scratch_key_order());
    StoreFixture field(sd::exit_payload_key_order());
    if (production.backing.status() != ScratchStatus::ok
        || production.store.status() != ScratchStatus::ok
        || field.backing.status() != ScratchStatus::ok
        || field.store.status() != ScratchStatus::ok)
        return 1;

    PlacementSnapshot original;
    original.filled_exit = true;

    sd::ExitScratchVersionsProduction versions(production.store);
    const auto production_version = versions.placement(1, original);
    const auto production_roundtrip = versions.placement(production_version);
    if (!production_roundtrip.filled_exit)
        return 1;

    sd::ExitScratchContext context{field.store, {"p2-aux-payload", 1}};
    ScratchHandle field_version = 0;
    if (sd::freeze_placement(context, 1, original, field_version) != ScratchStatus::ok)
        return 1;
    PlacementReadback field_roundtrip(field.store, field_version);
    PayloadField filled;
    bool field_value = false;
    if (field_roundtrip.find("filled_exit", filled) != ScratchStatus::ok
        || filled.boolean_value(field_value) != ScratchStatus::ok
        || field_value != production_roundtrip.filled_exit)
        return 1;
    return 0;
}
