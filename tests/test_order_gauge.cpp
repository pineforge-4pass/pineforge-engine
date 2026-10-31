#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <pineforge/detail/order_gauge.hpp>
#include <pineforge/report_mode.h>
#include <pineforge/report_sink.h>

#include "global_allocation_replacement.hpp"
#include "native_report_truth_fixture.hpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace gauge = pineforge::detail::gauge;
using namespace r4_test;

namespace {

// Literal transport contract, independent of any constants in the writer.
static_assert(sizeof(gauge::Block) == 128);
static_assert(offsetof(gauge::Block, magic) == 0);
static_assert(offsetof(gauge::Block, version) == 8);
static_assert(offsetof(gauge::Block, length) == 12);
static_assert(offsetof(gauge::Block, seq) == 16);
static_assert(offsetof(gauge::Block, open_lots) == 24);
static_assert(offsetof(gauge::Block, live_orders) == 32);
static_assert(offsetof(gauge::Block, open_lots_peak) == 40);
static_assert(offsetof(gauge::Block, live_orders_peak) == 48);
static_assert(offsetof(gauge::Block, bars_done) == 56);
static_assert(offsetof(gauge::Block, bars_total) == 64);
static_assert(offsetof(gauge::Block, updates) == 72);
static_assert(offsetof(gauge::Block, live_exit_requests) == 80);
static_assert(offsetof(gauge::Block, live_exit_requests_peak) == 88);
static_assert(offsetof(gauge::Block, match_work_peak) == 96);
static_assert(offsetof(gauge::Block, match_work_sum) == 104);
static_assert(offsetof(gauge::Block, fold_work_sum) == 112);
static_assert(offsetof(gauge::Block, roster_work_sum) == 120);
static_assert(noexcept(gauge::note_state(0, 0, 0)));
static_assert(noexcept(gauge::add_fold_work(0)));
static_assert(noexcept(gauge::add_roster_work(0)));
static_assert(noexcept(gauge::note_bars(0, 0)));
static_assert(noexcept(gauge::attach_from_environment()));
static_assert(noexcept(gauge::attached()));
static_assert(noexcept(gauge::publish()));
static_assert(noexcept(gauge::detail::reset_for_test()));

constexpr int required_seals = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
#if defined(F_SEAL_FUTURE_WRITE)
constexpr int future_write_seal = F_SEAL_FUTURE_WRITE;
#else
constexpr int future_write_seal = 0x0010;
#endif

struct Environment {
    bool present = std::getenv("PF_GAUGE_FD") != nullptr;
    std::string saved = present ? std::getenv("PF_GAUGE_FD") : "";
    ~Environment() {
        gauge::detail::reset_for_test();
        if (present) (void)::setenv("PF_GAUGE_FD", saved.c_str(), 1);
        else (void)::unsetenv("PF_GAUGE_FD");
    }
};

void select_fd(int fd) {
    char number[32];
    REQUIRE(std::snprintf(number, sizeof number, "%d", fd) > 0);
    REQUIRE(::setenv("PF_GAUGE_FD", number, 1) == 0);
}

struct Backing {
    enum class Kind { memfd, regular, sealing_disabled };
    int fd = -1;

    explicit Backing(std::size_t bytes = 4096, int seals = required_seals,
                     Kind kind = Kind::memfd) {
        int raw;
        if (kind == Kind::regular) {
            char path[] = "/tmp/pineforge-order-gauge-XXXXXX";
            raw = ::mkstemp(path);
            REQUIRE(raw >= 0);
            REQUIRE(::unlink(path) == 0);
        } else {
            const unsigned flags = kind == Kind::memfd
                ? MFD_CLOEXEC | MFD_ALLOW_SEALING : MFD_CLOEXEC;
            raw = static_cast<int>(::syscall(SYS_memfd_create, "order-gauge", flags));
            REQUIRE(raw >= 0);
        }
        fd = ::fcntl(raw, F_DUPFD_CLOEXEC, 10);
        (void)::close(raw);
        REQUIRE(fd >= 10);
        REQUIRE(::ftruncate(fd, static_cast<off_t>(bytes)) == 0);
        std::array<unsigned char, 4096> sentinel{};
        sentinel.fill(0xa5);
        REQUIRE(bytes <= sentinel.size());
        if (bytes != 0) {
            REQUIRE(::pwrite(fd, sentinel.data(), bytes, 0)
                    == static_cast<ssize_t>(bytes));
        }
        if (kind == Kind::memfd && seals != 0)
            REQUIRE(::fcntl(fd, F_ADD_SEALS, seals) == 0);
    }
    ~Backing() { if (fd >= 0) (void)::close(fd); }
    Backing(const Backing&) = delete;
    Backing& operator=(const Backing&) = delete;

    void zero_block() const {
        std::array<unsigned char, 128> zeros{};
        REQUIRE(::pwrite(fd, zeros.data(), zeros.size(), 0)
                == static_cast<ssize_t>(zeros.size()));
    }

    std::vector<unsigned char> image() const {
        struct stat status{};
        REQUIRE(::fstat(fd, &status) == 0);
        REQUIRE(status.st_size >= 0);
        std::vector<unsigned char> bytes(static_cast<std::size_t>(status.st_size));
        REQUIRE(::pread(fd, bytes.data(), bytes.size(), 0)
                == static_cast<ssize_t>(bytes.size()));
        return bytes;
    }
};

using Bytes = std::array<unsigned char, 128>;

Bytes read_block(int fd) {
    Bytes bytes{};
    REQUIRE(::pread(fd, bytes.data(), 128, 0) == 128);
    return bytes;
}

std::uint32_t u32(const Bytes& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8 * i);
    return value;
}

