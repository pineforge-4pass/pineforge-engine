#pragma once

// Native-private, capability-driven history preparation. No command-share
// factory, medium creation, environment lookup, resize, or optional cache.
// This is NOT yet the authority behind the installed legacy cohort/range
// reference getters. Their compatibility/failure boundary is a separate need.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/detail/scratch_readback.hpp>
#include <pineforge/native_order_identity.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>
#if defined(__linux__)
#include <pineforge/detail/scratch_directory.hpp>
#endif

namespace pineforge::detail {

inline std::uint64_t native_history_add(std::uint64_t a, std::uint64_t b) {
    if (b > std::numeric_limits<std::uint64_t>::max() - a)
        throw std::overflow_error("native history byte/count sum overflow");
    return a + b;
}
inline std::uint64_t native_history_round(std::uint64_t bytes, std::uint64_t unit) {
    if (!unit) throw std::invalid_argument("native history zero geometry");
    const auto remainder = bytes % unit;
    return remainder ? native_history_add(bytes, unit - remainder) : bytes;
}
inline void native_history_word(std::byte* out, std::uint64_t word) noexcept {
    for (unsigned i = 0; i != 8; ++i) out[i] = std::byte((word >> (8 * i)) & 255);
}
inline std::uint64_t native_history_word(const std::byte* in) noexcept {
    std::uint64_t word = 0;
    for (unsigned i = 0; i != 8; ++i)
        word |= std::uint64_t(std::to_integer<unsigned char>(in[i])) << (8 * i);
    return word;
}
[[noreturn]] inline void native_history_io(int error, const char* operation) {
    // The existing exact typed scratch-I/O transport, with its original
    // errno. No capacity status, new failure code, or success sentinel.
    throw ReportScratchIoError(RunFailureValue{RunFailureCode::scratch_io_failed, {}},
                               error, operation);
}

class NativeHistoryFd {
public:
    explicit NativeHistoryFd(int fd = -1) noexcept : fd_(fd) {}
    ~NativeHistoryFd() { if (fd_ >= 0) ::close(fd_); }
    NativeHistoryFd(const NativeHistoryFd&) = delete;
    NativeHistoryFd& operator=(const NativeHistoryFd&) = delete;
    NativeHistoryFd(NativeHistoryFd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}
    NativeHistoryFd& operator=(NativeHistoryFd&& other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const noexcept { return fd_; }
private:
    int fd_;
};

enum class NativeHistoryMedium { memfd, tmpfs, ramfs };

// Explicit private input, not a shipping C/C++/JSON carrier. Ownership of
// this fd transfers to ONE core. Geometry/identity must match actual fstat/
// fstatvfs/fstatfs. Provisioning, exclusive domain ownership and page charge
// still require genuine external receipts; st_size does not prove them.
struct NativeHistoryCapability {
    NativeHistoryFd fd;
    std::uint64_t capability_id = 0;
    std::uint64_t extent_bytes = 0; // required positive E_i, no default
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t filesystem_type = 0;
    std::uint64_t filesystem_block_bytes = 0;
    std::uint64_t os_page_bytes = 0;
    NativeHistoryMedium medium = NativeHistoryMedium::memfd;
};

class NativeHistoryProvider {
public:
    virtual ~NativeHistoryProvider() = default;
    virtual NativeHistoryCapability acquire(const native_order::RunIdentity&) = 0;
};

enum class NativeHistoryRecordKind : std::uint64_t {
    issued = 1, cohort_open = 2, cohort_receipt = 3
};

struct NativeHistoryOwnerStats {
    std::uint64_t owner_id = 0;
    std::uint64_t capability_id = 0;
    std::uint64_t extent_bytes = 0;
    std::uint64_t rounded_reservation_bytes = 0;
    std::uint64_t demand_high_water_bytes = 0; // includes failed/pending demand
    std::uint64_t io_high_water_bytes = 0;     // includes possibly partial writes
    std::uint64_t committed_bytes = 0;
    std::uint64_t committed_records = 0;
    // Active ranges within the SAME fully charged E_i, not extra extents.
    std::uint64_t pending_bytes = 0;
    std::uint64_t candidate_bytes = 0;
    std::uint64_t relocation_bytes = 0;
    std::uint64_t fixed_workspace_bytes = 0;  // requested object bytes only
    std::uint64_t active_reader_bytes = 0;
    std::uint64_t reader_high_water_bytes = 0;
    std::uint64_t borrowed_codec_workspace_bytes = 0; // separate, NOT summed twice
    int descriptor = -1;
    bool codec_bound = false;
    bool raw_codec_append_seen = false;
};

struct NativeHistoryRegistryStats {
    std::uint64_t cores = 0;
    std::uint64_t unsupplied_cores = 0;
    std::uint64_t owners = 0;
    std::uint64_t extent_bytes = 0;
    std::uint64_t rounded_reservation_bytes = 0;
    std::uint64_t demand_high_water_bytes = 0;
    std::uint64_t io_high_water_bytes = 0;
    std::uint64_t committed_bytes = 0;
    std::uint64_t committed_records = 0;
    std::uint64_t pending_bytes = 0;
    std::uint64_t candidate_bytes = 0;
    std::uint64_t relocation_bytes = 0;
    std::uint64_t fixed_workspace_bytes = 0;
    std::uint64_t active_reader_bytes = 0;
    std::uint64_t reader_high_water_bytes = 0;
    // These counters describe this private registry, NOT complete B_other,
    // allocator usable bytes, fd/kernel cost, process RSS or domain charge.
};

class NativeHistoryOwner;
class NativeCoreRegistration;
// All managed cores, including owners from earlier context snapshots. This
// private setup/registry is serialized by the single-threaded native owner.
// A context-local census alone would omit retained cores after a scope swap.
struct NativeHistoryRegistry {
    mutable std::recursive_mutex mutex;
    NativeHistoryOwner* first = nullptr;
    NativeCoreRegistration* first_core = nullptr;
    std::uint64_t next_owner = 1;
    std::uint64_t next_core = 1;
    std::uint64_t contexts = 0;
    NativeHistoryRegistryStats stats() const;
    template<class Visit> void visit_owners(Visit&& visit) const;
    template<class Visit> void visit_cores(Visit&& visit) const;
};
inline NativeHistoryRegistry native_history_registry;

// Embedded in EVERY core's existing opaque JournalStore, even when no
// capability is supplied. Missing bindings are counted explicitly instead
// of disappearing from the owner census. Moving the journal transfers this
// same registration; reset of a borrowed codec updates it only after clear.
class NativeCoreRegistration {
public:
    explicit NativeCoreRegistration(native_order::RunIdentity identity)
        : identity_(std::move(identity)) {
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        if (native_history_registry.next_core == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native core registration identity exhausted");
        core_id_ = native_history_registry.next_core++;
        next_ = native_history_registry.first_core;
        if (next_) next_->previous_ = this;
        native_history_registry.first_core = this;
    }
    ~NativeCoreRegistration() {
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        if (previous_) previous_->next_ = next_;
        else native_history_registry.first_core = next_;
        if (next_) next_->previous_ = previous_;
    }
    NativeCoreRegistration(const NativeCoreRegistration&) = delete;
    NativeCoreRegistration& operator=(const NativeCoreRegistration&) = delete;
    std::uint64_t core_id() const noexcept { return core_id_; }
    const native_order::RunIdentity& identity() const noexcept { return identity_; }
    const NativeHistoryOwner* owner() const noexcept { return owner_; }
    void bind(native_order::RunIdentity identity, const NativeHistoryOwner* owner) noexcept {
        identity_ = std::move(identity);
        owner_ = owner;
    }
private:
    friend struct NativeHistoryRegistry;
    native_order::RunIdentity identity_;
    const NativeHistoryOwner* owner_ = nullptr;
    NativeCoreRegistration* previous_ = nullptr;
    NativeCoreRegistration* next_ = nullptr;
    std::uint64_t core_id_ = 0;
};

class NativeHistoryContext final : public std::enable_shared_from_this<NativeHistoryContext> {
public:
    explicit NativeHistoryContext(std::shared_ptr<NativeHistoryProvider> provider)
        : provider_(std::move(provider)) {
        if (!provider_) throw std::invalid_argument("native history provider is unsupplied");
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        native_history_registry.contexts = native_history_add(native_history_registry.contexts, 1);
    }
    ~NativeHistoryContext() {
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        --native_history_registry.contexts;
    }
    NativeHistoryContext(const NativeHistoryContext&) = delete;
    NativeHistoryContext& operator=(const NativeHistoryContext&) = delete;
    std::unique_ptr<NativeHistoryOwner> acquire(const native_order::RunIdentity&);
    NativeHistoryRegistryStats stats() const;
    template<class Visit> void visit_owners(Visit&& visit) const;
private:
    friend class NativeHistoryOwner;
    std::shared_ptr<NativeHistoryProvider> provider_;
};

// Startup/teardown on a single thread. Later core creation snapshots the
// then-current shared context; an existing owner is never retargeted.
inline thread_local std::shared_ptr<NativeHistoryContext> native_history_context;
class NativeHistoryContextScope {
public:
    explicit NativeHistoryContextScope(std::shared_ptr<NativeHistoryContext> context) noexcept
        : previous_(std::exchange(native_history_context, std::move(context))) {}
    ~NativeHistoryContextScope() { native_history_context = std::move(previous_); }
    NativeHistoryContextScope(const NativeHistoryContextScope&) = delete;
    NativeHistoryContextScope& operator=(const NativeHistoryContextScope&) = delete;
private:
    std::shared_ptr<NativeHistoryContext> previous_;
};

// An append-only preparation log: [header page][rounded payload][witness
// page]. Logical committed offsets remain dense. A displaced last record has
// one explicit physical location until the next preparation compacts it.
// Candidate writes never touch a pending record or that displaced source.
// Publication is only fixed scalar stores, after ALL native fallible work.
// One 512-byte I/O frame, zero mapped bytes, zero optional prefix/cache,
// zero per-history-page heap directory. Retention ends with the owning core.
class NativeHistoryOwner final {
public:
    static constexpr std::uint64_t page_bytes = 512;
    static constexpr std::uint64_t reservation_unit = 1024 * 1024;
    struct ReadFrame {
        std::array<std::byte, page_bytes> page{};
        std::array<std::byte, 64> header{};
        std::array<std::byte, 64> witness{};
    };
    struct Capacity {
        std::uint64_t payload_bytes;
        std::uint64_t payload_pages;
        std::uint64_t record_pages; // includes header and witness
        std::uint64_t complete_records; // dense history, excludes retarget headroom
        std::uint64_t unused_tail_bytes;
    };

