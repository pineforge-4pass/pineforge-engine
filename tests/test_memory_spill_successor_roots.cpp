// Successor-root backing growth, retention past request end and explicit slab
// bounds are checked, along with release at core reset.
// Include the actual implementation to complete the named
// private storage type. Explicit instantiation below grants test-side member
// access; no alternate layout, product accessor, or implementation is supplied.
#include "../src/native_order.cpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace memory_spill_successor_roots {
namespace no = pineforge::native_order;
namespace pd = pineforge::detail;
using Core = no::WorkingRequestCore;
using Handle = no::RequestHandle;
using Word = std::uint64_t;
using Row = std::pair<Word, Word>;

template<class Tag, auto Member> struct Access {
    friend auto access(Tag) { return Member; }
};
struct Roots { friend auto access(Roots); };
template struct Access<Roots, &Core::successor_roots_>;

std::size_t checks = 0, failures = 0;
std::string trial;
struct PrerequisiteFailure final : std::exception {};

bool check(bool passed, const char* expression, int line) {
    ++checks;
    if (!passed) {
        ++failures;
        std::fprintf(stderr, "FAIL [successor-roots] %s:%d: %s (trial=%s)\n",
                     __FILE__, line, expression, trial.c_str());
    }
    return passed;
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)
// Only a prerequisite needed for safe observation ends a trial. All ordinary
// proof failures continue, and the dispatcher always runs the remaining trials.
#define NEED(expression) do { \
    if (!check(bool(expression), "fixture prerequisite: " #expression, __LINE__)) \
        throw PrerequisiteFailure{}; \
} while (false)

template<class Action>
void run(const std::string& name, Action&& action) {
    trial = name;
    try {
        action();
    } catch (const PrerequisiteFailure&) {
        // Already recorded. Continue with the next independent trial.
    } catch (const std::exception& error) {
        check(false, error.what(), __LINE__);
    } catch (...) {
        check(false, "unknown exception", __LINE__);
    }
}

// Two fixed-width words are written verbatim by prepare_append; neither
// sizeof(the private Row) nor the observed file length supplies this model.
constexpr Word kRowBytes = 2 * sizeof(Word);
constexpr std::size_t kDefaultRows = 64; // Derived from TailWindowConfig's default.

no::Request request() {
    no::Request value{no::Transact{1.0}, "moving-root", ""};
    value.trigger = no::Limit{90.0};
    return value;
}

struct Model {
    std::vector<Row> rows;
    std::size_t flushed = 0;
    std::size_t capacity;

    explicit Model(std::size_t configured)
        : capacity(configured > 1 ? configured : 1) {}

    // Preparation precedes every incarnation-consuming command, including
    // rootless submissions. It writes the full pending batch before commit.
    // This recurrence consumes only our command history and requested policy.
    void prepare() {
        if (rows.size() - flushed == capacity) flushed = rows.size();
    }
};

struct Driver {
    no::RunIdentity identity{"successor-roots", 1};
    Core core;
    Word incarnation = 1, ordinal = 1;
    bool direct;

    Driver(bool use_direct, no::TailWindowConfig windows)
        : core(identity, windows), direct(use_direct) {}

    Handle submit() {
        const Handle expected{identity, incarnation};
        no::SubmitResult result;
        if (direct) {
            result = core.apply_submit(request(), {}, incarnation, ordinal).result;
            ++incarnation;
            ++ordinal;
        } else {
            result = core.submit(request(), 0, incarnation, ordinal);
        }
        NEED(result.status == no::SubmitStatus::Accepted && result.handle);
        CHECK(*result.handle == expected);
        return *result.handle;
    }

    Handle replace(Handle target) {
        const Handle expected{identity, incarnation};
        no::ReplaceResult result;
        if (direct) {
            result = core.apply_replace(target, request(), {}, incarnation, ordinal).result;
            ++incarnation;
            ++ordinal;
        } else {
            result = core.replace(target, request(), 0, incarnation, ordinal);
        }
        NEED(result.status == no::ReplaceStatus::Replaced && result.successor);
        CHECK(*result.successor == expected && *result.successor != target);
        return *result.successor;
    }

    void cancel(Handle target) {
        no::CancelResult result;
        if (direct) {
            result = core.apply_cancel(target, ordinal).result;
            ++ordinal;
        } else {
            result = core.cancel(target, ordinal);
        }
        CHECK(result.status == no::CancelStatus::Cancelled);
    }
};

void observe(const Driver& driver, const Model& model,
             const std::shared_ptr<pd::ScratchSlab>& slab = {}) {
    const auto& index = driver.core.*access(Roots{});
    NEED(index);
    CHECK(index->tail.size() == model.capacity);
    CHECK(index->disk_rows == model.flushed
          && index->tail_size == model.rows.size() - model.flushed);
    CHECK(index->tail_size <= model.capacity
          && index->disk_rows + index->tail_size == model.rows.size());
    CHECK(index->slab == slab);
    if (slab) {
        const auto& region = slab->region(pd::ScratchResource::successor_roots);
        CHECK(region.capacity == slab->manifest().R * kRowBytes);
        CHECK(region.logical_extent == model.flushed * kRowBytes);
        CHECK(index->disk_rows + index->tail_size <= slab->manifest().R);
        CHECK(!index->file.opened());
    } else {
        CHECK(index->file.storage_bytes() == model.flushed * kRowBytes);
        CHECK(index->file.disk_backed() == (model.flushed != 0));
        CHECK(!index->file.arena_backed());
    }

    // Compare the whole authoritative sequence, reading disk bytes directly
    // rather than using find()/read(), which share the production row decoder.
    std::vector<Row> actual;
    for (Word i = 0; i < index->disk_rows; ++i) {
        std::array<Word, 2> words{};
        if (slab) {
            slab->read_at(pd::ScratchResource::successor_roots,
                          i * kRowBytes, words.data(), kRowBytes);
        } else {
            index->file.read(i * kRowBytes, words.data(), kRowBytes);
        }
        actual.emplace_back(words[0], words[1]);
    }
    // A broken size must be reported without indexing past the allocation.
    if (CHECK(index->tail_size <= index->tail.size())) {
        for (std::size_t i = 0; i < index->tail_size; ++i)
            actual.emplace_back(index->tail[i].successor, index->tail[i].root);
    }
    CHECK(actual.size() == model.rows.size() && actual == model.rows);
}

void retire_journal(Driver& driver) {
    const auto retained = driver.core.history().size();
    CHECK(driver.core.retire_history(driver.core.last_ordinal()) == retained);
    CHECK(driver.core.history().empty());
}

void reset_releases_index(Driver& driver) {
    // A weak observer does not extend the backing file's ownership.
    std::weak_ptr old = driver.core.*access(Roots{});
    driver.core.reset({"successor-roots", 2});
    CHECK(old.expired() && !(driver.core.*access(Roots{})));
    CHECK(driver.core.live_size() == 0 && driver.core.history().empty());
}

struct Growth {
    Word bytes = 0;
    std::size_t rows = 0, capacity = 0;
};

Growth file_growth(std::size_t replacements, bool direct, std::size_t configured) {
    no::TailWindowConfig windows;
    CHECK(windows.successor_root_rows == kDefaultRows);
    windows.successor_root_rows = configured;
    Driver driver(direct, windows);
    Model model(configured);
    // Three independently rooted chains prevent a constant-root fake model.
    std::array<Handle, 3> roots{driver.submit(), driver.submit(), driver.submit()};
    auto tails = roots;
    // Enroll while the root is live: WorkingRequestCore::cohort_add
    // rejects a terminal origin and records Applied only after inserting its
    // canonical root. Adding after cancellation would leave an empty roster.
    const auto cohort = driver.core.cohort_open();
    driver.core.cohort_add(cohort, roots.front());
    CHECK(!driver.core.cohort_receipts().empty()
          && driver.core.cohort_receipts().back().status == no::CohortReceiptStatus::Applied);
    observe(driver, model);
    for (std::size_t i = 0; i < replacements; ++i) {
        const auto slot = i % tails.size();
        const Word predicted_successor = driver.incarnation;
        model.prepare();
        tails[slot] = driver.replace(tails[slot]);
        model.rows.emplace_back(predicted_successor, roots[slot].incarnation);
        if (i + 1 == model.capacity) {
            observe(driver, model); // A full tail still has no disk prefix.
            model.prepare();
            const auto plain = driver.submit(); // Flush, but append NO root.
            observe(driver, model);
            driver.cancel(plain);
            observe(driver, model);
        }
        if (i == 0 || (i + 1) % model.capacity == 1 || i + 1 == replacements)
            observe(driver, model);
    }
    CHECK(model.rows.size() == replacements);
    for (const auto tail : tails) driver.cancel(tail);
    CHECK(driver.core.live_size() == 0);
    observe(driver, model); // Ending every request does not retire any root.
    retire_journal(driver);
    observe(driver, model); // Window-style history retirement also keeps roots.

    // Exercise root lookup after all requests AND journal definitions are gone.
    // Each replacement i advances chain i % 3. Both replacement paths inherit
    // that chain's root, and WorkingRequestCore::cohort_contains uses the
    // root index for terminal lookup, then the roster.
    for (std::size_t i = 0; i < model.rows.size(); ++i) {
        const Handle alias{driver.identity, model.rows[i].first};
        CHECK(driver.core.cohort_contains(cohort, alias) == (i % roots.size() == 0));
    }
    observe(driver, model);
    const auto& index = driver.core.*access(Roots{});
    NEED(index);
    const Growth result{index->file.storage_bytes(),
                        static_cast<std::size_t>(index->disk_rows) + index->tail_size,
                        index->tail.size()};
    CHECK(result.bytes > 0 && result.rows == replacements);
    reset_releases_index(driver);
    return result;
}

void slab_limit(Word limit, bool direct) {
    pd::ScratchManifest manifest{};
    manifest.R = limit; // An explicit parameter, not an inferred file-mode cap.
    auto slab = std::make_shared<pd::ScratchSlab>(manifest);
    Driver driver(direct, no::TailWindowConfig{});
    driver.core.bind_scratch_slab(slab);
    Model model(kDefaultRows);
    const auto root = driver.submit(); // R=0 still permits rootless commands.
    auto tail = root;
    observe(driver, model, slab);
    for (Word i = 0; i < limit; ++i) {
        const Word predicted_successor = driver.incarnation;
        model.prepare();
        tail = driver.replace(tail);
        model.rows.emplace_back(predicted_successor, root.incarnation);
    }
    observe(driver, model, slab);
    CHECK(model.rows.size() == limit);
    // These limits leave a partial batch: a missing admission check could
    // commit R+1 inside the allocated tail, which the row count would detect.
    const auto before_history = driver.core.history().size();
    const auto before_ordinal = driver.core.last_ordinal();
    const auto before_incarnation = driver.incarnation;
    const auto before_next_ordinal = driver.ordinal;
    bool refused = false;
    try {
        (void)driver.replace(tail);
    } catch (const pd::ScratchCapacityError& error) {
        const auto failure = pineforge::classify_run_failure(error);
        CHECK(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded);
        CHECK(failure.args && *failure.args == "{\"resource\":\"successor_roots\"}");
        refused = true;
    }
    CHECK(refused);
    CHECK(driver.core.history().size() == before_history
          && driver.core.last_ordinal() == before_ordinal
          && driver.incarnation == before_incarnation
          && driver.ordinal == before_next_ordinal);
    observe(driver, model, slab);
    retire_journal(driver);
    observe(driver, model, slab); // Admission R does not renew on retirement.
    reset_releases_index(driver);
}
} // namespace memory_spill_successor_roots

int main() {
    using namespace memory_spill_successor_roots;
    for (const bool direct : {false, true}) {
        const std::string mode = direct ? "/direct" : "/prepared";
        for (const std::size_t configured : {std::size_t{64}, std::size_t{7}, std::size_t{0}}) {
            run("file/window=" + std::to_string(configured) + mode, [&] {
                // PER ORDER COMMAND: N and 4N replacements, three live chains
                // in both cases. Disk grows; the requested resident window does not.
                constexpr std::size_t n = 193;
                const auto small = file_growth(n, direct, configured);
                const auto large = file_growth(4 * n, direct, configured);
                CHECK(small.rows == n && large.rows == 4 * n);
                CHECK(small.capacity == large.capacity
                      && small.capacity == (configured > 1 ? configured : 1));
                CHECK(large.bytes > small.bytes);
            });
        }
        for (const Word limit : {Word{0}, Word{19}, Word{131}})
            run("slab/R=" + std::to_string(limit) + mode, [&] { slab_limit(limit, direct); });
    }
    std::printf("memory proof spill_successor_roots: %zu checks, %zu failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
