// Differential runs use the complete reader (zero payload budget) as well
// as the frozen in-test target oracle. Run this target WITHOUT the counting
// overlay in Release too. No expected output is recorded by the candidate.
#include "../src/native_cohort_target_work.hpp"
#include "native_cohort_target_fixture.hpp"

#include <cerrno>
#include <linux/memfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
using namespace cohort_target_test;

struct Sink {
    // Only the stream socket is given to the engine. The reader's anonymous
    // file is a test-owned byte capture, so neither disk backing nor buffering
    // limits the report witness. A process reader needs no extra link flags.
    int fd = static_cast<int>(::syscall(SYS_memfd_create, "cohort-report-capture", MFD_CLOEXEC));
    int descriptors[2]{-1, -1};
    pid_t reader = -1;
    Sink() {
        require(fd >= 0, "cannot create the anonymous report witness");
        if (::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) != 0) {
            ::close(fd);
            throw std::runtime_error("cannot create the report socket");
        }
        reader = ::fork();
        if (reader < 0) {
            ::close(descriptors[0]); ::close(descriptors[1]); ::close(fd);
            throw std::runtime_error("cannot start the report reader");
        }
        if (reader == 0) {
            ::close(descriptors[1]);
            std::array<char, 4096> buffer{};
            for (;;) {
                const auto got = ::read(descriptors[0], buffer.data(), buffer.size());
                if (got < 0 && errno == EINTR) continue;
                if (got < 0) ::_exit(1);
                if (got == 0) ::_exit(0);
                std::size_t at = 0;
                while (at < static_cast<std::size_t>(got)) {
                    const auto written = ::write(fd, buffer.data() + at,
                                                static_cast<std::size_t>(got) - at);
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) ::_exit(1);
                    at += static_cast<std::size_t>(written);
                }
            }
        }
        ::close(descriptors[0]);
        descriptors[0] = -1;
    }
    Sink(const Sink&) = delete;
    Sink& operator=(const Sink&) = delete;
    int finish_reader() noexcept {
        if (descriptors[1] >= 0) ::close(std::exchange(descriptors[1], -1));
        if (reader < 0) return 0;
        int status = 0;
        pid_t waited;
        do { waited = ::waitpid(reader, &status, 0); } while (waited < 0 && errno == EINTR);
        reader = -1;
        return waited > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    ~Sink() {
        (void)finish_reader();
        if (fd >= 0) ::close(fd);
    }
    void bind(Host& h) {
        const pf_report_sink_fd_v1 descriptor{
            sizeof(descriptor), 1, descriptors[1], "cohort-transcript"};
        require(h.set_report_sink_fd_v1(&descriptor) == 0, "report sink refused");
    }
    std::string bytes() {
        require(finish_reader() == 0, "report stream reader failed");
        const auto size = ::lseek(fd, 0, SEEK_END);
        require(size > 0, "report byte witness is empty");
        std::string out(static_cast<std::size_t>(size), '\0');
        std::size_t at = 0;
        while (at < out.size()) {
            const auto got = ::pread(fd, out.data() + at, out.size() - at, static_cast<off_t>(at));
            if (got < 0 && errno == EINTR) continue;
            require(got > 0, "report byte read failed");
            at += static_cast<std::size_t>(got);
        }
        return out;
    }
};
struct Outcome {
    std::string bytes;
    std::vector<std::vector<std::uint64_t>> events, trades;
    std::vector<std::uint64_t> continuation, broker;
    std::uint64_t core = 0;
    pf_metrics_t metrics{};
    std::size_t fills = 0, target_reads = 0, slices = 0, closed_rows = 0;
};

