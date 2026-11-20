// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "json.hpp"
#include "report_delta.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <unistd.h>

namespace pineforge::live {

// Receives a serialized report in order, so that no caller has to hold the
// whole text. write_file hands over a byte range of an open regular file; the
// default copies it through one fixed buffer.
class ReportByteSink {
public:
    virtual ~ReportByteSink() = default;
    virtual void write(std::string_view bytes) = 0;
    virtual void write_file(int descriptor, std::uint64_t offset, std::uint64_t count) {
        char buffer[65536];
        while (count) {
            const auto wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(count, sizeof buffer));
            const auto got = ::pread(descriptor, buffer, wanted, static_cast<off_t>(offset));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) throw std::runtime_error("cumulative report spool read failed");
            write(std::string_view(buffer, static_cast<std::size_t>(got)));
            offset += static_cast<std::uint64_t>(got);
            count -= static_cast<std::uint64_t>(got);
        }
    }
};

class ReportStringSink final : public ReportByteSink {
public:
    std::string text;
    void write(std::string_view bytes) override { text.append(bytes); }
};

class ReportCountingSink final : public ReportByteSink {
public:
    std::uint64_t bytes = 0;
    void write(std::string_view text) override { bytes += text.size(); }
    void write_file(int, std::uint64_t, std::uint64_t count) override { bytes += count; }
};

