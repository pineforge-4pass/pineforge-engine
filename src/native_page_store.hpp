#pragma once

// CR-150 o1-e3 (rows E06-E09, E11, E12): the consumer's readback streams, its
// notification and birth lists and its matcher scratch are sequences whose
// length the run decides -- B driver points, E fills, J commands, Q working
// requests -- so none of them may be a resident vector. This is the storage
// they move to.
//
// PageStore is a fixed-resident, file-backed sequence of trivially copyable
// rows: a staging tail of a prepared, fixed row count, and an unlinked scratch
// file that every row that leaves the tail is written to. Resident memory is
// the tail (and the one-page copy erase_if borrows) whatever the sequence's
// length; the file grows with the rows. Nothing here refuses a push: unlike
// the run's prepared ScratchSlab, whose regions are sized from the manifest,
// this file has no prepared row count to exceed, so a long run pages instead
// of failing. An I/O fault surfaces as detail::ScratchIoError, the run-failure
// code the rest of the scratch storage already raises (RunFailureCode::
// scratch_io_failed), never as a capacity refusal.
//
// Logical indices are stable across pushes. erase_prefix retires the oldest
// rows in O(1); pop_back drops the newest; erase_if compacts the live rows
// with the one scratch page. Reads and writes copy through read_bytes /
// write_bytes, so a sequence never hands out a pointer into its storage.
//
// Like WorkingRequestCore::SuccessorRootIndex (native_order.cpp), the file is
// created with mkstemp, unlinked before its descriptor is published and opened
// close-on-exec; an opened scratch file has no directory entry. The staged
// tail is written in one batch only when it is full, so a short write leaves
// the tail authoritative and a retry overwrites the same offsets.

#include <pineforge/detail/scratch_errors.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <string>
#include <sys/types.h>
#include <unistd.h>

namespace pineforge::detail {

class PageStore {
public:
    // The copy erase_if borrows: one page's worth of rows, allocated only when
    // a compaction actually runs.
    static constexpr std::uint64_t kScratchBytes = 4096;

    PageStore(std::size_t row_bytes, std::size_t resident_rows, const char* label)
        : row_bytes_(row_bytes == 0 ? 1 : row_bytes),
          resident_capacity_(resident_rows == 0 ? 1 : resident_rows),
          label_(label == nullptr ? "native page store" : label),
          tail_(new unsigned char[static_cast<std::size_t>(row_bytes_ * resident_capacity_)]) {}
    ~PageStore() {
        if (fd_ >= 0) ::close(fd_);
    }
    PageStore(const PageStore&) = delete;
    PageStore& operator=(const PageStore&) = delete;
    PageStore(PageStore&&) = delete;
    PageStore& operator=(PageStore&&) = delete;

    std::size_t row_bytes() const noexcept { return row_bytes_; }
    std::size_t resident_capacity() const noexcept { return resident_capacity_; }
    // The rows a reader can see: everything not retired.
    std::uint64_t size() const noexcept {
        return disk_rows_ + tail_size_ - retired_;
    }
    bool empty() const noexcept { return size() == 0; }
    // Rows this store has written to its file, retired or not. Readback of the
    // storage, never of the sequence.
    std::uint64_t spilled_rows() const noexcept { return disk_rows_; }
    // The fixed scratch this store holds, whatever its length: the tail plus,
    // once a compaction ran, its page. Allocation metadata is not counted.
    std::uint64_t resident_bytes() const noexcept {
        return static_cast<std::uint64_t>(row_bytes_ * resident_capacity_)
            + (scratch_ ? static_cast<std::uint64_t>(row_bytes_) * rows_per_scratch() : 0);
    }
    // The physical row index of logical row 0. erase_prefix moves it.
    std::uint64_t retired() const noexcept { return retired_; }

    // Drop every row and every retired position, in O(1) and without I/O: the
    // next flush overwrites the file from offset 0. The file keeps its bytes.
    void clear() noexcept {
        tail_size_ = 0;
        disk_rows_ = 0;
        retired_ = 0;
    }