    // A caller retains this guard across its remaining fallible reservations.
    // Destruction discards ONLY candidate ownership; the earlier pending
    // identity and its bytes remain intact. No rollback write is necessary.
    class Candidate {
    public:
        Candidate(const Candidate&) = delete;
        Candidate& operator=(const Candidate&) = delete;
        Candidate(Candidate&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr)), start_(other.start_),
              end_(other.end_), logical_end_(other.logical_end_),
              span_(other.span_), kind_(other.kind_) {}
        Candidate& operator=(Candidate&&) = delete;
        ~Candidate() { discard(); }
        void accept() noexcept {
            if (!owner_) std::terminate();
            auto& owner = *owner_;
            owner.pending_start_ = start_;
            owner.pending_end_ = end_;
            owner.pending_logical_end_ = logical_end_;
            owner.pending_kind_ = kind_;
            owner.pending_ = true;
            owner.stats_.pending_bytes = span_;
            discard();
        }
    private:
        friend class NativeHistoryOwner;
        Candidate(NativeHistoryOwner& owner, NativeHistoryRecordKind kind)
            : owner_(&owner), kind_(kind) {
            if (owner.staging_) throw std::logic_error("native history recursive preparation");
            owner.staging_ = true;
        }
        void discard() noexcept {
            if (!owner_) return;
            owner_->stats_.candidate_bytes = 0;
            owner_->staging_ = false;
            owner_ = nullptr;
        }
        NativeHistoryOwner* owner_;
        std::uint64_t start_ = 0, end_ = 0, logical_end_ = 0, span_ = 0;
        NativeHistoryRecordKind kind_;
    };

