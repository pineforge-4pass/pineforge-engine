#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/native_order.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace {
namespace no = pineforge::native_order;
constexpr std::size_t resident = 2048;
constexpr std::size_t comment_bytes = 40000;
constexpr double close_price = 100.0;

struct Mismatch {};

template<class A, class B>
void compare(const char* verdict, const char* field, const A& actual, const B& expected) {
    if (actual == expected) return;
    std::cerr << verdict << ": " << field << " expected=" << expected << " actual=" << actual << '\n';
    throw Mismatch{};
}

// Read the journal itself, with the same independent oracle before and after spill.
void check_big(const no::WorkingRequestCore& core, const char* verdict) {
    const auto* event = std::get_if<no::AcceptedEvent>(&core.history_at(0));
    compare(verdict, "first event is Accepted", event != nullptr, true);
    compare(verdict, "Big definition present", bool(event->definition), true);
    compare(verdict, "Big ordinal", event->ordinal, std::uint64_t{1});
    compare(verdict, "Big handle matches run and incarnation",
            event->handle() == no::RequestHandle{core.identity(), 1}, true);
    const auto& request = event->request();
    compare(verdict, "first order label", request.label, "Big");
    const auto* qty = std::get_if<no::Transact>(&request.intent);
    const auto* limit = std::get_if<no::Limit>(&request.trigger);
    compare(verdict, "quantity present", qty != nullptr, true);
    compare(verdict, "long quantity", qty->signed_units, 1.0);
    compare(verdict, "limit present", limit != nullptr, true);
    compare(verdict, "limit price", limit->price, 1.0);
    compare(verdict, "Big comment bytes", request.comment.size(), comment_bytes);
    const auto wrong = request.comment.find_first_not_of('x');
    if (wrong != std::string::npos)
        compare(verdict, "first differing Big comment byte", request.comment[wrong], 'x');
}

bool read_exact(int fd, off_t offset, void* out, std::size_t length) {
    auto* bytes = static_cast<unsigned char*>(out);
    while (length != 0) {
        const auto n = ::pread(fd, bytes, length, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        bytes += n;
        offset += n;
        length -= static_cast<std::size_t>(n);
    }
    return true;
}

std::vector<int> unlinked_files() {
    const auto limit = ::sysconf(_SC_OPEN_MAX);
    compare("NOT COMPARED", "descriptor limit available", limit > 0, true);
    std::vector<int> result;
    for (int fd = 3; fd < std::min<long>(limit, 4096); ++fd) {
        struct stat st{};
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_nlink == 0)
            result.push_back(fd);
    }
    return result;
}

// The first event starts the data file. Identify it by its serialized tag,
// ordinal and two runs of the seed's comment, one on each side of 32 KiB.
// Mutating a comment byte still leaves a decodable event if the seal is lost.
void check_spanning_seal(no::WorkingRequestCore& core, const std::vector<int>& before) {
    constexpr off_t offset = 36000;
    int data_fd = -1;
    unsigned matches = 0;
    for (const int fd : unlinked_files()) {
        if (std::binary_search(before.begin(), before.end(), fd)) continue;
        std::uint32_t tag;
        std::uint64_t ordinal;
        if (!read_exact(fd, 0, &tag, sizeof tag)
            || !read_exact(fd, sizeof tag, &ordinal, sizeof ordinal)
            || tag != no::CommandEvent{no::AcceptedEvent{}}.index() || ordinal != 1) continue;
        std::array<char, 64> bytes;
        const auto comment_at = [&](off_t at) {
            return read_exact(fd, at, bytes.data(), bytes.size())
                && std::all_of(bytes.begin(), bytes.end(), [](char c) { return c == 'x'; });
        };
        if (comment_at(4096) && comment_at(offset)) { ++matches; data_fd = fd; }
    }
    compare("NOT COMPARED", "identified spanning data files", matches, 1u);
    const auto put = [&](char value) {
        ssize_t n;
        do { n = ::pwrite(data_fd, &value, 1, offset); } while (n < 0 && errno == EINTR);
        compare("NOT COMPARED", "fault fixture bytes written", n, 1);
        char observed = 0;
        compare("NOT COMPARED", "fault fixture readback available",
                read_exact(data_fd, offset, &observed, 1), true);
        compare("NOT COMPARED", "fault fixture readback byte", observed, value);
    };
    put('y');
    bool refused = false;
    try {
        (void)core.history_at(0);
    } catch (const pineforge::detail::ScratchIoError& e) {
        const auto& failure = e.run_failure();
        refused = e.code() == std::error_code(EIO, std::generic_category())
            && failure.code == pineforge::RunFailureCode::report_sink_failed && failure.args
            && failure.args->find("\"reason\":\"scratch_io\"") != std::string::npos
            && std::string(e.what()).find("journal page does not read back as written")
                != std::string::npos;
    }
    put('x');
    compare("FAIL", "changed second transfer refused with the page-seal error", refused, true);
    check_big(core, "FAIL");
}

