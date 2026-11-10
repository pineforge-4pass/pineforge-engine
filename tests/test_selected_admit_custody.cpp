#include "../tools/selected_admit/custody.hpp"
#include "../src/selected_capacity_csv_reader.hpp"
#include "../tools/selected_admit/stage_selected.hpp"
#include "../tools/selected_admit/stage_source.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <numeric>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <type_traits>
#include <unistd.h>

namespace {
namespace sa = pineforge::selected_admit;
namespace sc = pineforge::detail::selected_capacity;
constexpr std::size_t kChunkBytes = 65536;
static_assert(kChunkBytes == sa::kStreamChunkBytes);
const std::string kHeader = "timestamp,open,high,low,close,volume\n";
const std::string kPrefix = "0,21,23,19,22,102,";
const std::string kSource = kHeader + "-60000,11,13,9,12,101\n"
    "0,21,23,19,22,102\n60000,31,33,29,32,103\n900000,41,43,39,42,104\n";
const std::string kPrimary = kHeader + "0,51,53,49,52,201\n60000,61,63,59,62,202\n";
constexpr char kSourceHash[] = "7b1c8591c4469fca050983b6cde522ecf540c19ff77ff2e09466a3ca10c3488e";
constexpr char kPrimaryHash[] = "34921118749927d5f67a08c2bc50e31787b936fa754526cba6fabc5a97bc4349";

struct Memory {
    std::string bytes;
    std::size_t position = 0;
    std::size_t short_count = kChunkBytes;
    int interrupted = 0;
    int again = 0;
    std::size_t error_at = std::numeric_limits<std::size_t>::max();
    bool truncate = false;
    bool grow = false;
    bool mutate = false;
    int stat_calls = 0;

