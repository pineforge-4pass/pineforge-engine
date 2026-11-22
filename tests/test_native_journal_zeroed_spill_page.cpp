// A spilled page of the native command journal that reads back as zeros: does
// the public read NativeStrategyHost::native_events(after) then answer wrong
// command rows with no error?
//
// The journal keeps its newest events resident and writes the older ones, a
// chunk at a time, as pages of an unlinked scratch file (JournalStore in
// src/native_order.cpp). A page is its serialized events one after another,
// each a uint32_t alternative, a uint64_t ordinal and the alternative's
// fields; a second scratch file holds the page table, one JournalPage record
// per page (the positions and ordinals it covers, its offset and its length).
// Neither carries a checksum. native_events(after) finds its first command
// row with NativeExecutionConsumer::first_command_after
// (src/native_execution_consumer.cpp), which walks 33 events back from the
// tail and then bisects the rest of the retained window by ordinal, reading
// spilled events back page by page.
//
// The run. Bar 1 submits 3,000 resting buy limits, each under a label of its
// own, and cancels each right after its submit: 6,000 command events, an
// Accepted and a Cancelled per order, under NativeEventRetention::Commands,
// which retires nothing. The expected rows are what the commands themselves
// answered (SubmitResult::event_ordinal, CancelResult::event_ordinal) with
// the label each order was given: no expected value is read from the journal
// and none is a literal.
//
// The cursor and the page. `after` is the ordinal of the event at the first
// position the bisection probes, (events - 33) / 2 from the oldest; the page
// is the one whose record covers that position. The first row of the right
// answer is the next event, and the test requires it to be on the same page.
// So the boundary the search is looking for lies inside the page: whatever
// positions a search probes, it cannot place that boundary without comparing
// events of this page, and the first rows of the right answer are events that
// only this page holds.
//
// Two hosts run the same batch. The control asks native_events(after) with
// nothing zeroed and must answer exactly the expected rows. In the other the
// page is overwritten with zeros after the run and before anything has read
// the journal back, so no decoded copy of it can be in memory: a spilled
// event leaves the resident part when its page is written, and the file is
// then its only copy. The overwrite goes through the process's own descriptor
// of the unlinked data file (pwrite); the test opens no path and follows no
// file name. The journal's two files are among the unlinked descriptors that
// appeared with the run: the page table is the one whose records continue the
// journal from its first event with the positions and ordinals the run
// produced, and the data file is the one whose bytes at the chosen record's
// offset begin with that page's first event and hold the label of every order
// on the page. The page is read back as zeros through the same descriptor
// before the read under test.
//
// What the zeroed read can do, and what this row makes of it:
//   wrong command rows, no error   the defect is real: the row fails, exit 1;
//   a coded failure                the page was detected: the row passes;
//   the right rows                 the first rows of the answer are stored
//                                  nowhere but in the zeroed page, so this
//                                  read did not come from the file and the
//                                  measurement proves nothing: INVALID,
//                                  exit 2, never a pass;
//   anything else                  an exception without a code: the row fails,
//                                  exit 3; std::terminate: the handler names
//                                  the read it interrupted, then aborts.
// A run that did not spill, files that cannot be told apart, a first row
// outside the page, or a control that is not exactly right is INVALID too
// (exit 2). Where the journal's spill has no descriptor of its own (a scratch
// arena is attached) the row skips with exit 77.
//
// Source-free: public headers only, so it also runs in the kernel profile.
#include <pineforge/detail/scratch_arena.hpp>
#include <pineforge/native_host.hpp>
#include <pineforge/run_failure.hpp>

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using namespace pineforge;
namespace no = pineforge::native_order;

constexpr std::size_t kOrders = 3000;
// first_command_after reads this many events back from the tail before it
// bisects what is left of the window.
constexpr std::size_t kTailWalk = 33;
// The most one page holds: JournalStore::kChunk events, kSpillPageBytes bytes.
constexpr std::uint64_t kPageEvents = 32;
constexpr std::uint64_t kPageBytes = 32768;