Outcome execute(Host& h, std::size_t budget, bool direct, std::uint64_t run_number,
                bool short_side, bool book_spill = false) {
    Outcome out;
    Probe::budget(h.consumer(), budget);
    h.consumer().set_direct_mutation(direct);
    h.set_trade_history_streamable(false);
    h.set_broker_state_hash_recording(true);
    std::vector<no::RequestHandle> closes;
    no::RequestHandle a, b;
    no::CohortHandle a_cohort, b_cohort, both;
    const double sign = short_side ? -1 : 1;
    auto check = [&](Host& host) {
        for (const auto& handle : closes) {
            if (host.consumer().request_core().find_live(handle)) {
                judge(host, live(host, handle));
                ++out.target_reads;
            }
        }
        out.continuation.push_back(host.native_continuation_hash());
        out.broker.push_back(host.broker_state_hash());
    };
    h.calculation = [&](Host& host) {
        if (host.bars == 1) {
            no::Request entry{no::Transact{3 * sign}, "sliced opening", "successor"};
            entry.capacity = no::PointBudget{1};
            // A point-budgeted market entry leaves multiple physical slices.
            a = put(host, entry);
            a_cohort = host.cohort_open();
            both = host.cohort_open();
            host.cohort_add(a_cohort, a); host.cohort_add(both, a);
            for (unsigned i = 0; i < 66; ++i) {
                const auto changed = host.replace(a, entry);
                require(changed.status == no::ReplaceStatus::Replaced && changed.successor,
                        "replacement chain did not reach the backing prefix");
                a = *changed.successor;
            }
            const no::Request second{no::Transact{2 * sign}, "outside opening", ""};
            b = put(host, second);
            b_cohort = host.cohort_open();
            host.cohort_add(b_cohort, b); host.cohort_add(both, b);
            // Push A's eventual physical successor into the root prefix,
            // rather than merely creating a file that no target reads.
            for (unsigned i = 0; i < 66; ++i) {
                const auto changed = host.replace(b, second);
                require(changed.status == no::ReplaceStatus::Replaced && changed.successor,
                        "second successor chain did not flush the first");
                b = *changed.successor;
            }
            (void)apply(host, b);
            const auto cancelled = put(host, {no::Transact{sign}, "cancel before fill", ""});
            host.cohort_add(both, cancelled);
            require(host.cancel(cancelled).status == no::CancelStatus::Cancelled,
                    "unfilled root was not cancelled");
            host.cohort_remove(both, cancelled);
            for (const auto cohort : {a_cohort, b_cohort, both}) {
                auto request = close(host, cohort, "retained cohort", false, 1.5);
                request.trigger = no::Limit{short_side ? 1.0 : 1000.0};
                closes.push_back(put(host, request));
            }
            if (book_spill) {
                // The unchanged resident-book ceiling is crossed by real
                // unrelated requests; physical lots and target sizes stay small.
                for (std::size_t i = 0; i < 4097; ++i) {
                    no::Request resting{no::Transact{1}, "resting-" + std::to_string(i), ""};
                    resting.trigger = no::Limit{50};
                    (void)put(host, resting);
                }
                require(host.consumer().request_core().book_spilled(), "report fixture did not spill");
            }
        }
        if (host.bars == 3) {
            const auto lots = host.native_open_lots(100);
            out.slices = std::count_if(lots.begin(), lots.end(), [&](const auto& lot) {
                return lot.entry_incarnation == a.incarnation;
            });
            require(out.slices >= 2, "fixture never produced successor slices");
            // Hand arithmetic: A opened 3 units and this close requests 1.5,
            // leaving 1.5 of A alongside B's untouched 2.
            auto partial = close(host, a_cohort, "reached partial close", false, 1.5);
            partial.trigger = no::Limit{short_side ? 92.0 : 108.0};
            const auto changed = host.replace(closes.front(), partial);
            require(changed.successor.has_value(), "partial close replacement rejected");
            closes.front() = *changed.successor;
        }
        if (host.bars == 5) {
            require(host.physical_position().signed_units == sign * 3.5,
                    "partial close changed the wrong physical lots");
            require(host.closed_trade_count() > 0, "reached close never booked a trade");
            (void)apply(host, put(host, {no::Flatten{}, "final close", ""}));
        }
        check(host);
    };
    h.notification = [&](Host& host, const no::ExecutionAppliedEvent&) { check(host); };
    auto feed = tape(6);
    for (std::size_t i = 3; i < feed.size(); ++i) {
        const double price = short_side ? 90 : 110;
        feed[i] = {price, price, price, price, 1, feed[i].timestamp};
    }
    Sink sink;
    sink.bind(h);
    run(h, spec(run_number), feed);
    ReportC report{};
    try {
        h.fill_report(&report);
        out.metrics = report.metrics;
        pf_report_sink_receipt_v1 receipt{};
        receipt.struct_size = sizeof(receipt); receipt.version = 1;
        require(h.report_sink_receipt_v1(&receipt) == 0 && receipt.complete
                && receipt.count_e == static_cast<std::int64_t>(feed.size())
                && receipt.count_t > 1, "report stream did not complete its real rows");
        require(h.set_report_sink_fd_v1(nullptr) == 0, "completed report sink did not detach");
        out.bytes = sink.bytes();
    } catch (...) {
        BacktestEngine::free_report(&report);
        throw;
    }
    BacktestEngine::free_report(&report);
    out.events = event_words(h);
    out.trades = trade_words(h);
    out.core = full_fold::core_value(h.consumer().request_core());
    out.fills = h.fills;
    out.closed_rows = h.closed_trade_count();
    require(out.fills > out.slices && out.closed_rows > 1 && out.target_reads > 0,
            "execution transcript lacks real fills, closes or target reads");
    h.calculation = {};
    h.notification = {};
    return out;
}

void equal(const Outcome& a, const Outcome& b) {
    require(a.bytes == b.bytes, "complete ordered report bytes differ from complete reader");
    require(a.events == b.events, "ordered full-field command/fill transcripts differ");
    require(a.trades == b.trades, "ordered trade fields or strings differ");
    require(a.continuation == b.continuation && a.broker == b.broker && a.core == b.core,
            "ids, cohort receipts or continuation hashes differ");
    require(std::memcmp(&a.metrics, &b.metrics, sizeof(a.metrics)) == 0, "report metrics bytes differ");
    require(std::tie(a.fills, a.target_reads, a.slices, a.closed_rows)
                == std::tie(b.fills, b.target_reads, b.slices, b.closed_rows),
            "transcript positive witnesses differ");
}

void twins(bool direct, bool short_side) {
    for (const auto budget : {std::size_t{1}, std::size_t{16}, payload_bytes}) {
        Host reference_host, indexed_host;
        for (std::uint64_t generation = 1; generation <= 2; ++generation) {
            const auto expected = execute(reference_host, 0, direct, generation, short_side);
            const auto actual = execute(indexed_host, budget, direct, generation, short_side);
            equal(actual, expected);
        }
    }
}
void spilled_twins() {
    Host reference_host, indexed_host;
    for (std::uint64_t generation = 1; generation <= 2; ++generation) {
        const auto expected = execute(reference_host, 0, true, generation, false, true);
        const auto actual = execute(indexed_host, payload_bytes, true, generation, false, true);
        equal(actual, expected);
    }
}
}

int main() {
    row("direct long reports and reuse", [] { twins(true, false); });
    row("prepared long reports and reuse", [] { twins(false, false); });
    row("direct short reports and reuse", [] { twins(true, true); });
    row("prepared short reports and reuse", [] { twins(false, true); });
    row("spilled book reports and reuse", spilled_twins);
    return failures ? 1 : 0;
}