    ~NativeHistoryOwner() {
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        if (previous_) previous_->next_ = next_;
        else native_history_registry.first = next_;
        if (next_) next_->previous_ = previous_;
    }
    NativeHistoryOwner(const NativeHistoryOwner&) = delete;
    NativeHistoryOwner& operator=(const NativeHistoryOwner&) = delete;
    const native_order::RunIdentity& identity() const noexcept { return identity_; }
    const NativeHistoryCapability& capability() const noexcept { return capability_; }
    NativeHistoryOwnerStats stats() const noexcept { return stats_; }
    Capacity capacity(std::uint64_t payload_bytes) const {
        const auto rounded = native_history_round(payload_bytes, page_bytes);
        const auto pages = native_history_add(rounded / page_bytes, 2);
        const auto extent_pages = capability_.extent_bytes / page_bytes;
        const auto count = extent_pages / pages;
        return {payload_bytes, rounded / page_bytes, pages, count,
                capability_.extent_bytes - count * pages * page_bytes};
    }

    [[nodiscard]] Candidate stage(NativeHistoryRecordKind kind, const ScratchByteSource& source) {
        Candidate candidate(*this, kind);
        // A previous displaced publication is still read from its intact
        // source if compaction fails, including after a partial copy.
        compact_relocation();
        const auto size = source.size();
        const auto payload = native_history_round(size, page_bytes);
        const auto span = native_history_add(payload, 2 * page_bytes);
        const auto logical_start = stats_.committed_bytes;
        const auto logical_end = native_history_add(logical_start, span);
        auto start = logical_start;
        if (pending_) {
            if (pending_start_ < logical_start) std::terminate();
            // Reuse the free gap before A when it fits. Otherwise place B
            // beyond A AND beyond B's eventual dense destination, so later
            // compaction can fail without overwriting its readable source.
            if (span > pending_start_ - logical_start)
                start = std::max(pending_end_, logical_end);
        }
        const auto end = native_history_add(start, span);
        const auto sequence = native_history_add(stats_.committed_records, 1);
        stats_.demand_high_water_bytes = std::max(stats_.demand_high_water_bytes, end);
        stats_.candidate_bytes = span;
        if (end > capability_.extent_bytes)
            native_history_io(ENOSPC, "native history extent exhausted");
        verify_geometry();
        frame_.fill(std::byte{});
        header(frame_.data(), kind, size, payload / page_bytes, sequence, logical_start);
        write(start, frame_.data(), frame_.size());
        std::uint64_t digest = 14695981039346656037ULL;
        for (std::uint64_t at = 0; at < payload; at += page_bytes) {
            frame_.fill(std::byte{});
            const auto n = static_cast<std::size_t>(std::min(page_bytes, size - at));
            const auto status = source.read(at, {frame_.data(), n});
            if (status != ScratchStatus::ok) {
                rethrow_carried_scratch_failure(source.carried_failure());
                native_history_io(EIO, "native history source read");
            }
            for (std::size_t i = 0; i != n; ++i)
                digest = (digest ^ std::to_integer<unsigned char>(frame_[i])) * 1099511628211ULL;
            write(start + page_bytes + at, frame_.data(), frame_.size());
        }
        frame_.fill(std::byte{});
        header(frame_.data(), kind, size, payload / page_bytes, sequence, logical_start);
        native_history_word(frame_.data() + 56, digest);
        write(end - page_bytes, frame_.data(), frame_.size());
        candidate.start_ = start;
        candidate.end_ = end;
        candidate.logical_end_ = logical_end;
        candidate.span_ = span;
        return candidate;
    }

