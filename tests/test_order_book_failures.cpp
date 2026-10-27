// CR-307 C0182. Expected Release wall time <5s (not measured here).
// Open/exhaustion checks are portable POSIX; Linux additionally injects actual
// pread/pwrite failures in this executable only, with no production fault hook.
#include "order_book_fixture.hpp"
#include <pineforge/detail/order_book_storage.hpp>

#include <cerrno>
#include <cstdio>
#include <system_error>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

using namespace r4_test;
using namespace order_book_test;

#if defined(__linux__)
namespace order_book_fault {
enum class Write { none, any, first_id_node };
inline bool read = false;
inline Write write = Write::none;
}
// Preserve the ordinary syscall semantics when unarmed. LP64 lab hosts use
// one 64-bit positional offset for these Linux syscalls.
extern "C" ssize_t pread(int fd, void* data, size_t length, off_t offset) {
    if (std::exchange(order_book_fault::read, false)) {
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pread64, fd, data, length, offset));
}
extern "C" ssize_t pwrite(int fd, const void* data, size_t length, off_t offset) {
    using order_book_fault::Write;
    if (order_book_fault::write == Write::any
        || (order_book_fault::write == Write::first_id_node && offset == 1 && length == 56)) {
        order_book_fault::write = Write::none;
        errno = EIO;
        return -1;
    }
    return static_cast<ssize_t>(::syscall(SYS_pwrite64, fd, data, length, offset));
}
#endif

namespace {
using Store = detail::OrderBookStorage<std::string>;
detail::OrderBookEntry entry(std::uint64_t incarnation) {
    return {{no::RunIdentity{"cr307-store-fault", 1}, incarnation}, false};
}
struct Environment {
    bool present = std::getenv("TMPDIR") != nullptr;
    std::string old = present ? std::getenv("TMPDIR") : "";
    Environment() { REQUIRE(::setenv("TMPDIR", "/dev/null/order-book-refusal", 1) == 0); }
    ~Environment() {
        if (present) ::setenv("TMPDIR", old.c_str(), 1);
        else ::unsetenv("TMPDIR");
    }
};
template<class F> void io_failure(F operation, int expected) {
    bool caught = false;
    try { operation(); }
    catch (const detail::AuxiliaryScratchIoError& error) {
        caught = true;
        CHECK(error.code().value() == expected);
        CHECK(classify_run_failure(error).code == RunFailureCode::scratch_io_failed);
    }
    REQUIRE(caught);
}

void lazy_open_refusal() {
    Environment invalid;
    Store store; // construction, unknown lookups and erase must remain lazy
    CHECK(!store.find("unknown"));
    store.erase("unknown");
    CHECK(store.size() == 0 && store.resident_size() == 0);
    io_failure([&] { store.put("", entry(1)); }, ENOTDIR);
    CHECK(store.size() == 0 && store.resident_size() == 0);
    CHECK(!store.find(""));
}

void arena_exhaustion() {
    Arena arena(detail::ScratchArena::kChunkBytes);
    Store store;
    // One record is 56+32+L, plus the null byte; L=one MiB cannot fit.
    // Growth must propagate the finite-backing failure, never prune/fallback.
    bool caught = false;
    try { store.put(std::string(detail::ScratchArena::kChunkBytes, 'x'), entry(1)); }
    catch (const detail::ReportScratchIoError& error) {
        caught = true;
        CHECK(classify_run_failure(error).code == RunFailureCode::report_sink_failed);
    }
    REQUIRE(caught);
    CHECK(store.size() == 0 && store.resident_size() == 0);
}

#if defined(__linux__)
void read_write_and_copy_refusals() {
    Store store;
    for (std::size_t i = 0; i < 65; ++i) store.put(key(i), entry(i + 1));
    REQUIRE(store.size() == 65 && store.resident_size() == 64);
    order_book_fault::read = true;
    io_failure([&] { (void)store.find(key(0)); }, EIO);
    CHECK(!order_book_fault::read);
    CHECK(store.find(key(0))->handle == entry(1).handle);
    order_book_fault::write = order_book_fault::Write::any;
    io_failure([&] { store.put(key(0), entry(900)); }, EIO);
    CHECK(store.find(key(0))->handle == entry(1).handle);
    CHECK(store.size() == 65);
    order_book_fault::write = order_book_fault::Write::any;
    io_failure([&] { store.erase(key(0)); }, EIO);
    CHECK(store.find(key(0))->handle == entry(1).handle && store.size() == 65);
    order_book_fault::write = order_book_fault::Write::any;
    io_failure([&] { store.put("new", entry(901)); }, EIO);
    CHECK(!store.find("new") && store.size() == 65);

    Store destination;
    destination.put("destination", entry(42));
    order_book_fault::read = true;
    io_failure([&] { destination = store; }, EIO);
    CHECK(destination.size() == 1);
    CHECK(destination.find("destination")->handle == entry(42).handle);
    CHECK(store.size() == 65 && store.find(key(64))->handle == entry(65).handle);
}

void accepted_command_then_store_write_failure() {
    Host host;
    Book book(host);
    host.beginning = [&](Host& base) {
        // Warm the host's own scratch before arming the new id-node failure.
        const auto warm = put(base, resting());
        REQUIRE(base.cancel(warm).status == no::CancelStatus::Cancelled);
        const auto accepted_before = events<no::AcceptedEvent>(base).size();
        order_book_fault::write = order_book_fault::Write::first_id_node;
        io_failure([&] { (void)book.submit_or_replace("fails-after-submit", resting()); }, EIO);
        CHECK(order_book_fault::write == order_book_fault::Write::none);
        const auto accepted = events<no::AcceptedEvent>(base);
        REQUIRE(accepted.size() == accepted_before + 1);
        // Kernel accepted, but the book did not fabricate a successful result
        // or publish a binding whose write failed.
        CHECK(book.empty() && resident(book) == 0);
        REQUIRE(base.native_working_requests().size() == 1);
        REQUIRE(base.cancel(accepted.back().handle()).status == no::CancelStatus::Cancelled);
    };
    run(host, spec("cr307-post-command-io"), {100.0});
    completed(host);
}
#endif
}

int main() {
    test("lazy scratch open failure", lazy_open_refusal);
    test("finite arena exhaustion", arena_exhaustion);
#if defined(__linux__)
    test("scratch read write and copy failures", read_write_and_copy_refusals);
    test("accepted command then failed binding write", accepted_command_then_store_write_failure);
#endif
    std::printf("OrderBook failures: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