int reproduce(const std::string& control) {
    const bool damage = control == "--damage-spanning";
    if (damage && pineforge::detail::ScratchArena::attached()) {
        std::cerr << "SKIP: spanning fault requires a journal file descriptor; arena attached\n";
        return 77;
    }
    const auto prior_files = damage ? unlinked_files() : std::vector<int>{};
    no::WorkingRequestCore core{no::RunIdentity{"issue-937", 1}};
    std::uint64_t incarnation = 1, ordinal = 1;
    if (control != "--empty") {
        no::Request big{no::Transact{1.0}, "Big", std::string(comment_bytes, 'x'),
                        no::Limit{close_price * 0.01}};
        if (control == "--wrong-input") big.comment.back() = 'y';
        const auto result = core.submit(big, 0, incarnation, ordinal);
        compare("NOT COMPARED", "Big accepted", result.status == no::SubmitStatus::Accepted, true);
    }
    compare("NOT COMPARED", "seed event count", core.history_end(), std::size_t{1});
    check_big(core, "NOT COMPARED");
    const auto before = reinterpret_cast<std::uintptr_t>(&core.history_at(0));

    // The script keeps two unrelated one-unit limit orders working. On each
    // later use of the same label, replace that order via its current handle.
    std::array<no::RequestHandle, 2> working{};
    const auto last_bar = resident + (damage ? 1 : 0);
    for (std::size_t bar = 0; bar <= last_bar; ++bar) {
        const auto id = bar % 2;
        const bool setup = bar + 1 < resident;
        const char* verdict = setup ? "NOT COMPARED" : "FAIL";
        const auto expected_ordinal = bar + 2;
        const no::Request order{no::Transact{1.0}, id == 0 ? "o0" : "o1", "",
                                no::Limit{close_price * 0.01}};
        try {
            if (bar < working.size()) {
                const auto result = core.submit(order, bar, incarnation, ordinal);
                compare(verdict, "short order accepted", result.status == no::SubmitStatus::Accepted, true);
                compare(verdict, "short order handle present", bool(result.handle), true);
                working[id] = *result.handle;
            } else {
                const auto result = core.replace(working[id], order, bar, incarnation, ordinal);
                compare(verdict, "short order replaced", result.status == no::ReplaceStatus::Replaced, true);
                compare(verdict, "short order successor present", bool(result.successor), true);
                compare(verdict, "replacement ordinal", result.event_ordinal, expected_ordinal);
                working[id] = *result.successor;
            }
        } catch (const pineforge::detail::ScratchIoError& e) {
            if (!setup && e.code() == std::error_code(EFBIG, std::generic_category()) &&
                std::string(e.what()).find("native o1 journal event exceeds the spill page byte bound")
                    != std::string::npos) {
                std::cerr << "FAIL: command " << expected_ordinal
                          << " expected=Replaced actual=journal spill EFBIG retained="
                          << core.history_end() - core.history_base() << '\n';
                return 1;
            }
            throw;
        }
        compare(verdict, "retained journal base", core.history_base(), std::size_t{0});
        compare(verdict, "committed journal length", core.history_end(), expected_ordinal);
        if (!setup) {
            const auto* row = std::get_if<no::ReplacedEvent>(&core.history_at(bar + 1));
            compare(verdict, "new journal event is Replaced", row != nullptr, true);
            compare(verdict, "new definition present", bool(row->successor_definition), true);
            compare(verdict, "new event ordinal", row->ordinal, expected_ordinal);
            compare(verdict, "new order label", row->successor_request().label, order.label);
            compare(verdict, "new order handle matches result", row->successor() == working[id], true);
        }
        if (damage && bar == resident) check_spanning_seal(core, prior_files);
    }
    compare("FAIL", "Big evicted from resident slot",
            reinterpret_cast<std::uintptr_t>(&core.history_at(0)) != before, true);
    check_big(core, "FAIL");
    if (damage)
        std::cout << "PASS: changed second transfer refused; Big restored; command 2051 replaced o1\n";
    else
        std::cout << "PASS: 2050 retained events; Big reloaded with 40000 x bytes; o1 and o0 replaced\n";
    return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string control = argc == 2 ? argv[1] : "";
        compare("NOT COMPARED", "test arguments valid",
                argc == 1 || (argc == 2 && (control == "--empty" || control == "--wrong-input"
                                            || control == "--damage-spanning")), true);
        return reproduce(control);
    } catch (const Mismatch&) {
        return 1;
    } catch (const std::exception& e) {
        std::cerr << "NOT COMPARED: journal operation expected=available actual=" << e.what() << '\n';
        return 1;
    }
}
