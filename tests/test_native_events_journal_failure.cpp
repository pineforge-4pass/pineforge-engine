// A journal read that fails inside the search native_events makes is reported
// to the caller, typed; it does not end the process.
//
// NativeStrategyHost::native_events(after) finds its first command row with a
// search of the command journal, NativeExecutionConsumer::first_command_after
// (src/native_execution_consumer.cpp). The journal keeps at most 2,048 of its
// newest events in memory (kMaxResident in src/native_order.cpp) and writes
// the older ones to spill pages in scratch files. The search walks back 33
// events from the tail and then bisects the rest of the retained window, so
// on a long journal it reads spill pages back, and such a read can fail. The
// search was declared noexcept: the failure met that boundary and
// std::terminate ended the process. It must instead reach the caller of
// native_events as the typed scratch failure the journal's store raised,
// report_sink_failed with the reason scratch_io.
//
// The run: one bar's callback submits and cancels kOrders resting limit
// orders, a command event each way, under NativeEventRetention::Commands,
// which retires nothing, so the journal ends with at least 2 * kOrders
// events. With N events the bisection's first probe is position (N - 33) / 2,
// and no position below N - 2,048 is in memory, so for N above 4,063 that
// probe is a spilled event however many events the spill left resident (the
// static_assert below).
//
// Fault injection is the seam test_continuation_hash_spilled_read_failure
// uses: the scratch files are unlinked regular files this process holds open
// (file mode, no arena), and truncating one to zero bytes makes every later
// positional read of it short, which ScratchFile reports as EIO. The
// journal's spill files are found as the descriptors that appear while the
// orders are issued, after a first order has let the run open whatever its
// first command opens. Only those descriptors are touched: the test opens no
// path and follows no file name.
//
// Each case runs in a forked child, because std::terminate ends the process
// that reaches it. The child makes the one public call inside a catch-all, so
// an exception that leaves native_events is always caught and reported: the
// child can only abort when an exception meets a noexcept frame inside the
// call, and its terminate handler reports that, with the exception's text,
// before it aborts. The control child injects nothing and must get every
// command row back, so the same read is shown to succeed while the pages can
// be read.
//
// One fault case, and why there is no second. The journal can refuse a read
// in one more way on this path: history_at (src/native_order.cpp:2991-2996)
// throws std::out_of_range for a position outside the retained window,
// [history_base(), history_end()). No sequence of calls on one thread makes
// the search ask for such a position, so no case here can drive it. The
// line numbers below are those of the tree at the change that added this
// test.
//
//  * The search (src/native_execution_consumer.cpp:12702-12721) reads only
//    positions it has just derived from those two bounds, in the same call:
//    the tail of a window that is not empty (:12704), the event before an
//    iterator that is not begin() (:12708-12710) and a bisection of
//    [begin(), it) (:12713-12716). The view computes its size, its tail and
//    both iterators from the bounds each time it is asked
//    (include/pineforge/native_order.hpp:2838-2854), and an iterator reads
//    the absolute position it holds (:2769).
//  * Nothing the search calls moves a bound. It calls no host callback and
//    no writer of the core: only standard algorithms over the view's
//    iterators, a read of each event's ordinal
//    (src/native_execution_consumer.cpp:1256-1258), the view's readers,
//    history_at and, under it, the journal store's const readers (at,
//    page_for_position and load: src/native_order.cpp:2054, :2013, :2028;
//    codec_at, :1786, for a journal with a codec bound). Those read scratch
//    storage, allocate, and change read caches and counters, never a bound.
//  * history_base_ is written in three places: retire_history
//    (src/native_order.cpp:3065), the move assignment (:3729) and
//    clear_unbound (:2780). The journal's end only grows, by an append:
//    push (:1924; :1918 for a journal with a codec bound, which no
//    production core has) and, with a codec only, append_journal_record
//    (:2158). It goes back only when the whole journal is replaced or
//    cleared, and the base goes with it: the move assignment and
//    clear_unbound again, and the codec branch of reset (:3778-3780).
//  * retire_history (src/native_order.cpp:3044-3071) reads every page it
//    needs before it changes anything (:3048-3056), moves the base after
//    that (:3061-3066) and catches what a read throws (:3068-3070): a
//    retirement that fails leaves both bounds where they were.
//  * A core that was moved from or cleared has no journal and a zero base
//    (src/native_order.cpp:3725-3729, :2777-2780), so history_end() is 0
//    (:2218-2220): its window is empty, and the search returns before it
//    reads (src/native_execution_consumer.cpp:12704).
//
// Two narrower range checks lie under that read on the page path, and the
// same argument closes both. page_for_position
// (src/native_order.cpp:2013-2027) asks the page table (TailLog::get,
// :1199-1209) only for indices from the base up to the size it has just
// read, and at (:2054-2070) indexes a loaded page (:2069) only inside the
// count page_for_position checked (:2023), the size load gave that page
// (:2044).
//
// What is left is a bound that moves while the search runs. That takes a
// second thread (a run that retires, or begins again, while another thread
// reads) or test code that re-enters the core from inside a replaced
// allocator. Both bounds are plain integers
// (include/pineforge/native_order.hpp:2652, src/native_order.cpp:1859) and
// neither the consumer nor the core takes a lock, so the first is a data
// race with no schedule a test controls; no caller of the kernel does the
// second. Neither is a real sequence, so this test has one fault case, the
// failed spill read, beside its control. The fix codes the refusal all the
// same, as a defence: with it history_at throws a coded std::out_of_range
// (engine_invariant, its text unchanged), so a later change that makes such
// a position reachable gives the caller a coded failure, not a bare
// std::out_of_range.
//
// Source-free, and only the public NativeStrategyHost API, so this TU
// compiles against the unfixed search; there the fault case prints
// "terminated" and the test fails.
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <vector>

