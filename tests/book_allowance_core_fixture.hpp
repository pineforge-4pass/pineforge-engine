#pragma once

#include <pineforge/native_order.hpp>
#include "native_order_full_fold.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace book_allowance_core {
namespace no = pineforge::native_order;
using Core = no::WorkingRequestCore;
constexpr std::size_t resident_rows = 4096;
constexpr std::uint64_t old_point = 50000;
constexpr std::uint64_t new_point = 90000;

inline void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Only the fixture can seed a row's alternatives before its real admission
// spill. Production reads, refreshes, writes and token installs are exercised.
template<class Tag, auto Member> struct Access {
    friend auto member(Tag) { return Member; }
};
struct Residents { friend auto member(Residents); };
template struct Access<Residents, &Core::live_>;
struct Epoch { friend auto member(Epoch); };
template struct Access<Epoch, &Core::epoch_>;
struct Generation { friend auto member(Generation); };
template struct Access<Generation, &Core::book_generation>;
struct ScanOffset { friend auto member(ScanOffset); };
template struct Access<ScanOffset, &Core::LiveView::const_iterator::spilled_at>;
using Refresh = bool (Core::*)(const no::RequestHandle&, const no::EvaluationContext&,
    const no::TargetObservation&, std::uint64_t*, bool, std::uint64_t, std::uint64_t);
struct RefreshAt { friend auto member(RefreshAt); };
template struct Access<RefreshAt, static_cast<Refresh>(&Core::refresh_allowance)>;

inline no::EvaluationContext context(std::uint64_t point = new_point) {
    no::EvaluationContext value;
    value.cursor.point.ordinal = point;
    value.cursor.point.effective_time_ms = 1000;
    value.cursor.point.provenance = pineforge::NativePriceProvenance::ObservedPrint;
    value.driver_class = no::DriverEligibilityClass::ObservedPrint;
    value.existing_matching_bit = true;
    return value;
}

inline std::uint64_t folded(const no::LiveRequest& row) {
    full_fold::Fold out;
    full_fold::fold(out, row);
    return out.h;
}

struct Fixture {
    Core core{{"working-book-allowance", 1}};
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    explicit Fixture(bool spilled = true) {
        if (spilled) {
            for (std::size_t i = 0; i < resident_rows - 1; ++i) submit();
        }
    }
    no::RequestHandle submit() {
        no::Request request{no::Transact{2}, "limit", ""};
        request.trigger = no::Limit{1};
        request.capacity = no::PointBudget{0.75};
        const auto answer = core.submit(request, 0, incarnation, ordinal);
        require(answer.handle.has_value(), "fixture admission succeeds");
        return *answer.handle;
    }
    no::LiveRequest& newest() { return (core.*member(Residents{})).back(); }
    std::uint64_t epoch() const { return core.*member(Epoch{}); }
    std::uint64_t generation() const { return (core.*member(Generation{}))(); }
    bool resident(const no::RequestHandle& handle) const {
        for (const auto& row : core.*member(Residents{})) {
            if (row.handle() == handle) return true;
        }
        return false;
    }
    std::uint64_t locator(const no::RequestHandle& handle) const {
        const auto view = core.live();
        for (auto it = view.begin(); it != view.end(); ++it) {
            if (it->handle() == handle) return (it.*member(ScanOffset{}))();
        }
        throw std::runtime_error("fixture locator names a live row");
    }
    bool refresh(const no::RequestHandle& handle, const no::EvaluationContext& eval,
                 const no::TargetObservation& observation, bool timeline = true,
                 std::uint64_t at = 0, std::uint64_t gen = 0) {
        return (core.*member(RefreshAt{}))(handle, eval, observation,
            timeline ? &ordinal : nullptr, false, at, gen);
    }
};

inline void no_churn(const Core::BookRefreshWork& work) {
    require(work.promotions == 0 && work.evictions == 0 && work.tombstones == 0
        && work.compactions == 0, "refresh never promotes, evicts, tombstones or compacts");
}
} // namespace book_allowance_core