// JournalPage as the page table file holds it: six native 64-bit words, the
// record of page n at byte 48 * n.
struct PageRecord {
    std::uint64_t first_position = 0;
    std::uint64_t count = 0;
    std::uint64_t first_ordinal = 0;
    std::uint64_t last_ordinal = 0;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};
static_assert(sizeof(PageRecord) == 48, "a page record is six 64-bit words");

// One command event, as the command that produced it answered.
struct Expected {
    std::uint64_t ordinal = 0;
    std::size_t kind = 0;    // the CommandEvent alternative
    std::size_t order = 0;   // the order it belongs to: its label is label_of(order)
};

std::size_t accepted_kind() { return no::CommandEvent(no::AcceptedEvent{}).index(); }
std::size_t cancelled_kind() { return no::CommandEvent(no::CancelledEvent{}).index(); }

const char* kind_name(std::size_t kind) {
    if (kind == accepted_kind()) return "accepted";
    if (kind == cancelled_kind()) return "cancelled";
    return "another";
}

std::string label_of(std::size_t order) {
    char text[32];
    std::snprintf(text, sizeof text, "spill-order-%07zu", order);
    return std::string(text);
}

// Bar 1: every order is submitted and cancelled before the next one, so the
// journal is Accepted(0), Cancelled(0), Accepted(1), Cancelled(1), ...
class BatchHost final : public NativeStrategyHost {
public:
    std::vector<Expected> expected;   // in the order the commands committed
    std::size_t refused = 0;          // commands that did not answer Accepted / Cancelled

    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        if (issued_) return;
        issued_ = true;
        const std::size_t accepted = accepted_kind();
        const std::size_t cancelled = cancelled_kind();
        expected.reserve(2 * kOrders);
        for (std::size_t order = 0; order != kOrders; ++order) {
            no::Request request{no::Transact{1.0}, label_of(order), ""};
            request.trigger = no::Limit{50.0};
            const no::SubmitResult submitted = submit(request);
            if (submitted.status != no::SubmitStatus::Accepted || !submitted.handle) {
                ++refused;
                continue;
            }
            expected.push_back(Expected{submitted.event_ordinal, accepted, order});
            const no::CancelResult withdrawn = cancel(*submitted.handle);
            if (withdrawn.status != no::CancelStatus::Cancelled) {
                ++refused;
                continue;
            }
            expected.push_back(Expected{withdrawn.event_ordinal, cancelled, order});
        }
    }

private:
    bool issued_ = false;
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

NativeRunSpec run_spec() {
    NativeRunSpec value;
    value.identity = {"journal-zeroed-spill-page", 1};
    value.input_tf = "5";
    value.script_tf = "5";
    value.tickerid = "TEST:SPILLPAGE";
    value.timezone = "UTC";
    value.session = "24x7";
    value.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
    value.initial_capital = 1.0e9;
    value.point_value = 1;
    value.account_fx = 1;
    value.price_tick = 0.25;
    value.event_retention = NativeEventRetention::Commands;
    return value;
}

// Runs the batch on `host` and answers why the run cannot carry the
// measurement, or nothing when it can. It reads nothing back from the journal.
std::string run_orders(BatchHost& host) {
    const auto setup = host.configure_native(run_spec());
    if (setup.status != NativeSetupStatus::Applied) return "the run spec was refused";
    const std::vector<Bar> bars = tape();
    host.run(bars.data(), static_cast<int>(bars.size()));
    if (host.native_state().kind != NativeLifecycleKind::Completed) {
        return "the run did not complete: " + host.last_error();
    }
    if (run_failure_value_of(host).code != RunFailureCode::none) {
        return "the run recorded a failure: " + host.last_error();
    }
    if (host.refused != 0) return "a submit or a cancel was refused";
    if (host.expected.size() != 2 * kOrders) return "the run did not issue every command";
    if (host.expected.front().ordinal == 0) return "a command answered the ordinal 0";
    for (std::size_t index = 1; index < host.expected.size(); ++index) {
        if (host.expected[index].ordinal <= host.expected[index - 1].ordinal) {
            return "the commands' ordinals do not ascend";
        }
    }
    return std::string();
}