std::uint64_t u64(const Bytes& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8 * i);
    return value;
}

// The file-byte consumer's acceptance rule is independent of the writer.
bool accepts_pread_copy(const Bytes& bytes) {
    return std::memcmp(bytes.data(), "PFGAUGE1", 8) == 0
        && u32(bytes, 8) == 1
        && u32(bytes, 12) == 128
        && (u64(bytes, 16) & 1) == 0;
}

void write_valid_header(const Backing& backing) {
    Bytes bytes{};
    std::memcpy(bytes.data(), "PFGAUGE1", 8);
    bytes[8] = 1;
    bytes[12] = 128;
    REQUIRE(::pwrite(backing.fd, bytes.data(), bytes.size(), 0)
            == static_cast<ssize_t>(bytes.size()));
}

bool counters_zero() {
    const auto& c = gauge::counters;
    return c.open_lots == 0 && c.live_orders == 0
        && c.open_lots_peak == 0 && c.live_orders_peak == 0
        && c.bars_done == 0 && c.bars_total == 0
        && c.updates == 0 && c.live_exit_requests == 0
        && c.live_exit_requests_peak == 0 && c.match_work_peak == 0
        && c.match_work_sum == 0 && c.fold_work_sum == 0
        && c.roster_work_sum == 0;
}

void fresh_attachment(const Backing& backing) {
    gauge::detail::reset_for_test();
    // A reset keeps the published bytes by contract. Each test re-attach
    // therefore starts from a separately zeroed header.
    backing.zero_block();
    select_fd(backing.fd);
    REQUIRE(gauge::attach_from_environment());
}

// Pipes are nonblocking so an accidental diagnostic cannot hang this row.
// Assertions run after both original descriptors have been restored.
class Capture {
public:
    Capture() {
        std::fflush(stdout);
        std::fflush(stderr);
        for (int i = 0; i < 2; ++i) {
            saved_[i] = ::fcntl(i + 1, F_DUPFD_CLOEXEC, 10);
            REQUIRE(saved_[i] >= 10);
            REQUIRE(::pipe2(pipes_[i].data(), O_CLOEXEC | O_NONBLOCK) == 0);
        }
        for (int i = 0; i < 2; ++i)
            REQUIRE(::dup2(pipes_[i][1], i + 1) == i + 1);
        active_ = true;
    }
    ~Capture() {
        restore();
        for (int i = 0; i < 2; ++i) {
            if (saved_[i] >= 0) (void)::close(saved_[i]);
            for (int fd : pipes_[i]) if (fd >= 0) (void)::close(fd);
        }
    }
    std::array<std::size_t, 2> finish() {
        std::fflush(stdout);
        std::fflush(stderr);
        restore();
        REQUIRE(restored_);
        std::array<std::size_t, 2> counts{};
        char bytes[256];
        for (int i = 0; i < 2; ++i) {
            (void)::close(pipes_[i][1]);
            pipes_[i][1] = -1;
            for (;;) {
                const auto n = ::read(pipes_[i][0], bytes, sizeof bytes);
                if (n > 0) counts[i] += static_cast<std::size_t>(n);
                else if (n < 0 && errno == EINTR) continue;
                else {
                    REQUIRE(n == 0);
                    break;
                }
            }
        }
        return counts;
    }
private:
    void restore() noexcept {
        if (!active_) return;
        restored_ = true;
        for (int i = 0; i < 2; ++i)
            if (::dup2(saved_[i], i + 1) != i + 1) restored_ = false;
        active_ = false;
    }
    int saved_[2]{-1, -1};
    std::array<int, 2> pipes_[2]{{-1, -1}, {-1, -1}};
    bool active_ = false;
    bool restored_ = false;
};

struct DescriptorAt {
    int target;
    int saved;
    DescriptorAt(int target_fd, int backing_fd)
        : target(target_fd), saved(::fcntl(target_fd, F_DUPFD_CLOEXEC, 10)) {
        REQUIRE(saved >= 0 || errno == EBADF);
        REQUIRE(::dup2(backing_fd, target) == target);
    }
    ~DescriptorAt() {
        if (saved >= 0) {
            (void)::dup2(saved, target);
            (void)::close(saved);
        } else {
            (void)::close(target);
        }
    }
};

