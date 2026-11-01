// Pending-book storage follows admissions, fills and retirement.
// Include the implementation verbatim to complete the named BookSpill type,
// as test_adapter_tree_ram does for its private implementation types. No
// alternate core, injected rows, or product accessor is used; archive byte
// expectations use a sizeof model of the production format.
#include "../src/native_order.cpp"
#include <pineforge/source/pine_strategy_host.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace memory_pending_book {
using namespace pineforge;
namespace no = pineforge::native_order;
using Adapter = source::PineExecutionAdapter;
using Handle = no::RequestHandle;
using Word = std::uint64_t;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Spill { friend auto access(Spill); };
struct Repriced { friend auto access(Repriced); };
struct Cohorts { friend auto access(Cohorts); };
struct LivePositions { friend auto access(LivePositions); };
template struct Access<Spill, &no::WorkingRequestCore::book_spill_>;
template struct Access<Repriced, &no::WorkingRequestCore::repriced_>;
template struct Access<Cohorts, &Adapter::cohorts_by_id_>;
template struct Access<LivePositions, &Adapter::live_origin_positions>;

std::size_t checks = 0, failures = 0;
const char* current_row = "startup";
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(const char* row, bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [%s] %s:%d: %s (trial=%s)\n",
                     row, __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define ROW_CHECK(row, expression) check(row, bool(expression), #expression, __LINE__)
#define NEED(expression) do { \
    if (!check(current_row, bool(expression), "fixture prerequisite: " #expression, __LINE__)) \
        throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run_trial(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // The failed prerequisite already names the row. Continue other trials.
    } catch (const std::exception& error) {
        check(current_row, false, error.what(), __LINE__);
    } catch (...) {
        check(current_row, false, "unknown exception", __LINE__);
    }
}

constexpr std::size_t kResident = 4096; // native_order.cpp: kBookMax, not a fixture override.
static_assert(kResident == no::kBookMax, "resident-book geometry matches the visible product constant");
constexpr char kLabel[] = "book-pending";
constexpr double kNa = std::numeric_limits<double>::quiet_NaN();
constexpr std::int64_t kStart = 1736121600000LL;

no::Request resting(bool host_sized, double price = 1.0) {
    no::Request request{no::Transact{1.0}, kLabel, ""};
    if (host_sized) request.intent = no::HostSized{no::HostSizedKind::Open, no::Side::Long};
    request.trigger = no::Limit{price};
    return request;
}

struct Book {
    no::RunIdentity run{"pending-book", 1};
    no::WorkingRequestCore core{run};
    Word incarnation = 1, ordinal = 1;
    Handle put(bool sized) {
        const auto result = core.submit(resting(sized), 0, incarnation, ordinal);
        NEED(result.status == no::SubmitStatus::Accepted && result.handle);
        return *result.handle;
    }
    Word reprice(Handle handle, bool sized, double price) {
        const Word new_priority = incarnation; // The next command's input sequence number.
        no::ReplaceOptions options;
        options.keep_handle = true;
        const auto result = core.replace(handle, resting(sized, price), 0,
                                         incarnation, ordinal, std::nullopt, options);
        NEED(result.status == no::ReplaceStatus::Replaced && result.successor);
        NEED(*result.successor == handle);
        return new_priority;
    }
    void cancel(Handle handle) {
        NEED(core.cancel(handle, ordinal).status == no::CancelStatus::Cancelled);
    }
};

// Independent arithmetic for the EXACT serialized shape of resting() after a
// keep-handle reprice. archive_value writes each trivial leaf with sizeof(T),
// variants with a uint32 tag, strings with a uint64 length, optionals with one
// presence byte, and a definition with one presence byte. Neither serialization
// helpers nor an observed blob/slot length is used to obtain this expectation.
// kept_successor preserves the initially absent predecessor/root/kept_binding;
// there is no evaluation, so allowance is unset and all sizing optionals empty.
Word record_bytes(bool sized) {
    static_assert(std::is_trivially_copyable_v<Handle>);
    static_assert(std::is_trivially_copyable_v<no::HostSized>);
    constexpr Word tag = sizeof(std::uint32_t), length = sizeof(Word), absent = 1;
    const Word request = tag + (sized ? sizeof(no::HostSized) : sizeof(no::Transact))
        + length + (sizeof(kLabel) - 1) + length // label, empty comment
        + tag + sizeof(no::Limit)
        + tag + sizeof(no::ImmediateRemaining)
        + tag + sizeof(no::Independent)
        + tag + sizeof(no::NoGroup)
        + tag + sizeof(no::Absolute);
    const Word definition = 1 + sizeof(Handle) + request + sizeof(no::Birth)
        + absent + sizeof(no::RequestOrigin) + absent + sizeof(Word) + absent;
    return definition
        + tag + (sized ? sizeof(no::RemainingDeferred) : sizeof(no::RemainingUnits))
        + tag + sizeof(no::BookTransaction)
        + tag + sizeof(no::LimitReady)
        + tag + sizeof(no::AllowanceUnset)
        + tag + sizeof(no::PendingNone) + 3 * absent;
}

// Keys in these two external tables are eight big-endian bytes. Decode them
// here rather than borrowing book_key_value() from the implementation.
Word key_number(const std::string& key) {
    NEED(key.size() == 8);
    Word result = 0;
    for (unsigned char byte : key) result = result * 256 + byte;
    return result;
}

using TableRow = std::tuple<Word, Word, Word>; // key, incarnation/priority, state
struct SpillModel {
    struct Entry { Word incarnation; bool dead = false; };
    std::vector<Word> resident;
    std::map<Word, Entry> rows; // full physical table, including tombstones
    std::map<Word, std::pair<Word, bool>> links;
    Word bytes_per_row, blob_bytes = 0, garbage = 0;
    std::size_t rebuilds = 0;
    explicit SpillModel(bool sized) : bytes_per_row(record_bytes(sized)) {}
    void spilled(Word priority, Handle handle) {
        rows.emplace(priority, Entry{handle.incarnation, false});
        links.emplace(handle.incarnation, std::make_pair(priority, false));
        blob_bytes += bytes_per_row;
    }
    void before_promotion() {
        const auto dead = static_cast<Word>(std::count_if(rows.begin(), rows.end(),
            [](const auto& row) { return row.second.dead; }));
        const Word live = rows.size() - dead;
        // Source-derived hysteresis: > twice the live count plus 64 dead rows,
        // OR garbage > half the file plus 1 MiB. Compute it on the model only.
        if (dead <= 2 * live + 64 && garbage <= blob_bytes / 2 + (Word{1} << 20)) return;
        for (auto at = rows.begin(); at != rows.end();) {
            if (at->second.dead) at = rows.erase(at);
            else ++at;
        }
        for (auto at = links.begin(); at != links.end();) {
            if (at->second.second) at = links.erase(at);
            else ++at;
        }
        blob_bytes = live * bytes_per_row;
        garbage = 0;
        ++rebuilds;
    }
    void retired(Word priority, Handle handle) {
        rows.at(priority).dead = true;
        links.at(handle.incarnation).second = true;
        garbage += bytes_per_row;
    }
};

enum class Field { Resident, Rows, Links, Blobs };

void observe_spill(const Book& book, const SpillModel& model, Field field) {
    const auto& spill = book.core.*access(Spill{});
    NEED(spill);
    if (field == Field::Resident) {
        std::vector<Word> actual;
        for (const auto& meta : spill->resident) actual.push_back(meta.priority);
        ROW_CHECK("workingrequestcore_bookspill_resident", spill->resident.size() == model.resident.size()
                  && spill->resident.size() <= kResident && actual == model.resident);
    } else if (field == Field::Rows) {
        std::vector<TableRow> actual, expected;
        for (auto at = spill->rows.first_sorted(); at != 0; at = spill->rows.next_sorted(at)) {
            const auto slot = spill->rows.get(at);
            actual.emplace_back(key_number(spill->rows.key(at)), slot.incarnation, slot.state);
        }
        for (const auto& row : model.rows)
            expected.emplace_back(row.first, row.second.incarnation, row.second.dead ? 1 : 0);
        ROW_CHECK("workingrequestcore_bookspill_rows", spill->rows.size() == model.rows.size() && actual == expected);
    } else if (field == Field::Links) {
        std::vector<TableRow> actual, expected;
        for (auto at = spill->links.first_sorted(); at != 0; at = spill->links.next_sorted(at)) {
            const auto slot = spill->links.get(at);
            actual.emplace_back(key_number(spill->links.key(at)), slot.priority, slot.state);
        }
        for (const auto& link : model.links)
            expected.emplace_back(link.first, link.second.first, link.second.second ? 1 : 0);
        ROW_CHECK("workingrequestcore_bookspill_links", spill->links.size() == model.links.size() && actual == expected);
    } else {
        ROW_CHECK("workingrequestcore_bookspill_blobs", spill->blobs.size() == model.blob_bytes);
    }
}

void spilled_book(Field field, std::size_t surplus, bool sized) {
    Book book;
    SpillModel model(sized);
    std::vector<Handle> handles;
    std::map<Word, Word> priority;
    const std::size_t population = kResident + surplus;
    for (std::size_t i = 0; i < population; ++i) {
        if (i >= kResident) {
            // Move the previous newest resident to a fresh priority, then let
            // the next real submission spill it. Every spill therefore fills
            // rows, links AND blobs through the production insert paths.
            const auto prior = handles.back();
            const Word p = book.reprice(prior, sized, 2.0);
            priority[prior.incarnation] = p;
            model.resident.back() = p;
            model.spilled(p, prior);
            model.resident.pop_back();
        }
        const auto handle = book.put(sized);
        handles.push_back(handle);
        priority[handle.incarnation] = handle.incarnation;
        model.resident.push_back(handle.incarnation);
    }
    // No fills or cancellations occurred: C resident + surplus spilled rows.
    NEED(book.core.live_size() == population);
    observe_spill(book, model, field);

    // Remove the C-1 oldest resident rows first. Promotions can then happen
    // without making room, so each remaining external entry has one lifetime:
    // spill -> tombstone -> optional rebuild. No model reads the product table.
    for (std::size_t i = 0; i + 1 < kResident; ++i) book.cancel(handles[i]);
    model.resident.erase(model.resident.begin(), model.resident.end() - 1);
    observe_spill(book, model, field);
    for (std::size_t i = kResident - 1; i + 1 < population; ++i) {
        model.before_promotion();
        book.cancel(handles[i]);
        model.retired(priority.at(handles[i].incarnation), handles[i]);
        observe_spill(book, model, field);
    }
    book.cancel(handles.back());
    model.resident.clear();
    NEED(book.core.live_size() == 0);
    NEED(model.rebuilds != 0); // Both surplus choices cross the >64 dead-row slack.
    observe_spill(book, model, field);
    // Lazy compaction keeps its final small tombstone suffix. Immediately
    // before the last promotion, alive=1 and gone<=2*1+64 unless a rebuild
    // runs. That promotion adds one tombstone: at most 2*1+64+1=67 remain.
    constexpr Word final_slots = 2 * 1 + 64 + 1;
    if (field == Field::Rows)
        ROW_CHECK("workingrequestcore_bookspill_rows", book.core.*access(Spill{}) != nullptr
                  && (book.core.*access(Spill{}))->rows.size() <= final_slots);
    if (field == Field::Links)
        ROW_CHECK("workingrequestcore_bookspill_links", (book.core.*access(Spill{}))->links.size() <= final_slots);
    if (field == Field::Blobs)
        ROW_CHECK("workingrequestcore_bookspill_blobs", (book.core.*access(Spill{}))->blobs.size() <= final_slots * record_bytes(sized));
}

void repriced_book(std::size_t width, bool sized) {
    Book book;
    std::set<Word> active;
    std::map<Word, Word> expected;
    std::size_t model_prunes = 0;
    const auto observe = [&] {
        const std::vector<std::pair<Word, Word>> wanted(expected.begin(), expected.end());
        const auto& actual = book.core.*access(Repriced{});
        ROW_CHECK("workingrequestcore_repriced", actual == wanted && actual.size() <= 2 * width + 8);
    };
    const auto reprice = [&](Handle handle, double price) {
        expected[handle.incarnation] = book.reprice(handle, sized, price);
        // The model retains historical keys until there are more than
        // 2*current-live+8 entries. Then only currently pending keys survive.
        if (expected.size() > 2 * active.size() + 8) {
            for (auto at = expected.begin(); at != expected.end();) {
                if (!active.count(at->first)) at = expected.erase(at);
                else ++at;
            }
            ++model_prunes;
        }
        observe();
    };
    for (int wave = 0; wave < 12; ++wave) {
        std::vector<Handle> pending;
        for (std::size_t i = 0; i < width; ++i) {
            const auto handle = book.put(sized);
            pending.push_back(handle);
            active.insert(handle.incarnation);
        }
        NEED(book.core.live_size() == width);
        for (const auto& handle : pending) {
            reprice(handle, 2.0);
            reprice(handle, 3.0); // Overwrite the same key, not another entry.
        }
        for (const auto& handle : pending) {
            book.cancel(handle);
            active.erase(handle.incarnation);
            observe(); // Cancellation leaves precisely the model's lazy keys.
        }
    }
    NEED(model_prunes != 0);
    // A final one-row book forces the large old suffix back to the small
    // book's bound; after cancellation one lazy key is exactly what remains.
    const auto last = book.put(sized);
    active.insert(last.incarnation);
    reprice(last, 2.0);
    ROW_CHECK("workingrequestcore_repriced", (book.core.*access(Repriced{})).size() == 1);
    book.cancel(last);
    active.clear();
    observe();
    ROW_CHECK("workingrequestcore_repriced", book.core.live_size() == 0
              && (book.core.*access(Repriced{})).size() == 1);
}

std::vector<Bar> tape(int count) {
    std::vector<Bar> result;
    for (int i = 0; i < count; ++i)
        result.push_back({100.0, 101.0, 99.0, 100.0, 1.0, kStart + i * 60000LL});
    return result;
}

// All cohort history here comes from accepted source commands. The model
// retains their handles in acceptance order, including cancelled predecessors;
// it selects global positions using only the script's current pending handle.
class OriginHost final : public source::PineStrategyHost {
public:
    OriginHost(int width, int hold, bool sized) : width_(width), hold_(hold), sized_(sized) {
        source::PineStrategyConfig config;
        config.initial_capital = 1e9;
        config.default_qty_type = static_cast<int>(QtyType::FIXED);
        config.default_qty_value = 1.0;
        config.pyramiding = width + 2;
        config.margin_long = config.margin_short = 0.0;
        configure_pine_strategy(config);
        set_syminfo_timezone("UTC");
        set_syminfo_session("24x7");
        set_syminfo_mintick(0.25);
    }
    void on_source_bar(const Bar&) override {
        const int bar = pine_bar_index();
        if (bar == 0 || (bar < hold_ && bar % 3 == 0)) {
            for (int slot = 0; slot < width_; ++slot) {
                const auto id = name(slot);
                strategy_order(id, true, sized_ ? kNa : 1.0,
                               1.0 + (bar % 2) * 0.25);
                Handle accepted{};
                std::size_t found = 0;
                for (const auto& working : native_working_requests()) {
                    if (working.definition->request.label == id) {
                        accepted = working.definition->handle;
                        ++found;
                    }
                }
                NEED(found == 1 && accepted.incarnation != 0);
                history_[id].push_back(accepted);
                pending_[id] = accepted;
                observe(); // Includes the replaced origin before a bar-open sweep.
            }
        }
        if (bar == hold_) {
            for (int slot = 0; slot < width_; ++slot) {
                strategy_cancel(name(slot));
                pending_.erase(name(slot));
                observe(); // False condition for this cohort, others still live.
            }
            finished = true;
        }
        const auto actual_count = observe();
        if (bar == hold_ - 1) held_count = actual_count;
    }
    std::size_t held_count = 0;
    bool finished = false;
private:
    std::string name(int slot) const { return "book-origin-" + std::to_string(slot); }
    std::size_t observe() const {
        const auto& cohorts = adapter_.*access(Cohorts{});
        std::size_t total = 0, model_total = 0;
        for (const auto& item : history_) {
            const auto cohort = cohorts.find(item.first);
            NEED(cohort != cohorts.end());
            std::vector<std::size_t> expected;
            const auto active = pending_.find(item.first);
            for (std::size_t position = 0; position < item.second.size(); ++position)
                if (active != pending_.end() && item.second[position] == active->second)
                    expected.push_back(position);
            std::sort(expected.rbegin(), expected.rend());
            // This is the returned production vector, not a reconstruction of it.
            const auto actual = (adapter_.*access(LivePositions{}))(cohort->second);
            ROW_CHECK("pineexecutionadapter_live_origin_positions_positions", actual == expected && actual.size() == expected.size());
            total += actual.size();
            model_total += expected.size();
        }
        ROW_CHECK("pineexecutionadapter_live_origin_positions_positions", total == model_total && total == pending_.size());
        return total;
    }
    int width_, hold_;
    bool sized_;
    std::map<std::string, std::vector<Handle>> history_;
    std::map<std::string, Handle> pending_;
};

void origin_positions() {
    for (int width : {8, 32}) for (bool sized : {false, true}) {
        std::size_t short_count = 0, long_count = 0;
        for (int hold : {32, 128}) {
            run_trial("origin width=" + std::to_string(width) + " bars=" + std::to_string(hold)
                      + (sized ? " default" : " explicit"), [&] {
                OriginHost host(width, hold, sized);
                const auto bars = tape(hold + 4);
                host.run(bars.data(), static_cast<int>(bars.size()), "1", "1");
                NEED(host.last_error().empty() && host.finished);
                ROW_CHECK("pineexecutionadapter_live_origin_positions_positions", host.held_count == static_cast<std::size_t>(width));
                if (hold == 32) short_count = host.held_count;
                else long_count = host.held_count;
            });
        }
        ROW_CHECK("pineexecutionadapter_live_origin_positions_positions", short_count == long_count
                  && short_count == static_cast<std::size_t>(width));
    }
}

// Physical market fills create the enrolled provenances. The request core
// then holds N never-filled, selected closes, each with an N-element vector.
// Replacement narrows each cohort to one provenance; cancellation retires its
// owning live row. Historical journal receipts are intentionally not counted.
class SelectedHost final : public NativeStrategyHost {
public:
    SelectedHost(std::size_t width, int hold) : width_(width), hold_(hold) {}
    void on_native_bar(const Bar&, const NativeDecisionContext&) override {
        ++bar_;
        if (bar_ == 1) {
            for (std::size_t i = 0; i < width_; ++i) {
                const auto result = submit(no::Request{no::Transact{1.0}, "book-open", ""});
                NEED(result.status == no::SubmitStatus::Accepted && result.handle);
                openings_.push_back(*result.handle);
            }
        } else if (bar_ == 2) {
            const auto lots = native_open_lots(100.0);
            NEED(lots.size() == width_ && physical_position().signed_units == double(width_));
            cycle_ = lots.front().cycle;
            for (std::size_t i = 0; i < width_; ++i) {
                auto reversed = openings_;
                std::reverse(reversed.begin(), reversed.end());
                const auto result = submit(close_request(reversed));
                NEED(result.status == no::SubmitStatus::Accepted && result.handle);
                expected_[result.handle->incarnation] = openings_;
            }
            observe();
        } else if (bar_ == hold_ + 2) {
            held_elements = observe_count();
            std::vector<Handle> old;
            for (const auto& item : expected_) old.push_back(Handle{openings_[0].run, item.first});
            NEED(old.size() == width_);
            for (std::size_t i = 0; i < old.size(); ++i) {
                const std::vector<Handle> singleton{openings_[i]};
                const auto result = replace(old[i], close_request(singleton));
                NEED(result.status == no::ReplaceStatus::Replaced && result.successor);
                expected_.erase(old[i].incarnation);
                expected_[result.successor->incarnation] = singleton;
                observe();
            }
            ROW_CHECK("openingsclose_openings", observe_count() == width_);
            std::vector<Handle> pending;
            for (const auto& item : expected_) pending.push_back(Handle{openings_[0].run, item.first});
            for (const auto& handle : pending) {
                NEED(cancel(handle).status == no::CancelStatus::Cancelled);
                expected_.erase(handle.incarnation);
                observe();
            }
            retired = true;
            // Only continue physical settlement when the live-vector population
            // is coherent; a failed retirement still reaches later rows.
            if (observe()) {
                const auto result = submit(no::Request{no::Flatten{}, "book-flat", ""});
                NEED(result.status == no::SubmitStatus::Accepted && result.handle);
                flatten_sent_ = true;
            }
        } else if (bar_ > hold_ + 2) {
            observe();
            if (flatten_sent_) NEED(physical_position().signed_units == 0.0);
        } else if (bar_ > 2) {
            observe();
        }
    }
    std::size_t held_elements = 0;
    bool retired = false;
private:
    const no::WorkingRequestCore& core() const {
        return as_native_consumer(execution_consumer()).request_core();
    }
    no::Request close_request(const std::vector<Handle>& members) const {
        no::Request request{no::Reduce{no::ExplicitUnits{1.0}}, "book-close", ""};
        request.trigger = no::Limit{250.0};
        request.owner = no::BindOpenings{members, cycle_};
        return request;
    }
    std::size_t observe_count() const {
        std::size_t result = 0;
        for (const auto& row : core().live())
            if (const auto* close = std::get_if<no::OpeningsClose>(&row.authority))
                result += close->openings.size();
        return result;
    }
    bool observe() const {
        bool equal = true;
        std::size_t actual_rows = 0, actual_elements = 0, expected_elements = 0;
        for (const auto& item : expected_) expected_elements += item.second.size();
        for (const auto& row : core().live()) {
            const auto* close = std::get_if<no::OpeningsClose>(&row.authority);
            if (!close) continue;
            ++actual_rows;
            actual_elements += close->openings.size();
            const auto model = expected_.find(row.handle().incarnation);
            equal = equal && model != expected_.end();
            if (model != expected_.end())
                equal = equal && close->openings == model->second && !close->external_openings;
        }
        return ROW_CHECK("openingsclose_openings", equal && actual_rows == expected_.size()
                         && actual_elements == expected_elements);
    }
    std::size_t width_;
    int hold_, bar_ = 0;
    std::int64_t cycle_ = 0;
    bool flatten_sent_ = false;
    std::vector<Handle> openings_;
    std::map<Word, std::vector<Handle>> expected_;
};

void selected_openings() {
    for (std::size_t width : {std::size_t{8}, std::size_t{32}}) {
        std::size_t short_count = 0, long_count = 0;
        for (int hold : {16, 64}) {
            run_trial("selected width=" + std::to_string(width) + " bars=" + std::to_string(hold), [&] {
                SelectedHost host(width, hold);
                NativeRunSpec spec;
                spec.identity = {"pending-window", 1};
                spec.input_tf = "1"; spec.script_tf = "1";
                spec.tickerid = "TEST:BOOK"; spec.timezone = "UTC"; spec.session = "24x7";
                spec.slot_label_policy = NativeSlotLabelPolicy::FeedTolerant;
                spec.initial_capital = 1e9; spec.point_value = 1.0; spec.account_fx = 1.0;
                spec.report_policy = NativeReportPolicy::HostRecorded;
                NEED(host.configure_native(spec).status == NativeSetupStatus::Applied);
                const auto bars = tape(hold + 6);
                host.run(bars.data(), static_cast<int>(bars.size()));
                NEED(host.native_state().kind == NativeLifecycleKind::Completed && host.retired);
                ROW_CHECK("openingsclose_openings", host.held_elements == width * width);
                if (hold == 16) short_count = host.held_elements;
                else long_count = host.held_elements;
            });
        }
        ROW_CHECK("openingsclose_openings", short_count == long_count && short_count == width * width);
    }
}

void resident_row() { for (std::size_t n : {96, 384}) for (bool sized : {false, true})
    run_trial("resident surplus=" + std::to_string(n) + (sized ? " host" : " explicit"),
              [&] { spilled_book(Field::Resident, n, sized); }); }
void rows_row() { for (std::size_t n : {96, 384}) for (bool sized : {false, true})
    run_trial("rows surplus=" + std::to_string(n) + (sized ? " host" : " explicit"),
              [&] { spilled_book(Field::Rows, n, sized); }); }
void links_row() { for (std::size_t n : {96, 384}) for (bool sized : {false, true})
    run_trial("links surplus=" + std::to_string(n) + (sized ? " host" : " explicit"),
              [&] { spilled_book(Field::Links, n, sized); }); }
void blobs_row() { for (std::size_t n : {96, 384}) for (bool sized : {false, true})
    run_trial("blobs surplus=" + std::to_string(n) + (sized ? " host" : " explicit"),
              [&] { spilled_book(Field::Blobs, n, sized); }); }
void repriced_row() { for (std::size_t n : {16, 64}) for (bool sized : {false, true})
    run_trial("repriced width=" + std::to_string(n) + (sized ? " host" : " explicit"),
              [&] { repriced_book(n, sized); }); }

} // namespace memory_pending_book

int main() {
    using namespace memory_pending_book;
    struct Row { const char* id; void (*run)(); };
    const Row rows[] = {
        {"pineexecutionadapter_live_origin_positions_positions", origin_positions},
        {"workingrequestcore_bookspill_resident", resident_row},
        {"workingrequestcore_bookspill_rows", rows_row},
        {"workingrequestcore_bookspill_links", links_row},
        {"workingrequestcore_bookspill_blobs", blobs_row},
        {"workingrequestcore_repriced", repriced_row},
        {"openingsclose_openings", selected_openings},
    };
    for (const auto& row : rows) {
        current_row = row.id;
        run_trial(row.id, row.run);
    }
    std::printf("memory proof pending_book: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