#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

// The journal's resident bound (kMaxResident in src/native_order.cpp) and the
// events the search reads from the tail before it bisects.
constexpr std::size_t kResidentEvents = 2048;
constexpr std::size_t kTailWalk = 33;
constexpr std::size_t kOrders = 3000;
constexpr std::size_t kEvents = 2 * kOrders;
static_assert((kEvents - kTailWalk) / 2 < kEvents - kResidentEvents,
              "the bisection's first probe must be a spilled event");

// What the caller of native_events must be given for a failed spill read: the
// failure the journal's scratch file raised, as that file typed it.
constexpr const char* kExpectedCode = "report_sink_failed";
constexpr const char* kExpectedReason = "\"reason\":\"scratch_io\"";

// A child's exit statuses. Zero is the outcome its case asks for.
constexpr int kExitExpected = 0;
constexpr int kExitSetup = 2;      // the run did not reach the state the case needs
constexpr int kExitUncoded = 3;    // an exception that carries no run-failure code
constexpr int kExitReturned = 4;   // rows came back where a failure was due, or too few
constexpr int kExitUnexpected = 5; // the control met a failure

// The child's end of the pipe its parent reads.
int report_fd = -1;

// One record, one line: an exception's text never splits a record.
__attribute__((format(printf, 1, 2))) void report_line(const char* format, ...) {
    char text[768];
    va_list arguments;
    va_start(arguments, format);
    const int length = std::vsnprintf(text, sizeof text, format, arguments);
    va_end(arguments);
    if (length <= 0) return;
    const std::size_t size = std::min(static_cast<std::size_t>(length), sizeof text - 1);
    for (std::size_t at = 0; at + 1 < size; ++at) {
        if (text[at] == '\n') text[at] = ' ';
    }
    text[size - 1] = '\n';
    std::size_t done = 0;
    while (done < size) {
        const ssize_t wrote = ::write(report_fd, text + done, size - done);
        if (wrote < 0) {
            if (errno == EINTR) continue;
            return;
        }
        done += static_cast<std::size_t>(wrote);
    }
}