void layout_and_attach() {
    Environment environment;
    for (const auto kind : {Backing::Kind::memfd, Backing::Kind::regular,
                            Backing::Kind::sealing_disabled}) {
        Backing backing(4096, required_seals, kind);
        fresh_attachment(backing);
        const auto bytes = read_block(backing.fd);
        CHECK(std::memcmp(bytes.data(), "PFGAUGE1", 8) == 0);
        CHECK(u32(bytes, 8) == 1);
        CHECK(u32(bytes, 12) == 128);
        CHECK(u64(bytes, 16) == 0);
        CHECK((u64(bytes, 16) & 1) == 0);
        for (std::size_t offset = 24; offset <= 120; offset += 8)
            CHECK(u64(bytes, offset) == 0);
        const auto full = backing.image();
        for (std::size_t offset = 128; offset < 4096; ++offset)
            CHECK(full[offset] == 0xa5);
    }
    // Exactly 128 is sufficient; 4096 above is the launcher's actual shape.
    Backing exact(128);
    fresh_attachment(exact);
    CHECK(u32(read_block(exact.fd), 12) == 128);

    gauge::note_state(7, 9, 2);
    gauge::note_bars(3, 11);
    gauge::publish();
    const auto first = read_block(exact.fd);
    REQUIRE(gauge::attach_from_environment());
    CHECK(read_block(exact.fd) == first);
    Backing other;
    const auto untouched = other.image();
    select_fd(other.fd);
    REQUIRE(gauge::attach_from_environment());
    gauge::publish();
    CHECK(other.image() == untouched);
    const auto second = read_block(exact.fd);
    CHECK(u64(second, 16) == 4);
    CHECK(u64(second, 24) == 7);
    CHECK(u64(second, 32) == 9);
    CHECK(u64(second, 40) == 7);
    CHECK(u64(second, 48) == 9);
    CHECK(u64(second, 56) == 3);
    CHECK(u64(second, 64) == 11);
    CHECK(u64(second, 72) == 2);
    CHECK(u64(second, 80) == 2);
    CHECK(u64(second, 88) == 2);
    CHECK(u64(second, 96) == 14);
    CHECK(u64(second, 104) == 14); // Publishing twice must not charge another bar.
    gauge::detail::reset_for_test();
    gauge::publish();
    CHECK(read_block(exact.fd) == second);
    CHECK(gauge::counters.open_lots == 0);
    CHECK(gauge::counters.updates == 0);
    CHECK(gauge::counters.match_work_sum == 0);
}

void valid_backing_is_refused() {
    Environment environment;
    gauge::detail::reset_for_test();
    Backing backing;
    write_valid_header(backing);
    const auto before = backing.image();
    select_fd(backing.fd);
    CHECK(!gauge::attached());
    CHECK(!gauge::attach_from_environment());
    CHECK(!gauge::attached());
    CHECK(gauge::counters.updates == 0);
    CHECK(backing.image() == before);
}

void feed_counters() noexcept {
    if (!gauge::attached()) return;
    gauge::note_state(7, 9, 2);
    gauge::note_bars(3, 11);
    gauge::add_fold_work(4);
    gauge::add_roster_work(6);
    gauge::publish();
}

void expect_disabled(const char* value, const Backing& sentinel) {
    gauge::detail::reset_for_test();
    if (value) REQUIRE(::setenv("PF_GAUGE_FD", value, 1) == 0);
    else REQUIRE(::unsetenv("PF_GAUGE_FD") == 0);
    const auto before = sentinel.image();
    Capture capture;
    const bool attached = gauge::attach_from_environment();
    feed_counters();
    const auto output = capture.finish();
    CHECK(!attached);
    CHECK(output[0] == 0);
    CHECK(output[1] == 0);
    CHECK(sentinel.image() == before);
}

void expect_fd_disabled(int fd, const Backing& sentinel) {
    char number[32];
    REQUIRE(std::snprintf(number, sizeof number, "%d", fd) > 0);
    expect_disabled(number, sentinel);
}

void disabled_and_invalid_backings() {
    Environment environment;
    Backing sentinel;
    expect_disabled(nullptr, sentinel);
    expect_disabled("", sentinel);
    expect_disabled("garbage", sentinel);
    expect_disabled("-1", sentinel);
    expect_disabled("+10", sentinel);
    expect_disabled(" 10", sentinel);
    expect_disabled("10x", sentinel);
    expect_disabled("999999999999999999999999", sentinel);
    const int closed = ::fcntl(sentinel.fd, F_DUPFD_CLOEXEC, 200);
    REQUIRE(closed >= 200);
    REQUIRE(::close(closed) == 0);
    expect_fd_disabled(closed, sentinel);
    Backing small(64);
    expect_fd_disabled(small.fd, small);
    Backing write_sealed(4096, required_seals | F_SEAL_WRITE);
    expect_fd_disabled(write_sealed.fd, write_sealed);
    Backing future_write_sealed(4096, required_seals | future_write_seal);
    expect_fd_disabled(future_write_sealed.fd, future_write_sealed);
    for (int seals : {0, F_SEAL_GROW | F_SEAL_SHRINK,
                      F_SEAL_SEAL | F_SEAL_GROW, F_SEAL_SEAL | F_SEAL_SHRINK}) {
        Backing incomplete(4096, seals);
        expect_fd_disabled(incomplete.fd, incomplete);
    }
    char path[64];
    REQUIRE(std::snprintf(path, sizeof path, "/proc/self/fd/%d", sentinel.fd) > 0);
    const int readonly = ::open(path, O_RDONLY | O_CLOEXEC);
    REQUIRE(readonly >= 0);
    expect_fd_disabled(readonly, sentinel);
    REQUIRE(::close(readonly) == 0);
    int pipe_fds[2];
    REQUIRE(::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0);
    expect_fd_disabled(pipe_fds[0], sentinel);
    REQUIRE(::close(pipe_fds[0]) == 0);
    REQUIRE(::close(pipe_fds[1]) == 0);
    expect_disabled("2", sentinel); // Both diagnostics streams are pipes here.

    // Each reserved number also holds a valid writable, sealed backing.
    // For stdout/stderr the backing itself detects any diagnostic; the
    // surrounding pipes are restored before their byte counts are checked.
    for (int reserved : {0, 1, 2, 5}) {
        gauge::detail::reset_for_test();
        select_fd(reserved);
        const auto before = sentinel.image();
        Capture capture;
        bool attached;
        {
            DescriptorAt descriptor(reserved, sentinel.fd);
            attached = gauge::attach_from_environment();
            feed_counters();
        }
        const auto output = capture.finish();
        CHECK(!attached);
        CHECK(output[0] == 0);
        CHECK(output[1] == 0);
        CHECK(sentinel.image() == before);
    }

    // Positive control: this same sentinel really can be attached and changed.
    fresh_attachment(sentinel);
    feed_counters();
    const auto bytes = read_block(sentinel.fd);
    CHECK(std::memcmp(bytes.data(), "PFGAUGE1", 8) == 0);
    CHECK(u64(bytes, 72) == 1);
    CHECK(u64(bytes, 104) == 14);
    CHECK(u64(bytes, 112) == 20);
    CHECK(u64(bytes, 120) == 6);
}