    void push_back(const void* row) {
        if (row == nullptr) throw ScratchIoError(EINVAL, label_);
        if (tail_size_ == resident_capacity_) flush_tail();
        std::memcpy(tail_.get()
                        + static_cast<std::size_t>(tail_size_ * row_bytes_),
                    row, row_bytes_);
        ++tail_size_;
    }

    // The newest row, for a FIFO's back(). The caller proves size() != 0.
    void back(void* row) const { read(size() - 1, row); }

    void read(std::uint64_t index, void* out) const {
        if (out == nullptr) throw ScratchIoError(EINVAL, label_);
        if (index >= size()) throw ScratchIoError(EIO, label_);
        read_physical(retired_ + index, out);
    }

    void write(std::uint64_t index, const void* row) {
        if (row == nullptr) throw ScratchIoError(EINVAL, label_);
        if (index >= size()) throw ScratchIoError(EIO, label_);
        write_physical(retired_ + index, row);
    }

    void pop_back() noexcept {
        if (tail_size_ != 0) {
            --tail_size_;
        } else if (disk_rows_ > retired_) {
            --disk_rows_;
        }
    }

    // Drop the oldest `count` rows. Storage only: the rows stay in the file
    // until a later push reuses their offsets.
    void erase_prefix(std::uint64_t count) noexcept {
        count = std::min(count, size());
        retired_ += count;
        if (retired_ > disk_rows_) {
            // The retired rows are the file's tail; drop them from the staged
            // tail and let the file's first live row be the staging's.
            const auto dropped = static_cast<std::size_t>(retired_ - disk_rows_);
            std::memmove(tail_.get(),
                         tail_.get() + dropped * row_bytes_,
                         static_cast<std::size_t>((tail_size_ - dropped) * row_bytes_));
            tail_size_ -= dropped;
            disk_rows_ = retired_;
        }
    }

    // Keep the live rows pred answers false for, in their order. The rows are
    // read and rewritten through one fixed page, and a survivor never moves
    // past a row not yet read, so the two cursors never cross.
    template <class Pred>
    void erase_if(Pred pred) {
        if (disk_rows_ == 0) {
            // The whole sequence is still staged: compact it in place, with no
            // file and no syscall. (disk_rows_ == 0 implies retired_ == 0.)
            std::uint64_t kept = 0;
            for (std::uint64_t index = 0; index < tail_size_; ++index) {
                auto* row = tail_.get() + static_cast<std::size_t>(index * row_bytes_);
                if (pred(static_cast<const void*>(row))) continue;
                if (kept != index) {
                    std::memmove(tail_.get() + static_cast<std::size_t>(kept * row_bytes_),
                                 row, row_bytes_);
                }
                ++kept;
            }
            tail_size_ = kept;
            return;
        }
        flush_tail();
        if (retired_ == disk_rows_) {
            tail_size_ = 0;
            return;
        }
        if (!scratch_) {
            scratch_.reset(new unsigned char[static_cast<std::size_t>(
                rows_per_scratch() * row_bytes_)]);
        }
        const std::uint64_t per_page = rows_per_scratch();
        std::uint64_t read_at = retired_;
        std::uint64_t write_at = retired_;
        while (read_at < disk_rows_) {
            const auto rows = std::min(per_page, disk_rows_ - read_at);
            read_bytes(read_at * row_bytes_, scratch_.get(),
                       static_cast<std::size_t>(rows * row_bytes_));
            std::uint64_t kept = 0;
            for (std::uint64_t i = 0; i < rows; ++i) {
                const auto* row = scratch_.get() + static_cast<std::size_t>(i * row_bytes_);
                if (pred(static_cast<const void*>(row))) continue;
                if (kept != i) {
                    std::memmove(scratch_.get() + static_cast<std::size_t>(kept * row_bytes_),
                                 row, row_bytes_);
                }
                ++kept;
            }
            if (kept != 0) {
                write_bytes(write_at * row_bytes_, scratch_.get(),
                            static_cast<std::size_t>(kept * row_bytes_));
            }
            read_at += rows;
            write_at += kept;
        }
        disk_rows_ = write_at;
        tail_size_ = 0;
    }

private:
    std::uint64_t rows_per_scratch() const noexcept {
        return std::max<std::uint64_t>(1, kScratchBytes / row_bytes_);
    }

