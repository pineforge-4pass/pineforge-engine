#pragma once

#include <pineforge/source/exit_scratch.hpp>

namespace pineforge::source::detail {

// Borrows the run's exit store. Payloads, version allocation and scope
// ownership live in that store; this object owns no retained payload memory.
class ExitScratchVersionsProduction final : public ExitScratchVersions {
public:
    explicit ExitScratchVersionsProduction(CommandScratchStore& store) noexcept : store_(store) {}

    std::uint64_t placement(std::uint64_t scope, const PlacementSnapshot&) override;
    PlacementSnapshot placement(std::uint64_t version) const override;
    std::uint64_t request(std::uint64_t scope, const native_order::Request&) override;
    native_order::Request request(std::uint64_t version) const override;
    std::uint64_t pending(std::uint64_t scope, const native_order::Request&,
                          const PlacementSnapshot&, const std::string& key) override;
    void pending(std::uint64_t version, native_order::Request&,
                 PlacementSnapshot&, std::string& key) const override;
    native_order::Owner freeze_openings(const native_order::RunIdentity&,
        std::int64_t cycle, std::uint64_t count, void*, OpeningReader) override;
    void release_scope(std::uint64_t scope) noexcept override;

private:
    CommandScratchStore& store_;
};

} // namespace pineforge::source::detail
