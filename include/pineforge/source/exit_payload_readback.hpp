#pragma once

#include <pineforge/native_host.hpp>
#include <stdexcept>

namespace pineforge::source {
struct PlacementSnapshot;
class PlacementReadback final : public pineforge::detail::PayloadReadback {
public:
    PlacementReadback() noexcept = default;
    PlacementReadback(const pineforge::detail::ScratchStore& store,
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
    PendingReadback(const pineforge::detail::ScratchStore& store,
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
using pineforge::detail::ScratchByteSource;
using pineforge::detail::ScratchHandle;
using pineforge::detail::ScratchStatus;
using pineforge::detail::ScratchStore;
struct ExitScratchContext;
class ExitPayloadError final : public std::runtime_error {
public:
    explicit ExitPayloadError(ScratchStatus status)
        : std::runtime_error("exit payload operation failed"), status_(status) {}
    ScratchStatus status() const noexcept { return status_; }
private:
    ScratchStatus status_;
};

// Concrete provider; all version directories and payload bytes are records in
// the caller's store. It owns no token map, snapshots, strings or vectors.
class ExitScratchVersions {
public:
    explicit ExitScratchVersions(ExitScratchContext& context) noexcept : context_(context) {}
    ScratchHandle placement(std::uint64_t scope, const PlacementSnapshot&);
    PlacementReadback placement(ScratchHandle) const noexcept;
    ScratchHandle request(std::uint64_t scope, const native_order::Request&);
    native_order::RequestReadback request(ScratchHandle) const noexcept;
    ScratchHandle pending(std::uint64_t scope, const native_order::Request&,
                          const PlacementSnapshot&, const ScratchByteSource& replacement_key);
    ScratchHandle pending(std::uint64_t scope, const native_order::Request& request,
                          const PlacementSnapshot& placement, const std::string& key) {
        pineforge::detail::ScratchSpanSource source({
            reinterpret_cast<const std::byte*>(key.data()), key.size()});
        return pending(scope, request, placement, source);
    }
    PendingReadback pending(ScratchHandle) const noexcept;
    using OpeningReader = native_order::OpeningInputCursor::Reader;
    native_order::Owner freeze_openings(const native_order::RunIdentity&, std::int64_t cycle,
                                       std::uint64_t count, void*, OpeningReader);
    ScratchStatus release_scope(std::uint64_t scope) noexcept;
private:
    ExitScratchContext& context_;
};

// Same key order and scope prefix as x5. next_scope is monotonic storage
// metadata, shared with the scope owner; payload kinds have their own namespace.
struct ExitScratchContext {
    ScratchStore& store;
    native_order::RunIdentity run;
    ExitScratchVersions* versions = nullptr;
    std::uint64_t next_scope = 0;
    void* observation = nullptr;
    void (*observe_rows)(void*, std::uint64_t, std::size_t) noexcept = nullptr;
    void (*observe_lookup)(void*, std::size_t) noexcept = nullptr;
    ScratchStatus (*working)(const NativeStrategyHost&, void*,
                            bool (*)(void*, const NativeWorkingRequest&)) =
        [](const NativeStrategyHost& host, void* context,
           bool (*visit)(void*, const NativeWorkingRequest&)) {
            return host.visit_native_working_requests(context, visit);
        };
    ScratchStatus (*lot_views)(const NativeStrategyHost&, double, void*,
                              bool (*)(void*, const NativeOpenLotView&)) =
        [](const NativeStrategyHost& host, double mark, void* context,
           bool (*visit)(void*, const NativeOpenLotView&)) {
            return host.visit_native_open_lots(mark, context, visit);
        };
    ScratchStatus (*lot_values)(const NativeStrategyHost&, void*,
                               bool (*)(void*, std::uint64_t, double, double)) =
        [](const NativeStrategyHost& host, void* context,
           bool (*visit)(void*, std::uint64_t, double, double)) {
            return host.visit_native_open_lot_values(context, visit);
        };
};

pineforge::detail::ScratchKeyOrder exit_payload_key_order() noexcept;
ScratchStatus freeze_placement(ExitScratchContext&, std::uint64_t scope,
                              const PlacementSnapshot&, ScratchHandle& version);
ScratchStatus freeze_request(ExitScratchContext&, std::uint64_t scope,
                            const native_order::Request&, ScratchHandle& version);
ScratchStatus freeze_pending(ExitScratchContext&, std::uint64_t scope,
                            const native_order::Request&, const PlacementSnapshot&,
                            const ScratchByteSource& replacement_key, ScratchHandle& version);
} // namespace detail
} // namespace pineforge::source
