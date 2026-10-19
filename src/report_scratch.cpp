#include "report_scratch.hpp"

#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <type_traits>
#include <unistd.h>

namespace pineforge {
namespace report_sink {
namespace {

static_assert(std::is_trivially_copyable<pf_equity_point_t>::value,
              "scratch points must preserve their complete binary representation");
static_assert(std::numeric_limits<off_t>::is_signed
                  && sizeof(off_t) <= sizeof(std::int64_t),
              "scratch offsets must fit the P00 signed 64-bit region");
static_assert(kScratchWindowPoints > 0, "scratch window must hold a complete point");

// L19a preserves errno through std::system_error. Also carry the established
// sink-I/O run code: the classifier otherwise treats an uncoded system_error
// as engine_unclassified_error in this integration epoch.
class ScratchIoError final : public std::system_error, public RunFailureInfo {
public:
    ScratchIoError(int error_number, const char* operation)
        : std::system_error(error_number, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::report_sink_failed, {}) {}
};

[[noreturn]] void fail_io(int error_number, const char* operation) {
    throw ScratchIoError(error_number, operation);
}

constexpr std::int64_t maximum_offset() {
    return static_cast<std::int64_t>(std::numeric_limits<off_t>::max());
}

void check_span(std::int64_t offset, std::size_t length) {
    if (offset < 0 || offset > maximum_offset()
        || static_cast<std::uintmax_t>(length)
            > static_cast<std::uintmax_t>(maximum_offset() - offset)) {
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                            "report scratch byte range is invalid");
    }
}

void check_region(const metrics::ScratchCurveRegion& region) {
    if ((region.point_count != 0 && region.descriptor < 0) || region.byte_offset < 0
        || region.byte_offset > maximum_offset() || region.point_count < 0
        || region.point_count
            > (maximum_offset() - region.byte_offset)
                / static_cast<std::int64_t>(sizeof(pf_equity_point_t))) {
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                            "report scratch curve region is invalid");
    }
}

void check_buffer(const void* bytes, std::size_t length) {
    if (length != 0 && bytes == nullptr) {
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                            "report scratch buffer is null");
    }
}

void read_exact(int descriptor, std::int64_t offset, void* bytes, std::size_t length) {
    check_span(offset, length);
    check_buffer(bytes, length);
    auto* destination = static_cast<unsigned char*>(bytes);
    std::size_t received = 0;
    while (received < length) {
        const std::size_t chunk = std::min(length - received, kScratchWindowBytes);
        const ssize_t count = ::pread(descriptor, destination + received, chunk,
            static_cast<off_t>(offset + static_cast<std::int64_t>(received)));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail_io(count < 0 ? errno : EIO, "report scratch read");
        received += static_cast<std::size_t>(count);
    }
}

int open_private_file(const std::string& directory) {
    if (directory.empty() || directory.find('\0') != std::string::npos) {
        throw coded<std::invalid_argument>(RunFailureCode::engine_invariant, {},
                                            "report scratch directory is invalid");
    }
#if defined(O_TMPFILE) && defined(O_CLOEXEC)
    int anonymous;
    do {
        anonymous = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
    } while (anonymous < 0 && errno == EINTR);
    if (anonymous >= 0) return anonymous;
    // Filesystems/older kernels may lack O_TMPFILE. Other failures (permissions,
    // quota, missing directory) must keep their errno and requested directory.
    if (errno != EOPNOTSUPP && errno != EINVAL && errno != EISDIR && errno != ENOSYS)
        fail_io(errno, "report scratch create");
#endif
    std::string name = directory;
    if (name.back() != '/') name += '/';
    name += "pineforge-scratch-XXXXXX";
    const int descriptor = ::mkstemp(name.data());
    if (descriptor < 0) fail_io(errno, "report scratch create");
    if (::unlink(name.c_str()) != 0) {
        const int error_number = errno;
        ::close(descriptor);
        fail_io(error_number, "report scratch unlink");
    }
    int flags;
    do {
        flags = ::fcntl(descriptor, F_GETFD);
    } while (flags < 0 && errno == EINTR);
    int result = -1;
    if (flags >= 0) {
        do {
            result = ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
        } while (result < 0 && errno == EINTR);
    }
    if (result < 0) {
        const int error_number = errno;
        ::close(descriptor);
        fail_io(error_number, "report scratch cloexec");
    }
    return descriptor;
}

} // namespace

