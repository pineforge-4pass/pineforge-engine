#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    // State is incomplete in the installed headers; pin the chunk/block relation.
    static constexpr std::uint64_t kZeroBlock = ScratchArena::kChunkBytes / 256;
    static_assert(ScratchArena::kChunkBytes / 256 == 4096);
    static void retire_history(TradeLedger& ledger) {
        ledger.set_history_retirement(true);
    }
    static std::array<std::uint64_t, 2> bytes(const TradeLedger& ledger) {
        return ledger.scratch_storage_bytes();
    }
};
} // namespace pineforge::detail

namespace {
using pineforge::Trade;
using pineforge::TradeLedger;
using Count = std::uint64_t;
constexpr Count kZeroBlock = pineforge::detail::TradeLedgerStorageProbe::kZeroBlock;

struct FileIo {
    int fd = -1;
    dev_t device = 0;
    ino_t inode = 0;
    off_t first_zero_offset = -1;
    Count successful_zero_writes = 0;
};

struct IoState {
    std::array<FileIo, 2> files{};
    bool classification_error = false;
    bool fail_zero = false;
    bool failed = false;
    off_t failed_offset = -1;
    std::size_t failed_bytes = 0;
    off_t last_zero_offset = -1;
    std::size_t last_zero_bytes = 0;
    Count successful_zero_writes = 0;
} io;

FileIo* identify(int fd) noexcept {
    char path[64]{}, name[4096]{};
    struct stat status{};
    const auto length = std::snprintf(path, sizeof path, "/proc/self/fd/%d", fd);
    if (length <= 0 || static_cast<std::size_t>(length) >= sizeof path
        || ::fstat(fd, &status) != 0) {
        io.classification_error = true;
        return nullptr;
    }
    const auto used = ::readlink(path, name, sizeof name - 1);
    if (used < 0 || static_cast<std::size_t>(used) == sizeof name - 1) {
        io.classification_error = true;
        return nullptr;
    }
    name[used] = '\0';
    const char* slash = std::strrchr(name, '/');
    const char* base = slash ? slash + 1 : name;
    const std::array<const char*, 2> prefixes{
        "pineforge-trade-index", "pineforge-trade-payload"};
    for (std::size_t i = 0; i < prefixes.size(); ++i) {
        const auto bytes = std::strlen(prefixes[i]);
        if (std::strncmp(base, prefixes[i], bytes) != 0 || base[bytes] != '-') continue;
        if (!S_ISREG(status.st_mode)) {
            io.classification_error = true;
            return nullptr;
        }
        auto& file = io.files[i];
        if (file.fd != fd || file.device != status.st_dev || file.inode != status.st_ino)
            file = {};
        file.fd = fd;
        file.device = status.st_dev;
        file.inode = status.st_ino;
        return &file;
    }
    return nullptr;
}

bool all_zero(const void* data, std::size_t bytes) {
    const auto* first = static_cast<const unsigned char*>(data);
    return std::all_of(first, first + bytes, [](unsigned char value) { return value == 0; });
}

Trade row(int index, bool wide = false) {
    Trade value{};
    value.entry_time = 1704067200000LL + index * 60000LL;
    value.exit_time = value.entry_time + 60000;
    value.entry_price = 100.0 + index;
    value.exit_price = 101.0 + index;
    value.qty = 1.0;
    value.pnl = index + 0.5;
    value.pnl_pct = value.pnl / value.entry_price;
    value.entry_bar_index = index;
    value.exit_bar_index = index + 1;
    value.entry_id = "entry-" + std::to_string(index);
    value.exit_id = "exit-" + std::to_string(index);
    value.entry_incarnation = static_cast<std::uint64_t>(index + 1);
    // Sixteen rows of padding fill one zero block; 29 retired rows exceed it.
    if (wide) value.entry_comment.assign(kZeroBlock / 16, 'x');
    return value;
}

Count text_end(int end, bool wide) {
    Count bytes = 0;
    for (int index = 0; index < end; ++index) {
        const auto value = row(index, wide);
        bytes += value.entry_id.size() + value.entry_comment.size()
            + value.exit_comment.size() + value.exit_id.size();
    }
    return bytes;
}

void same_row(const Trade& actual, const Trade& expected) {
    if (actual.entry_time != expected.entry_time
        || actual.exit_time != expected.exit_time
        || std::memcmp(&actual.entry_price, &expected.entry_price, sizeof(double)) != 0
        || std::memcmp(&actual.exit_price, &expected.exit_price, sizeof(double)) != 0
        || actual.qty != expected.qty || actual.pnl != expected.pnl
        || actual.entry_bar_index != expected.entry_bar_index
        || actual.exit_bar_index != expected.exit_bar_index
        || actual.entry_id != expected.entry_id || actual.exit_id != expected.exit_id
        || actual.entry_comment != expected.entry_comment
        || actual.exit_comment != expected.exit_comment
        || actual.entry_incarnation != expected.entry_incarnation) {
        throw std::runtime_error("retained ledger row changed");
    }
}

struct TemporaryDirectory {
    std::string path;
    TemporaryDirectory() {
        path = "/tmp/pineforge-ledger-blocks-XXXXXX";
        if (::mkdtemp(path.data()) == nullptr) throw std::runtime_error("mkdtemp failed");
    }
    ~TemporaryDirectory() { ::rmdir(path.c_str()); }
};

void reset_and_retry_rows(const std::string& directory) {
    TradeLedger ledger;
    ledger.set_scratch_directory(directory);
    pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
    for (int index = 0; index < 32; ++index) ledger.push_back(row(index));

    io = {};
    io.fail_zero = true;
    bool typed_failure = false;
    try {
        ledger.seal(29);
    } catch (const pineforge::detail::ScratchIoError& error) {
        typed_failure = error.code().value() == EIO;
    }
    if (!typed_failure || !io.failed || ledger.base() != 0)
        throw std::runtime_error("failed block write did not preserve the publication base");
    if (io.failed_offset < 0 || io.failed_bytes == 0)
        throw std::runtime_error("failed block write was not observed");

    const auto failed_offset = io.failed_offset;
    const auto failed_bytes = io.failed_bytes;
    ledger.seal(29);
    if (ledger.base() != 29 || io.last_zero_offset != failed_offset
        || io.last_zero_bytes != failed_bytes)
        throw std::runtime_error("retry did not zero the same block");
    for (int index = 29; index < 32; ++index)
        same_row(ledger.read(static_cast<std::size_t>(index)), row(index));

    TradeLedger copied(ledger);
    TradeLedger moved(std::move(copied));
    if (moved.base() != ledger.base() || moved.size() != ledger.size())
        throw std::runtime_error("copy or move changed ledger bounds");
    for (int index = 29; index < 32; ++index)
        same_row(moved.read(static_cast<std::size_t>(index)), row(index));

    const auto writes_before_clear = io.successful_zero_writes;
    ledger.clear();
    if (io.successful_zero_writes != writes_before_clear)
        throw std::runtime_error("clear performed a zeroing write");
}

void empty_truncate_restarts_the_watermark(const std::string& directory) {
    for (const bool wide : {false, true}) {
        TradeLedger ledger;
        ledger.set_scratch_directory(directory);
        pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
        io = {};
        for (int index = 0; index < 32; ++index) ledger.push_back(row(index, wide));
        // Index dead_end = 29 * kRecordBytes (168); floor(dead_end/block) = 1.
        // Wide payload dead_end = sum(text lengths in rows [0,29)), also > block.
        ledger.seal(29);
        if (io.classification_error || io.files[0].successful_zero_writes == 0
            || (wide && io.files[1].successful_zero_writes == 0))
            throw std::runtime_error("nonempty retirement did not advance the watermarks");
        for (int index = 32; index < 40; ++index) ledger.push_back(row(index, wide));
        ledger.seal(ledger.size());
        const auto empty = pineforge::detail::TradeLedgerStorageProbe::bytes(ledger);
        if (empty[0] != 0 || empty[1] != 0 || ledger.base() != 40)
            throw std::runtime_error("empty retirement did not truncate both files");

        const auto writes_before = io.successful_zero_writes;
        io.last_zero_offset = -1;
        for (auto& file : io.files) file.first_zero_offset = -1;
        for (int index = 40; index < 72; ++index) ledger.push_back(row(index, wide));
        ledger.seal(69);
        // New origin is row 40: index dead_end = (69-40)*168; payload uses
        // sum(text lengths in [40,69)). Both wide extents complete one block.
        // MUTANT: retaining the pre-truncate watermark skips block zero.
        if (io.files[0].first_zero_offset != 0)
            throw std::runtime_error("index zeroing after truncate did not restart at offset zero");
        if (wide && io.files[1].first_zero_offset != 0)
            throw std::runtime_error("payload zeroing after truncate did not restart at offset zero");
        if (ledger.base() != 69 || io.successful_zero_writes <= writes_before)
            throw std::runtime_error("normal retirement did not restart zeroing after truncate");
        for (int index = 69; index < 72; ++index)
            same_row(ledger.read(static_cast<std::size_t>(index)), row(index, wide));
    }
}

std::vector<unsigned char> raw_bytes(const FileIo& file, Count first, Count stop) {
    struct stat status{};
    if (stop < first || ::fstat(file.fd, &status) != 0 || status.st_dev != file.device
        || status.st_ino != file.inode || status.st_size < 0
        || static_cast<Count>(status.st_size) < stop)
        throw std::runtime_error("raw ledger inspection lost its backing extent");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(stop - first));
    // Only pwrite is wrapped in this executable: ::pread is the real syscall,
    // bypassing the descriptor ring, decoded cache and payload tail entirely.
    if (::pread(file.fd, bytes.data(), bytes.size(), static_cast<off_t>(first))
        != static_cast<ssize_t>(bytes.size()))
        throw std::runtime_error("raw ledger read failed");
    return bytes;
}