    static ssize_t pread(void* ctx, int, void* out, std::size_t cap, off_t offset) {
        auto& m = *static_cast<Memory*>(ctx);
        if (m.interrupted) { --m.interrupted; errno = EINTR; return -1; }
        if (static_cast<std::size_t>(offset) >= m.error_at) { errno = EIO; return -1; }
        if (m.truncate && static_cast<std::size_t>(offset) >= kHeader.size()) return 0;
        if (static_cast<std::size_t>(offset) >= m.bytes.size()) {
            if (m.grow) { *static_cast<char*>(out) = 'a'; return 1; }
            return 0;
        }
        const auto n = std::min({cap, m.short_count, m.bytes.size() - static_cast<std::size_t>(offset)});
        std::memcpy(out, m.bytes.data() + offset, n);
        return static_cast<ssize_t>(n);
    }
    static ssize_t read(void* ctx, int fd, void* out, std::size_t cap) {
        auto& m = *static_cast<Memory*>(ctx);
        if (m.again) { --m.again; errno = EAGAIN; return -1; }
        const auto n = pread(ctx, fd, out, cap, static_cast<off_t>(m.position));
        if (n > 0) m.position += static_cast<std::size_t>(n);
        return n;
    }
    static int stat(void* ctx, int, struct stat* out) {
        auto& m = *static_cast<Memory*>(ctx);
        *out = {};
        out->st_mode = S_IFREG | 0400;
        out->st_dev = 1;
        out->st_ino = 1;
        out->st_size = static_cast<off_t>(m.bytes.size());
        if (m.mutate && ++m.stat_calls > 0) out->st_ino = 2;
        return 0;
    }
    sa::Seam seam() {
        sa::Seam s;
        s.ctx = this;
        s.pread = &pread;
        s.read = &read;
        s.fstat = &stat;
        return s;
    }
};

struct Parsed {
    sa::CustodyReason custody;
    sc::CsvStatus status;
    std::uint64_t delivered, row, record_octets;
    bool complete, io;
    std::vector<std::uint64_t> lengths;
};

Parsed parse(Memory& memory, const std::string& digest, std::uint64_t cap,
             bool file = true, int ready_fd = -1) {
    sa::StreamSpec spec;
    spec.file = file;
    spec.fd = ready_fd;
    spec.bytes = memory.bytes.size();
    spec.sha256 = digest;
    struct stat snapshot{};
    Memory::stat(&memory, -1, &snapshot);
    memory.stat_calls = 0;
    if (memory.mutate) snapshot.st_ino = 1;
    sa::FdStreambuf buffer(spec, cap, snapshot, memory.seam());
    std::istream stream(&buffer);
    sc::CsvReader reader(stream);
    Parsed result{};
    auto read = reader.read_header();
    result.lengths.push_back(read.csv_bytes);
    assert(read.csv_bytes == buffer.tracker().record_octets());
    while (read.status == sc::CsvStatus::Ok) {
        read = reader.next();
        if (read.status == sc::CsvStatus::End) break;
        result.lengths.push_back(read.csv_bytes);
        assert(read.csv_bytes == buffer.tracker().record_octets());
    }
    result.custody = buffer.reason();
    result.io = buffer.io_failed();
    result.status = read.status;
    result.delivered = buffer.bytes_read();
    result.row = buffer.tracker().row_index();
    result.record_octets = buffer.tracker().record_octets();
    result.complete = buffer.custody_complete();
    return result;
}

void check_fixture(const std::string& text, const std::string& hash, std::uint64_t longest,
                   std::uint64_t crossing_row) {
    Memory m{text};
    auto good = parse(m, hash, longest);
    assert(good.status == sc::CsvStatus::End);
    assert(good.custody == sa::CustodyReason::None && good.complete);
    assert(good.delivered == text.size());
    pineforge::report_sink::Sha256 independent_call_pattern;
    independent_call_pattern.update(text);
    assert(independent_call_pattern.hex_digest() == hash);
    m = Memory{text};
    auto bad = parse(m, hash, longest - 1);
    assert(bad.status == sc::CsvStatus::IoError);
    assert(bad.custody == sa::CustodyReason::RecordTooLong && !bad.complete);
    assert(bad.row == crossing_row);
    const auto preceding = std::accumulate(bad.lengths.begin(), bad.lengths.end() - 1,
                                          std::uint64_t{0});
    assert(bad.delivered == preceding + longest - 1);
}

void known_answers_and_hash_sensitivity() {
    check_fixture(kHeader, "8c16152e4bd556aa7ba69aead7982b82cc8f295ef97ea18da2ced47a86975d0b",
                  kHeader.size(), 0);
    check_fixture(kSource, kSourceHash, kHeader.size(), 0);
    check_fixture(kPrimary, kPrimaryHash, kHeader.size(), 0);
    const std::string auxiliary = kHeader + "120000,71,73,69,72,301\n"
        "180000,81,83,79,82,302\n240000,91,93,89,92,303\n";
    check_fixture(auxiliary,
        "bd2144b79b8e4be8cb18b95d8c3bd2675c9eaeac4c70fa5420aacfb4785a73af",
        kHeader.size(), 0);
    for (const auto& pair : {std::pair{kSource, std::string(kSourceHash)},
                             std::pair{kPrimary, std::string(kPrimaryHash)}}) {
        Memory m{pair.first};
        const auto volume = m.bytes.find("201") != std::string::npos
            ? m.bytes.find("201") : m.bytes.find("101");
        assert(volume != std::string::npos);
        m.bytes[volume + 2] = '9';
        auto result = parse(m, pair.second, 4096);
        assert(result.custody == sa::CustodyReason::IdentityMismatch);
        assert(!result.complete);
    }
    // Same extent as the declared Primary: only the digest can reject it.
    Memory wrong{kPrimary};
    auto result = parse(wrong, kSourceHash, 4096);
    assert(result.custody == sa::CustodyReason::IdentityMismatch);
}

void multichunk_extent_and_hashes() {
    const char* hashes[] = {
        "057b05a72f16ffe48df9370ce28f5e1f8a0c651584f1fb937d2c82ceca110b4e",
        "98d58b1aa40aeb4f20d22f97d5ec95ad3d093c5b13b8906cff384c1e9b22da20"};
    for (std::size_t extra = 0; extra != 2; ++extra) {
        const auto total = 2 * kChunkBytes + extra;
        const auto pad = total - kHeader.size() - kPrefix.size() - 1;
        const std::string text = kHeader + kPrefix + std::string(pad, 'a') + '\n';
        const auto record = text.size() - kHeader.size();
        check_fixture(text, hashes[extra], record, 1);
        for (const auto offset : {std::size_t{100}, kChunkBytes + 34464, total - 1}) {
            Memory m{text};
            m.bytes[offset] = offset == total - 1 ? 'a' : 'b';
            auto result = parse(m, hashes[extra], record);
            assert(result.custody == sa::CustodyReason::IdentityMismatch && !result.complete);
            assert(result.delivered == text.size());
        }
        Memory m{text};
        m.grow = true;
        auto growth = parse(m, hashes[extra], record);
        assert(growth.custody == sa::CustodyReason::Extended && !growth.complete);
    }
    Memory m{kSource};
    m.short_count = kHeader.size();
    m.truncate = true;
    assert(parse(m, kSourceHash, 4096).custody == sa::CustodyReason::Truncated);
    m = Memory{kSource};
    m.mutate = true;
    assert(parse(m, kSourceHash, 4096).custody == sa::CustodyReason::Mutated);
}

void record_tokenization_controls() {
    auto cr = kSource;
    std::replace(cr.begin(), cr.end(), '\n', '\r');
    check_fixture(cr, "7276dd723b4eafe3a0c80233ffa7bc5e242d905cbab9119fe75f015e62331eea",
                  kHeader.size(), 0);
    std::string crlf;
    for (char c : kSource) crlf += c == '\n' ? "\r\n" : std::string(1, c);
    check_fixture(crlf, "a81b2ef0bf02b90c2392dc26f98d11f8101b9a2eabcb06c9fe863e894dc43688",
                  kHeader.size() + 1, 0);
    const std::string header = "timestamp,open,high,low,close,volume,note\n";
    std::string quoted = "\"";
    for (int i = 0; i < 6; ++i) quoted += "aaaaaaaa\r\n";
    quoted += "x\ny\rz\"\"w\"";
    const auto first = kPrefix + quoted + '\n';
    const auto text = header + first + "60000,31,33,29,32,103,x\n";
    assert(first.size() == 89 && text.size() == 155);
    Memory m{text};
    auto good = parse(m, "", first.size(), false);
    assert(good.status == sc::CsvStatus::End && good.delivered == text.size());
    m = Memory{text};
    auto bad = parse(m, "", first.size() - 1, false);
    assert(bad.custody == sa::CustodyReason::RecordTooLong && bad.row == 1);
    assert(bad.delivered == header.size() + first.size() - 1);
    const std::string unclosed = kPrefix + '"' + [] {
        std::string s;
        for (int i = 0; i < 2048; ++i) s += "x\n";
        return s;
    }();
    for (const auto cap : {unclosed.size(), unclosed.size() - 1, std::size_t{64}}) {
        m = Memory{header + unclosed};
        const auto result = parse(m, "", cap, false);
        if (cap == unclosed.size()) assert(result.status == sc::CsvStatus::End);
        else {
            assert(result.custody == sa::CustodyReason::RecordTooLong && result.row == 1);
            assert(result.delivered == header.size() + cap && !result.complete);
        }
    }
    const char* hashes[] = {
        "d1863761e9b70a7b5efd5f093c4f27333df645992e9df0bbcda13f6168536411",
        "04ce4d8719d5bd4d8f2d150eab604c4b321c26eef3c558ad893cae4391f3828b"};
    for (std::size_t variant = 0; variant != 2; ++variant) {
        const auto pad = kChunkBytes - kHeader.size() - kPrefix.size() - 1;
        const std::string first_record = kPrefix + std::string(pad, 'a')
            + (variant == 0 ? "\r\n" : "\r");
        const std::string input = kHeader + first_record + "60000,31,33,29,32,103\n";
        assert(input[kChunkBytes - 1] == '\r');
        check_fixture(input, hashes[variant], first_record.size(), 1);
        m = Memory{input};
        m.short_count = kChunkBytes;
        good = parse(m, "", first_record.size(), false);
        assert(good.status == sc::CsvStatus::End && good.delivered == input.size());
    }
}

void exhaustive_tracker_differential() {
    constexpr char alphabet[] = {'a', '"', ',', '\r', '\n'};
    std::uint64_t cases = 0;
    std::size_t population = 1;
    for (std::size_t length = 0; length <= 7; ++length) {
        for (std::size_t code = 0; code < population; ++code) {
            std::string suffix(length, 'a');
            auto remaining = code;
            for (auto& c : suffix) { c = alphabet[remaining % 5]; remaining /= 5; }
            Memory m{kHeader + kPrefix + suffix};
            const auto result = parse(m, "", 1048576, false);
            assert(result.custody == sa::CustodyReason::None);
            if (result.status == sc::CsvStatus::End)
                assert(std::accumulate(result.lengths.begin(), result.lengths.end(),
                                       std::uint64_t{0}) == m.bytes.size());
            ++cases;
        }
        population *= 5;
    }
    assert(cases == 97656);
}

void syscall_and_descriptor_snapshot_controls() {
    Memory m{kSource};
    m.short_count = 7;
    m.interrupted = 1;
    auto good = parse(m, kSourceHash, 4096);
    assert(good.complete && good.delivered == kSource.size());
    assert(good.record_octets == 0);
    assert(good.row == static_cast<std::uint64_t>(std::count(kSource.begin(), kSource.end(), '\n')));
    m = Memory{kSource.substr(0, kSource.size() - 1)};
    good = parse(m, "", 4096, false);
    assert(good.status == sc::CsvStatus::End && good.complete);
    assert(good.row == static_cast<std::uint64_t>(std::count(m.bytes.begin(), m.bytes.end(), '\n')));
    assert(good.record_octets == m.bytes.size() - m.bytes.rfind('\n') - 1);
    m = Memory{kSource};
    m.short_count = kHeader.size();
    m.error_at = kHeader.size();
    auto bad = parse(m, kSourceHash, 4096);
    assert(bad.io && bad.delivered == kHeader.size() && !bad.complete);
    int ready[2];
    assert(::pipe(ready) == 0);
    assert(::write(ready[1], "x", 1) == 1);
    m = Memory{kSource};
    m.again = 1;
    m.short_count = 7;
    m.interrupted = 1;
    good = parse(m, "", 4096, false, ready[0]);
    assert(good.complete && good.delivered == kSource.size());
    ::close(ready[0]);
    ::close(ready[1]);
    m = Memory{kSource};
    m.short_count = 1;
    m.interrupted = 1;
    std::string snapshot, why;
    assert(sa::read_descriptor_snapshot(-1, kSourceHash, snapshot, why, m.seam())
           == sa::DecodeStatus::Ok);
    assert(snapshot == m.bytes);
    assert(sa::read_descriptor_snapshot(-1, std::string(64, '0'), snapshot, why, m.seam())
           == sa::DecodeStatus::HashMismatch);
    m.bytes.assign(sa::kDescriptorBytes + 1, ' ');
    assert(sa::read_descriptor_snapshot(-1, std::string(64, '0'), snapshot, why, m.seam())
           == sa::DecodeStatus::TooLarge);
    m = Memory{"{}"};
    m.error_at = 0;
    assert(sa::read_descriptor_snapshot(-1, std::string(64, '0'), snapshot, why, m.seam())
           == sa::DecodeStatus::Invalid);
    assert(why == "helper:descriptor_invalid");
    struct Capture {
        std::string bytes;
        unsigned calls = 0;
        int interrupted = 1, again = 1;
        static ssize_t write(void* ctx, int, const void* data, std::size_t size) {
            auto& c = *static_cast<Capture*>(ctx);
            ++c.calls;
            if (c.interrupted) { --c.interrupted; errno = EINTR; return -1; }
            if (c.again) { --c.again; errno = EAGAIN; return -1; }
            const auto count = std::min<std::size_t>(size, 5);
            c.bytes.append(static_cast<const char*>(data), count);
            return static_cast<ssize_t>(count);
        }
    } capture;
    int writable[2];
    assert(::pipe(writable) == 0);
    sa::Seam writes;
    writes.ctx = &capture;
    writes.write = &Capture::write;
    assert(sa::write_all(writable[1], kSource, writes));
    assert(capture.bytes == kSource && capture.calls > kSource.size() / 5);
    assert(capture.interrupted == 0 && capture.again == 0);
    ::close(writable[0]);
    ::close(writable[1]);
}

struct FixtureFile {
    std::string path;
    int fd = -1;
    explicit FixtureFile(const std::string& bytes) {
        static unsigned sequence = 0;
        path = "selected-admit-" + std::to_string(::getpid()) + "-"
            + std::to_string(++sequence) + ".csv";
        const int writer = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(writer >= 0);
        assert(sa::write_all(writer, bytes));
        assert(::close(writer) == 0);
        fd = ::open(path.c_str(), O_RDONLY);
        assert(fd >= 3);
    }
    ~FixtureFile() { if (fd >= 0) ::close(fd); }
    FixtureFile(const FixtureFile&) = delete;
    FixtureFile& operator=(const FixtureFile&) = delete;
};

void actual_fd_preflight_and_growth() {
    FixtureFile fixture(kSource);
    sa::StreamSpec spec;
    spec.fd = fixture.fd;
    spec.bytes = kSource.size();
    spec.sha256 = kSourceHash;
    struct stat initial{};
    assert(sa::preflight_file(spec.fd, spec, sa::Seam{}, &initial) == sa::CustodyReason::None);
    for (auto extent : {spec.bytes - 1, spec.bytes + 1}) {
        auto wrong = spec;
        wrong.bytes = extent;
        assert(sa::preflight_file(spec.fd, wrong) == sa::CustodyReason::ExtentMismatch);
    }
    const int directory = ::open(".", O_RDONLY | O_DIRECTORY);
    assert(directory >= 0);
    assert(sa::preflight_file(directory, spec) == sa::CustodyReason::FdType);
    ::close(directory);
    assert(sa::preflight_file(directory, spec) == sa::CustodyReason::FdInvalid);
    const int writer = ::open(fixture.path.c_str(), O_WRONLY);
    assert(writer >= 0);
    assert(sa::preflight_file(writer, spec) == sa::CustodyReason::FdMode);
    ::close(writer);
    for (std::size_t extra = 0; extra != 2; ++extra) {
        const auto total = 2 * kChunkBytes + extra;
        const std::string text = kHeader + kPrefix
            + std::string(total - kHeader.size() - kPrefix.size() - 1, 'a') + '\n';
        FixtureFile large(text);
        spec.fd = large.fd;
        spec.bytes = text.size();
        spec.sha256 = extra == 0
            ? "057b05a72f16ffe48df9370ce28f5e1f8a0c651584f1fb937d2c82ceca110b4e"
            : "98d58b1aa40aeb4f20d22f97d5ec95ad3d093c5b13b8906cff384c1e9b22da20";
        assert(sa::preflight_file(spec.fd, spec, sa::Seam{}, &initial) == sa::CustodyReason::None);
        struct Growth {
            int writer;
            std::uint64_t extent;
            bool appended = false;
            static ssize_t read(void* ctx, int fd, void* out, std::size_t n, off_t at) {
                auto& g = *static_cast<Growth*>(ctx);
                const auto count = ::pread(fd, out, n, at);
                if (!g.appended && count > 0
                    && static_cast<std::uint64_t>(at) + static_cast<std::uint64_t>(count) == g.extent) {
                    assert(::pwrite(g.writer, "a", 1, static_cast<off_t>(g.extent)) == 1);
                    g.appended = true;
                }
                return count;
            }
        } growth{::open(large.path.c_str(), O_WRONLY), spec.bytes};
        assert(growth.writer >= 0);
        sa::Seam seam;
        seam.ctx = &growth;
        seam.pread = &Growth::read;
        sa::FdStreambuf buffer(spec, text.size() - kHeader.size(), initial, seam);
        std::istream input(&buffer);
        sc::CsvReader reader(input);
        assert(reader.read_header().status == sc::CsvStatus::Ok);
        assert(reader.next().status == sc::CsvStatus::Ok);
        assert(reader.next().status == sc::CsvStatus::IoError);
        assert(growth.appended && buffer.reason() == sa::CustodyReason::Extended);
        assert(!buffer.custody_complete() && buffer.bytes_read() == text.size());
        ::close(growth.writer);
    }
}

void selected_callback_passthrough() {
    static_assert(std::is_trivially_copyable_v<sa::CountingState>);
    static_assert(std::is_trivially_destructible_v<sa::CountingState>);
    static_assert(sizeof(sa::CountingState) <= 4 * sizeof(std::uint64_t));
    sa::CountingState count;
    auto [bars, first, last, present] = count;
    assert(bars == 0 && first == 0 && last == 0 && !present);
    FixtureFile source(kSource), primary(kPrimary);
    sa::Descriptor d;
    d.max_record_bytes = 4096;
    d.bounds.source_rows = {true, 4};
    sa::StreamSpec s;
    s.id = "s"; s.fd = source.fd; s.bytes = kSource.size(); s.sha256 = kSourceHash;
    sa::StreamSpec p;
    p.id = "p"; p.role = sc::Role::Primary; p.fd = primary.fd;
    p.bytes = kPrimary.size(); p.sha256 = kPrimaryHash;
    d.streams = {s, p};
    for (const auto bound : {std::uint64_t{0}, std::uint64_t{3}}) {
        d.bounds.source_rows.value = bound;
        const auto out = sa::execute_stage_selected(d);
        assert(out.exit == 21 && out.reason == "selected_source_rows_limit_exceeded");
        assert(out.failure.args);
        assert(*out.failure.args == "{\"limit\":\"" + std::to_string(bound)
               + "\",\"requested\":\"1\",\"used\":\"" + std::to_string(bound) + "\"}");
    }
    d.bounds.source_rows.value = 4;
    sa::Seam seam;
    seam.on_bar = [](void*, std::uint64_t) { throw std::runtime_error("callback failed"); };
    auto out = sa::execute_stage_selected(d, seam);
    assert(out.exit == 70 && out.status == "internal_error");
    seam.on_bar = [](void*, std::uint64_t) {
        throw pineforge::coded<std::logic_error>(pineforge::RunFailureCode::engine_invariant,
                                                {}, "callback invariant");
    };
    out = sa::execute_stage_selected(d, seam);
    assert(out.exit == 70 && out.reason == "engine_invariant");
    assert((out.failure.args ? *out.failure.args : std::string("{}")) == "{}");
    seam.on_bar = [](void*, std::uint64_t) {
        throw pineforge::coded<std::logic_error>(pineforge::RunFailureCode::engine_invariant,
            {{"reason", "order_key_hash_collision"}}, "callback invariant with arguments");
    };
    out = sa::execute_stage_selected(d, seam);
    assert(out.exit == 70 && out.failure.args);
    assert(*out.failure.args == "{\"reason\":\"order_key_hash_collision\"}");
    seam = {};
    seam.pread = [](void*, int, void*, std::size_t, off_t) -> ssize_t { throw std::bad_alloc(); };
    out = sa::execute_stage_selected(d, seam);
    assert(out.exit == 70 && out.reason == "helper:bad_alloc");
    seam = {};
    seam.write = [](void*, int, const void*, std::size_t) -> ssize_t { errno = EIO; return -1; };
    assert(sa::run_stage_selected(d, -1, seam) == 32);
}

void selected_five_code_callback_family() {
    constexpr std::int64_t base = 1704067200000;
    const std::array<std::string, 6> text{{
        kHeader + "1704067140000,11,13,9,12,101\n1704067200000,21,23,19,22,102\n",
        kHeader + "1704067200000,51,53,49,52,201\n1704067260000,61,63,59,62,202\n",
        kHeader + "1704067320000,31,33,29,32,301\n",
        kHeader + "1704068160000,41,43,39,42,103\n",
        kHeader + "1704067500000,71,73,69,72,203\n1704067560000,81,83,79,82,204\n"
                  "1704067800000,91,93,89,92,205\n",
        kHeader + "1704067380000,91,93,89,92,303\n1704067440000,101,103,99,102,304\n",
    }};
    const std::array<sc::Role, 6> roles{{sc::Role::Source, sc::Role::Primary,
        sc::Role::Auxiliary, sc::Role::Source, sc::Role::Primary, sc::Role::Auxiliary}};
    std::array<std::unique_ptr<FixtureFile>, 6> files;
    sa::Descriptor d;
    d.max_record_bytes = 4096;
    d.calendar.start_ms = base + 5 * 60000;
    d.calendar.end_ms = base + 15 * 60000;
    d.calendar.fed_start_ms = base;
    d.calendar.preroll_bars = 1;
    d.calendar.input_tf = "1";
    d.calendar.script_tf = "5";
    d.calendar.chart_timezone = d.calendar.engine_timezone = "UTC";
    d.calendar.session = "24x7";
    std::array<std::uint64_t, 5> totals{};
    std::set<std::int64_t> buckets;
    for (std::size_t i = 0; i < text.size(); ++i) {
        files[i] = std::make_unique<FixtureFile>(text[i]);
        pineforge::report_sink::Sha256 hash;
        hash.update(text[i]);
        sa::StreamSpec spec;
        spec.id = std::to_string(i);
        spec.fd = files[i]->fd;
        spec.role = roles[i];
        spec.bytes = text[i].size();
        spec.sha256 = hash.hex_digest();
        d.streams.push_back(spec);
        totals[4] += text[i].size();
        const auto rows = static_cast<std::uint64_t>(std::count(text[i].begin(), text[i].end(), '\n') - 1);
        const auto slot = roles[i] == sc::Role::Source ? 0U
            : roles[i] == sc::Role::Primary ? 1U : 3U;
        totals[slot] += rows;
        if (roles[i] == sc::Role::Primary) {
            std::istringstream lines(text[i]);
            std::string line;
            std::getline(lines, line);
            while (std::getline(lines, line))
                buckets.insert((std::stoll(line.substr(0, line.find(','))) - base) / 300000);
        }
    }
    totals[2] = buckets.size();
    const auto set_bounds = [&] {
        d.bounds = {{true, totals[0]}, {true, totals[1]}, {true, totals[2]},
                    {true, totals[3]}, {true, totals[4]}};
    };
    set_bounds();
    auto completed = sa::execute_stage_selected(d);
    assert(completed.exit == 0 && completed.sink.bars == totals[1]);
    const std::array<pineforge::RunFailureCode, 5> codes{{
        pineforge::RunFailureCode::selected_source_rows_limit_exceeded,
        pineforge::RunFailureCode::selected_primary_rows_limit_exceeded,
        pineforge::RunFailureCode::selected_script_bars_limit_exceeded,
        pineforge::RunFailureCode::selected_auxiliary_rows_limit_exceeded,
        pineforge::RunFailureCode::selected_bytes_limit_exceeded,
    }};
    for (std::size_t index = 0; index < codes.size(); ++index) {
        set_bounds();
        const std::array<sc::Bound*, 5> bounds{{&d.bounds.source_rows, &d.bounds.primary_rows,
            &d.bounds.script_bars, &d.bounds.auxiliary_rows, &d.bounds.bytes}};
        assert(bounds[index]->value > 0);
        --bounds[index]->value;
        const auto out = sa::execute_stage_selected(d);
        assert(out.exit == 21 + static_cast<int>(index));
        assert(out.failure.code == codes[index] && out.failure.args);
        const auto requested = index == 4
            ? text.back().size() - text.back().rfind('\n', text.back().size() - 2) - 1
            : std::uint64_t{1};
        const auto used = index == 4 ? totals[4] - requested : bounds[index]->value;
        const auto args = "{\"limit\":\"" + std::to_string(bounds[index]->value)
            + "\",\"requested\":\"" + std::to_string(requested) + "\",\"used\":\""
            + std::to_string(used) + "\"}";
        assert(*out.failure.args == args);
        assert(pineforge::run_failure_code_class(out.failure.code) == pineforge::RunFailureClass::resource);
        assert(!pineforge::run_failure_code_retryable(out.failure.code));
    }
}

void source_callback_and_retry_controls() {
    int producer[2], forward[2], result_pipe[2];
    assert(::pipe(producer) == 0 && ::pipe(forward) == 0 && ::pipe(result_pipe) == 0);
    assert(::write(producer[1], "x", 1) == 1);
    struct Context {
        Memory memory;
        int forward_fd = -1, result_fd = -1;
        std::string forwarded, result;
        std::size_t fail_after = std::numeric_limits<std::size_t>::max();
        bool retry = false;
        unsigned forward_calls = 0, result_calls = 0;
        static ssize_t read(void* ctx, int fd, void* out, std::size_t cap) {
            return Memory::read(&static_cast<Context*>(ctx)->memory, fd, out, cap);
        }
        static ssize_t write(void* ctx, int fd, const void* data, std::size_t cap) {
            auto& c = *static_cast<Context*>(ctx);
            const bool forwarding = fd == c.forward_fd;
            assert(forwarding || fd == c.result_fd);
            auto& calls = forwarding ? c.forward_calls : c.result_calls;
            ++calls;
            if (c.retry && calls == 1) { errno = EINTR; return -1; }
            if (c.retry && calls == 2) { errno = EAGAIN; return -1; }
            auto& output = forwarding ? c.forwarded : c.result;
            if (forwarding && output.size() >= c.fail_after) { errno = EIO; return -1; }
            auto count = c.retry ? std::min<std::size_t>(cap, 7) : cap;
            if (forwarding) count = std::min(count, c.fail_after - output.size());
            output.append(static_cast<const char*>(data), count);
            return static_cast<ssize_t>(count);
        }
        sa::Seam seam() {
            sa::Seam s;
            s.ctx = this;
            s.read = &read;
            s.write = &write;
            return s;
        }
    } context{Memory{kSource}, forward[1], result_pipe[1]};
    sa::Descriptor d;
    d.stage = sa::Stage::Source;
    d.max_record_bytes = 4096;
    d.bounds.source_rows = {true, 4};
    sa::StreamSpec stream;
    stream.id = "source";
    stream.fd = producer[0];
    stream.file = false;
    stream.forward_fd = forward[1];
    d.streams = {stream};
    auto outcome = sa::execute_stage_source(d, context.seam());
    assert(outcome.exit == 0 && outcome.custody_complete);
    assert(outcome.native.coverage.source_rows
           == static_cast<std::size_t>(std::count(kSource.begin(), kSource.end(), '\n') - 1));
    assert(outcome.streams[0].bytes_read == kSource.size());
    assert(outcome.forwarded_bytes == kSource.size() && context.forwarded == kSource);
    assert(outcome.forward_sha256 == kSourceHash);
    const auto baseline = sa::render_result(outcome);
    context = Context{Memory{kSource}, forward[1], result_pipe[1]};
    context.retry = true;
    context.memory.short_count = 7;
    context.memory.interrupted = 1;
    context.memory.again = 1;
    assert(sa::run_stage_source(d, result_pipe[1], context.seam()) == 0);
    assert(context.result == baseline && context.forwarded == kSource);
    for (const bool bytes : {false, true}) {
        context = Context{Memory{kSource}, forward[1], result_pipe[1]};
        d.bounds = {};
        if (bytes) d.bounds.bytes = {true, kSource.size() - 1};
        else d.bounds.source_rows = {true, static_cast<std::uint64_t>(
            std::count(kSource.begin(), kSource.end(), '\n') - 2)};
        outcome = sa::execute_stage_source(d, context.seam());
        const auto last_start = kSource.rfind('\n', kSource.size() - 2) + 1;
        assert(outcome.exit == (bytes ? 25 : 21) && outcome.failure.args);
        const auto limit = bytes ? d.bounds.bytes.value : d.bounds.source_rows.value;
        const auto used = bytes ? last_start : limit;
        const auto requested = bytes ? kSource.size() - last_start : 1;
        assert(*outcome.failure.args == "{\"limit\":\"" + std::to_string(limit)
            + "\",\"requested\":\"" + std::to_string(requested) + "\",\"used\":\""
            + std::to_string(used) + "\"}");
        assert(outcome.forwarded_bytes == last_start && context.forwarded == kSource.substr(0, last_start));
        assert(!outcome.custody_complete && outcome.streams[0].bytes_read == kSource.size());
    }
    context = Context{Memory{kSource}, forward[1], result_pipe[1]};
    context.fail_after = kHeader.size() + 3;
    d.bounds = {};
    d.bounds.source_rows = {true, 4};
    outcome = sa::execute_stage_source(d, context.seam());
    assert(outcome.exit == 32 && outcome.reason == "helper:forward_write_failed");
    assert(outcome.forwarded_bytes == kHeader.size());
    assert(context.forwarded == kSource.substr(0, kHeader.size() + 3));
    assert(outcome.forward_sha256
           == "8c16152e4bd556aa7ba69aead7982b82cc8f295ef97ea18da2ced47a86975d0b");
    context = Context{Memory{kSource}, forward[1], result_pipe[1]};
    context.memory.short_count = kHeader.size();
    context.memory.error_at = kHeader.size();
    outcome = sa::execute_stage_source(d, context.seam());
    assert(outcome.exit == 32 && outcome.reason == "input_read_failure");
    assert(outcome.native.coverage.source_bytes == kHeader.size());
    assert(outcome.streams[0].bytes_read == kHeader.size());
    assert(outcome.row_index == 1 && outcome.forwarded_bytes == kHeader.size());
    context = Context{Memory{kSource}, forward[1], result_pipe[1]};
    auto allocation = context.seam();
    allocation.read = [](void*, int, void*, std::size_t) -> ssize_t { throw std::bad_alloc(); };
    outcome = sa::execute_stage_source(d, allocation);
    assert(outcome.exit == 70 && outcome.reason == "helper:bad_alloc");
    assert(outcome.forwarded_bytes == 0 && context.forwarded.empty());
    for (const bool crlf : {true, false}) {
        const auto pad = kChunkBytes - kHeader.size() - kPrefix.size() - 1;
        const std::string first = kPrefix + std::string(pad, 'a') + (crlf ? "\r\n" : "\r");
        const std::string input = kHeader + first + "60000,31,33,29,32,103\n";
        for (const auto cap : {first.size(), first.size() - 1}) {
            context = Context{Memory{input}, forward[1], result_pipe[1]};
            d.max_record_bytes = cap;
            outcome = sa::execute_stage_source(d, context.seam());
            assert(outcome.exit == (cap == first.size() ? 0 : 31));
            assert(outcome.streams[0].bytes_read
                   == (cap == first.size() ? input.size() : kHeader.size() + cap));
            assert(context.forwarded == (cap == first.size() ? input : kHeader));
        }
    }
    for (int fd : {producer[0], producer[1], forward[0], forward[1], result_pipe[0], result_pipe[1]})
        ::close(fd);
}
}  // namespace

int main() {
    known_answers_and_hash_sensitivity();
    multichunk_extent_and_hashes();
    record_tokenization_controls();
    exhaustive_tracker_differential();
    syscall_and_descriptor_snapshot_controls();
    actual_fd_preflight_and_growth();
    selected_callback_passthrough();
    selected_five_code_callback_family();
    source_callback_and_retry_controls();
    std::cout << "custody scenarios: 9 complete; differential cases: 97656\n";
}
