#pragma once

#include <cerrno>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

namespace fd_flag_diagnostics {

#if defined(__APPLE__)
// Darwin FWASWRITTEN is kernel bookkeeping: "descriptor was written".
// The engine-free test_pipe_writer_fd_flags macOS run observed 0x00010000
// appear on the first successful pipe write without any engine involved.
#if defined(FWASWRITTEN)
inline constexpr int darwin_was_written = FWASWRITTEN;
#else
inline constexpr int darwin_was_written = 0x00010000;
#endif
static_assert(darwin_was_written == 0x00010000, "unexpected Darwin FWASWRITTEN bit");
#endif

inline int comparable_status_flags(int flags) {
#if defined(__APPLE__)
    return flags < 0 ? flags : flags & ~darwin_was_written;
#else
    return flags;
#endif
}

struct Snapshot {
    const char* boundary;
    int flags;
    int flags_errno;
    int descriptor_flags;
    int descriptor_flags_errno;
    struct stat identity{};
    int stat_result;
    int stat_errno;
};

inline Snapshot snapshot(int descriptor, const char* boundary) {
    const int saved_errno = errno;
    Snapshot result{};
    result.boundary = boundary;
    result.flags = ::fcntl(descriptor, F_GETFL);
    result.flags_errno = result.flags == -1 ? errno : 0;
    // Always query F_GETFD, including when F_GETFL failed or changed.
    result.descriptor_flags = ::fcntl(descriptor, F_GETFD);
    result.descriptor_flags_errno = result.descriptor_flags == -1 ? errno : 0;
    result.stat_result = ::fstat(descriptor, &result.identity);
    result.stat_errno = result.stat_result == -1 ? errno : 0;
    errno = saved_errno;
    return result;
}

inline bool same_flags(const Snapshot& before, const Snapshot& after) {
    return before.flags >= 0 && after.flags >= 0
        && comparable_status_flags(before.flags) == comparable_status_flags(after.flags)
        && before.descriptor_flags == after.descriptor_flags;
}

inline void append_identity(std::ostream& output, const Snapshot& value) {
    output << "{result=" << value.stat_result << ",errno=" << value.stat_errno;
    if (value.stat_result == 0) {
        output << ",st_mode=0x" << std::hex << static_cast<std::uintmax_t>(value.identity.st_mode)
               << ",st_dev=0x" << static_cast<std::uintmax_t>(value.identity.st_dev)
               << ",st_ino=0x" << static_cast<std::uintmax_t>(value.identity.st_ino) << std::dec;
    } else {
        output << ",st_mode=unavailable,st_dev=unavailable,st_ino=unavailable";
    }
    output << '}';
}

inline void append_query(std::ostream& output, const char* name, int before, int after,
                         int before_errno, int after_errno) {
    output << name << "{baseline=0x" << std::hex << static_cast<unsigned int>(before)
           << ",final=0x" << static_cast<unsigned int>(after)
           << ",xor=0x" << (static_cast<unsigned int>(before) ^ static_cast<unsigned int>(after))
           << std::dec << ",baseline_errno=" << before_errno << ",final_errno=" << after_errno << '}';
}

class Trace {
public:
    Trace(int descriptor, const char* baseline_boundary) : descriptor_(descriptor) {
        record(baseline_boundary);
    }

    const Snapshot& baseline() const { return snapshots_.front(); }

    Snapshot record(const char* boundary) {
        const int saved_errno = errno;
        const auto current = snapshot(descriptor_, boundary);
        snapshots_.push_back(current);
        errno = saved_errno;
        return current;
    }

    // Observations never assert: existing callers retain their final checks.
    std::string message(const char* reason) const {
        const auto& before = snapshots_.front();
        const auto& after = snapshots_.back();
        const char* first_difference = "none";
        for (const auto& current : snapshots_) {
            if (!same_flags(before, current)) {
                first_difference = current.boundary;
                break;
            }
        }
        std::ostringstream output;
        output << reason << ": fd=" << descriptor_
               << " baseline_boundary=\"" << before.boundary
               << "\" final_boundary=\"" << after.boundary
               << "\" first_difference=\"" << first_difference << "\" ";
        append_query(output, "F_GETFL", before.flags, after.flags, before.flags_errno, after.flags_errno);
        output << ' ';
        append_query(output, "F_GETFD", before.descriptor_flags, after.descriptor_flags,
                     before.descriptor_flags_errno, after.descriptor_flags_errno);
        output << " fstat_before";
        append_identity(output, before);
        output << " fstat_after";
        append_identity(output, after);
        output << " snapshots=[";
        bool first = true;
        for (const auto& current : snapshots_) {
            if (!first) output << ',';
            first = false;
            output << "{boundary=\"" << current.boundary << "\",F_GETFL=0x"
                   << std::hex << static_cast<unsigned int>(current.flags) << std::dec
                   << ",F_GETFL_errno=" << current.flags_errno << ",F_GETFD=0x"
                   << std::hex << static_cast<unsigned int>(current.descriptor_flags) << std::dec
                   << ",F_GETFD_errno=" << current.descriptor_flags_errno << ",fstat";
            append_identity(output, current);
            output << '}';
        }
        output << ']';
        return output.str();
    }

private:
    int descriptor_;
    std::vector<Snapshot> snapshots_;
};

} // namespace fd_flag_diagnostics