// std::terminate was reached: say so, with the exception that met the
// noexcept frame, and abort as the default handler does.
[[noreturn]] void report_terminate() {
    if (const std::exception_ptr active = std::current_exception()) {
        try {
            std::rethrow_exception(active);
        } catch (const std::exception& error) {
            report_line("terminate %s\n", error.what());
        } catch (...) {
            report_line("terminate non-standard exception\n");
        }
    } else {
        report_line("terminate no active exception\n");
    }
    std::abort();
}

[[noreturn]] void fail_setup(const char* reason, const std::string& detail = std::string()) {
    report_line("setup-failed %s %s\n", reason, detail.c_str());
    ::_exit(kExitSetup);
}

// The unlinked regular files among this process's low descriptors: the
// scratch files (mkstemp, then unlink) the kernel holds open.
std::vector<int> unlinked_descriptors() {
    std::vector<int> found;
    const long limit = ::sysconf(_SC_OPEN_MAX);
    const long end = limit > 0 ? std::min(limit, 4096L) : 1024L;
    for (int fd = 3; fd < end; ++fd) {
        struct stat status{};
        if (::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_nlink == 0) {
            found.push_back(fd);
        }
    }
    return found;
}

no::Request resting_buy() {
    no::Request request{no::Transact{1.0}, "rest", ""};
    request.trigger = no::Limit{50.0};
    return request;
}

class JournalHost final : public NativeStrategyHost {
public:
    std::vector<int> spill_files;  // the descriptors opened while the orders were issued
    std::size_t orders_accepted = 0;
    std::size_t orders_cancelled = 0;

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (++bar_ != 1) return;
        std::vector<int> before;
        for (std::size_t order = 0; order != kOrders; ++order) {
            // The first order has let the run open what its first command opens.
            if (order == 1) before = unlinked_descriptors();
            const no::SubmitResult submitted = submit(resting_buy());
            if (submitted.status != no::SubmitStatus::Accepted || !submitted.handle) continue;
            ++orders_accepted;
            if (cancel(*submitted.handle).status == no::CancelStatus::Cancelled) {
                ++orders_cancelled;
            }
        }
        for (const int fd : unlinked_descriptors()) {
            if (std::find(before.begin(), before.end(), fd) == before.end()) {
                spill_files.push_back(fd);
            }
        }
    }

private:
    int bar_ = 0;
};

std::vector<Bar> tape() {
    const std::int64_t t0 = 1736121600000LL;
    std::vector<Bar> bars;
    for (int index = 0; index != 3; ++index) {
        Bar bar{};
        bar.open = 100.0;
        bar.high = 100.0;
        bar.low = 100.0;
        bar.close = 100.0;
        bar.volume = 1.0;
        bar.timestamp = t0 + static_cast<std::int64_t>(index) * 300000;
        bars.push_back(bar);
    }
    return bars;
}

NativeRunSpec spec() {
    NativeRunSpec spec;
    spec.identity = {"events-journal-read-failure", 1};
    spec.input_tf = "5";
    spec.script_tf = "5";
    spec.tickerid = "TEST:JOURNAL";
    spec.timezone = "UTC";
    spec.session = "24x7";
    spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    spec.initial_capital = 1.0e9;
    spec.point_value = 1;
    spec.account_fx = 1;
    spec.price_tick = 0.25;
    // The whole command journal stays: nothing is retired, so it spills.
    spec.event_retention = NativeEventRetention::Commands;
    return spec;
}