constexpr const char* protected_descriptor_names[]{
    "PF_RUN_QUOTA_NATIVE_FD", "PF_RUN_QUOTA_PYTHON_FD", "PF_SCRATCH_ARENA_FD"
};

struct ProtectedEnvironment {
    std::array<bool, 3> present{};
    std::array<std::string, 3> saved;

    ProtectedEnvironment() {
        for (std::size_t i = 0; i < 3; ++i) {
            const char* value = std::getenv(protected_descriptor_names[i]);
            present[i] = value != nullptr;
            if (value) saved[i] = value;
            REQUIRE(::unsetenv(protected_descriptor_names[i]) == 0);
        }
    }
    ~ProtectedEnvironment() {
        for (std::size_t i = 0; i < 3; ++i) {
            if (present[i])
                (void)::setenv(protected_descriptor_names[i], saved[i].c_str(), 1);
            else (void)::unsetenv(protected_descriptor_names[i]);
        }
    }
};

void select_protected_fd(const char* name, int fd) {
    char number[32];
    REQUIRE(std::snprintf(number, sizeof number, "%d", fd) > 0);
    REQUIRE(::setenv(name, number, 1) == 0);
}

struct LinkedBacking {
    char path[64] = "/tmp/pineforge-order-gauge-linked-XXXXXX";
    int fd = -1;

    LinkedBacking() {
        const int raw = ::mkstemp(path);
        REQUIRE(raw >= 0);
        fd = ::fcntl(raw, F_DUPFD_CLOEXEC, 10);
        (void)::close(raw);
        REQUIRE(fd >= 10);
        REQUIRE(::ftruncate(fd, 4096) == 0);
        std::array<unsigned char, 4096> sentinel{};
        sentinel.fill(0xa5);
        REQUIRE(::pwrite(fd, sentinel.data(), 4096, 0) == 4096);
    }
    ~LinkedBacking() {
        if (fd >= 0) (void)::close(fd);
        (void)::unlink(path);
    }
};

// A separate open file description reads all bytes through the OS, without
// consulting the module's mapping, layout helpers or counter values.
struct FileReader {
    int fd = -1;

    explicit FileReader(int backing_fd) {
        char path[64];
        REQUIRE(std::snprintf(path, sizeof path, "/proc/self/fd/%d", backing_fd) > 0);
        const int raw = ::open(path, O_RDONLY | O_CLOEXEC);
        REQUIRE(raw >= 0);
        fd = ::fcntl(raw, F_DUPFD_CLOEXEC, 10);
        (void)::close(raw);
        REQUIRE(fd >= 10);
    }
    ~FileReader() { if (fd >= 0) (void)::close(fd); }

    std::array<unsigned char, 4096> image() const {
        struct stat status{};
        REQUIRE(::fstat(fd, &status) == 0);
        CHECK(status.st_size == 4096);
        std::array<unsigned char, 4096> bytes{};
        REQUIRE(::pread(fd, bytes.data(), 4096, 0) == 4096);
        return bytes;
    }
};

struct AttachmentResult {
    bool attached;
    int attach_errno;
    int publish_errno;
};

AttachmentResult try_attachment(int fd) {
    gauge::detail::reset_for_test();
    select_fd(fd);
    errno = EDOM;
    const bool attached = gauge::attach_from_environment();
    const int attach_errno = errno;
    feed_counters();
    return {attached, attach_errno, errno};
}

void expect_refused(const AttachmentResult& result, const FileReader& reader) {
    CHECK(result.attached == false);
    CHECK(result.attach_errno == EDOM);
    CHECK(result.publish_errno == EDOM);
    CHECK(gauge::counters.updates == 0);
    std::array<unsigned char, 4096> expected{};
    expected.fill(0xa5);
    CHECK(reader.image() == expected);
}

