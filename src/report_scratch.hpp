#pragma once

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/metrics_stream.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pineforge {
namespace report_sink {

// Internal storage helpers for DESIGN 3.8. The installed point-source API
// remains metrics::ScratchCurveRegion / metrics::SpillCurve in metrics_stream.hpp.
// L7c owns SpillCurve and can delegate its single reader binding to
// read_scratch_points or ScratchCurveWindow below.
inline constexpr std::size_t kScratchWindowBytes = 64U * 1024U;
inline constexpr std::size_t kScratchWindowPoints =
    kScratchWindowBytes / sizeof(pf_equity_point_t);

// One positional store through the frozen detail::ScratchFile facade. Startup
// explicitly attaches an arena; otherwise the facade uses a private unlinked
// file in the supplied directory (empty selects its usual TMPDIR default).
// L18 owns a separate position_tail store for variable-length records.
//
// I/O errors remain std::system_error (including their errno) and carry
// RunFailureCode::report_sink_failed through the existing failure classifier.
// Invalid arguments/offsets are engine invariants. No shared file cursor is
// used: callers may have independent readers over immutable regions.
class ScratchFile {
public:
    explicit ScratchFile(const std::string& directory,
                         detail::ScratchStore store = detail::ScratchStore::unknown);
    ~ScratchFile();
    ScratchFile(const ScratchFile&) = delete;
    ScratchFile& operator=(const ScratchFile&) = delete;

    void write(std::int64_t byte_offset, const void* bytes, std::size_t length);
    void read(std::int64_t byte_offset, void* bytes, std::size_t length) const;
    void ensure_size(std::int64_t byte_count);
    void clear() noexcept;
    metrics::ScratchCurveRegion curve_region(std::int64_t byte_offset,
                                             std::int64_t point_count) const;

private:
    detail::ScratchFile file_;
};

// A binary curve with at most one 64 KiB RAM batch (2730 complete points).
// Append accepts a point only after a preceding full batch has been written.
// A failed/partial flush keeps that entire batch authoritative in RAM; retry
// overwrites the same file offset. Destruction closes without trying to flush.
// A run boundary must explicitly flush/region() before reporting success.
class ScratchCurveWriter {
public:
    explicit ScratchCurveWriter(const std::string& directory);
    ~ScratchCurveWriter();
    ScratchCurveWriter(const ScratchCurveWriter&) = delete;
    ScratchCurveWriter& operator=(const ScratchCurveWriter&) = delete;

    void append(const pf_equity_point_t& point);
    void replace_last(const pf_equity_point_t& point);
    // Replace a suffix of at most one scratch batch without changing size.
    // Reuses the authoritative pending batch, including after a failed flush.
    // Readers must be rebound after replacement, as with replace_last().
    void replace_tail(const pf_equity_point_t* points, std::size_t count);
    void clear();
    void flush() const;
    std::int64_t size() const noexcept;
    // Flush first, then borrow the facade/count. The owner must outlive
    // all readers. Appending preserves an old region; replace_last/clear need
    // fresh readers so a cached window cannot return an earlier revision.
    metrics::ScratchCurveRegion region() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Exact binary positional read, with no cache or allocation. The same adapter
// serves facade regions and the older explicitly borrowed-descriptor form.
// File reads retry positive short reads and EINTR. Arena reads retain the
// frozen facade's typed short-backing refusal and size recheck. Premature EOF
// is EIO, never a manufactured zero/NaN point.
void read_scratch_points(const metrics::ScratchCurveRegion& region,
                         std::int64_t first, pf_equity_point_t* points,
                         std::int64_t count);

// Internal implementation of the bounded reader window; it has the P00
// point-source shape and returns values independent of later refills. Its
// region may start at any byte offset, including one not aligned to a point.
// Independent instances can read concurrently; sharing one cache needs
// caller synchronization. No writer may mutate the borrowed region.
class ScratchCurveWindow {
public:
    explicit ScratchCurveWindow(const metrics::ScratchCurveRegion& region);
    ~ScratchCurveWindow();
    ScratchCurveWindow(const ScratchCurveWindow&) = delete;
    ScratchCurveWindow& operator=(const ScratchCurveWindow&) = delete;

    std::int64_t size() const noexcept;
    pf_equity_point_t operator[](std::int64_t index) const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

// Debug reference for L6b-2: replay the committed scratch prefix and optionally
// its still-editable held point, with the engine's initial-capital seed.
// Does not mutate the engine's four extrema or the held point. Kept callable
// in Release too so the same independent array comparison runs in both proofs.
void check_scratch_equity_extremes(const metrics::ScratchCurveRegion& prefix,
                                   const pf_equity_point_t* held,
                                   double initial_capital,
                                   const double (&expected)[4]);

} // namespace report_sink
} // namespace pineforge
