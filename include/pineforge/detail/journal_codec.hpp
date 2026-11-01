#pragma once

#include <pineforge/detail/scratch_store.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>

namespace pineforge::detail::journal_codec {

enum class WorkOperation { payload_source, payload_read, encoder_word, writer_status };
struct WorkObserver {
    void* context;
    void (*observe)(void*, WorkOperation, std::uint64_t) noexcept;
};

// Borrowed registration on the calling thread. A nonnull observer must have a
// valid callback and outlive its registration; restore the returned pointer.
// payload_source/read cover stored and admission-encoded sources alike.
// payload_read reports requested bytes; the other operations report zero.
inline thread_local const WorkObserver* work_observer = nullptr;
inline const WorkObserver* set_work_observer(const WorkObserver* observer) noexcept {
    const auto previous = work_observer;
    work_observer = observer;
    return previous;
}
inline void observe_work(WorkOperation operation, std::uint64_t bytes = 0) noexcept {
    if (work_observer)
        work_observer->observe(work_observer->context, operation, bytes);
}

// A counting/slicing sink. Each read spells intersecting bytes directly into
// the caller's destination, without constructing a contiguous record.
struct Writer {
    std::uint64_t position = 0;
    std::uint64_t offset = 0;
    ScratchBuffer destination{};
    ScratchStatus status = ScratchStatus::ok;

    // Explicit caller status reads, excluding the internal bytes() guard.
    ScratchStatus checked_status() const noexcept {
        observe_work(WorkOperation::writer_status);
        return status;
    }
    void bytes(const void* data, std::uint64_t length) noexcept {
        if (status != ScratchStatus::ok) return;
        if (length > std::numeric_limits<std::uint64_t>::max() - position) {
            status = ScratchStatus::programming_error;
            return;
        }
        const auto end = position + length;
        const auto begin = std::max(position, offset);
        const auto finish = std::min(end, offset + destination.size);
        if (begin < finish)
            std::memcpy(destination.data + static_cast<std::size_t>(begin - offset),
                        static_cast<const std::byte*>(data)
                            + static_cast<std::size_t>(begin - position),
                        static_cast<std::size_t>(finish - begin));
        position = end;
    }
    void word(std::uint64_t value) noexcept {
        observe_work(WorkOperation::encoder_word);
        std::array<std::byte, 8> bytes{};
        for (unsigned i = 0; i != 8; ++i)
            bytes[i] = static_cast<std::byte>(value >> (8 * i));
        this->bytes(bytes.data(), bytes.size());
    }
    void text(const std::string& value) noexcept {
        word(value.size());
        bytes(value.data(), value.size());
    }
};

struct Reader {
    const ScratchByteSource& source;
    ScratchBuffer page;
    std::uint64_t position = 0;
    ScratchStatus status = ScratchStatus::ok;
    std::uint64_t cached_at = 0;
    std::size_t cached_size = 0;

    std::uint64_t remaining() const noexcept {
        return position <= source.size() ? source.size() - position : 0;
    }
    void bytes(void* destination, std::size_t length) noexcept {
        if (status != ScratchStatus::ok) return;
        if (!page.data || !page.size) {
            status = ScratchStatus::programming_error;
            return;
        }
        if (length > remaining()) { status = ScratchStatus::corrupt; return; }
        auto* out = static_cast<std::byte*>(destination);
        while (length) {
            if (!cached_size || position < cached_at
                || position - cached_at >= cached_size) {
                cached_at = position;
                cached_size = static_cast<std::size_t>(
                    std::min<std::uint64_t>(page.size, remaining()));
                status = source.read(position, {page.data, cached_size});
                if (status != ScratchStatus::ok) return;
            }
            const auto at = static_cast<std::size_t>(position - cached_at);
            const auto count = std::min(length, cached_size - at);
            std::memcpy(out, page.data + at, count);
            out += count;
            position += count;
            length -= count;
        }
    }
    void skip(std::uint64_t length) noexcept {
        if (status != ScratchStatus::ok) return;
        if (length > remaining()) { status = ScratchStatus::corrupt; return; }
        position += length;
    }
    std::uint64_t word() noexcept {
        std::array<std::byte, 8> bytes{};
        this->bytes(bytes.data(), bytes.size());
        std::uint64_t value = 0;
        for (unsigned i = 0; i != 8; ++i)
            value |= std::uint64_t(std::to_integer<unsigned>(bytes[i])) << (8 * i);
        return value;
    }
    bool presence() noexcept {
        std::byte value{};
        bytes(&value, 1);
        if (value != std::byte{0} && value != std::byte{1})
            status = ScratchStatus::corrupt;
        return value == std::byte{1};
    }
    template<class T> void integer(T& out) noexcept {
        const auto value = word();
        if constexpr (std::is_signed_v<T>) {
            const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<T>::max());
            const auto minimum = static_cast<std::uint64_t>(std::numeric_limits<T>::min());
            if (value > maximum && value < minimum) {
                status = ScratchStatus::corrupt;
                return;
            }
            std::int64_t signed_value = 0;
            std::memcpy(&signed_value, &value, sizeof value);
            out = static_cast<T>(signed_value);
        } else {
            if (value > std::numeric_limits<T>::max()) {
                status = ScratchStatus::corrupt;
                return;
            }
            out = static_cast<T>(value);
        }
    }
    void real(double& out) noexcept {
        const auto value = word();
        std::memcpy(&out, &value, sizeof value);
    }
    void text(std::string& out, bool owning) {
        const auto length = word();
        if (status != ScratchStatus::ok) return;
        if (length > remaining() || length > out.max_size()) {
            status = ScratchStatus::corrupt;
            return;
        }
        if (!owning) { skip(length); return; }
        out.resize(static_cast<std::size_t>(length));
        bytes(out.data(), out.size());
    }
    ScratchStatus finish() const noexcept {
        if (status != ScratchStatus::ok) return status;
        return position == source.size() ? ScratchStatus::ok : ScratchStatus::corrupt;
    }
};

