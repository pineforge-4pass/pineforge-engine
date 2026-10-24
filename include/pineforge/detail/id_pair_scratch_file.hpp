#pragma once

#include <pineforge/detail/scratch_file.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace pineforge::detail {

// Keep L19a's system_error surface, carrying the run's resource failure too.
class IdPairStoreError final : public std::system_error, public RunFailureInfo {
public:
    IdPairStoreError(int error, const char* operation)
        : std::system_error(error, std::generic_category(), operation),
          RunFailureInfo(RunFailureCode::scratch_io_failed, {}) {}
};

class IdPairScratchFile {
public:
    // Explicit per-instance fixture seam. Counters observe actual transfers
    // (syscalls in file mode, checked copies in arena mode);
    // a selected write writes a real prefix, then reports an IO failure.
    struct FixtureIO {
        std::uint64_t reads = 0, writes = 0;
        std::uint64_t read_bytes = 0, write_bytes = 0;
        std::uint64_t fail_write = 0;
        std::size_t partial_write = 0;
    };
    void fixture_count_io(FixtureIO* io) noexcept { fixture_io_ = io; }
    FixtureIO* fixture_io() const noexcept { return fixture_io_; }
    void set_store(ScratchStore store) noexcept { file_.set_store(store); }

    explicit IdPairScratchFile(ScratchStore store = ScratchStore::unknown) {
        file_.set_store(store);
    }
    IdPairScratchFile(const IdPairScratchFile&) = delete;
    IdPairScratchFile& operator=(const IdPairScratchFile&) = delete;
    IdPairScratchFile(IdPairScratchFile&& other) noexcept
        : file_(std::move(other.file_)),
          fixture_io_(std::exchange(other.fixture_io_, nullptr)) {}
    IdPairScratchFile& operator=(IdPairScratchFile&& other) noexcept {
        if (this != &other) {
            reset();
            file_ = std::move(other.file_);
            fixture_io_ = std::exchange(other.fixture_io_, nullptr);
        }
        return *this;
    }
    ~IdPairScratchFile() = default;

    bool opened() const noexcept { return file_.opened(); }
    bool arena_backed() const noexcept { return file_.arena_backed(); }
    void reset() noexcept { file_.clear(); }
    void open(const std::string& directory) {
        if (opened()) return;
        file_.set_directory(directory);
        auxiliary("id table create", [&] { file_.open("pineforge-id-pairs"); });
    }
    void resize(std::uint64_t bytes) {
        check_range(0, bytes);
        if (!opened()) throw IdPairStoreError(EBADF, "id table resize");
        // The production sizing caller owns a fresh replacement index.
        // Monotone zero-filled growth also works without a sizing syscall.
        auxiliary("id table resize", [&] { file_.ensure_size(bytes); });
    }
    void read(std::uint64_t offset, void* destination, std::size_t length) const {
        check_range(offset, length);
        auto* bytes = static_cast<unsigned char*>(destination);
        std::size_t received = 0;
        while (received < length) {
            if (fixture_io_) ++fixture_io_->reads;
            const ssize_t count = auxiliary("id table read", [&] {
                return file_.read_some(offset + received, bytes + received, length - received);
            });
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw IdPairStoreError(count < 0 ? errno : EIO, "id table read");
            if (fixture_io_) fixture_io_->read_bytes += static_cast<std::uint64_t>(count);
            received += static_cast<std::size_t>(count);
        }
    }
    void write(std::uint64_t offset, const void* source, std::size_t length) {
        check_range(offset, length);
        const auto* bytes = static_cast<const unsigned char*>(source);
        std::size_t written = 0;
        while (written < length) {
            if (fixture_io_ && ++fixture_io_->writes == fixture_io_->fail_write) {
                const auto prefix = std::min(fixture_io_->partial_write, length - written);
                if (prefix != 0) {
                    const auto count = auxiliary("id table fixture short write", [&] {
                        return file_.write_some(offset + written, bytes + written, prefix);
                    });
                    if (count > 0) fixture_io_->write_bytes += static_cast<std::uint64_t>(count);
                }
                throw IdPairStoreError(EIO, "id table fixture short write");
            }
            const ssize_t count = auxiliary("id table write", [&] {
                return file_.write_some(offset + written, bytes + written, length - written);
            });
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0)
                throw IdPairStoreError(count < 0 ? errno : EIO, "id table write");
            if (fixture_io_) fixture_io_->write_bytes += static_cast<std::uint64_t>(count);
            written += static_cast<std::size_t>(count);
        }
    }

private:
    template<class Operation>
    static auto auxiliary(const char* operation, Operation transfer) -> decltype(transfer()) {
        try { return transfer(); }
        catch (const AuxiliaryScratchIoError& error) {
            throw IdPairStoreError(error.code().value(), scratch_with_detail(operation, error.what(), error.code().value()).c_str());
        }
        // Arena backing/capacity failures retain their report-scratch type
        // and payload. Only auxiliary I/O becomes the historical ID error.
    }
    static void check_range(std::uint64_t offset, std::uint64_t length) {
        constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw IdPairStoreError(EFBIG, "id table offset exhausted");
    }
    ScratchFile file_{ScratchFile::IoRaiser::auxiliary};
    FixtureIO* fixture_io_ = nullptr;
};