// Called at most once per process, on the first spool file that cannot be
// created or written, with the cause; the spool then keeps its bytes in memory.
using ReportSpoolFailureHandler = void (*)(const std::string& cause);
inline std::atomic<ReportSpoolFailureHandler>& report_spool_failure_handler() noexcept {
    static std::atomic<ReportSpoolFailureHandler> handler{nullptr};
    return handler;
}
// Every spool that fell back to memory, counted (the handler runs only once).
inline std::atomic<std::uint64_t>& report_spool_fallbacks() noexcept {
    static std::atomic<std::uint64_t> count{0};
    return count;
}
// A test seam, 0 (off) by default: the largest number of bytes one spool file
// may hold, on top of the process's own per-file size limit (see below).
inline std::atomic<std::uint64_t>& report_spool_file_limit() noexcept {
    static std::atomic<std::uint64_t> limit{0};
    return limit;
}
// The largest size, in bytes, a spool file may reach: the smaller of the test
// seam's cap (when non-zero) and the process's soft RLIMIT_FSIZE (when not
// infinite; a soft limit of 0 is a limit of 0). The maximum value means no
// limit. A spool never writes past it, so it never raises SIGXFSZ: the write
// lands up to the limit and then fails with EFBIG.
inline std::uint64_t report_spool_write_limit() noexcept {
    auto result = std::numeric_limits<std::uint64_t>::max();
    const auto seam = report_spool_file_limit().load();
    if (seam != 0) result = seam;
#if defined(RLIMIT_FSIZE)
    struct rlimit limit {};
    if (::getrlimit(RLIMIT_FSIZE, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        result = std::min<std::uint64_t>(result, static_cast<std::uint64_t>(limit.rlim_cur));
#endif
    return result;
}
// Set once the cause was handed to the handler (or a caller warned itself).
inline std::atomic<bool>& report_spool_failure_reported() noexcept {
    static std::atomic<bool> reported{false};
    return reported;
}
inline void report_spool_failed(const std::string& cause) {
    report_spool_fallbacks().fetch_add(1);
    if (report_spool_failure_reported().exchange(true)) return;
    if (const auto handler = report_spool_failure_handler().load()) handler(cause);
}

// Bytes a report has settled. They are only ever appended: a byte once written
// keeps its value, so a range already handed to the kernel for a descriptor
// (write_file) can never change under its reader. With a directory the bytes
// live in an anonymous file there and the process keeps one fixed write
// buffer; with no directory they stay in a string, for a reader that holds a
// small report in memory.
// A spool never writes past the process's per-file size limit or the test
// seam's cap (report_spool_write_limit); the refusal is reported as EFBIG.
// A write that fails (a full disk, a file-size limit) loses nothing: the
// bytes from the first one that did not land stay in memory from then on,
// behind the file's bytes at the same offsets, and report_spool_failed is
// called. Bytes [0, written_) are in the file, [written_, size_) in pending_.
class ReportSpool {
public:
    explicit ReportSpool(const std::string& directory) {
        if (directory.empty()) return;
#ifdef O_TMPFILE
        do {
            descriptor_ = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
        } while (descriptor_ < 0 && errno == EINTR);
#endif
        if (descriptor_ < 0) {
            // A filesystem without unnamed files: create a name and drop it at once.
            std::string name = directory + "/.pineforge-report-spool-XXXXXX";
            descriptor_ = ::mkstemp(name.data());
            if (descriptor_ < 0)
                throw std::runtime_error("cumulative report spool cannot be created");
            if (::unlink(name.c_str()) != 0) {
                ::close(descriptor_);
                (void)::unlink(name.c_str()); // Best effort; no report bytes have been written.
                throw std::runtime_error("cumulative report spool cannot be created");
            }
            const int flags = ::fcntl(descriptor_, F_GETFD);
            if (flags >= 0) ::fcntl(descriptor_, F_SETFD, flags | FD_CLOEXEC);
        }
        file_ = true;
    }
    ~ReportSpool() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
    ReportSpool(const ReportSpool&) = delete;
    ReportSpool& operator=(const ReportSpool&) = delete;

    std::uint64_t size() const noexcept { return size_; }

    void append(std::string_view bytes) {
        if (!file_) {
            memory_.append(bytes);
        } else if (in_memory_) {
            pending_.append(bytes);
        } else {
            if (pending_.size() + bytes.size() > kPendingBytes) flush();
            if (in_memory_ || bytes.size() <= kPendingBytes) {
                pending_.append(bytes);
            } else {
                // pending_ is empty here (flushed above): what does not land stays in it.
                const auto landed = put(bytes);
                if (landed < bytes.size()) pending_.append(bytes.substr(landed));
            }
        }
        size_ += bytes.size();
    }

    void read(std::uint64_t offset, char* output, std::size_t count) const {
        check_range(offset, count);
        if (!file_) {
            if (count) std::memcpy(output, memory_.data() + offset, count);
            return;
        }
        flush();
        while (count && offset < written_) {
            const auto wanted = static_cast<std::size_t>(
                std::min<std::uint64_t>(count, written_ - offset));
            const auto got = ::pread(descriptor_, output, wanted, static_cast<off_t>(offset));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) throw std::runtime_error("cumulative report spool read failed");
            output += got;
            offset += static_cast<std::uint64_t>(got);
            count -= static_cast<std::size_t>(got);
        }
        if (count)
            std::memcpy(output, pending_.data() + static_cast<std::size_t>(offset - written_), count);
    }

    void copy_to(ReportByteSink& sink, std::uint64_t offset, std::uint64_t count) const {
        check_range(offset, count);
        if (!count) return;
        if (!file_) {
            sink.write(std::string_view(memory_).substr(
                static_cast<std::size_t>(offset), static_cast<std::size_t>(count)));
            return;
        }
        flush();
        if (offset < written_) {
            const auto in_file = std::min<std::uint64_t>(count, written_ - offset);
            sink.write_file(descriptor_, offset, in_file);
            offset += in_file;
            count -= in_file;
        }
        if (count)
            sink.write(std::string_view(pending_).substr(
                static_cast<std::size_t>(offset - written_), static_cast<std::size_t>(count)));
    }

private:
    static constexpr std::size_t kPendingBytes = 64 * 1024;

    void check_range(std::uint64_t offset, std::uint64_t count) const {
        if (offset > size_ || count > size_ - offset)
            throw std::logic_error("cumulative report spool range is outside its bytes");
    }
    // After a failed write pending_ is the memory copy of every byte past the
    // file, and nothing is written again.
    void flush() const {
        if (in_memory_ || pending_.empty()) return;
        const auto landed = put(pending_);
        pending_.erase(0, landed);
    }
    // Writes `bytes` at written_ and returns how many landed. On the first
    // failure (including a partial write) it switches to memory and reports.
    std::size_t put(std::string_view bytes) const {
        std::size_t landed = 0;
        const auto limit = report_spool_write_limit();
        while (landed < bytes.size()) {
            auto wanted = bytes.size() - landed;
            if (limit != std::numeric_limits<std::uint64_t>::max()) {
                if (written_ >= limit) return fail(landed, EFBIG);
                wanted = static_cast<std::size_t>(std::min<std::uint64_t>(wanted, limit - written_));
            }
            const auto wrote = ::pwrite(descriptor_, bytes.data() + landed, wanted,
                                        static_cast<off_t>(written_));
            if (wrote < 0 && errno == EINTR) continue;
            if (wrote <= 0) return fail(landed, wrote < 0 ? errno : 0);
            landed += static_cast<std::size_t>(wrote);
            written_ += static_cast<std::uint64_t>(wrote);
        }
        return landed;
    }
    std::size_t fail(std::size_t landed, int error_number) const {
        in_memory_ = true;
        report_spool_failed(std::string("cumulative report spool write failed: ") +
                            (error_number ? std::strerror(error_number) : "no progress"));
        return landed;
    }

    int descriptor_ = -1;
    bool file_ = false;
    std::uint64_t size_ = 0;
    std::string memory_;
    // Appended bytes not yet in the file; every read and copy flushes them.
    // After a failed write, every byte past the file, kept for good.
    mutable std::string pending_;
    mutable std::uint64_t written_ = 0;
    mutable bool in_memory_ = false;
};

// A spool in `directory`, or, when no file can be created there, one that
// keeps its bytes in memory (reported once through report_spool_failed).
inline std::unique_ptr<ReportSpool> make_report_spool(const std::string& directory) {
    try {
        return std::make_unique<ReportSpool>(directory);
    } catch (const std::runtime_error& error) {
        report_spool_failed(error.what());
        return std::make_unique<ReportSpool>(std::string{});
    }
}

class ReportSpoolSink final : public ReportByteSink {
public:
    explicit ReportSpoolSink(ReportSpool& spool) : spool_(spool) {}
    void write(std::string_view bytes) override { spool_.append(bytes); }

private:
    ReportSpool& spool_;
};

// One array of a cumulative report. A patch rewrites the array from an offset,
// so every item below the newest patch offset is settled: its serialized text
// leaves the process and only the items of that newest patch stay here.
class SpooledReportArray {
public:
    explicit SpooledReportArray(std::string directory) : directory_(std::move(directory)) {}
    SpooledReportArray(const SpooledReportArray&) = delete;
    SpooledReportArray& operator=(const SpooledReportArray&) = delete;

    std::size_t size() const noexcept { return count_; }

    std::string item(std::size_t index) const {
        if (index >= count_) throw std::out_of_range("cumulative report array index");
        if (index >= settled_) return tail_[index - settled_];
        const auto begin = index ? end_of(index - 1) + 1 : 0;
        const auto end = end_of(index);
        std::string text(static_cast<std::size_t>(end - begin), '\0');
        data_->read(begin, text.data(), text.size());
        return text;
    }

    // Truncate to `offset` items, then append `items`: the array semantics of
    // one cumulative delta patch.
    void patch(std::size_t offset, std::vector<std::string> items) {
        if (offset > count_) throw std::runtime_error("invalid cumulative report delta offset");
        if (offset < settled_) {
            // A patch below the settled items: keep their prefix in new spools.
            // The old bytes are never rewritten in place (see ReportSpool).
            // A spool in memory mode copies its memory bytes the same way.
            auto data = make_report_spool(directory_);
            auto index = make_report_spool(directory_);
            ReportSpoolSink data_sink(*data), index_sink(*index);
            data_->copy_to(data_sink, 0, offset ? end_of(offset - 1) : 0);
            index_->copy_to(index_sink, 0, sizeof(std::uint64_t) * static_cast<std::uint64_t>(offset));
            data_ = std::move(data);
            index_ = std::move(index);
            settled_ = offset;
            tail_.clear();
        } else {
            tail_.resize(offset - settled_);
        }
        count_ = offset + items.size();
        tail_.insert(tail_.end(), std::make_move_iterator(items.begin()),
                     std::make_move_iterator(items.end()));
        settle(offset);
    }

    // Every item of [first, first + count) in order, as its serialized text.
    template<class Visitor>
    void visit(std::size_t first, std::size_t count, Visitor&& visitor) const {
        if (first > count_ || count > count_ - first)
            throw std::out_of_range("cumulative report array range");
        const auto last = first + count;
        auto index = first;
        std::vector<std::uint64_t> ends;
        std::string bytes;
        while (index < last && index < settled_) {
            auto batch = std::min<std::size_t>(std::min(last, settled_) - index, kVisitItems);
            const auto begin = index ? end_of(index - 1) + 1 : 0;
            ends.resize(batch);
            index_->read(sizeof(std::uint64_t) * static_cast<std::uint64_t>(index),
                         reinterpret_cast<char*>(ends.data()), sizeof(std::uint64_t) * batch);
            while (batch > 1 && ends[batch - 1] - begin > kVisitBytes) --batch;
            bytes.resize(static_cast<std::size_t>(ends[batch - 1] - begin));
            data_->read(begin, bytes.data(), bytes.size());
            auto at = begin;
            for (std::size_t item = 0; item < batch; ++item) {
                visitor(std::string_view(bytes).substr(
                    static_cast<std::size_t>(at - begin), static_cast<std::size_t>(ends[item] - at)));
                at = ends[item] + 1;
            }
            index += batch;
        }
        for (; index < last; ++index) visitor(std::string_view(tail_[index - settled_]));
    }

    void write(ReportByteSink& sink) const {
        sink.write("[");
        if (settled_) data_->copy_to(sink, 0, data_->size());
        for (std::size_t item = 0; item < tail_.size(); ++item) {
            if (settled_ + item) sink.write(",");
            sink.write(tail_[item]);
        }
        sink.write("]");
    }

private:
    static constexpr std::size_t kVisitItems = 2048;
    static constexpr std::uint64_t kVisitBytes = 256 * 1024;

    std::uint64_t end_of(std::size_t index) const {
        std::uint64_t end = 0;
        index_->read(sizeof end * static_cast<std::uint64_t>(index),
                     reinterpret_cast<char*>(&end), sizeof end);
        return end;
    }
    // The spool holds settled items joined by commas, and one end offset per item.
    void settle(std::size_t upto) {
        if (upto <= settled_) return;
        if (!data_) {
            // Both or neither: a failed second file must not leave the first.
            auto data = make_report_spool(directory_);
            index_ = make_report_spool(directory_);
            data_ = std::move(data);
        }
        const auto moved = upto - settled_;
        for (std::size_t item = 0; item < moved; ++item) {
            if (settled_ + item) data_->append(",");
            data_->append(tail_[item]);
            const std::uint64_t end = data_->size();
            index_->append(std::string_view(reinterpret_cast<const char*>(&end), sizeof end));
        }
        tail_.erase(tail_.begin(), tail_.begin() + static_cast<std::ptrdiff_t>(moved));
        settled_ = upto;
    }

    std::string directory_;
    std::size_t count_ = 0;
    std::size_t settled_ = 0;
    std::vector<std::string> tail_;
    std::unique_ptr<ReportSpool> data_;
    std::unique_ptr<ReportSpool> index_;
};

// A cumulative report document whose arrays are spooled: the scalar members
// stay a small tree, closed_trades and the arrays of "report" are
// SpooledReportArray. It applies the same deltas as apply_report_delta and
// writes the same bytes as that document's Json::dump().
class SpooledReportDocument {
public:
    explicit SpooledReportDocument(std::string directory = {}, Json scalars = {})
        : directory_(std::move(directory)), scalars_(std::move(scalars)) {}
    SpooledReportDocument(const SpooledReportDocument&) = delete;
    SpooledReportDocument& operator=(const SpooledReportDocument&) = delete;

    // The document without its spooled arrays.
    Json& scalars() noexcept { return scalars_; }
    const Json& scalars() const noexcept { return scalars_; }

    const SpooledReportArray* array(const std::string& name) const {
        const auto& arrays = name == "closed_trades" ? root_arrays_ : report_arrays_;
        const auto found = arrays.find(name);
        return found == arrays.end() ? nullptr : &found->second;
    }

    // A stored row that is a whole document replaces everything.
    void assign(Json document) {
        scalars_ = std::move(document);
        root_arrays_.clear();
        report_arrays_.clear();
    }

    void apply(const Json& delta) {
        const bool v2 = apply_report_delta_fields(scalars_, delta);
        refuse_replaced_arrays();
        for (const auto& [name, patch] : delta.at("arrays").members) {
            check_report_array_patch(v2, name, patch);
            const bool root = name == "closed_trades";
            auto& holder = root ? scalars_ : scalars_.members["report"];
            holder.kind = Json::Kind::Object;
            auto& arrays = root ? root_arrays_ : report_arrays_;
            auto found = arrays.find(name);
            if (found == arrays.end()) {
                found = arrays.try_emplace(name, directory_).first;
                // A whole-document row leaves its arrays in the scalar tree.
                // The first patch of one moves it here, with the same items.
                const auto member = holder.members.find(name);
                if (member != holder.members.end()) {
                    std::vector<std::string> items;
                    items.reserve(member->second.items.size());
                    for (const auto& item : member->second.items) items.push_back(item.dump());
                    holder.members.erase(member);
                    found->second.patch(0, std::move(items));
                }
            }
            const auto offset = patch.at("offset").integer<std::size_t>();
            if (offset > found->second.size() || patch.at("items").kind != Json::Kind::Array)
                throw std::runtime_error("invalid cumulative report delta offset");
            std::vector<std::string> items;
            items.reserve(patch.at("items").items.size());
            for (const auto& item : patch.at("items").items) items.push_back(item.dump());
            found->second.patch(offset, std::move(items));
        }
    }

    void write(ReportByteSink& sink) const {
        if (scalars_.kind != Json::Kind::Object) {
            sink.write(scalars_.dump());
            return;
        }
        write_members(sink, scalars_, root_arrays_, true);
    }

    std::string json() const {
        ReportStringSink sink;
        write(sink);
        return std::move(sink.text);
    }

    std::uint64_t serialized_bytes() const {
        ReportCountingSink sink;
        write(sink);
        return sink.bytes;
    }

private:
    using Arrays = std::map<std::string, SpooledReportArray>;

    // Fields never name an array in a delta this runner writes. One that did
    // would replace the array inside the tree; refuse it instead of guessing.
    void refuse_replaced_arrays() const {
        if (root_arrays_.empty() && report_arrays_.empty()) return;
        bool replaced = scalars_.kind != Json::Kind::Object;
        for (auto name = root_arrays_.begin(); !replaced && name != root_arrays_.end(); ++name)
            replaced = scalars_.members.count(name->first) != 0;
        if (!replaced && !report_arrays_.empty()) {
            const auto report = scalars_.members.find("report");
            replaced = report == scalars_.members.end() || report->second.kind != Json::Kind::Object;
            for (auto name = report_arrays_.begin(); !replaced && name != report_arrays_.end(); ++name)
                replaced = report->second.members.count(name->first) != 0;
        }
        if (replaced)
            throw std::runtime_error("unsupported cumulative report delta: fields replace an array");
    }

    // Json::dump() of an object whose members are `node`'s and `arrays`, in key order.
    void write_members(ReportByteSink& sink, const Json& node, const Arrays& arrays, bool root) const {
        sink.write("{");
        bool first = true;
        const auto key = [&](const std::string& name) {
            if (!first) sink.write(",");
            first = false;
            sink.write(Json::quote(name));
            sink.write(":");
        };
        auto array = arrays.begin();
        for (const auto& [name, value] : node.members) {
            for (; array != arrays.end() && array->first < name; ++array) {
                key(array->first);
                array->second.write(sink);
            }
            if (array != arrays.end() && array->first == name)
                throw std::logic_error("cumulative report array is also a scalar member");
            key(name);
            if (root && name == "report" && value.kind == Json::Kind::Object)
                write_members(sink, value, report_arrays_, false);
            else
                sink.write(value.dump());
        }
        for (; array != arrays.end(); ++array) {
            key(array->first);
            array->second.write(sink);
        }
        sink.write("}");
    }

    std::string directory_;
    Json scalars_;
    Arrays root_arrays_;
    Arrays report_arrays_;
};

} // namespace pineforge::live