ScratchFile::ScratchFile(const std::string& directory)
    : descriptor_(open_private_file(directory)) {}

ScratchFile::~ScratchFile() {
    if (descriptor_ >= 0) ::close(descriptor_);
}

int ScratchFile::descriptor() const noexcept {
    return descriptor_;
}

void ScratchFile::write(std::int64_t byte_offset, const void* bytes, std::size_t length) {
    check_span(byte_offset, length);
    check_buffer(bytes, length);
    const auto* source = static_cast<const unsigned char*>(bytes);
    std::size_t written = 0;
    while (written < length) {
        const std::size_t chunk = std::min(length - written, kScratchWindowBytes);
        const ssize_t count = ::pwrite(descriptor_, source + written, chunk,
            static_cast<off_t>(byte_offset + static_cast<std::int64_t>(written)));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail_io(count < 0 ? errno : EIO, "report scratch write");
        written += static_cast<std::size_t>(count);
    }
}

void ScratchFile::read(std::int64_t byte_offset, void* bytes, std::size_t length) const {
    read_exact(descriptor_, byte_offset, bytes, length);
}

void ScratchFile::truncate(std::int64_t byte_count) {
    check_span(byte_count, 0);
    int result;
    do {
        result = ::ftruncate(descriptor_, static_cast<off_t>(byte_count));
    } while (result < 0 && errno == EINTR);
    if (result < 0) fail_io(errno, "report scratch truncate");
}

struct ScratchCurveWriter::State {
    ScratchFile file;
    std::array<pf_equity_point_t, kScratchWindowPoints> pending{};
    std::size_t pending_count = 0;
    std::int64_t disk_points = 0;

    explicit State(const std::string& directory) : file(directory) {}
};

ScratchCurveWriter::ScratchCurveWriter(const std::string& directory)
    : state_(std::make_unique<State>(directory)) {}

ScratchCurveWriter::~ScratchCurveWriter() = default;

void ScratchCurveWriter::append(const pf_equity_point_t& point) {
    auto& state = *state_;
    if (size() == maximum_offset() / static_cast<std::int64_t>(sizeof(point))) {
        throw coded<std::overflow_error>(RunFailureCode::engine_invariant, {},
                                          "report scratch curve offset exhausted");
    }
    if (state.pending_count == state.pending.size()) flush();
    std::memcpy(&state.pending[state.pending_count], &point, sizeof(point));
    ++state.pending_count;
}

void ScratchCurveWriter::replace_last(const pf_equity_point_t& point) {
    auto& state = *state_;
    if (size() == 0) {
        throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                        "report scratch curve has no last point");
    }
    if (state.pending_count == 0) {
        // Bring the last disk row back into the authoritative RAM batch.
        // A partial flush of its replacement is then retryable exactly like
        // an append batch, without losing the replacement's complete bytes.
        --state.disk_points;
        state.pending_count = 1;
    }
    std::memcpy(&state.pending[state.pending_count - 1], &point, sizeof(point));
}

void ScratchCurveWriter::clear() {
    auto& state = *state_;
    state.file.truncate(0);
    state.disk_points = 0;
    state.pending_count = 0;
}

void ScratchCurveWriter::replace_tail(const pf_equity_point_t* points, std::size_t count) {
    auto& state = *state_;
    if (count > state.pending.size() || static_cast<std::int64_t>(count) > size()) {
        throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                        "report scratch replacement tail is invalid");
    }
    check_buffer(points, count * sizeof(*points));
    if (count == 0) return;
    if (count > state.pending_count) {
        // Complete the old batch before changing its authoritative range.
        // On failure flush() leaves every old pending point available to retry.
        flush();
        state.disk_points -= static_cast<std::int64_t>(count);
        state.pending_count = count;
    }
    std::memcpy(state.pending.data() + state.pending_count - count,
                points, count * sizeof(*points));
}