    // Epoch/token checks belong to WorkingRequestCore. No syscall, allocation
    // or resource failure can occur in this noexcept publication.
    void publish(NativeHistoryRecordKind kind) noexcept {
        if (!pending_ || pending_kind_ != kind || staging_ || relocated_) std::terminate();
        relocation_logical_ = stats_.committed_bytes;
        relocation_start_ = pending_start_;
        relocated_ = pending_start_ != relocation_logical_;
        stats_.relocation_bytes = relocated_ ? stats_.pending_bytes : 0;
        stats_.committed_bytes = pending_logical_end_;
        ++stats_.committed_records;
        pending_ = false;
        stats_.pending_bytes = 0;
    }
    void abandon() noexcept {
        if (staging_) std::terminate();
        pending_ = false;
        stats_.pending_bytes = 0;
    }
    void note_codec(std::uint64_t workspace) noexcept {
        stats_.codec_bound = true;
        stats_.borrowed_codec_workspace_bytes = workspace;
    }
    void note_raw_codec_append() noexcept { stats_.raw_codec_append_seen = true; }

    template<class Visit> void visit_records(ReadFrame& frame, Visit&& visit) const {
        ReaderLease lease(*this, sizeof(ReadFrame) + sizeof(RecordSource));
        // Prefix snapshot permits append and independent nested read frames.
        const auto end = stats_.committed_bytes;
        const auto count = stats_.committed_records;
        std::uint64_t offset = 0;
        for (std::uint64_t index = 0; index < count; ++index) {
            const auto sequence = index + 1;
            read(offset, frame.header.data(), frame.header.size());
            const auto* h = frame.header.data();
            const auto size = native_history_word(h + 16);
            const auto pages = native_history_word(h + 24);
            const auto kind = native_history_word(h + 8);
            const auto rounded = native_history_round(size, page_bytes);
            if (native_history_word(h) != magic || pages != rounded / page_bytes
                || native_history_word(h + 32) != sequence
                || native_history_word(h + 40) != offset
                || native_history_word(h + 48) != stats_.owner_id
                || native_history_word(h + 56) != 0 || kind < 1 || kind > 3)
                native_history_io(EIO, "native history header corrupt");
            const auto next = native_history_add(offset, native_history_add(rounded, 2 * page_bytes));
            if (next > end) native_history_io(EIO, "native history prefix corrupt");
            std::uint64_t digest = 14695981039346656037ULL;
            for (std::uint64_t at = 0; at < size;) {
                const auto n = static_cast<std::size_t>(std::min(page_bytes, size - at));
                read(offset + page_bytes + at, frame.page.data(), n);
                for (std::size_t i = 0; i != n; ++i)
                    digest = (digest ^ std::to_integer<unsigned char>(frame.page[i])) * 1099511628211ULL;
                at += n;
            }
            read(next - page_bytes, frame.witness.data(), frame.witness.size());
            if (std::memcmp(h, frame.witness.data(), 56) != 0
                || native_history_word(frame.witness.data() + 56) != digest)
                native_history_io(EIO, "native history witness corrupt");
            const RecordSource source(*this, offset + page_bytes, size);
            if (!visit(static_cast<NativeHistoryRecordKind>(kind), source)) return;
            offset = next;
        }
        if (offset != end) native_history_io(EIO, "native history end corrupt");
    }

private:
    friend class NativeHistoryContext;
    friend struct NativeHistoryRegistry;
    static constexpr std::uint64_t magic = 0x3154534948464e50ULL;
    NativeHistoryOwner(std::shared_ptr<NativeHistoryContext> context,
                       NativeHistoryCapability capability,
                       native_order::RunIdentity identity, std::uint64_t owner)
        : context_(std::move(context)), capability_(std::move(capability)),
          identity_(std::move(identity)) {
        verify_geometry();
        stats_.owner_id = owner;
        stats_.capability_id = capability_.capability_id;
        stats_.extent_bytes = capability_.extent_bytes;
        stats_.rounded_reservation_bytes =
            native_history_round(capability_.extent_bytes, reservation_unit);
        stats_.fixed_workspace_bytes = sizeof(NativeHistoryOwner);
        stats_.descriptor = capability_.fd.get();
        // Detect aliases before registration or the first payload write.
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        for (auto* other = native_history_registry.first; other; other = other->next_) {
            const auto& c = other->capability_;
            if (c.capability_id == capability_.capability_id
                || (c.device == capability_.device && c.inode == capability_.inode))
                throw std::invalid_argument("native history capability already has a live owner");
        }
        next_ = native_history_registry.first;
        if (next_) next_->previous_ = this;
        native_history_registry.first = this;
    }
    void header(std::byte* out, NativeHistoryRecordKind kind, std::uint64_t size,
                std::uint64_t pages, std::uint64_t sequence, std::uint64_t start) const noexcept {
        const std::uint64_t words[]{magic, static_cast<std::uint64_t>(kind),
            size, pages, sequence, start, stats_.owner_id, 0};
        for (unsigned i = 0; i != 8; ++i) native_history_word(out + 8 * i, words[i]);
    }
    void verify_geometry() const {
        const auto& c = capability_;
        if (c.fd.get() < 0 || !c.capability_id || !c.extent_bytes
            || !c.os_page_bytes || !c.filesystem_block_bytes
            || (c.medium != NativeHistoryMedium::memfd && c.medium != NativeHistoryMedium::tmpfs
                && c.medium != NativeHistoryMedium::ramfs)
            || c.extent_bytes > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
            throw std::invalid_argument("native history capability geometry is unsupplied or invalid");
        struct stat info{};
        if (::fstat(c.fd.get(), &info) != 0) native_history_io(errno, "native history fstat");
        struct statvfs filesystem{};
        if (::fstatvfs(c.fd.get(), &filesystem) != 0)
            native_history_io(errno, "native history fstatvfs");
        const auto os_page = ::sysconf(_SC_PAGESIZE);
        if (!S_ISREG(info.st_mode) || info.st_size < 0
            || static_cast<std::uint64_t>(info.st_size) != c.extent_bytes
            || static_cast<std::uint64_t>(info.st_dev) != c.device
            || static_cast<std::uint64_t>(info.st_ino) != c.inode
            || filesystem.f_bsize != c.filesystem_block_bytes
            || os_page <= 0 || static_cast<std::uint64_t>(os_page) != c.os_page_bytes)
            native_history_io(EIO, "native history capability identity changed");
        const int flags = ::fcntl(c.fd.get(), F_GETFL);
        const int fdflags = ::fcntl(c.fd.get(), F_GETFD);
        if (flags < 0 || fdflags < 0) native_history_io(errno, "native history fd flags");
        if ((flags & O_ACCMODE) != O_RDWR || (flags & O_APPEND) || !(fdflags & FD_CLOEXEC))
            throw std::invalid_argument("native history needs an owned CLOEXEC read/write fd");
#if defined(__linux__)
        struct statfs fs{};
        if (::fstatfs(c.fd.get(), &fs) != 0) native_history_io(errno, "native history fstatfs");
        const auto type = static_cast<std::uint64_t>(fs.f_type);
        if (type != c.filesystem_type || !scratch_filesystem_is_memory(type)
            || std::strcmp(scratch_memory_filesystem_name(type),
                           c.medium == NativeHistoryMedium::ramfs ? "ramfs" : "tmpfs") != 0)
            throw std::invalid_argument("native history requires its declared RAM capability");
        if (c.medium == NativeHistoryMedium::memfd) {
            constexpr int get_seals = 1034;
            constexpr int seal_seal = 1, seal_shrink = 2, seal_grow = 4;
            constexpr int seal_write = 8, seal_future_write = 16;
            const int seals = ::fcntl(c.fd.get(), get_seals);
            if (seals < 0 || (seals & (seal_seal | seal_shrink | seal_grow))
                                != (seal_seal | seal_shrink | seal_grow)
                || (seals & (seal_write | seal_future_write)))
                throw std::invalid_argument("native history memfd size/seal geometry invalid");
        }
#else
        throw std::invalid_argument("native history RAM capability validation is unavailable");
#endif
    }
    void compact_relocation() {
        if (!relocated_) return;
        if (pending_) std::terminate();
        const auto bytes = stats_.relocation_bytes;
        if (relocation_start_ < stats_.committed_bytes) std::terminate();
        for (std::uint64_t at = 0; at < bytes; at += page_bytes) {
            read_physical(relocation_start_ + at, frame_.data(), frame_.size());
            write(relocation_logical_ + at, frame_.data(), frame_.size());
        }
        // The source remains authoritative until EVERY copy write succeeds.
        // A failed copy leaves this mapping and its complete source intact.
        relocated_ = false;
        stats_.relocation_bytes = 0;
    }
    void read(std::uint64_t offset, void* destination, std::size_t bytes) const {
        verify_geometry();
        if (offset > stats_.committed_bytes || bytes > stats_.committed_bytes - offset)
            native_history_io(EIO, "native history read outside committed history");
        auto* out = static_cast<std::byte*>(destination);
        while (bytes) {
            const bool displaced = relocated_ && offset >= relocation_logical_;
            const auto n = relocated_ && !displaced
                ? static_cast<std::size_t>(std::min<std::uint64_t>(bytes, relocation_logical_ - offset))
                : bytes;
            const auto physical = displaced ? relocation_start_ + (offset - relocation_logical_) : offset;
            read_physical(physical, out, n);
            offset += n;
            out += n;
            bytes -= n;
        }
    }
    void read_physical(std::uint64_t offset, void* destination, std::size_t bytes) const {
        verify_geometry();
        if (offset > capability_.extent_bytes || bytes > capability_.extent_bytes - offset)
            native_history_io(EIO, "native history read outside capability");
        auto* out = static_cast<std::byte*>(destination);
        for (std::size_t done = 0; done != bytes;) {
            const auto n = ::pread(capability_.fd.get(), out + done, bytes - done,
                                   static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) native_history_io(n < 0 ? errno : EIO, "native history pread");
            done += static_cast<std::size_t>(n);
        }
    }
    void write(std::uint64_t offset, const void* source, std::size_t bytes) {
        verify_geometry();
        if (offset > capability_.extent_bytes || bytes > capability_.extent_bytes - offset)
            native_history_io(ENOSPC, "native history write outside capability");
        // Even a partial/failed syscall may have touched this whole range.
        stats_.io_high_water_bytes = std::max(stats_.io_high_water_bytes, offset + bytes);
        const auto* in = static_cast<const std::byte*>(source);
        for (std::size_t done = 0; done != bytes;) {
            const auto n = ::pwrite(capability_.fd.get(), in + done, bytes - done,
                                    static_cast<off_t>(offset + done));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) native_history_io(n < 0 ? errno : EIO, "native history pwrite");
            done += static_cast<std::size_t>(n);
        }
    }
    class RecordSource final : public ScratchByteSource {
    public:
        RecordSource(const NativeHistoryOwner& owner, std::uint64_t offset, std::uint64_t size)
            : owner_(owner), offset_(offset), size_(size) {}
        std::uint64_t size() const noexcept override { return size_; }
        const ReportScratchIoError* carried_failure() const noexcept override {
            return failure_ ? &*failure_ : nullptr;
        }
        ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
            if (offset > size_ || out.size > size_ - offset || (out.size && !out.data))
                return ScratchStatus::programming_error;
            try {
                owner_.read(offset_ + offset, out.data, out.size);
                return ScratchStatus::ok;
            } catch (const ReportScratchIoError& error) {
                failure_ = error;
            } catch (const std::bad_alloc&) {
                failure_ = scratch_out_of_memory_failure();
            } catch (...) {
                return ScratchStatus::io_error;
            }
            return ScratchStatus::io_error;
        }
    private:
        const NativeHistoryOwner& owner_;
        const std::uint64_t offset_, size_;
        mutable std::optional<ReportScratchIoError> failure_;
    };
    class ReaderLease {
    public:
        ReaderLease(const NativeHistoryOwner& owner, std::uint64_t bytes)
            : owner_(owner), bytes_(bytes) {
            owner_.stats_.active_reader_bytes = native_history_add(owner_.stats_.active_reader_bytes, bytes);
            owner_.stats_.reader_high_water_bytes =
                std::max(owner_.stats_.reader_high_water_bytes, owner_.stats_.active_reader_bytes);
        }
        ~ReaderLease() { owner_.stats_.active_reader_bytes -= bytes_; }
    private:
        const NativeHistoryOwner& owner_;
        std::uint64_t bytes_;
    };