void raw_survivors(const FileIo& file, const FileIo& reference,
                   Count dead_end, Count end, const char* message) {
    const Count zeroed_end = dead_end - dead_end % kZeroBlock;
    const auto prefix = raw_bytes(file, 0, zeroed_end);
    if (!all_zero(prefix.data(), prefix.size()))
        throw std::runtime_error("retired scratch prefix contains nonzero bytes");
    // The reference never retires: its descriptor/text bytes cannot have been
    // changed by release_head. Compare every byte from the first live row.
    if (raw_bytes(file, dead_end, end) != raw_bytes(reference, dead_end, end))
        throw std::runtime_error(message);
}

void raw_survivors_and_move_retirement(const std::string& directory) {
    constexpr int first_end = 32, first_retired = 29;
    constexpr int moved_end = 2 * first_end, moved_retired = 2 * first_retired;
    TradeLedger reference;
    reference.set_scratch_directory(directory);
    io = {};
    for (int index = 0; index < moved_end; ++index) reference.push_back(row(index, true));
    const auto expected_files = io.files;
    if (io.classification_error)
        throw std::runtime_error("reference file classification failed");

    TradeLedger ledger;
    ledger.set_scratch_directory(directory);
    pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
    io = {};
    for (int index = 0; index < first_end; ++index) ledger.push_back(row(index, true));
    ledger.seal(first_retired);
    if (io.classification_error)
        throw std::runtime_error("retired file classification failed");
    const Count index_dead = first_retired * TradeLedger::kRecordBytes;
    const Count payload_dead = text_end(first_retired, true);
    if (index_dead % kZeroBlock == 0 || payload_dead % kZeroBlock == 0)
        throw std::runtime_error("raw survivor fixture requires unaligned dead ends");
    // Index: dead_end=29*168, end=32*168. Payload: sum all four text lengths
    // in rows [0,29) and [0,32). Prefix=[0,floor(dead_end/block)*block).
    // MUTANT: rounding dead_end up instead erases the first live descriptor/text.
    raw_survivors(io.files[0], expected_files[0], index_dead,
                  first_end * TradeLedger::kRecordBytes,
                  "raw index survivors changed after unaligned retirement");
    raw_survivors(io.files[1], expected_files[1], payload_dead, text_end(first_end, true),
                  "raw payload survivors changed after unaligned retirement");

    const std::array<Count, 2> old_watermarks{
        index_dead - index_dead % kZeroBlock, payload_dead - payload_dead % kZeroBlock};
    TradeLedger moved(std::move(ledger));
    for (int index = first_end; index < moved_end; ++index)
        moved.push_back(row(index, true));
    for (auto& file : io.files) file.first_zero_offset = -1;
    moved.seal(moved_retired);
    // Doubling the retired rows completes the next block in each file.
    // MUTANT: losing the watermark on move rewrites offset zero; losing the
    // pending tail leaves nonzero bytes in the raw prefix checked below.
    for (std::size_t file = 0; file < io.files.size(); ++file) {
        if (io.files[file].first_zero_offset != static_cast<off_t>(old_watermarks[file]))
            throw std::runtime_error("move retirement did not resume at the next zero block");
    }
    raw_survivors(io.files[0], expected_files[0], moved_retired * TradeLedger::kRecordBytes,
                  moved_end * TradeLedger::kRecordBytes,
                  "raw index survivors changed after move retirement");
    raw_survivors(io.files[1], expected_files[1], text_end(moved_retired, true),
                  text_end(moved_end, true),
                  "raw payload survivors changed after move retirement");
}
} // namespace

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
    const int saved = errno;
    auto* file = identify(fd);
    errno = saved;
    if (io.fail_zero && bytes != 0 && bytes <= kZeroBlock && all_zero(data, bytes)) {
        io.fail_zero = false;
        io.failed = true;
        io.failed_offset = offset;
        io.failed_bytes = bytes;
        errno = EIO;
        return -1;
    }
    const auto result = __real_pwrite(fd, data, bytes, offset);
    if (result == static_cast<ssize_t>(bytes) && bytes != 0 && bytes <= kZeroBlock
        && all_zero(data, bytes)) {
        io.last_zero_offset = offset;
        io.last_zero_bytes = bytes;
        ++io.successful_zero_writes;
        if (file) {
            if (file->first_zero_offset < 0) file->first_zero_offset = offset;
            ++file->successful_zero_writes;
        }
    }
    return result;
}

int main() {
    try {
        TemporaryDirectory directory;
        empty_truncate_restarts_the_watermark(directory.path);
        raw_survivors_and_move_retirement(directory.path);
        reset_and_retry_rows(directory.path);
        std::printf("test_ledger_block_zeroing: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_ledger_block_zeroing: FAIL %s\n", error.what());
        return 1;
    }
}