class WordSource final : public ScratchByteSource {
public:
    explicit WordSource(std::uint64_t value) noexcept {
        for (unsigned i = 0; i != 8; ++i)
            bytes_[i] = static_cast<std::byte>(value >> (8 * i));
    }
    std::uint64_t size() const noexcept override { return bytes_.size(); }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        return ScratchSpanSource({bytes_.data(), bytes_.size()}).read(offset, out);
    }
private:
    std::array<std::byte, 8> bytes_{};
};

class StoreSource final : public ScratchByteSource {
public:
    StoreSource(const ScratchReadback& store, ScratchHandle handle,
                std::uint64_t length) noexcept : store_(store), handle_(handle), length_(length) {
        observe_work(WorkOperation::payload_source);
    }
    std::uint64_t size() const noexcept override { return length_; }
    const ReportScratchIoError* carried_failure() const noexcept override {
        return store_.carried_failure();
    }
    ScratchStatus read(std::uint64_t offset, ScratchBuffer out) const noexcept override {
        observe_work(WorkOperation::payload_read, out.size);
        return store_.read(handle_, ScratchField::value, offset, out);
    }
private:
    const ScratchReadback& store_;
    ScratchHandle handle_;
    std::uint64_t length_;
};

inline ScratchStatus find(CommandScratchStore& store, std::uint64_t key,
                          ScratchRecordInfo& out) noexcept {
    const WordSource probe(key);
    ScratchHandle handle = scratch_no_handle;
    auto status = store.lower_bound(probe, handle);
    if (status == ScratchStatus::end) return ScratchStatus::not_found;
    if (status != ScratchStatus::ok) return status;
    std::array<std::byte, 8> bytes{}, expected{};
    status = store.read(handle, ScratchField::key, 0, {bytes.data(), bytes.size()});
    if (status != ScratchStatus::ok) return status;
    probe.read(0, {expected.data(), expected.size()});
    if (bytes != expected) return ScratchStatus::not_found;
    return store.point(handle, out);
}

inline ScratchStatus finish(CommandScratchStore& store, ScratchStatus status) noexcept {
    if (status == ScratchStatus::ok)
        status = store.commit(ScratchDurability::buffered);
    if (status != ScratchStatus::ok && store.transaction_active()) {
        const auto rollback = store.rollback();
        if (rollback != ScratchStatus::ok) return rollback;
    }
    return status;
}
inline ScratchStatus put(CommandScratchStore& store, std::uint64_t key,
                         const ScratchByteSource& value) noexcept {
    auto status = store.begin();
    if (status != ScratchStatus::ok) return status;
    ScratchHandle handle = scratch_no_handle;
    const WordSource probe(key);
    return finish(store, store.put(probe, value, handle));
}
inline ScratchStatus clear(CommandScratchStore& store) noexcept {
    auto status = store.begin();
    if (status != ScratchStatus::ok) return status;
    ScratchHandle handle = scratch_no_handle;
    while ((status = store.first(handle)) == ScratchStatus::ok) {
        status = store.erase(handle);
        if (status != ScratchStatus::ok) break;
    }
    if (status == ScratchStatus::end) status = ScratchStatus::ok;
    return finish(store, status);
}

} // namespace pineforge::detail::journal_codec