    std::shared_ptr<NativeHistoryContext> context_;
    NativeHistoryCapability capability_;
    native_order::RunIdentity identity_;
    NativeHistoryOwner* previous_ = nullptr;
    NativeHistoryOwner* next_ = nullptr;
    mutable NativeHistoryOwnerStats stats_;
    std::array<std::byte, page_bytes> frame_{};
    std::uint64_t pending_start_ = 0;
    std::uint64_t pending_end_ = 0;
    std::uint64_t pending_logical_end_ = 0;
    std::uint64_t relocation_logical_ = 0;
    std::uint64_t relocation_start_ = 0;
    NativeHistoryRecordKind pending_kind_ = NativeHistoryRecordKind::issued;
    bool pending_ = false;
    bool staging_ = false;
    bool relocated_ = false;
};

inline std::unique_ptr<NativeHistoryOwner> NativeHistoryContext::acquire(
        const native_order::RunIdentity& identity) {
    std::uint64_t id = 0;
    {
        const std::lock_guard<std::recursive_mutex> lock(native_history_registry.mutex);
        if (native_history_registry.next_owner == std::numeric_limits<std::uint64_t>::max())
            throw std::overflow_error("native history owner identity exhausted");
        id = native_history_registry.next_owner++;
    }
    auto capability = provider_->acquire(identity); // actual provider invocation
    auto owner = std::unique_ptr<NativeHistoryOwner>(new NativeHistoryOwner(
        shared_from_this(), std::move(capability), identity, id));
    return owner;
}
inline NativeHistoryRegistryStats NativeHistoryContext::stats() const {
    // Explicitly includes earlier contexts; callers cannot accidentally turn
    // a later-context subset into the all-live-core denominator.
    return native_history_registry.stats();
}
inline NativeHistoryRegistryStats NativeHistoryRegistry::stats() const {
    // Observations require a closed-call checkpoint for every inspected core;
    // this list lock is not an all-threads memory barrier or sampler.
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    NativeHistoryRegistryStats result;
    result.fixed_workspace_bytes = sizeof(NativeHistoryRegistry);
    // Checked SUM rather than an unchecked contexts*sizeof multiplication.
    for (std::uint64_t i = 0; i < contexts; ++i)
        result.fixed_workspace_bytes =
            native_history_add(result.fixed_workspace_bytes, sizeof(NativeHistoryContext));
    visit_cores([&](const NativeCoreRegistration& core) {
        result.cores = native_history_add(result.cores, 1);
        if (!core.owner()) result.unsupplied_cores = native_history_add(result.unsupplied_cores, 1);
        result.fixed_workspace_bytes =
            native_history_add(result.fixed_workspace_bytes, sizeof(NativeCoreRegistration));
    });
    visit_owners([&](const NativeHistoryOwner& owner) {
        const auto s = owner.stats();
        result.owners = native_history_add(result.owners, 1);
        result.extent_bytes = native_history_add(result.extent_bytes, s.extent_bytes);
        result.rounded_reservation_bytes =
            native_history_add(result.rounded_reservation_bytes, s.rounded_reservation_bytes);
        result.demand_high_water_bytes =
            native_history_add(result.demand_high_water_bytes, s.demand_high_water_bytes);
        result.io_high_water_bytes = native_history_add(result.io_high_water_bytes, s.io_high_water_bytes);
        result.committed_bytes = native_history_add(result.committed_bytes, s.committed_bytes);
        result.committed_records = native_history_add(result.committed_records, s.committed_records);
        result.pending_bytes = native_history_add(result.pending_bytes, s.pending_bytes);
        result.candidate_bytes = native_history_add(result.candidate_bytes, s.candidate_bytes);
        result.relocation_bytes = native_history_add(result.relocation_bytes, s.relocation_bytes);
        result.fixed_workspace_bytes =
            native_history_add(result.fixed_workspace_bytes, s.fixed_workspace_bytes);
        result.active_reader_bytes = native_history_add(result.active_reader_bytes, s.active_reader_bytes);
        result.reader_high_water_bytes =
            native_history_add(result.reader_high_water_bytes, s.reader_high_water_bytes);
    });
    return result;
}
template<class Visit> void NativeHistoryContext::visit_owners(Visit&& visit) const {
    native_history_registry.visit_owners(std::forward<Visit>(visit));
}
template<class Visit> void NativeHistoryRegistry::visit_owners(Visit&& visit) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    // Serialized, passive enumeration. A visitor must not destroy/move cores
    // or create owners; it may inspect immutable records and nested readers.
    for (const auto* owner = first; owner; owner = owner->next_) visit(*owner);
}
template<class Visit> void NativeHistoryRegistry::visit_cores(Visit&& visit) const {
    const std::lock_guard<std::recursive_mutex> lock(mutex);
    for (const auto* core = first_core; core; core = core->next_) visit(*core);
}