void linked_file_is_untouched() {
    Environment environment;
    ProtectedEnvironment protected_environment;
    LinkedBacking backing;
    FileReader reader(backing.fd);
    struct stat status{};
    REQUIRE(::fstat(backing.fd, &status) == 0);
    REQUIRE(status.st_nlink == 1);
    expect_refused(try_attachment(backing.fd), reader);

    REQUIRE(::unlink(backing.path) == 0);
    REQUIRE(::fstat(backing.fd, &status) == 0);
    REQUIRE(status.st_nlink == 0);
    const auto result = try_attachment(backing.fd);
    CHECK(result.attached == true);
    CHECK(result.attach_errno == EDOM);
    CHECK(result.publish_errno == EDOM);
    const auto bytes = read_block(backing.fd);
    CHECK(std::memcmp(bytes.data(), "PFGAUGE1", 8) == 0);
    CHECK(u32(bytes, 8) == 1);
    CHECK(u32(bytes, 12) == 128);
    CHECK(u64(bytes, 16) == 2);
    CHECK(u64(bytes, 72) == 1);
    CHECK(u64(bytes, 104) == 14);
    CHECK(u64(bytes, 112) == 20);
    CHECK(u64(bytes, 120) == 6);
    const auto full = reader.image();
    for (std::size_t offset = 128; offset < 4096; ++offset)
        CHECK(full[offset] == 0xa5);
}

void protected_number_is_untouched(const char* name) {
    Environment environment;
    ProtectedEnvironment protected_environment;
    Backing backing;
    FileReader reader(backing.fd);
    select_protected_fd(name, backing.fd);
    expect_refused(try_attachment(backing.fd), reader);

    char padded[32];
    REQUIRE(std::snprintf(padded, sizeof padded, "000%d", backing.fd) > 0);
    REQUIRE(::setenv(name, padded, 1) == 0);
    expect_refused(try_attachment(backing.fd), reader);

    // An unrelated protected file, malformed values and a closed descriptor
    // must leave this otherwise valid gauge backing usable.
    Backing unrelated;
    select_protected_fd(name, unrelated.fd);
    backing.zero_block();
    auto result = try_attachment(backing.fd);
    CHECK(result.attached == true);
    CHECK(u64(read_block(backing.fd), 72) == 1);
    const auto number = std::to_string(backing.fd);
    const std::array<std::string, 7> malformed{{
        "", "garbage", "-1", "+" + number, " " + number, number + "x",
        "999999999999999999999999"
    }};
    for (const auto& value : malformed) {
        REQUIRE(::setenv(name, value.c_str(), 1) == 0);
        backing.zero_block();
        result = try_attachment(backing.fd);
        CHECK(result.attached == true);
        CHECK(u64(read_block(backing.fd), 72) == 1);
    }
    const int closed = ::fcntl(unrelated.fd, F_DUPFD_CLOEXEC, 200);
    REQUIRE(closed >= 200);
    REQUIRE(::close(closed) == 0);
    select_protected_fd(name, closed);
    backing.zero_block();
    result = try_attachment(backing.fd);
    CHECK(result.attached == true);
    CHECK(u64(read_block(backing.fd), 72) == 1);
}

void protected_aliases_are_untouched() {
    Environment environment;
    ProtectedEnvironment protected_environment;
    for (const char* name : protected_descriptor_names) {
        {
            Backing backing;
            FileReader reader(backing.fd);
            const int alias = ::fcntl(backing.fd, F_DUPFD_CLOEXEC, 10);
            REQUIRE(alias >= 10);
            REQUIRE(alias != backing.fd);
            select_protected_fd(name, alias);
            expect_refused(try_attachment(backing.fd), reader);
            REQUIRE(::close(alias) == 0);
        }
        {
            Backing backing;
            FileReader reader(backing.fd);
            DescriptorAt descriptor(5, backing.fd);
            select_protected_fd(name, 5);
            expect_refused(try_attachment(backing.fd), reader);
        }
        REQUIRE(::unsetenv(name) == 0);
    }
}

void standard_stream_aliases_are_untouched() {
    Environment environment;
    ProtectedEnvironment protected_environment;
    for (int stream : {0, 1, 2}) {
        Backing backing;
        FileReader reader(backing.fd);
        AttachmentResult result{};
        {
            DescriptorAt descriptor(stream, backing.fd);
            const int alias = ::fcntl(stream, F_DUPFD_CLOEXEC, 10);
            REQUIRE(alias >= 10);
            REQUIRE(alias != backing.fd);
            result = try_attachment(alias);
            REQUIRE(::close(alias) == 0);
        }
        // Restore stdout/stderr before any expected regression assertion.
        expect_refused(result, reader);
    }
}

// The test reader uses atomic payload loads too: retrying plain reads would
// still be a C++ data race. File-byte layout is checked independently above.
bool snapshot(const gauge::Block* mapped, gauge::Block& out) noexcept {
    const auto before = __atomic_load_n(&mapped->seq, __ATOMIC_ACQUIRE);
    if ((before & 1) != 0) return false;
    out.seq = before;
#define READ_FIELD(field) out.field = __atomic_load_n(&mapped->field, __ATOMIC_RELAXED)
    READ_FIELD(open_lots);
    READ_FIELD(live_orders);
    READ_FIELD(open_lots_peak);
    READ_FIELD(live_orders_peak);
    READ_FIELD(bars_done);
    READ_FIELD(bars_total);
    READ_FIELD(updates);
    READ_FIELD(live_exit_requests);
    READ_FIELD(live_exit_requests_peak);
    READ_FIELD(match_work_peak);
    READ_FIELD(match_work_sum);
    READ_FIELD(fold_work_sum);
    READ_FIELD(roster_work_sum);
#undef READ_FIELD
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    const auto after = __atomic_load_n(&mapped->seq, __ATOMIC_ACQUIRE);
    return before == after && (after & 1) == 0;
}