// Adapter-owned auxiliary files have no external reader or reopen contract.
// Keep their logical bytes in a fixed window, writing a dirty victim BEFORE
// reuse. Reset/destruction discards the entire private unlinked file, including
// unreachable dirty bytes; it must not flush a position/run that has ended.
// The original immediate file above remains the standalone fault-test oracle.
class ResidentIdPairScratchFile {
public:
    static constexpr std::size_t kPageBytes = 512;
    static constexpr std::size_t kResidentPages = 8;

private:
    struct Page {
        std::array<unsigned char, kPageBytes> bytes{};
        std::uint64_t number = std::numeric_limits<std::uint64_t>::max();
        bool dirty = false;
    };
    struct Cache {
        std::array<Page, kResidentPages> pages{};
        std::uint64_t logical_end = 0;
        // Highest successfully written byte. Sparse/resized bytes beyond it
        // are known zeros, even if ftruncate already admitted a larger extent.
        std::uint64_t backed_end = 0;
        std::size_t next_victim = 0;
    };

public:
    explicit ResidentIdPairScratchFile(ScratchStore store = ScratchStore::unknown)
        : backing_(store) {}
    ResidentIdPairScratchFile(ResidentIdPairScratchFile&&) noexcept = default;
    ResidentIdPairScratchFile& operator=(ResidentIdPairScratchFile&&) noexcept = default;

    void set_store(ScratchStore store) noexcept { backing_.set_store(store); }
    bool opened() const noexcept { return backing_.opened(); }
    void fixture_count_io(IdPairScratchFile::FixtureIO* io) noexcept { backing_.fixture_count_io(io); }
    IdPairScratchFile::FixtureIO* fixture_io() const noexcept { return backing_.fixture_io(); }
    static constexpr std::size_t resident_cache_bytes() noexcept { return sizeof(Cache); }
    void enable_resident_io() {
        if (opened()) throw std::logic_error("id resident policy changed after open");
        resident_io_ = true;
    }
    bool resident_io_enabled() const noexcept { return resident_io_; }
    void open(const std::string& directory) {
        backing_.open(directory);
        // Arena/slab admission and checked copies retain their original path.
        if (resident_io_ && !backing_.arena_backed() && !cache_) cache_ = std::make_unique<Cache>();
    }
    void reset() noexcept {
        cache_.reset();
        backing_.reset();
    }
    void resize(std::uint64_t bytes) {
        backing_.resize(bytes); // Immediate extent/errno admission.
        if (cache_) cache_->logical_end = std::max(cache_->logical_end, bytes);
    }
    void read(std::uint64_t offset, void* destination, std::size_t length) const {
        if (!cache_) {
            backing_.read(offset, destination, length);
            return;
        }
        check_cached_range(offset, length);
        if (length == 0) return;
        if (offset > cache_->logical_end || length > cache_->logical_end - offset)
            throw IdPairStoreError(EIO, "id table read past logical end");
        auto* bytes = static_cast<unsigned char*>(destination);
        while (length != 0) {
            auto& page = load_page(offset / kPageBytes);
            const auto within = static_cast<std::size_t>(offset % kPageBytes);
            const auto count = std::min(length, kPageBytes - within);
            std::memcpy(bytes, page.bytes.data() + within, count);
            bytes += count;
            offset += count;
            length -= count;
        }
    }
    void write(std::uint64_t offset, const void* source, std::size_t length) {
        if (!cache_) {
            backing_.write(offset, source, length);
            return;
        }
        check_cached_range(offset, length);
        const auto* bytes = static_cast<const unsigned char*>(source);
        while (length != 0) {
            auto& page = load_page(offset / kPageBytes);
            const auto within = static_cast<std::size_t>(offset % kPageBytes);
            const auto count = std::min(length, kPageBytes - within);
            std::memcpy(page.bytes.data() + within, bytes, count);
            page.dirty = true;
            bytes += count;
            offset += count;
            length -= count;
            cache_->logical_end = std::max(cache_->logical_end, offset);
        }
    }

private:
    static void check_cached_range(std::uint64_t offset, std::size_t length) {
        const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
        if (offset > maximum || length > maximum - offset)
            throw IdPairStoreError(EFBIG, "id table offset exhausted");
    }
    Page& load_page(std::uint64_t number) const {
        // A fixed eight-row search is O(1) and uses every slot before
        // eviction. Distant page numbers cannot conflict below the budget.
        for (auto& candidate : cache_->pages)
            if (candidate.number == number) return candidate;
        auto& page = cache_->pages[cache_->next_victim];
        if (page.dirty) {
            const auto offset = page.number * kPageBytes;
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(kPageBytes, cache_->logical_end - offset));
            // A short/error write leaves the complete owned victim dirty.
            // This const read may evict; only physical backing state changes.
            backing_.write(offset, page.bytes.data(), count);
            cache_->backed_end = std::max(cache_->backed_end, offset + count);
            page.dirty = false;
        }
        // Do not replace the old page until a read has completely succeeded.
        std::array<unsigned char, kPageBytes> bytes{};
        const auto offset = number * kPageBytes;
        if (offset < cache_->backed_end) {
            const auto count = static_cast<std::size_t>(
                std::min<std::uint64_t>(kPageBytes, cache_->backed_end - offset));
            backing_.read(offset, bytes.data(), count);
        }
        page.bytes = bytes;
        page.number = number;
        page.dirty = false;
        cache_->next_victim = (cache_->next_victim + 1) % kResidentPages;
        return page;
    }
    mutable IdPairScratchFile backing_;
    std::unique_ptr<Cache> cache_;
    bool resident_io_ = false;
};

} // namespace pineforge::detail
