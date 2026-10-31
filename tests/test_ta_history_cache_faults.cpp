#include <pineforge/series.hpp>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <sys/types.h>
#include <unistd.h>

namespace window_fault {
bool fail_allocation = false, fail_read = false, fail_write = false;
bool partial_read = false, partial_write = false;
std::uint64_t reads = 0, writes = 0, allocation_faults = 0, io_faults = 0;
}

extern "C" ssize_t __real_pread(int, void*, std::size_t, off_t);
extern "C" ssize_t __real_pwrite(int, const void*, std::size_t, off_t);
extern "C" void* __real__ZnwmRKSt9nothrow_t(std::size_t, const std::nothrow_t&) noexcept;

extern "C" ssize_t __wrap_pread(int fd, void* data, std::size_t size, off_t offset) {
    ++window_fault::reads;
    if (window_fault::fail_read) {
        window_fault::fail_read = false;
        ++window_fault::io_faults;
        errno = EIO;
        return -1;
    }
    if (window_fault::partial_read) {
        window_fault::partial_read = false;
        const auto transferred = __real_pread(fd, data, size / 2, offset);
        window_fault::fail_read = transferred > 0;
        return transferred;
    }
    return __real_pread(fd, data, size, offset);
}
extern "C" ssize_t __wrap_pwrite(int fd, const void* data, std::size_t size, off_t offset) {
    ++window_fault::writes;
    if (window_fault::fail_write) {
        window_fault::fail_write = false;
        ++window_fault::io_faults;
        errno = ENOSPC;
        return -1;
    }
    if (window_fault::partial_write) {
        window_fault::partial_write = false;
        const auto transferred = __real_pwrite(fd, data, size / 2, offset);
        window_fault::fail_write = transferred > 0;
        return transferred;
    }
    return __real_pwrite(fd, data, size, offset);
}
extern "C" void* __wrap__ZnwmRKSt9nothrow_t(
        std::size_t size, const std::nothrow_t& tag) noexcept {
    if (window_fault::fail_allocation) {
        window_fault::fail_allocation = false;
        ++window_fault::allocation_faults;
        return nullptr;
    }
    return __real__ZnwmRKSt9nothrow_t(size, tag);
}

static void require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

using Values = pineforge::detail::WindowValues<double>;

static void allocation_bypass() {
    Values values(3 * Values::kChunk, 0.0, {1});
    values.write(3, 91.0);
    const auto reads = window_fault::reads;
    window_fault::fail_allocation = true;
    require(values.read(3) == 91.0, "optional allocation failure reads backing");
    require(values.read(3) == 91.0, "allocation bypass remains usable");
    require(!window_fault::fail_allocation && window_fault::allocation_faults == 1,
            "real shared-cache allocation consumed the injected failure");
    require(window_fault::reads - reads == 2, "both bypass reads reached pread");
    const auto stats = values.cache_stats();
    require(stats.allocation_bypasses == 1 && stats.bypass_reads == 2,
            "allocation bypass counters report actual work");
    require(stats.resident_payload_bytes == 0, "failed optional cache holds no payload");
}

static void backing_failures() {
    Values values(3 * Values::kChunk, 0.0, {1});
    values.write(3, 19.0);
    values.write(Values::kChunk + 3, 27.0);
    require(values.read(3) == 19.0, "seed the real cache");
    require(values.read(Values::kChunk + 3) == 27.0, "evict the first page");
    const auto reads = window_fault::reads;
    window_fault::fail_read = true;
    bool caught = false;
    try { (void)values.read(3); }
    catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        require(error.code().value() == EIO, "cold backing error retains errno");
        require(error.run_failure_code() == pineforge::RunFailureCode::scratch_io_failed,
                "cold backing error retains typed code");
        caught = true;
    }
    require(caught && !window_fault::fail_read, "evicted cold page surfaces backing failure");
    require(window_fault::reads - reads == 1, "cold failure was a physical pread");
    require(values.read(3) == 19.0, "failed refill leaves the backing retryable");
    require(window_fault::reads - reads == 2, "failed refill cannot publish a cache hit");

    const auto writes = window_fault::writes;
    window_fault::fail_write = true;
    caught = false;
    try { values.write(3, 88.0); }
    catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        require(error.code().value() == ENOSPC, "backing write retains ENOSPC");
        require(error.run_failure_code() == pineforge::RunFailureCode::scratch_io_failed,
                "backing write retains typed code");
        caught = true;
    }
    require(caught && !window_fault::fail_write, "real failed write reaches the caller");
    require(window_fault::writes - writes == 1, "write fault was a physical pwrite");
    const auto after_failure = window_fault::reads;
    require(values.read(3) == 19.0, "failed write keeps authoritative old backing bytes");
    require(window_fault::reads - after_failure == 1, "failed write invalidated its tag");
    require(values.cache_stats().io_errors == 2, "read and write errors are counted at I/O");
}