bool consistent(const gauge::Block& row) noexcept {
    const auto k = row.open_lots;
    // Independently specified invariants for the writer's ascending tape.
    if (k > 200000) return false;
    return row.seq == 2 * k
        && row.live_orders == k && row.open_lots_peak == k
        && row.live_orders_peak == k && row.bars_done == k
        && row.bars_total == (k == 0 ? 0 : 200000)
        && row.updates == k && row.live_exit_requests == k
        && row.live_exit_requests_peak == k && row.match_work_peak == k * k
        && row.match_work_sum == k * (k + 1) * (2 * k + 1) / 6
        && row.fold_work_sum == 2 * k && row.roster_work_sum == 3 * k;
}

void concurrent_publication() {
    Environment environment;
    Backing backing;
    fresh_attachment(backing);
    void* address = ::mmap(nullptr, 128, PROT_READ, MAP_SHARED, backing.fd, 0);
    REQUIRE(address != MAP_FAILED);
    const auto* mapped = static_cast<const gauge::Block*>(address);
    std::atomic<bool> start{false}, first_read{false}, done{false};
    std::atomic<std::uint64_t> iterations{0};
    std::thread writer([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for (std::uint64_t k = 1; k <= 200000; ++k) {
            gauge::note_state(k, k, k);
            gauge::note_bars(k, 200000);
            gauge::add_fold_work(1);
            gauge::add_roster_work(3);
            gauge::publish();
            iterations.store(k, std::memory_order_release);
            if (k == 1) {
                while (!first_read.load(std::memory_order_acquire))
                    std::this_thread::yield();
            }
        }
        done.store(true, std::memory_order_release);
    });
    std::uint64_t accepted = 0, retries = 0, nonzero = 0, inconsistent = 0;
    std::uint64_t attempts_after_done = 0;
    start.store(true, std::memory_order_release);
    // The writer cannot finish its loop before this reader has tried the
    // first real publication. Release it even if that snapshot is broken.
    while (iterations.load(std::memory_order_acquire) == 0
           && !done.load(std::memory_order_acquire)) std::this_thread::yield();
    gauge::Block first{};
    const bool first_accepted = snapshot(mapped, first);
    first_read.store(true, std::memory_order_release);
    do {
        if (done.load(std::memory_order_acquire)) ++attempts_after_done;
        gauge::Block row{};
        if (snapshot(mapped, row)) {
            ++accepted;
            if (row.open_lots != 0) ++nonzero;
            if (!consistent(row)) ++inconsistent;
        } else {
            ++retries;
        }
    } while (!done.load(std::memory_order_acquire)
             || (accepted < 1000 && attempts_after_done < 10000));
    writer.join();
    CHECK(iterations.load(std::memory_order_relaxed) == 200000);
    CHECK(first_accepted);
    CHECK(first.open_lots == 1);
    CHECK(first.updates == 1);
    CHECK(consistent(first));
    CHECK(inconsistent == 0);
    CHECK(nonzero > 0);
    CHECK(retries > 0 || accepted >= 1000);
    const auto final = read_block(backing.fd);
    CHECK(u64(final, 16) == 400000);
    CHECK(u64(final, 24) == 200000);
    CHECK(u64(final, 72) == 200000);
    CHECK(u64(final, 96) == 40000000000ULL);
    CHECK(u64(final, 104) == 2666686666700000ULL);
    CHECK(u64(final, 112) == 400000);
    CHECK(u64(final, 120) == 600000);
    CHECK(::munmap(address, 128) == 0);
}

void saturating_work_and_peaks() {
    Environment environment;
    Backing backing;
    fresh_attachment(backing);
    gauge::add_fold_work(UINT64_MAX);
    gauge::publish();
    CHECK(u64(read_block(backing.fd), 112) == 18446744073709551615ULL);

    fresh_attachment(backing);
    gauge::add_fold_work(4294967295ULL);
    gauge::publish();
    CHECK(u64(read_block(backing.fd), 112) == 18446744069414584320ULL);
    gauge::add_fold_work(65536);
    gauge::publish();
    CHECK(u64(read_block(backing.fd), 112) == 18446744073709551615ULL);
    fresh_attachment(backing);
    gauge::add_fold_work(4294967296ULL);
    gauge::publish();
    CHECK(u64(read_block(backing.fd), 112) == 18446744073709551615ULL);

    fresh_attachment(backing);
    gauge::note_state(18446744073709551614ULL, 99, 1);
    gauge::add_roster_work(18446744073709551614ULL);
    gauge::publish();
    auto bytes = read_block(backing.fd);
    CHECK(u64(bytes, 104) == 18446744073709551614ULL);
    CHECK(u64(bytes, 120) == 18446744073709551614ULL);
    gauge::note_state(5, 1, 1);
    gauge::add_roster_work(5);
    gauge::publish();
    bytes = read_block(backing.fd);
    CHECK(u64(bytes, 24) == 5);
    CHECK(u64(bytes, 32) == 1);
    CHECK(u64(bytes, 40) == 18446744073709551614ULL);
    CHECK(u64(bytes, 48) == 99);
    CHECK(u64(bytes, 96) == 18446744073709551614ULL);
    CHECK(u64(bytes, 104) == 18446744073709551615ULL);
    CHECK(u64(bytes, 120) == 18446744073709551615ULL);
    gauge::add_roster_work(UINT64_MAX);
    gauge::note_state(9223372036854775808ULL, 2, 2);
    gauge::publish();
    bytes = read_block(backing.fd);
    CHECK(u64(bytes, 88) == 2);
    CHECK(u64(bytes, 96) == 18446744073709551615ULL);
    CHECK(u64(bytes, 104) == 18446744073709551615ULL);
    CHECK(u64(bytes, 120) == 18446744073709551615ULL);
    gauge::note_state(1, 1, 0);
    gauge::note_bars(UINT64_MAX, 0);
    gauge::publish();
    bytes = read_block(backing.fd);
    CHECK(u64(bytes, 40) == 18446744073709551614ULL);
    CHECK(u64(bytes, 48) == 99);
    CHECK(u64(bytes, 80) == 0);
    CHECK(u64(bytes, 88) == 2);
    CHECK(u64(bytes, 96) == 18446744073709551615ULL);
    CHECK(u64(bytes, 56) == 18446744073709551615ULL);
    CHECK(u64(bytes, 64) == 0);

    // Updates is a saturating counter too; seq remains an even protocol word.
    gauge::counters.updates = 18446744073709551614ULL;
    gauge::publish();
    gauge::publish();
    CHECK(u64(read_block(backing.fd), 72) == 18446744073709551615ULL);
}