// The one public call, after the run has ended. Nothing it throws leaves this
// function: an abort from here is an exception that met a noexcept frame
// inside native_events.
[[noreturn]] void read_events(const JournalHost& host, bool inject) {
    try {
        const std::vector<NativeMarketEvent> rows = host.native_events(0);
        std::size_t commands = 0;
        for (const NativeMarketEvent& row : rows) {
            if (row.kind == NativeEventKind::Command && row.command) ++commands;
        }
        report_line("returned %zu\n", commands);
        ::_exit(!inject && commands >= kEvents ? kExitExpected : kExitReturned);
    } catch (const std::exception& error) {
        const auto* typed = dynamic_cast<const RunFailureInfo*>(&error);
        if (typed != nullptr && typed->run_failure_code() != RunFailureCode::none) {
            const RunFailureValue& failure = typed->run_failure();
            report_line("coded %s %s %s\n", run_failure_code_name(failure.code),
                        failure.args ? failure.args->c_str() : "{}", error.what());
            ::_exit(inject ? kExitExpected : kExitUnexpected);
        }
        report_line("uncoded %s\n", error.what());
        ::_exit(kExitUncoded);
    } catch (...) {
        report_line("uncoded non-standard exception\n");
        ::_exit(kExitUncoded);
    }
}

// One case, in the child. It never returns to the parent's code, and it
// leaves by _exit, so nothing is torn down over the files it truncated.
[[noreturn]] void run_child(bool inject) {
    std::set_terminate(report_terminate);
    const struct rlimit no_core{0, 0};
    (void) ::setrlimit(RLIMIT_CORE, &no_core);
    try {
        JournalHost host;
        if (host.configure_native(spec()).status != NativeSetupStatus::Applied) {
            fail_setup("configure_native refused the spec");
        }
        const std::vector<Bar> bars = tape();
        host.run(bars.data(), static_cast<int>(bars.size()));
        if (host.native_state().kind != NativeLifecycleKind::Completed) {
            fail_setup("the run did not complete:", host.last_error());
        }
        if (host.orders_accepted != kOrders || host.orders_cancelled != kOrders) {
            fail_setup("an order was not accepted and then cancelled");
        }
        // Nothing was retired: the journal still holds every event, the first
        // one included, so the search bisects the whole run.
        if (host.native_event_window_start() != 1
            || host.native_events_dropped(NativeEventKind::Command) != 0) {
            fail_setup("the journal retired events");
        }
        // The journal spilled: issuing the orders opened scratch files, and
        // pages were written to them.
        if (host.spill_files.empty()) fail_setup("no scratch file was opened by the orders");
        std::uint64_t bytes = 0;
        for (const int fd : host.spill_files) {
            struct stat status{};
            if (::fstat(fd, &status) != 0 || status.st_size < 0) {
                fail_setup("a spill file could not be measured");
            }
            bytes += static_cast<std::uint64_t>(status.st_size);
        }
        if (bytes == 0) fail_setup("the spill files are empty");
        if (inject) {
            // From here on every read of the journal's spill files is short.
            for (const int fd : host.spill_files) {
                if (::ftruncate(fd, 0) != 0) fail_setup("a spill file could not be truncated");
            }
        }
        report_line("armed files=%zu bytes=%llu\n", host.spill_files.size(),
                    static_cast<unsigned long long>(bytes));
        read_events(host, inject);
    } catch (const std::exception& error) {
        report_line("setup-failed exception %s\n", error.what());
    } catch (...) {
        report_line("setup-failed non-standard exception\n");
    }
    ::_exit(kExitSetup);
}

struct Child {
    bool waited = false;
    int status = 0;
    std::string report;
};

Child run_case(bool inject) {
    Child child;
    int channel[2];
    if (::pipe(channel) != 0) return child;
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(channel[0]);
        ::close(channel[1]);
        return child;
    }
    if (pid == 0) {
        ::close(channel[0]);
        report_fd = channel[1];
        run_child(inject);
    }
    ::close(channel[1]);
    char buffer[512];
    for (;;) {
        const ssize_t got = ::read(channel[0], buffer, sizeof buffer);
        if (got > 0) {
            child.report.append(buffer, static_cast<std::size_t>(got));
            continue;
        }
        if (got < 0 && errno == EINTR) continue;
        break;
    }
    ::close(channel[0]);
    pid_t reaped;
    do {
        reaped = ::waitpid(pid, &child.status, 0);
    } while (reaped < 0 && errno == EINTR);
    child.waited = reaped == pid;
    return child;
}

