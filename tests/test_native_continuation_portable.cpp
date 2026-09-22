// R5 follow-up lane E23: the continuation digest names the run's INPUTS.
//
// `native_continuation_hash()` folds the run's resolved timezone identity.
// Until this lane that fold carried `zoneinfo_root` and every entry of
// `resource_paths` -- absolute paths on the machine that ran it: on glibc
// whatever `$TZDIR` names (`/usr/share/zoneinfo` when it names nothing), on
// Darwin the realpath of `/var/db/timezone/zoneinfo`, which carries the
// installed tzdata release (`/private/var/db/timezone/tz/2026c.1.0/zoneinfo`).
// So the same spec over the same bars, booking the same trades, answered a
// different digest on two machines, and moving `$TZDIR` to a copy of the same
// tree on ONE machine moved it too: R5 INT13b measured 3087945503312965943
// against 101917661033736583 for one binary on one host with nothing but
// `$TZDIR` between the two runs. A digest like that can never be a resume
// token compared across hosts, a CI baseline or a cross-repo constant.
//
// What a zone contributes to a run is its IDENTITY: the kind of source it is,
// the definition it resolved to, and the CONTENT of the files the resolver
// read. That content is `TimezoneIdentityDescriptor::resource_digest`, and it
// is what the consumer folds now; the two path members stay on the descriptor
// as diagnostics. Two hosts carrying the same tzdata release then agree on the
// digest whatever their zoneinfo root is called, while a tzdata update that
// rewrites the zone's rules still moves it -- which is the honest behaviour,
// because such a run really did read different rules.
//
// The rows:
//   1. the digest IS the bytes. `resource_digest` equals a reference FNV-1a 64
//      written in this file over the same files, and flipping one byte of
//      those bytes moves the reference. The reference is independent by
//      construction -- the shape `test_native_live_pending_order_mirror_l4d`
//      uses to pin `id_hash64`.
//   2. a moved tree keeps the digest. With `$TZDIR` pointing at a copy of the
//      zone's resources in a different directory, the descriptor's root and
//      path move while `resource_digest` and the run's continuation digest do
//      not. On a libc that does not read `$TZDIR` (Darwin: tzset(3) reads
//      /var/db/timezone/zoneinfo and documents no override) the row asserts
//      that instead -- the root did not move, so neither did the digest.
//   3. different rules move the digest. With `$TZDIR` pointing at a tree whose
//      file for the run's zone holds ANOTHER zone's rules, both the resource
//      digest and the continuation digest move. Where the libc ignores
//      `$TZDIR` the same claim is carried by row 1's pin plus the two zones'
//      own byte folds: the digest IS the fold over the bytes, and those two
//      byte strings fold differently.
//   4. the printed lines are the cross-host measurement. The same resource and
//      continuation digests must come out of this binary on macOS/arm64 and on
//      Linux/aarch64 with the same tzdata release. They are printed, never
//      pinned as literals: a literal would pin one tzdata release, and the
//      release is an input this digest is supposed to see.
//
// Fail-before (this lane's base e9ad37dd) -- first diagnostic:
//   tests/test_native_continuation_portable.cpp:... error: no member named
//   'resource_digest' in 'pineforge::native_calendar::TimezoneIdentityDescriptor'
// and with the path fold still in place the pre-change probe on a glibc host
// printed two DIFFERENT continuation digests for one run under two roots
// holding the same bytes (exec/E23-probes/fail_before.cpp).
#include "native_current_fixture.hpp"

#include <pineforge/native_calendar.hpp>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace r4_test;

namespace {

using pineforge::native_calendar::TimezoneIdentityDescriptor;
using pineforge::native_calendar::TimezoneSourceKind;
using pineforge::native_calendar::timezone_identity_descriptor;

constexpr const char* kZoneDir = "America";
constexpr const char* kZoneLeaf = "New_York";
constexpr const char* kZone = "America/New_York";
constexpr const char* kOtherZone = "Asia/Taipei";

// ── the reference fold ──────────────────────────────────────────────────
// FNV-1a 64 written here from its constants, not called out of the engine:
// offset basis 1469598103934665603, prime 1099511628211, a length-prefixed
// blob per file and the file count first.
struct Ref {
    std::uint64_t h = 1469598103934665603ULL;
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const unsigned char*>(p);
        for (std::size_t i = 0; i < n; ++i) {
            h ^= c[i];
            h *= 1099511628211ULL;
        }
    }
    void u(std::uint64_t v) { bytes(&v, sizeof v); }
    void blob(const std::string& s) {
        u(static_cast<std::uint64_t>(s.size()));
        bytes(s.data(), s.size());
    }
};

std::uint64_t reference_digest(const std::vector<std::string>& contents) {
    Ref r;
    r.u(static_cast<std::uint64_t>(contents.size()));
    for (const std::string& c : contents) r.blob(c);
    return r.h;
}

bool read_file(const std::string& path, std::string& out) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    out.clear();
    char buffer[4096];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return true;
}