// Whether two runs committed the same commands in the same order and, with
// `ordinals`, under the same ordinals too.
bool same_rows(const std::vector<Expected>& left, const std::vector<Expected>& right,
               bool ordinals) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index != left.size(); ++index) {
        if (left[index].kind != right[index].kind || left[index].order != right[index].order) {
            return false;
        }
        if (ordinals && left[index].ordinal != right[index].ordinal) return false;
    }
    return true;
}

// Which read the test asks for, from the rows the run produced.
struct Target {
    std::size_t probe = 0;       // the first position the bisection reads
    std::size_t first_row = 0;   // the position of the first row of the right answer
    std::uint64_t after = 0;     // the ordinal of the event at `probe`
};

std::string choose(const std::vector<Expected>& expected, Target& target) {
    if (expected.size() < kTailWalk + 4) return "the journal is too short to be bisected";
    target.probe = (expected.size() - kTailWalk) / 2;
    target.first_row = target.probe + 1;
    target.after = expected[target.probe].ordinal;
    return std::string();
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

bool read_exact(int fd, std::uint64_t offset, void* out, std::size_t length) {
    auto* bytes = static_cast<unsigned char*>(out);
    std::size_t done = 0;
    while (done < length) {
        const ssize_t got = ::pread(fd, bytes + done, length - done,
                                    static_cast<off_t>(offset + done));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return false;
        done += static_cast<std::size_t>(got);
    }
    return true;
}

bool write_zeros(int fd, std::uint64_t offset, std::size_t length) {
    const std::vector<unsigned char> zeros(length, 0);
    std::size_t done = 0;
    while (done < length) {
        const ssize_t put = ::pwrite(fd, zeros.data() + done, length - done,
                                     static_cast<off_t>(offset + done));
        if (put < 0 && errno == EINTR) continue;
        if (put <= 0) return false;
        done += static_cast<std::size_t>(put);
    }
    return true;
}

bool all_zero(const std::vector<unsigned char>& bytes) {
    return std::all_of(bytes.begin(), bytes.end(), [](unsigned char byte) { return byte == 0; });
}

// The page records `fd` holds, for as long as they continue this journal from
// its first event: each record starts where the one before it ended, in
// positions and in bytes, and names the ordinals the run produced at its first
// and last position. Empty for a file that is not this journal's page table.
std::vector<PageRecord> page_table(int fd, const std::vector<Expected>& expected) {
    std::vector<PageRecord> pages;
    struct stat status{};
    if (::fstat(fd, &status) != 0 || status.st_size <= 0) return pages;
    const std::uint64_t rows = static_cast<std::uint64_t>(status.st_size) / sizeof(PageRecord);
    std::uint64_t position = 0;
    std::uint64_t offset = 0;
    for (std::uint64_t index = 0; index != rows; ++index) {
        PageRecord record;
        if (!read_exact(fd, index * sizeof(PageRecord), &record, sizeof record)) break;
        if (record.first_position != position || record.offset != offset) break;
        if (record.count == 0 || record.count > kPageEvents) break;
        if (record.length == 0 || record.length > kPageBytes) break;
        if (record.count > expected.size() - position) break;
        const std::size_t first = static_cast<std::size_t>(position);
        const std::size_t last = first + static_cast<std::size_t>(record.count) - 1;
        if (record.first_ordinal != expected[first].ordinal) break;
        if (record.last_ordinal != expected[last].ordinal) break;
        pages.push_back(record);
        position += record.count;
        offset += record.length;
    }
    return pages;
}

// Whether the page's bytes begin with `event`: its alternative, then its ordinal.
bool begins_with(const std::vector<unsigned char>& bytes, const Expected& event) {
    std::uint32_t kind = 0;
    std::uint64_t ordinal = 0;
    if (bytes.size() < sizeof kind + sizeof ordinal) return false;
    std::memcpy(&kind, bytes.data(), sizeof kind);
    std::memcpy(&ordinal, bytes.data() + sizeof kind, sizeof ordinal);
    return kind == event.kind && ordinal == event.ordinal;
}

std::size_t occurrences(const std::vector<unsigned char>& bytes, const std::string& text) {
    std::size_t found = 0;
    auto at = bytes.begin();
    for (;;) {
        at = std::search(at, bytes.end(), text.begin(), text.end());
        if (at == bytes.end()) return found;
        ++found;
        at += static_cast<std::ptrdiff_t>(text.size());
    }
}

// Whether the bytes name every event of the page: each order's label at least
// once per event of that order on the page. `named` counts the labels found.
bool names_every_event(const std::vector<unsigned char>& bytes,
                       const std::vector<Expected>& expected, const PageRecord& page,
                       std::uint64_t& named) {
    named = 0;
    bool every = true;
    const std::size_t first = static_cast<std::size_t>(page.first_position);
    const std::size_t end = first + static_cast<std::size_t>(page.count);
    std::size_t position = first;
    while (position != end) {
        const std::size_t order = expected[position].order;
        std::size_t events = 0;
        while (position != end && expected[position].order == order) {
            ++events;
            ++position;
        }
        const std::size_t found = occurrences(bytes, label_of(order));
        named += found;
        if (found < events) every = false;
    }
    return every;
}

// One command row as native_events returned it.
struct Row {
    std::uint64_t ordinal = 0;         // the row's own ordinal
    std::uint64_t event_ordinal = 0;   // the ordinal inside its command event
    std::size_t kind = 0;
    bool labelled = false;             // the event carries a definition
    std::string label;
};

Row row_of(const NativeMarketEvent& market) {
    Row row;
    row.ordinal = market.ordinal;
    const no::CommandEvent& event = *market.command;
    row.kind = event.index();
    row.event_ordinal = std::visit(
        [](const auto& payload) -> std::uint64_t { return payload.ordinal; }, event);
    const no::RequestDefinition* definition = nullptr;
    if (const auto* accepted = std::get_if<no::AcceptedEvent>(&event)) {
        definition = accepted->definition.get();
    } else if (const auto* cancelled = std::get_if<no::CancelledEvent>(&event)) {
        definition = cancelled->definition.get();
    }
    if (definition != nullptr) {
        row.labelled = true;
        row.label = definition->request.label;
    }
    return row;
}

struct Answer {
    bool threw = false;
    bool coded = false;          // the exception carried a run failure code
    std::string code;            // that code's catalog name
    std::string text;            // what() of the exception
    std::vector<Row> rows;       // the command rows, in the order returned
    std::size_t other_rows = 0;  // rows of another kind
};

// The step whose read is in flight, for the terminate handler.
const char* g_reading = nullptr;

Answer ask(const BatchHost& host, std::uint64_t after, const char* step) {
    Answer answer;
    g_reading = step;
    try {
        const std::vector<NativeMarketEvent> events = host.native_events(after);
        answer.rows.reserve(events.size());
        for (const NativeMarketEvent& event : events) {
            if (event.kind != NativeEventKind::Command || !event.command) {
                ++answer.other_rows;
                continue;
            }
            answer.rows.push_back(row_of(event));
        }
    } catch (const std::exception& error) {
        answer.threw = true;
        answer.rows.clear();
        answer.text = error.what();
        if (const auto* info = dynamic_cast<const RunFailureInfo*>(&error)) {
            answer.coded = true;
            answer.code = run_failure_code_name(info->run_failure_code());
        }
    } catch (...) {
        answer.threw = true;
        answer.rows.clear();
        answer.text = "an exception that is not a std::exception";
    }
    g_reading = nullptr;
    return answer;
}

bool same_row(const Row& row, const Expected& want) {
    return row.ordinal == want.ordinal && row.event_ordinal == want.ordinal
        && row.kind == want.kind && row.labelled && row.label == label_of(want.order);
}

struct Match {
    bool exact = false;                  // the same rows, the same count, the same order
    std::size_t first_difference = 0;    // the first row that differs from the expected one
    bool ascending = true;               // the returned ordinals strictly ascend
    std::size_t missing_first_rows = 0;  // n: what came back is the expected rows without their first n
};

// The returned command rows against expected[first..end).
Match compare(const std::vector<Row>& rows, const std::vector<Expected>& expected,
              std::size_t first) {
    Match match;
    const std::size_t wanted = expected.size() - first;
    std::size_t index = 0;
    while (index < rows.size() && index < wanted && same_row(rows[index], expected[first + index])) {
        ++index;
    }
    match.first_difference = index;
    match.exact = index == wanted && rows.size() == wanted;
    for (std::size_t row = 1; row < rows.size(); ++row) {
        if (rows[row].ordinal <= rows[row - 1].ordinal) match.ascending = false;
    }
    if (!match.exact && !rows.empty() && rows.size() < wanted) {
        const std::size_t skipped = wanted - rows.size();
        bool tail = true;
        for (std::size_t row = 0; row != rows.size() && tail; ++row) {
            tail = same_row(rows[row], expected[first + skipped + row]);
        }
        if (tail) match.missing_first_rows = skipped;
    }
    return match;
}

enum class Outcome { Right, Wrong, Coded, Other };

struct Reading {
    Outcome outcome = Outcome::Other;
    std::string name;     // right | wrong | coded:<code> | other
    std::string detail;   // the failure's text, when there is one
};

Reading classify(const Answer& answer, const BatchHost& host, const Match& match) {
    Reading reading;
    if (answer.threw) {
        reading.detail = answer.text;
        if (answer.coded) {
            reading.outcome = Outcome::Coded;
            reading.name = "coded:" + answer.code;
        } else {
            reading.outcome = Outcome::Other;
            reading.name = "other";
        }
        return reading;
    }
    // A read that was refused through the run's state rather than by a throw.
    const RunFailureValue failure = run_failure_value_of(host);
    if (failure.code != RunFailureCode::none) {
        reading.outcome = Outcome::Coded;
        reading.name = std::string("coded:") + run_failure_code_name(failure.code);
        reading.detail = host.last_error();
        return reading;
    }
    reading.outcome = match.exact ? Outcome::Right : Outcome::Wrong;
    reading.name = match.exact ? "right" : "wrong";
    return reading;
}

void print_case(const char* name, const Reading& reading, const Target& target,
                const std::vector<Expected>& expected, const Answer& answer,
                const Match& match) {
    const Expected& want = expected[target.first_row];
    std::printf("SPILL_PAGE case=%s outcome=%s after=%llu expected_first_ordinal=%llu"
                " expected_first_kind=%s expected_first_label=%s expected_count=%zu",
                name, reading.name.c_str(), static_cast<unsigned long long>(target.after),
                static_cast<unsigned long long>(want.ordinal), kind_name(want.kind),
                label_of(want.order).c_str(), expected.size() - target.first_row);
    if (answer.threw) {
        std::printf(" returned=an_exception");
    } else if (answer.rows.empty()) {
        std::printf(" returned_count=0");
    } else {
        const Row& got = answer.rows.front();
        std::printf(" returned_first_ordinal=%llu returned_first_kind=%s"
                    " returned_first_label=%s returned_count=%zu order=%s",
                    static_cast<unsigned long long>(got.ordinal), kind_name(got.kind),
                    got.labelled ? got.label.c_str() : "none", answer.rows.size(),
                    match.ascending ? "ascending" : "not_ascending");
    }
    if (!answer.threw) {
        if (match.exact) {
            std::printf(" first_difference=none");
        } else {
            std::printf(" first_difference_at_row=%zu", match.first_difference);
        }
        if (match.missing_first_rows != 0) {
            std::printf(" missing_first_rows=%zu", match.missing_first_rows);
        }
        if (answer.other_rows != 0) std::printf(" rows_of_another_kind=%zu", answer.other_rows);
    }
    if (!reading.detail.empty()) std::printf(" text=\"%s\"", reading.detail.c_str());
    std::printf("\n");
}

int report_invalid(const std::string& reason) {
    std::printf("SPILL_PAGE summary verdict=INVALID reason=\"%s\"\n", reason.c_str());
    return 2;
}

[[noreturn]] void report_terminate() {
    if (g_reading != nullptr) {
        std::printf("SPILL_PAGE case=%s outcome=other text=\"std::terminate was called inside"
                    " the read\"\n", g_reading);
    }
    std::printf("SPILL_PAGE summary verdict=OTHER reason=\"the process was terminated during"
                " %s\"\n", g_reading != nullptr ? "the read" : "the setup");
    std::fflush(stdout);
    std::abort();
}

int run_measurement() {
    // The control: the same run and the same call, nothing zeroed.
    std::vector<Expected> control_rows;
    {
        BatchHost host;
        const std::string refusal = run_orders(host);
        if (!refusal.empty()) return report_invalid("control run: " + refusal);
        Target target;
        const std::string unusable = choose(host.expected, target);
        if (!unusable.empty()) return report_invalid(unusable);
        const Answer answer = ask(host, target.after, "control");
        const Match match = compare(answer.rows, host.expected, target.first_row);
        const Reading reading = classify(answer, host, match);
        print_case("control", reading, target, host.expected, answer, match);
        if (reading.outcome != Outcome::Right) {
            return report_invalid("the control read, with nothing zeroed, did not answer exactly"
                                  " the expected rows");
        }
        control_rows = host.expected;
    }

    // The same run again. Nothing reads its journal back before the page is zeroed.
    const std::vector<int> before = unlinked_descriptors();
    BatchHost host;
    const std::string refusal = run_orders(host);
    if (!refusal.empty()) return report_invalid("zeroed run: " + refusal);
    if (!same_rows(host.expected, control_rows, false)) {
        return report_invalid("the two runs did not commit the same commands");
    }
    const bool same_ordinals = same_rows(host.expected, control_rows, true);
    Target target;
    const std::string unusable = choose(host.expected, target);
    if (!unusable.empty()) return report_invalid(unusable);

    std::vector<int> opened;
    for (const int fd : unlinked_descriptors()) {
        if (std::find(before.begin(), before.end(), fd) == before.end()) opened.push_back(fd);
    }
    if (opened.empty()) {
        return report_invalid("the run opened no unlinked scratch file: the journal did not"
                              " spill to a file");
    }

    int table_fd = -1;
    std::size_t tables = 0;
    std::vector<PageRecord> pages;
    for (const int fd : opened) {
        std::vector<PageRecord> records = page_table(fd, host.expected);
        if (records.empty()) continue;
        ++tables;
        table_fd = fd;
        pages = std::move(records);
    }
    if (tables == 0) {
        return report_invalid("no scratch file holds page records of this journal: it did not"
                              " spill far enough to write its page table");
    }
    if (tables != 1) {
        return report_invalid("more than one scratch file reads as this journal's page table");
    }

    const PageRecord* page = nullptr;
    std::size_t page_index = 0;
    for (std::size_t index = 0; index != pages.size(); ++index) {
        const PageRecord& record = pages[index];
        if (target.probe >= record.first_position
            && target.probe - record.first_position < record.count) {
            page = &record;
            page_index = index;
            break;
        }
    }
    if (page == nullptr) {
        return report_invalid("the page table file holds no record for the probed position");
    }
    const std::uint64_t page_end = page->first_position + page->count;
    if (target.first_row >= page_end) {
        return report_invalid("the first expected row is not on the probed page");
    }
    const std::uint64_t rows_on_page = page_end - target.first_row;
    const std::size_t page_bytes = static_cast<std::size_t>(page->length);

    int data_fd = -1;
    std::size_t data_files = 0;
    std::uint64_t labels = 0;
    for (const int fd : opened) {
        if (fd == table_fd) continue;
        std::vector<unsigned char> bytes(page_bytes);
        if (!read_exact(fd, page->offset, bytes.data(), bytes.size())) continue;
        if (!begins_with(bytes, host.expected[static_cast<std::size_t>(page->first_position)])) {
            continue;
        }
        std::uint64_t named = 0;
        if (!names_every_event(bytes, host.expected, *page, named)) continue;
        ++data_files;
        data_fd = fd;
        labels = named;
    }
    if (data_files == 0) {
        return report_invalid("no scratch file holds the chosen page's events at its record's"
                              " offset");
    }
    if (data_files != 1) {
        return report_invalid("more than one scratch file holds the chosen page's events");
    }

    // One page, on disk, through the descriptor the journal itself reads.
    if (!write_zeros(data_fd, page->offset, page_bytes)) {
        return report_invalid("the page could not be overwritten through its descriptor");
    }
    std::vector<unsigned char> written(page_bytes, 1);
    if (!read_exact(data_fd, page->offset, written.data(), written.size()) || !all_zero(written)) {
        return report_invalid("the page does not read back as zeros through its descriptor");
    }
    std::printf("SPILL_PAGE page index=%zu records_in_file=%zu first_position=%llu count=%llu"
                " first_ordinal=%llu last_ordinal=%llu offset=%llu length=%llu"
                " probe_position=%zu first_row_position=%zu answer_rows_on_page=%llu"
                " scratch_files_opened=%zu labels_in_page=%llu zeroed_bytes=%zu"
                " reads_back_zero=yes same_ordinals_as_control=%s\n",
                page_index, pages.size(), static_cast<unsigned long long>(page->first_position),
                static_cast<unsigned long long>(page->count),
                static_cast<unsigned long long>(page->first_ordinal),
                static_cast<unsigned long long>(page->last_ordinal),
                static_cast<unsigned long long>(page->offset),
                static_cast<unsigned long long>(page->length), target.probe, target.first_row,
                static_cast<unsigned long long>(rows_on_page), opened.size(),
                static_cast<unsigned long long>(labels), page_bytes,
                same_ordinals ? "yes" : "no");

    const Answer answer = ask(host, target.after, "zeroed");
    const Match match = compare(answer.rows, host.expected, target.first_row);
    const Reading reading = classify(answer, host, match);
    print_case("zeroed", reading, target, host.expected, answer, match);

    std::vector<unsigned char> afterwards(page_bytes, 1);
    const bool still_zero =
        read_exact(data_fd, page->offset, afterwards.data(), afterwards.size())
        && all_zero(afterwards);
    const Expected& want = host.expected[target.first_row];
    const std::size_t wanted = host.expected.size() - target.first_row;

    switch (reading.outcome) {
    case Outcome::Wrong:
        std::printf("SPILL_PAGE summary verdict=REAL native_events(%llu) answered wrong command"
                    " rows with no error after one spilled page read back as zeros: expected"
                    " %zu rows from ordinal %llu (%s %s), got %zu rows",
                    static_cast<unsigned long long>(target.after), wanted,
                    static_cast<unsigned long long>(want.ordinal), kind_name(want.kind),
                    label_of(want.order).c_str(), answer.rows.size());
        if (!answer.rows.empty()) {
            const Row& got = answer.rows.front();
            std::printf(" from ordinal %llu (%s %s)", static_cast<unsigned long long>(got.ordinal),
                        kind_name(got.kind), got.labelled ? got.label.c_str() : "none");
        }
        if (match.missing_first_rows != 0) {
            std::printf("; the first %zu expected rows are missing", match.missing_first_rows);
        }
        std::printf("; rows of the right answer stored on the zeroed page: %llu; the page still"
                    " reads back as zeros: %s\n",
                    static_cast<unsigned long long>(rows_on_page), still_zero ? "yes" : "no");
        return 1;
    case Outcome::Coded:
        std::printf("SPILL_PAGE summary verdict=NOT_REAL the zeroed page was detected: the read"
                    " failed with %s (%s)\n", reading.name.c_str(), reading.detail.c_str());
        return 0;
    case Outcome::Right:
        return report_invalid("the zeroed read answered the right rows, although the first "
                              + std::to_string(rows_on_page)
                              + " of them are stored only in the zeroed page: this read did not"
                                " come from the file, so the page was never read back");
    case Outcome::Other:
        break;
    }
    std::printf("SPILL_PAGE summary verdict=OTHER the zeroed read failed without a coded"
                " failure: %s\n", reading.detail.c_str());
    return 3;
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::set_terminate(report_terminate);
    if (pineforge::detail::ScratchArena::attached() != nullptr) {
        std::printf("SPILL_PAGE skip reason=\"a scratch arena is attached: the journal's spill"
                    " has no descriptor of its own to overwrite\"\n");
        return 77;
    }
    try {
        return run_measurement();
    } catch (const std::exception& error) {
        return report_invalid(std::string("an exception outside the read under test: ")
                              + error.what());
    }
}