// Whether a line of the child's report begins with `key`, and the rest of the
// first such line.
bool find_record(const std::string& report, const std::string& key, std::string* rest) {
    std::size_t at = 0;
    while (at < report.size()) {
        std::size_t end = report.find('\n', at);
        if (end == std::string::npos) end = report.size();
        if (report.compare(at, key.size(), key) == 0) {
            if (rest != nullptr) *rest = report.substr(at + key.size(), end - at - key.size());
            return true;
        }
        at = end + 1;
    }
    return false;
}

std::string last_record(const std::string& report) {
    std::size_t end = report.size();
    while (end > 0 && report[end - 1] == '\n') --end;
    if (end == 0) return "no report";
    const std::size_t start = report.rfind('\n', end - 1);
    return report.substr(start == std::string::npos ? 0 : start + 1,
                         end - (start == std::string::npos ? 0 : start + 1));
}

std::string status_text(const Child& child) {
    if (!child.waited) return "not-started";
    if (WIFEXITED(child.status)) return "exit=" + std::to_string(WEXITSTATUS(child.status));
    if (WIFSIGNALED(child.status)) return "signal=" + std::to_string(WTERMSIG(child.status));
    return "status=" + std::to_string(child.status);
}

struct Verdict {
    std::string outcome;
    std::string detail;
    bool pass = false;
};

// The fault case: "terminated" is the death std::terminate causes, and
// nothing else; "coded:<code>" is the failure the public call reported, and
// passes only when it is the journal's own scratch failure.
Verdict judge_fault(const Child& child) {
    Verdict verdict;
    const bool armed = find_record(child.report, "armed ", nullptr);
    std::string rest;
    if (child.waited && WIFSIGNALED(child.status) && WTERMSIG(child.status) == SIGABRT && armed
        && find_record(child.report, "terminate ", &rest)) {
        verdict.outcome = "terminated";
        verdict.detail = rest;
        return verdict;
    }
    if (child.waited && WIFEXITED(child.status) && WEXITSTATUS(child.status) == kExitExpected
        && armed && find_record(child.report, "coded ", &rest)) {
        const std::size_t space = rest.find(' ');
        const std::string code = rest.substr(0, space);
        verdict.outcome = "coded:" + code;
        verdict.detail = space == std::string::npos ? std::string() : rest.substr(space + 1);
        verdict.pass = code == kExpectedCode
            && verdict.detail.find(kExpectedReason) != std::string::npos;
        return verdict;
    }
    verdict.outcome = "other:" + status_text(child);
    verdict.detail = last_record(child.report);
    return verdict;
}

// The control: no fault, so the call answers, with every command row.
Verdict judge_control(const Child& child) {
    Verdict verdict;
    std::string rest;
    if (child.waited && WIFEXITED(child.status) && WEXITSTATUS(child.status) == kExitExpected
        && find_record(child.report, "armed ", nullptr)
        && find_record(child.report, "returned ", &rest)) {
        verdict.outcome = "returned:" + rest;
        verdict.pass = true;
        return verdict;
    }
    verdict.outcome = "other:" + status_text(child);
    verdict.detail = last_record(child.report);
    return verdict;
}

}  // namespace

int main() {
    const Verdict control = judge_control(run_case(false));
    std::printf("NATIVE_EVENTS control=unfaulted outcome=%s%s%s\n", control.outcome.c_str(),
                control.detail.empty() ? "" : " detail=", control.detail.c_str());
    const Verdict fault = judge_fault(run_case(true));
    std::printf("NATIVE_EVENTS scenario=spill_read outcome=%s%s%s\n", fault.outcome.c_str(),
                fault.detail.empty() ? "" : " detail=", fault.detail.c_str());
    const bool pass = control.pass && fault.pass;
    std::printf("NATIVE_EVENTS summary scenarios=1 expected=coded:%s control=%s result=%s\n",
                kExpectedCode, control.pass ? "ok" : "failed", pass ? "PASS" : "FAIL");
    std::fflush(stdout);
    return pass ? 0 : 1;
}
