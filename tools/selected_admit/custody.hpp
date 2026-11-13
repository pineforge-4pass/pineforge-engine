#pragma once

#include "descriptor.hpp"
#include "record_tracker.hpp"
#include "../../src/sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <poll.h>
#include <streambuf>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace pineforge::selected_admit {
inline constexpr std::size_t kStreamChunkBytes = 65536;
using RecordTracker = sc::RecordTracker;

// Null members dispatch directly to the real syscall. Only tests supply hooks.
struct Seam {
    void* ctx = nullptr;
    ssize_t (*pread)(void*, int, void*, std::size_t, off_t) = nullptr;
    ssize_t (*read)(void*, int, void*, std::size_t) = nullptr;
    ssize_t (*write)(void*, int, const void*, std::size_t) = nullptr;
    int (*fstat)(void*, int, struct stat*) = nullptr;
    void (*on_bar)(void*, std::uint64_t) = nullptr;
};

inline ssize_t call_pread(const Seam& s, int fd, void* p, std::size_t n, off_t at) {
    return s.pread ? s.pread(s.ctx, fd, p, n, at) : ::pread(fd, p, n, at);
}
inline ssize_t call_read(const Seam& s, int fd, void* p, std::size_t n) {
    return s.read ? s.read(s.ctx, fd, p, n) : ::read(fd, p, n);
}
inline ssize_t call_write(const Seam& s, int fd, const void* p, std::size_t n) {
    return s.write ? s.write(s.ctx, fd, p, n) : ::write(fd, p, n);
}
inline int call_fstat(const Seam& s, int fd, struct stat* st) {
    return s.fstat ? s.fstat(s.ctx, fd, st) : ::fstat(fd, st);
}

inline bool wait_ready(int fd, short events) {
    struct pollfd p {fd, events, 0};
    for (;;) {
        const int result = ::poll(&p, 1, -1);
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0 || (p.revents & POLLNVAL)) return false;
        return (p.revents & (events | POLLERR | POLLHUP)) != 0;
    }
}

inline bool write_all(int fd, std::string_view bytes, const Seam& seam = Seam{}) {
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto n = call_write(seam, fd, bytes.data() + position, bytes.size() - position);
        if (n > 0 && static_cast<std::size_t>(n) <= bytes.size() - position) {
            position += static_cast<std::size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
            && wait_ready(fd, POLLOUT)) continue;
        return false;
    }
    return true;
}

inline DecodeStatus read_descriptor_snapshot(int fd, std::string_view expected,
                                             std::string& bytes, std::string& why,
                                             const Seam& seam = Seam{}) {
    struct stat st{};
    if (call_fstat(seam, fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        why = "helper:descriptor_invalid";
        return DecodeStatus::Invalid;
    }
    bytes.assign(kDescriptorBytes + 1, '\0');
    std::size_t count = 0;
    while (count < bytes.size()) {
        const auto n = call_pread(seam, fd, &bytes[count], bytes.size() - count,
                                  static_cast<off_t>(count));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || static_cast<std::size_t>(n) > bytes.size() - count) {
            why = "helper:descriptor_invalid";
            return DecodeStatus::Invalid;
        }
        if (n == 0) break;
        count += static_cast<std::size_t>(n);
    }
    bytes.resize(count);
    if (count > kDescriptorBytes) {
        why = "helper:descriptor_too_large";
        return DecodeStatus::TooLarge;
    }
    report_sink::Sha256 hash;
    hash.update(bytes);
    if (hash.hex_digest() != expected) {
        why = "helper:descriptor_hash_mismatch";
        return DecodeStatus::HashMismatch;
    }
    return DecodeStatus::Ok;
}

enum class CustodyReason {
    None, FdInvalid, FdType, FdMode, ExtentMismatch, Truncated, Extended,
    Mutated, IdentityMismatch, RecordTooLong
};

inline const char* custody_reason_name(CustodyReason r) {
    switch (r) {
    case CustodyReason::None: return "none";
    case CustodyReason::FdInvalid: return "helper:fd_invalid";
    case CustodyReason::FdType: return "helper:fd_type";
    case CustodyReason::FdMode: return "helper:fd_mode";
    case CustodyReason::ExtentMismatch: return "helper:extent_mismatch";
    case CustodyReason::Truncated: return "helper:truncated";
    case CustodyReason::Extended: return "helper:extended";
    case CustodyReason::Mutated: return "helper:mutated";
    case CustodyReason::IdentityMismatch: return "helper:identity_mismatch";
    case CustodyReason::RecordTooLong: return "helper:record_too_long";
    }
    return "helper:internal_error";
}

inline CustodyReason preflight_file(int fd, const StreamSpec& spec,
                                    const Seam& seam = Seam{}, struct stat* snapshot = nullptr) {
    struct stat st{};
    if (call_fstat(seam, fd, &st) != 0) return CustodyReason::FdInvalid;
    if (!S_ISREG(st.st_mode)) return CustodyReason::FdType;
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) return CustodyReason::FdInvalid;
    if ((flags & O_ACCMODE) != O_RDONLY) return CustodyReason::FdMode;
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) != spec.bytes)
        return CustodyReason::ExtentMismatch;
    if (snapshot) *snapshot = st;
    return CustodyReason::None;
}