static void checkpoint_clone_failure() {
    Values values(3 * Values::kChunk, 0.0, {1});
    values.write(3, 61.0);
    auto checkpoint = values;
    window_fault::fail_read = true;
    bool caught = false;
    try { values.write(3, 92.0); }
    catch (const pineforge::detail::AuxiliaryScratchIoError&) { caught = true; }
    require(caught && !window_fault::fail_read, "checkpoint clone reads real backing");
    require(values.read(3) == 61.0 && checkpoint.read(3) == 61.0,
            "failed clone publishes neither partial bytes nor a changed checkpoint");
    values.write(3, 92.0);
    require(values.read(3) == 92.0 && checkpoint.read(3) == 61.0,
            "successful clone separates the next tick from its checkpoint");
}

static void partial_io_failures() {
    Values values(3 * Values::kChunk, 0.0, {1});
    const std::uint64_t original_bits = UINT64_C(0x3ff123456789abcd);
    const std::uint64_t replacement_bits = UINT64_C(0x4009abcdef012345);
    double original, replacement;
    std::memcpy(&original, &original_bits, sizeof original);
    std::memcpy(&replacement, &replacement_bits, sizeof replacement);
    values.write(3, original);
    const auto reads = window_fault::reads;
    window_fault::partial_read = true;
    bool caught = false;
    try { (void)values.read(3); }
    catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        require(error.code().value() == EIO, "partial read retains its original EIO");
        caught = true;
    }
    require(caught && !window_fault::partial_read && !window_fault::fail_read
                && window_fault::reads - reads == 2,
            "partial read must transfer a prefix then physically fail");
    require(values.read(3) == original && window_fault::reads - reads == 3,
            "partial read must not publish a warm shared-cache tag");

    // Native bytes are authoritative even after a write changes only a prefix.
    double want = original;
    std::memcpy(&want, &replacement, sizeof(double) / 2);
    require(std::memcmp(&want, &original, sizeof(double)) != 0
                && std::memcmp(&want, &replacement, sizeof(double)) != 0,
            "partial-write oracle must differ from both old and complete values");
    const auto writes = window_fault::writes;
    window_fault::partial_write = true;
    caught = false;
    try { values.write(3, replacement); }
    catch (const pineforge::detail::AuxiliaryScratchIoError& error) {
        require(error.code().value() == ENOSPC, "partial write retains its original ENOSPC");
        caught = true;
    }
    require(caught && !window_fault::partial_write && !window_fault::fail_write
                && window_fault::writes - writes == 2,
            "partial write must change a prefix before failing");
    const auto before_retry = window_fault::reads;
    const double actual = values.read(3);
    require(std::memcmp(&actual, &want, sizeof(double)) == 0
                && window_fault::reads - before_retry == 1,
            "partial write cannot return stale successful cache bytes");
    values.write(3, replacement);
    require(values.read(3) == replacement, "partial write remains retryable");
}

int main() {
    allocation_bypass();
    backing_failures();
    checkpoint_clone_failure();
    partial_io_failures();
    require(window_fault::io_faults == 3 + 2, "every planted backing fault was consumed");
    std::puts("TA history cache faults: allocation bypass and typed cold I/O verified");
}