void ScratchCurveWriter::flush() const {
    auto& state = *state_;
    if (state.pending_count == 0) return;
    state.file.write(state.disk_points * static_cast<std::int64_t>(sizeof(pf_equity_point_t)),
                     state.pending.data(), state.pending_count * sizeof(pf_equity_point_t));
    state.disk_points += static_cast<std::int64_t>(state.pending_count);
    state.pending_count = 0;
}

std::int64_t ScratchCurveWriter::size() const noexcept {
    return state_->disk_points + static_cast<std::int64_t>(state_->pending_count);
}

metrics::ScratchCurveRegion ScratchCurveWriter::region() const {
    flush();
    return {state_->file.descriptor(), 0, state_->disk_points};
}

void read_scratch_points(const metrics::ScratchCurveRegion& region,
                         std::int64_t first, pf_equity_point_t* points,
                         std::int64_t count) {
    check_region(region);
    if (first < 0 || count < 0 || first > region.point_count
        || count > region.point_count - first
        || static_cast<std::uintmax_t>(count)
            > std::numeric_limits<std::size_t>::max() / sizeof(pf_equity_point_t)) {
        throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                        "report scratch point range is invalid");
    }
    const std::int64_t offset =
        region.byte_offset + first * static_cast<std::int64_t>(sizeof(pf_equity_point_t));
    read_exact(region.descriptor, offset, points,
                static_cast<std::size_t>(count) * sizeof(pf_equity_point_t));
}

struct ScratchCurveWindow::State {
    metrics::ScratchCurveRegion region;
    std::array<pf_equity_point_t, kScratchWindowPoints> window{};
    std::int64_t first = 0;
    std::int64_t count = 0;

    explicit State(const metrics::ScratchCurveRegion& value) : region(value) {
        check_region(region);
    }
};

ScratchCurveWindow::ScratchCurveWindow(const metrics::ScratchCurveRegion& region)
    : state_(std::make_unique<State>(region)) {}

ScratchCurveWindow::~ScratchCurveWindow() = default;

std::int64_t ScratchCurveWindow::size() const noexcept {
    return state_->region.point_count;
}

pf_equity_point_t ScratchCurveWindow::operator[](std::int64_t index) const {
    auto& state = *state_;
    if (index < 0 || index >= size()) {
        throw coded<std::out_of_range>(RunFailureCode::engine_invariant, {},
                                        "report scratch point index is invalid");
    }
    if (index < state.first || index - state.first >= state.count) {
        constexpr auto capacity = static_cast<std::int64_t>(kScratchWindowPoints);
        const std::int64_t first = index / capacity * capacity;
        const std::int64_t count = std::min(size() - first, capacity);
        // A failed refill may have overwritten part of the old window.
        // Invalidate its range before I/O, and publish only a complete read.
        state.count = 0;
        read_scratch_points(state.region, first, state.window.data(), count);
        state.first = first;
        state.count = count;
    }
    pf_equity_point_t point;
    std::memcpy(&point, &state.window[static_cast<std::size_t>(index - state.first)],
                sizeof(point));
    return point;
}

void check_scratch_equity_extremes(const metrics::ScratchCurveRegion& prefix,
                                   const pf_equity_point_t* held,
                                   double initial_capital,
                                   const double (&expected)[4]) {
    ScratchCurveWindow curve(prefix);
    double actual[] = {initial_capital, initial_capital, 0.0, 0.0};
    const auto fold = [&](double equity) {
        if (equity > actual[0]) {
            actual[0] = equity;
            actual[1] = equity;
        }
        if (equity < actual[1]) actual[1] = equity;
        const double drawdown = actual[0] - equity;
        if (drawdown > actual[2]) actual[2] = drawdown;
        const double runup = equity - actual[1];
        if (runup > actual[3]) actual[3] = runup;
    };
    for (std::int64_t i = 0; i < curve.size(); ++i) fold(curve[i].equity);
    if (held) fold(held->equity);
    if (std::memcmp(actual, expected, sizeof(actual)) != 0) {
        throw coded<std::logic_error>(RunFailureCode::engine_invariant, {},
            "report sink tail fold differs from full scratch equity walk");
    }
}

} // namespace report_sink
} // namespace pineforge