// Bounded native-private cohort projection over the same actual owner.
// Replays ordered issued candidates and applied add/remove receipts with
// disjoint frames. No full roster, per-page index or borrowed-row cache.
// This trades scan work for fixed workspace; indexed acceleration and the
// installed FULL/noexcept bridge remain separate integration work.
class NativeCohortSource {
public:
    explicit NativeCohortSource(const NativeHistoryOwner& owner) noexcept : owner_(owner) {}

    bool contains(std::uint64_t cohort, const native_order::RequestHandle& root) const {
        NativeHistoryOwner::ReadFrame frame;
        bool present = false;
        owner_.visit_records(frame, [&](NativeHistoryRecordKind kind, const ScratchByteSource& source) {
            if (kind != NativeHistoryRecordKind::cohort_receipt) return true;
            const auto words = prefix(source, frame);
            if (words[1] > 1 || words[2] > 3)
                native_history_io(EIO, "native cohort receipt enum corrupt");
            if (words[2] != 0 || words[3] != cohort) return true; // Applied = 0
            if (!words[7]) native_history_io(EIO, "native cohort applied origin missing");
            if (words[8] == root.run.run_number && words[9] == root.incarnation
                && session_matches(source, native_history_add(88, words[6]),
                                   words[10], root.run.session_key, frame))
                present = words[1] == 0; // Add = 0, Remove = 1
            return true;
        });
        return present;
    }