inline CustodyReason preflight_pipe(int fd, bool writer, const Seam& seam = Seam{}) {
    struct stat st{};
    if (call_fstat(seam, fd, &st) != 0) return CustodyReason::FdInvalid;
    if (!S_ISFIFO(st.st_mode)) return CustodyReason::FdType;
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) return CustodyReason::FdInvalid;
    if ((flags & O_ACCMODE) != (writer ? O_WRONLY : O_RDONLY)) return CustodyReason::FdMode;
    return CustodyReason::None;
}

inline bool same_file_snapshot(const struct stat& a, const struct stat& b) {
#if defined(__APPLE__)
    const auto am = a.st_mtimespec, bm = b.st_mtimespec;
    const auto ac = a.st_ctimespec, bc = b.st_ctimespec;
#else
    const auto am = a.st_mtim, bm = b.st_mtim;
    const auto ac = a.st_ctim, bc = b.st_ctim;
#endif
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size
        && am.tv_sec == bm.tv_sec && am.tv_nsec == bm.tv_nsec
        && ac.tv_sec == bc.tv_sec && ac.tv_nsec == bc.tv_nsec;
}

class FdStreambuf final : public std::streambuf {
public:
    FdStreambuf(const StreamSpec& spec, std::uint64_t maximum,
                const struct stat& snapshot, const Seam& seam = Seam{},
                std::size_t* active = nullptr, std::size_t index = 0)
        : spec_(spec), snapshot_(snapshot), seam_(seam), tracker_(maximum),
          active_(active), index_(index) {
        setg(nullptr, nullptr, nullptr);
    }
    std::uint64_t bytes_read() const noexcept { return consumed_; }
    const RecordTracker& tracker() const noexcept { return tracker_; }
    CustodyReason reason() const noexcept { return reason_; }
    bool io_failed() const noexcept { return io_failed_; }
    bool custody_complete() const noexcept { return complete_; }
    void rethrow_internal() const { if (internal_) std::rethrow_exception(internal_); }

protected:
    int_type underflow() override {
        if (active_) *active_ = index_;
        if (reason_ != CustodyReason::None || io_failed_) throw std::ios_base::failure("input failed");
        if (position_ != size_) return traits_type::to_int_type(chunk_[position_]);
        if (complete_) return traits_type::eof();
        try { refill(); }
        catch (const std::bad_alloc&) {
            internal_ = std::current_exception();
            throw;
        }
        return complete_ ? traits_type::eof() : traits_type::to_int_type(chunk_[position_]);
    }
    int_type uflow() override {
        tracker_.begin_get();
        const auto value = underflow();
        if (traits_type::eq_int_type(value, traits_type::eof())) return value;
        if (!tracker_.consume(static_cast<unsigned char>(traits_type::to_char_type(value))))
            fail(CustodyReason::RecordTooLong);
        ++position_;
        ++consumed_;
        return value;
    }

private:
    [[noreturn]] void fail(CustodyReason reason) {
        reason_ = reason;
        throw std::ios_base::failure(custody_reason_name(reason));
    }
    [[noreturn]] void io_failure() {
        io_failed_ = true;
        throw std::ios_base::failure("input_read_failure");
    }
    ssize_t obtain(void* out, std::size_t count, std::uint64_t offset) {
        for (;;) {
            const auto n = spec_.file
                ? call_pread(seam_, spec_.fd, out, count, static_cast<off_t>(offset))
                : call_read(seam_, spec_.fd, out, count);
            if (n >= 0 && static_cast<std::size_t>(n) <= count) return n;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)
                && wait_ready(spec_.fd, POLLIN)) continue;
            io_failure();
        }
    }
    void refill() {
        if (spec_.file && fetched_ == spec_.bytes) {
            char probe;
            if (obtain(&probe, 1, fetched_) != 0) fail(CustodyReason::Extended);
            struct stat after{};
            if (call_fstat(seam_, spec_.fd, &after) != 0) fail(CustodyReason::FdInvalid);
            if (!same_file_snapshot(snapshot_, after)) fail(CustodyReason::Mutated);
            if (hash_.hex_digest() != spec_.sha256) fail(CustodyReason::IdentityMismatch);
            complete_ = true;
            chunk_.reset();
            return;
        }
        if (!chunk_) chunk_ = std::make_unique<char[]>(kStreamChunkBytes);
        const auto amount = spec_.file
            ? static_cast<std::size_t>(std::min<std::uint64_t>(kStreamChunkBytes,
                                                             spec_.bytes - fetched_))
            : kStreamChunkBytes;
        const auto n = obtain(chunk_.get(), amount, fetched_);
        if (n == 0) {
            if (spec_.file) fail(CustodyReason::Truncated);
            complete_ = true;
            chunk_.reset();
            return;
        }
        position_ = 0;
        size_ = static_cast<std::size_t>(n);
        fetched_ += size_;
        if (spec_.file) hash_.update(std::string_view(chunk_.get(), size_));
    }

    const StreamSpec& spec_;
    struct stat snapshot_;
    Seam seam_;
    RecordTracker tracker_;
    report_sink::Sha256 hash_;
    std::unique_ptr<char[]> chunk_;
    std::size_t position_ = 0, size_ = 0;
    std::uint64_t fetched_ = 0, consumed_ = 0;
    CustodyReason reason_ = CustodyReason::None;
    bool complete_ = false, io_failed_ = false;
    std::size_t* active_;
    std::size_t index_;
    std::exception_ptr internal_;
};
}  // namespace pineforge::selected_admit