    void read_physical(std::uint64_t physical, void* out) const {
        if (physical >= disk_rows_) {
            const std::uint64_t at = physical - disk_rows_;
            if (at >= tail_size_) throw ScratchIoError(EIO, label_);
            std::memcpy(out, tail_.get() + static_cast<std::size_t>(at * row_bytes_),
                        row_bytes_);
            return;
        }
        read_bytes(physical * row_bytes_, out, row_bytes_);
    }

    void write_physical(std::uint64_t physical, const void* row) {
        if (physical >= disk_rows_) {
            const std::uint64_t at = physical - disk_rows_;
            if (at >= tail_size_) throw ScratchIoError(EIO, label_);
            std::memcpy(tail_.get() + static_cast<std::size_t>(at * row_bytes_),
                        row, row_bytes_);
            return;
        }
        write_bytes(physical * row_bytes_, row, row_bytes_);
    }

    // Publish the staging tail as the file's next rows, in one batch.
    void flush_tail() {
        if (tail_size_ == 0) return;
        ensure_file();
        const std::uint64_t maximum_rows =
            static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()) / row_bytes_;
        if (disk_rows_ > maximum_rows - tail_size_) {
            throw ScratchIoError(EFBIG, label_);
        }
        write_bytes(disk_rows_ * row_bytes_, tail_.get(),
                    static_cast<std::size_t>(tail_size_ * row_bytes_));
        disk_rows_ += tail_size_;
        tail_size_ = 0;
    }

    void ensure_file() {
        if (fd_ >= 0) return;
        const char* temporary = std::getenv("TMPDIR");
        std::string name = temporary && *temporary ? temporary : "/tmp";
        if (name.back() != '/') name += '/';
        name += "pineforge-pages-XXXXXX";
        const int opened = ::mkstemp(name.data());
        if (opened < 0) throw ScratchIoError(errno, label_);
        if (::unlink(name.c_str()) != 0) {
            const int error = errno;
            ::close(opened);
            throw ScratchIoError(error, label_);
        }
        if (::fcntl(opened, F_SETFD, FD_CLOEXEC) == -1) {
            const int error = errno;
            ::close(opened);
            throw ScratchIoError(error, label_);
        }
        fd_ = opened;
    }

    void read_bytes(std::uint64_t offset, void* out, std::size_t length) const {
        auto* bytes = static_cast<unsigned char*>(out);
        std::size_t received = 0;
        while (received < length) {
            const ssize_t count = ::pread(fd_, bytes + received, length - received,
                                          static_cast<off_t>(offset + received));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, label_);
            received += static_cast<std::size_t>(count);
        }
    }

    void write_bytes(std::uint64_t offset, const void* in, std::size_t length) {
        const auto* bytes = static_cast<const unsigned char*>(in);
        std::size_t written = 0;
        while (written < length) {
            const ssize_t count = ::pwrite(fd_, bytes + written, length - written,
                                           static_cast<off_t>(offset + written));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw ScratchIoError(count < 0 ? errno : EIO, label_);
            written += static_cast<std::size_t>(count);
        }
    }

    std::size_t row_bytes_;
    std::size_t resident_capacity_;
    const char* label_;
    // The staging tail: physical rows [disk_rows_, disk_rows_ + tail_size_).
    std::unique_ptr<unsigned char[]> tail_;
    // erase_if's one page, allocated at the first compaction only.
    std::unique_ptr<unsigned char[]> scratch_;
    std::uint64_t tail_size_ = 0;
    // The physical index of the tail's first row: every row below it is in the
    // file. Retired rows at the file's head may remain; they are dead.
    std::uint64_t disk_rows_ = 0;
    std::uint64_t retired_ = 0;
    int fd_ = -1;
};

}  // namespace pineforge::detail