void native_run_publishes_gauge() {
    Environment environment;
    gauge::detail::reset_for_test();
    Bytes empty{};
    CHECK(u64(empty, 16) == 0);
    CHECK(!accepts_pread_copy(empty));
    Backing backing;
    select_fd(backing.fd);

    r4_test::Host host;
    host.calculation = [](r4_test::Host& current) {
        if (current.calculations == 1) {
            const auto result = current.submit(l2_fixture::market(1.0, "real-open"));
            REQUIRE(result.status == no::SubmitStatus::Accepted);
        }
    };
    const auto run_spec = r4_test::spec("order-gauge-real-run");
    const auto bars = l2_fixture::feed(4);
    l2_fixture::run_feed(host, run_spec, bars);
    r4_test::completed(host);
    CHECK(host.lots().size() > 0);

    const auto bytes = read_block(backing.fd);
    CHECK(std::memcmp(bytes.data(), "PFGAUGE1", 8) == 0);
    CHECK(u32(bytes, 8) == 1);
    CHECK(u32(bytes, 12) == 128);
    CHECK((u64(bytes, 16) & 1) == 0);
    CHECK(u64(bytes, 24) > 0);
    CHECK(u64(bytes, 40) > 0);
    CHECK(u64(bytes, 56) == 4);
    CHECK(u64(bytes, 64) == 0);
    CHECK(u64(bytes, 72) >= 4);
}

void report_rule(r4_test::Host& host) {
    switch (host.calculations - 1) {
    case 10: host.submit(l2_fixture::market(2.0, "enter-long")); break;
    case 30: host.submit(l2_fixture::market(-2.0, "exit-long")); break;
    case 40: host.submit(l2_fixture::market(-1.0, "enter-short")); break;
    case 55: host.submit(l2_fixture::market(1.0, "exit-short")); break;
    default: break;
    }
}

struct ReportArtifact {
    std::string directory;
    std::string path;
    ReportArtifact() {
        const char* parent = std::getenv("TMPDIR");
        directory = std::string(parent && *parent ? parent : "/tmp")
            + "/pineforge-order-gauge-report-XXXXXX";
        REQUIRE(::mkdtemp(directory.data()) != nullptr);
        path = directory + "/report.ndjson";
    }
    ~ReportArtifact() {
        (void)::unlink(path.c_str());
        (void)::rmdir(directory.c_str());
    }
};

std::vector<unsigned char> native_report() {
    ReportArtifact artifact; // Outlives the host and its unlinked scratch files.
    r4_test::Host host;
    host.calculation = [](r4_test::Host& current) { report_rule(current); };
    auto spec = l2_fixture::report_spec("order-gauge-neutrality");
    spec.report_policy = pineforge::NativeReportPolicy::KernelRecorded;
    spec.close_execution = pineforge::NativeCloseExecution::AfterCalculation;
    host.set_broker_state_hash_recording(true);
    const pf_report_sink_v1 sink{sizeof sink, 1, artifact.path.c_str(),
                                "gauge-neutrality", artifact.directory.c_str()};
    REQUIRE(host.set_report_sink_v1(&sink) == 0);
    REQUIRE(host.set_report_mode_v1(pineforge::ReportMode::STREAMED)
            == PF_REPORT_MODE_ACCEPTED);
    l2_fixture::run_feed(host, spec, l2_fixture::feed(60));
    r4_test::completed(host);
    l2_fixture::Report report(host);
    CHECK(report.c.total_trades == 2);
    CHECK(report.c.script_bars_processed == 60);
    CHECK(report.c.input_bars_processed == 60);
    CHECK(report.c.equity_curve_len == 0); // All points/hashes live in the sink.
    CHECK(report.c.broker_state_hash_len == 0);
    pf_report_sink_receipt_v1 receipt{};
    receipt.struct_size = sizeof receipt;
    receipt.version = 1;
    REQUIRE(host.report_sink_receipt_v1(&receipt) == 0);
    CHECK(receipt.complete == 1);
    CHECK(receipt.count_e == 60);
    CHECK(receipt.count_t == 2);
    CHECK(receipt.count_h == 60);
    std::ifstream input(artifact.path, std::ios::binary);
    REQUIRE(input.good());
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
    REQUIRE(!bytes.empty());
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("\"trailer\":") != std::string::npos);
    return bytes; // Complete serialized report, including its hash/trailer.
}