bool write_file(const std::string& path, const std::string& bytes) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        written += static_cast<std::size_t>(n);
    }
    ::close(fd);
    return true;
}

// A zoneinfo root holding exactly the resources kZone reads, under a fresh
// directory. The bytes are the caller's, so the same helper builds both the
// faithful copy and the one whose file holds another zone's rules.
std::string install_root(const std::string& bytes) {
    char tmpl[] = "/tmp/pf-e23-zoneinfo-XXXXXX";
    char* root = ::mkdtemp(tmpl);
    if (root == nullptr) return {};
    const std::string dir = std::string(root) + "/" + kZoneDir;
    if (::mkdir(dir.c_str(), 0700) != 0) return {};
    if (!write_file(dir + "/" + kZoneLeaf, bytes)) return {};
    return std::string(root);
}

void remove_root(const std::string& root) {
    if (root.empty()) return;
    const std::string dir = root + "/" + kZoneDir;
    ::unlink((dir + "/" + kZoneLeaf).c_str());
    ::rmdir(dir.c_str());
    ::rmdir(root.c_str());
}

struct TzdirGuard {
    bool had = false;
    std::string old;
    TzdirGuard() {
        if (const char* v = std::getenv("TZDIR")) {
            had = true;
            old = v;
        }
    }
    void set(const std::string& v) { ::setenv("TZDIR", v.c_str(), 1); }
    ~TzdirGuard() {
        if (had) ::setenv("TZDIR", old.c_str(), 1);
        else ::unsetenv("TZDIR");
    }
};

#if defined(__GLIBC__)
constexpr bool kLibcReadsTzdir = true;
#else
constexpr bool kLibcReadsTzdir = false;
#endif

// ── the run whose identity is measured ──────────────────────────────────
struct Outcome {
    std::uint64_t continuation = 0;
    std::size_t rows = 0;
    double position = 0.0;
};

Outcome run_case(const char* zone) {
    Host host;
    host.calculation = [](Host& h) {
        if (h.calculations == 1) put(h, tx(1.0, "open"));
        if (h.calculations == 3) put(h, flat("close"));
    };
    NativeRunSpec s = spec("e23-portable");
    s.timezone = zone;
    Outcome out;
    REQUIRE(host.configure_native(s).status == NativeSetupStatus::Applied);
    const std::vector<Bar> bars{
        {100.0, 100.5, 99.5, 100.0, 1.0, T},
        {100.0, 101.0, 100.0, 100.8, 1.0, T + 60000},
        {100.8, 101.2, 100.4, 100.6, 1.0, T + 120000},
        {100.6, 100.9, 100.2, 100.4, 1.0, T + 180000},
    };
    host.run(bars.data(), static_cast<int>(bars.size()));
    completed(host);
    out.continuation = host.native_continuation_hash();
    out.rows = host.rows().size();
    out.position = host.physical_position().signed_units;
    return out;
}

// The bytes of every resource a descriptor names, read back independently.
bool resource_contents(const TimezoneIdentityDescriptor& d, std::vector<std::string>& out) {
    out.clear();
    for (const std::string& path : d.resource_paths) {
        std::string bytes;
        if (!read_file(path, bytes)) return false;
        out.push_back(std::move(bytes));
    }
    return true;
}

// ── 1. the digest is the zone's bytes ───────────────────────────────────
void the_digest_is_the_zones_bytes() {
    for (const char* zone : {"UTC", kZone, kOtherZone}) {
        auto d = timezone_identity_descriptor(zone);
        REQUIRE(d.has_value());
        CHECK(d->valid());
        std::vector<std::string> contents;
        REQUIRE(resource_contents(*d, contents));
        CHECK(d->resource_digest == reference_digest(contents));
        // The paths themselves are nowhere in it: the same bytes under any
        // name fold the same value.
        CHECK(reference_digest(contents) != 0);
        if (!contents.empty() && !contents.front().empty()) {
            std::vector<std::string> mutated = contents;
            mutated.front()[mutated.front().size() / 2] ^= 0x5A;
            CHECK(reference_digest(mutated) != d->resource_digest);
        }
    }
    // Two zones with different rules are two different digests.
    auto here = timezone_identity_descriptor(kZone);
    auto there = timezone_identity_descriptor(kOtherZone);
    REQUIRE(here.has_value());
    REQUIRE(there.has_value());
    CHECK(here->kind == TimezoneSourceKind::Tzfile);
    CHECK(there->kind == TimezoneSourceKind::Tzfile);
    CHECK(here->resource_digest != there->resource_digest);
}

