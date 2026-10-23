#pragma once

#include <pineforge/source/exit_scratch.hpp>

namespace pineforge::source {
struct PlacementSnapshot;
class PlacementReadback final : public pineforge::detail::PayloadReadback {
public:
    PlacementReadback() noexcept = default;
    PlacementReadback(const pineforge::detail::CommandScratchStore& store,
                     pineforge::detail::ScratchHandle version) noexcept
        : PayloadReadback(store, version, pineforge::detail::PayloadSchema::placement) {}
    explicit PlacementReadback(pineforge::detail::PayloadReadback view) noexcept : PayloadReadback(view) {}
    pineforge::detail::ScratchStatus source_id(pineforge::detail::PayloadField& out) const noexcept {
        return find("source_id", out);
    }
    pineforge::detail::ScratchStatus from_entry(pineforge::detail::PayloadField& out) const noexcept {
        return find("from_entry", out);
    }
    pineforge::detail::ScratchStatus comment(pineforge::detail::PayloadField& out) const noexcept {
        return find("comment", out);
    }
    pineforge::detail::ScratchStatus oca_name(pineforge::detail::PayloadField& out) const noexcept {
        return find("oca_name", out);
    }
    pineforge::detail::ScratchStatus close_first_id(pineforge::detail::PayloadField& out) const noexcept {
        return find("close_first_id", out);
    }
};
class PendingReadback final : public pineforge::detail::PayloadReadback {
public:
    PendingReadback() noexcept = default;
    PendingReadback(const pineforge::detail::CommandScratchStore& store,
                   pineforge::detail::ScratchHandle version) noexcept
        : PayloadReadback(store, version, pineforge::detail::PayloadSchema::pending) {}
    pineforge::detail::ScratchStatus replacement_key(
        pineforge::detail::PayloadChunkBytes& out) const noexcept;
    pineforge::detail::ScratchStatus request(native_order::RequestReadback& out) const noexcept {
        pineforge::detail::PayloadReadback view;
        const auto status = subobject("request", view);
        if (status == pineforge::detail::ScratchStatus::ok) out = native_order::RequestReadback(view);
        return status;
    }
    pineforge::detail::ScratchStatus placement(PlacementReadback& out) const noexcept {
        pineforge::detail::PayloadReadback view;
        const auto status = subobject("placement", view);
        if (status == pineforge::detail::ScratchStatus::ok) out = PlacementReadback(view);
        return status;
    }
};

namespace detail {
// Borrow the join's context and keep the field-stream readbacks separate from
// its owning-value ExitScratchVersions contract and production provider.
pineforge::detail::ScratchKeyOrder exit_payload_key_order() noexcept;
ScratchStatus freeze_placement(ExitScratchContext&, std::uint64_t scope,
                              const PlacementSnapshot&, ScratchHandle& version);
ScratchStatus freeze_request(ExitScratchContext&, std::uint64_t scope,
                            const native_order::Request&, ScratchHandle& version);
ScratchStatus freeze_pending(ExitScratchContext&, std::uint64_t scope,
                            const native_order::Request&, const PlacementSnapshot&,
                            const ScratchByteSource& replacement_key, ScratchHandle& version);
ScratchStatus release_payload_scope(ExitScratchContext&, std::uint64_t scope) noexcept;
} // namespace detail
} // namespace pineforge::source
