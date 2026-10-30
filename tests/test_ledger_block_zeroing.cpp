#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/trade_ledger.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void retire_history(TradeLedger& ledger) {
        ledger.set_history_retirement(true);
    }
};
} // namespace pineforge::detail

namespace {
using pineforge::Trade;
using pineforge::TradeLedger;
using Count = std::uint64_t;
constexpr Count kZeroBlock = pineforge::detail::ScratchArena::kChunkBytes / 256;

struct IoState {
    bool fail_zero = false;
    bool failed = false;
    off_t failed_offset = -1;
    std::size_t failed_bytes = 0;
    off_t last_zero_offset = -1;
    std::size_t last_zero_bytes = 0;
    Count successful_zero_writes = 0;
} io;

bool all_zero(const void* data, std::size_t bytes) {
    const auto* first = static_cast<const unsigned char*>(data);
    return std::all_of(first, first + bytes, [](unsigned char value) { return value == 0; });
}

Trade row(int index) {
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
    return value;
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
    TradeLedger ledger;
    ledger.set_scratch_directory(directory);
    pineforge::detail::TradeLedgerStorageProbe::retire_history(ledger);
    for (int index = 0; index < 40; ++index) ledger.push_back(row(index));
    ledger.seal(40);
    const auto empty = ledger.scratch_storage_bytes();
    if (empty[0] != 0 || empty[1] != 0 || ledger.base() != 40)
        throw std::runtime_error("empty retirement did not truncate both files");

    const auto writes_before = io.successful_zero_writes;
    for (int index = 40; index < 72; ++index) ledger.push_back(row(index));
    ledger.seal(69);
    if (ledger.base() != 69 || io.successful_zero_writes <= writes_before)
        throw std::runtime_error("normal retirement did not restart zeroing after truncate");
    for (int index = 69; index < 72; ++index)
        same_row(ledger.read(static_cast<std::size_t>(index)), row(index));
}
} // namespace

extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);

extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t bytes, off_t offset) {
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
    }
    return result;
}

int main() {
    try {
        TemporaryDirectory directory;
        reset_and_retry_rows(directory.path);
        empty_truncate_restarts_the_watermark(directory.path);
        std::printf("test_ledger_block_zeroing: PASS\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "test_ledger_block_zeroing: FAIL %s\n", error.what());
        return 1;
    }
}