// ── 2. a moved tree keeps the digest ────────────────────────────────────
void a_moved_tree_keeps_the_digest() {
    auto base = timezone_identity_descriptor(kZone);
    REQUIRE(base.has_value());
    REQUIRE(base->resource_paths.size() == 1);
    std::string bytes;
    REQUIRE(read_file(base->resource_paths.front(), bytes));
    const Outcome before = run_case(kZone);

    const std::string root = install_root(bytes);
    REQUIRE(!root.empty());
    {
        TzdirGuard guard;
        guard.set(root);
        auto moved = timezone_identity_descriptor(kZone);
        REQUIRE(moved.has_value());
        if (kLibcReadsTzdir) {
            // The tree really did move: a different root, a different file.
            CHECK(moved->zoneinfo_root != base->zoneinfo_root);
            CHECK(moved->resource_paths.size() == 1);
            CHECK(moved->resource_paths.front() != base->resource_paths.front());
        } else {
            // This libc does not read $TZDIR, so the resolver must not either:
            // the root is the same one, which is why the digest cannot move.
            CHECK(moved->zoneinfo_root == base->zoneinfo_root);
            CHECK(moved->resource_paths == base->resource_paths);
        }
        CHECK(moved->kind == base->kind);
        CHECK(moved->effective_definition == base->effective_definition);
        CHECK(moved->resource_digest == base->resource_digest);
        const Outcome after = run_case(kZone);
        CHECK(after.continuation == before.continuation);
        CHECK(after.rows == before.rows);
        CHECK(after.position == before.position);
        std::printf("  moved tree: root_moved=%d continuation=%llu (was %llu)\n",
                    moved->zoneinfo_root != base->zoneinfo_root ? 1 : 0,
                    static_cast<unsigned long long>(after.continuation),
                    static_cast<unsigned long long>(before.continuation));
    }
    remove_root(root);
}

// ── 3. different rules move the digest ──────────────────────────────────
void different_rules_move_the_digest() {
    auto base = timezone_identity_descriptor(kZone);
    auto other = timezone_identity_descriptor(kOtherZone);
    REQUIRE(base.has_value());
    REQUIRE(other.has_value());
    REQUIRE(base->resource_paths.size() == 1);
    REQUIRE(other->resource_paths.size() == 1);
    std::string base_bytes, other_bytes;
    REQUIRE(read_file(base->resource_paths.front(), base_bytes));
    REQUIRE(read_file(other->resource_paths.front(), other_bytes));
    CHECK(base_bytes != other_bytes);
    const Outcome before = run_case(kZone);

    // A tree where the run's own zone name carries another zone's rules.
    const std::string root = install_root(other_bytes);
    REQUIRE(!root.empty());
    {
        TzdirGuard guard;
        guard.set(root);
        auto swapped = timezone_identity_descriptor(kZone);
        REQUIRE(swapped.has_value());
        // The NAME is unchanged either way: only the content can carry this.
        CHECK(swapped->effective_definition == base->effective_definition);
        if (kLibcReadsTzdir) {
            CHECK(swapped->resource_digest != base->resource_digest);
            CHECK(swapped->resource_digest == other->resource_digest);
            const Outcome after = run_case(kZone);
            CHECK(after.continuation != before.continuation);
            std::printf("  swapped rules: continuation=%llu (was %llu)\n",
                        static_cast<unsigned long long>(after.continuation),
                        static_cast<unsigned long long>(before.continuation));
        } else {
            // $TZDIR cannot move this libc's tree, so the tree under the run
            // is still the system one and nothing moves. The claim is carried
            // by row 1's pin -- the digest IS the fold over the bytes -- and
            // by the two zones' folds being different values.
            CHECK(swapped->resource_digest == base->resource_digest);
            CHECK(reference_digest({other_bytes}) != reference_digest({base_bytes}));
            CHECK(reference_digest({other_bytes}) == other->resource_digest);
            std::printf("  swapped rules: $TZDIR not read by this libc; "
                        "folds %llu vs %llu\n",
                        static_cast<unsigned long long>(reference_digest({base_bytes})),
                        static_cast<unsigned long long>(reference_digest({other_bytes})));
        }
    }
    remove_root(root);
}

// ── 4. the cross-host measurement ───────────────────────────────────────
void the_cross_host_lines() {
    for (const char* zone : {"UTC", kZone}) {
        auto d = timezone_identity_descriptor(zone);
        REQUIRE(d.has_value());
        const Outcome out = run_case(zone);
        std::printf("  host facts  %-16s root=%s path=%s\n", zone,
                    d->zoneinfo_root.c_str(),
                    d->resource_paths.empty() ? "(none)" : d->resource_paths.front().c_str());
        std::printf("  PORTABLE    %-16s resource_digest=%llu continuation=%llu "
                    "rows=%zu position=%.17g\n",
                    zone, static_cast<unsigned long long>(d->resource_digest),
                    static_cast<unsigned long long>(out.continuation), out.rows, out.position);
    }
}

}  // namespace

int main() {
    test("the digest is the zone's bytes", the_digest_is_the_zones_bytes);
    test("a moved tree keeps the digest", a_moved_tree_keeps_the_digest);
    test("different rules move the digest", different_rules_move_the_digest);
    test("the cross-host lines", the_cross_host_lines);
    std::printf("%s native continuation portable: %d checks, %d failures\n",
                failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