void report_bytes_are_unchanged() {
    Environment environment;
    gauge::detail::reset_for_test();
    REQUIRE(::unsetenv("PF_GAUGE_FD") == 0);
    const auto detached = native_report();
    CHECK(counters_zero());

    gauge::detail::reset_for_test();
    REQUIRE(::setenv("PF_GAUGE_FD", "-1", 1) == 0);
    const auto invalid = native_report();
    CHECK(invalid == detached);
    CHECK(counters_zero());

    gauge::detail::reset_for_test();
    Backing backing;
    select_fd(backing.fd);
    const auto attached = native_report();
    CHECK(attached == detached);
    const auto bytes = read_block(backing.fd);
    CHECK(u64(bytes, 16) == 120);
    CHECK(u64(bytes, 56) == 60);
    CHECK(u64(bytes, 64) == 0);
    CHECK(u64(bytes, 72) == 60);
    CHECK(u64(bytes, 112) == 0);
    CHECK(u64(bytes, 120) == 0);
}

void allocation_free_updates() {
    Environment environment;
    // An explicit call through a volatile pointer avoids new-expression
    // allocation elision, so the positive control must reach the hook.
    void* (*volatile allocate)(std::size_t) = &::operator new;
    const auto control_before = global_allocation::allocations;
    void* control = allocate(16);
    CHECK(global_allocation::allocations == control_before + 1);
    ::operator delete(control);
    const auto refusal_before = global_allocation::refused;
    global_allocation::refusing = true;
    void* refused = ::operator new(16, std::nothrow);
    global_allocation::refusing = false;
    CHECK(refused == nullptr);
    CHECK(global_allocation::refused == refusal_before + 1);

    Backing backing;
    for (bool attached : {false, true}) {
        gauge::detail::reset_for_test();
        if (attached) fresh_attachment(backing);
        else REQUIRE(::unsetenv("PF_GAUGE_FD") == 0);
        const auto before = backing.image();
        const auto allocations = global_allocation::allocations;
        const auto refusals = global_allocation::refused;
        global_allocation::refusing = true;
        if (attached) {
            for (std::uint64_t k = 1; k <= 1000000; ++k) {
                gauge::note_state(k, k, k);
                gauge::note_bars(k, 1000000);
                gauge::add_fold_work(1);
                gauge::add_roster_work(3);
                gauge::publish();
            }
        }
        global_allocation::refusing = false;
        CHECK(global_allocation::allocations == allocations);
        CHECK(global_allocation::refused == refusals);
        if (attached) {
            CHECK(gauge::counters.open_lots == 1000000);
            CHECK(gauge::counters.match_work_sum == 333333833333500000ULL);
            CHECK(gauge::counters.fold_work_sum == 2000000);
            CHECK(gauge::counters.roster_work_sum == 3000000);
            const auto bytes = read_block(backing.fd);
            CHECK(u64(bytes, 16) == 2000000);
            CHECK(u64(bytes, 24) == 1000000);
            CHECK(u64(bytes, 72) == 1000000);
            CHECK(u64(bytes, 96) == 1000000000000ULL);
            CHECK(u64(bytes, 104) == 333333833333500000ULL);
            CHECK(u64(bytes, 112) == 2000000);
            CHECK(u64(bytes, 120) == 3000000);
        } else {
            CHECK(counters_zero());
            CHECK(backing.image() == before);
        }
    }
}

} // namespace

int main() {
    test("order_gauge_layout_and_attach", layout_and_attach);
    test("order_gauge_valid_backing_is_refused", valid_backing_is_refused);
    test("order_gauge_disabled_and_invalid_backings", disabled_and_invalid_backings);
    test("order_gauge_linked_file_is_untouched", linked_file_is_untouched);
    test("order_gauge_native_quota_number_is_untouched", [] {
        protected_number_is_untouched("PF_RUN_QUOTA_NATIVE_FD");
    });
    test("order_gauge_python_quota_number_is_untouched", [] {
        protected_number_is_untouched("PF_RUN_QUOTA_PYTHON_FD");
    });
    test("order_gauge_scratch_number_is_untouched", [] {
        protected_number_is_untouched("PF_SCRATCH_ARENA_FD");
    });
    test("order_gauge_protected_aliases_are_untouched", protected_aliases_are_untouched);
    test("order_gauge_standard_stream_aliases_are_untouched",
         standard_stream_aliases_are_untouched);
    test("order_gauge_concurrent_publication", concurrent_publication);
    test("order_gauge_saturating_work_and_peaks", saturating_work_and_peaks);
    test("order_gauge_native_run_publishes_gauge", native_run_publishes_gauge);
    test("order_gauge_report_bytes_are_unchanged", report_bytes_are_unchanged);
    test("order_gauge_allocation_free_updates", allocation_free_updates);
    std::printf("test_order_gauge: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