    template<class Visit> void visit_members(std::uint64_t cohort, Visit&& visit) const {
        NativeHistoryOwner::ReadFrame frame;
        std::uint64_t previous = 0;
        owner_.visit_records(frame, [&](NativeHistoryRecordKind kind, const ScratchByteSource& source) {
            if (kind != NativeHistoryRecordKind::issued) return true;
            const auto words = prefix(source, frame);
            if (!words[5] || words[5] <= previous || words[1] != words[5]
                || words[4] != owner_.identity().run_number
                || !session_matches(source, 88, words[6], owner_.identity().session_key, frame))
                native_history_io(EIO, "native cohort issued identity corrupt");
            previous = words[5];
            // A successor's predecessor-root candidate is already in the
            // ascending issued stream; emit each canonical origin only once.
            if (words[7]) return true;
            const native_order::RequestHandle root{owner_.identity(), words[5]};
            return !contains(cohort, root) || visit(root);
        });
    }
private:
    static void check(ScratchStatus status, const ScratchByteSource& source) {
        if (status == ScratchStatus::ok) return;
        rethrow_carried_scratch_failure(source.carried_failure());
        native_history_io(EIO, "native cohort source read");
    }
    static std::array<std::uint64_t, 11> prefix(
            const ScratchByteSource& source, NativeHistoryOwner::ReadFrame& frame) {
        if (source.size() < 88) native_history_io(EIO, "native cohort source prefix truncated");
        check(source.read(0, {frame.page.data(), 88}), source);
        std::array<std::uint64_t, 11> words{};
        for (unsigned i = 0; i != words.size(); ++i)
            words[i] = native_history_word(frame.page.data() + i * 8);
        if (words[0] != 1 || words[7] > 1
            || (!words[7] && (words[8] || words[9] || words[10]))
            || native_history_add(88, native_history_add(words[6], words[10])) != source.size())
            native_history_io(EIO, "native cohort source layout corrupt");
        return words;
    }
    static bool session_matches(const ScratchByteSource& source, std::uint64_t offset,
            std::uint64_t size, const native_order::SessionKey& session,
            NativeHistoryOwner::ReadFrame& frame) {
        if (size != session.size()) return false;
        for (std::uint64_t at = 0; at < size;) {
            const auto n = static_cast<std::size_t>(std::min(NativeHistoryOwner::page_bytes, size - at));
            check(source.read(offset + at, {frame.page.data(), n}), source);
            if (std::memcmp(frame.page.data(), session.data() + at, n) != 0) return false;
            at += n;
        }
        return true;
    }
    const NativeHistoryOwner& owner_;
};

inline std::unique_ptr<NativeHistoryOwner> acquire_native_history(
        const native_order::RunIdentity& identity) {
    // No supplied context preserves existing product behavior. It is
    // UNSUPPLIED in proof qualification, never a successful new input refusal.
    const auto context = native_history_context;
    return context ? context->acquire(identity) : nullptr;
}
} // namespace pineforge::detail
