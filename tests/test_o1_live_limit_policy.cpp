// Proposed native live-object contract, using only the baseline Shape/Store API.
// Source preparation only: admission above the former cap needs disk capacity
// measured on spark2. Unexpected admission refusals and infrastructure failures
// never count as PASS.
//
// Rows:
//   raised  [--objects N] [--limit N]
//   default [--objects N] [--policy unset|zero]
//   lower   [--limit N]
//   invalid
// CLI errors exit 2 and are harness errors, never native-input validation.
#include "a5u_test_store.hpp"

#include <pineforge/detail/scratch_errors.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <typeinfo>
#include <vector>

namespace {

using pineforge::RunFailureCode;
using pineforge::classify_run_failure;
using pineforge::detail::ScratchCapacityError;
using pineforge::detail::o1::Id;
using pineforge::detail::o1::Shape;
using pineforge::detail::o1::Stats;
using pineforge::detail::o1::Store;
using pineforge::detail::o1::kNullId;

class HarnessInputError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RequirementFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(bool condition, const char* reason) {
    if (!condition) throw RequirementFailure(reason);
}

struct Options {
    std::string row;
    std::uint64_t objects = UINT64_C(1048577);
    std::uint64_t limit = UINT64_C(2097152);
    bool zero_policy = false;
};

std::uint64_t positive_decimal(const std::string& text) {
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value == 0)
        throw HarnessInputError("numeric workload arguments must be positive uint64 decimals");
    return value;
}

Options parse_options(int argc, char** argv) {
    if (argc < 2)
        throw HarnessInputError("select raised, default, lower, or invalid");
    Options options;
    options.row = argv[1];
    if (options.row != "raised" && options.row != "default" &&
        options.row != "lower" && options.row != "invalid")
        throw HarnessInputError("unknown row");
    if (options.row == "lower") options.limit = 8;
    if (options.row == "default" || options.row == "invalid") options.limit = 0;

    bool saw_objects = false;
    bool saw_limit = false;
    bool saw_policy = false;
    for (int index = 2; index < argc; index += 2) {
        if (index + 1 == argc) throw HarnessInputError("option needs a value");
        const std::string key = argv[index];
        const std::string value = argv[index + 1];
        if (key == "--objects" &&
            (options.row == "raised" || options.row == "default") && !saw_objects) {
            options.objects = positive_decimal(value);
            saw_objects = true;
        } else if (key == "--limit" &&
                   (options.row == "raised" || options.row == "lower") && !saw_limit) {
            options.limit = positive_decimal(value);
            saw_limit = true;
        } else if (key == "--policy" && options.row == "default" && !saw_policy) {
            if (value != "unset" && value != "zero")
                throw HarnessInputError("--policy must be unset or zero");
            options.zero_policy = value == "zero";
            saw_policy = true;
        } else {
            throw HarnessInputError("unknown, duplicate, or inapplicable option");
        }
    }
    if (options.row == "lower") options.objects = options.limit;
    if (options.row == "invalid") options.objects = 0;
    if (options.row == "raised" && options.objects > options.limit)
        throw HarnessInputError("raised admission workload must fit its requested finite limit");
    // This bounds arithmetic in this workload, not the native policy domain.
    // A raised limit can still be any positive uint64, including UINT64_MAX.
    if (options.objects > std::numeric_limits<std::uint64_t>::max() / sizeof(std::uint64_t))
        throw HarnessInputError("workload logical-byte total is not representable");
    return options;
}

const char* exception_class(const std::exception& error) {
    if (dynamic_cast<const ScratchCapacityError*>(&error)) return "ScratchCapacityError";
    if (dynamic_cast<const RequirementFailure*>(&error)) return "RequirementFailure";
    if (dynamic_cast<const std::invalid_argument*>(&error)) return "std::invalid_argument";
    if (dynamic_cast<const std::system_error*>(&error)) return "std::system_error";
    if (dynamic_cast<const std::bad_alloc*>(&error)) return "std::bad_alloc";
    if (dynamic_cast<const std::overflow_error*>(&error)) return "std::overflow_error";
    if (dynamic_cast<const std::logic_error*>(&error)) return "std::logic_error";
    if (dynamic_cast<const std::runtime_error*>(&error)) return "std::runtime_error";
    return "std::exception";
}

bool is_cohort_refusal(const ScratchCapacityError& error) {
    const auto failure = classify_run_failure(error);
    return failure.code == RunFailureCode::scratch_capacity_exceeded &&
           failure.args && *failure.args == "{\"resource\":\"cohort\"}";
}

struct Observation {
    const Options& input;
    const Shape& shape;
    const std::vector<Id>& ids;
    const char* phase = "configure";
    a5u_test::FileBacking* backing = nullptr;
    Store* store = nullptr;
    std::uint64_t verified_ids = 0;
    std::uint64_t checksum = 0;
    bool readback_complete = false;
    std::int64_t signed_input = 0;

    void invalidate_readback() {
        verified_ids = 0;
        checksum = 0;
        readback_complete = false;
    }

    void report(const char* outcome, const std::exception* error = nullptr) const {
        const std::uint64_t encoded_limit = shape.max_live_objects;
        std::cerr << "row=" << input.row << " phase=" << phase << " outcome=" << outcome
                  << " requested_objects=" << input.objects << " input_limit=";
        if (input.row == "invalid") std::cerr << signed_input << "(signed-int64)";
        else if (input.row == "default")
            std::cerr << (input.zero_policy ? "0" : "unset");
        else std::cerr << input.limit;
        std::cerr << " shape_limit=" << encoded_limit
                  << " page_bytes=" << shape.page_bytes
                  << " resident_pages=" << shape.resident_pages
                  << " chunk_bytes=" << shape.chunk_bytes
                  << " journal_slots=" << shape.journal_slots
                  << " index_slots=" << shape.index_slots
                  << " retained_ids=" << ids.size()
                  << " verified_ids=" << verified_ids << " checksum=" << checksum
                  << " readback_complete=" << readback_complete;
        if (!ids.empty())
            std::cerr << " first_retained_id=" << ids.front() << " last_retained_id=" << ids.back();
        if (backing) {
            std::cerr << " backing=FileBacking"
                      << " backing_kind="
                      << (backing->kind() == pineforge::detail::ScratchBackingKind::disk
                              ? "disk" : "unexpected_non_disk")
                      << " backing_pages=" << backing->page_count()
                      << " backing_extent_bytes=" << backing->page_count() * backing->page_size();
        } else {
            std::cerr << " backing=unavailable";
        }
        std::cerr << " physical_capacity=unmeasured";
        if (store) {
            const Stats stats = store->stats();
            std::cerr << " successfully_live=" << stats.objects_live
                      << " stats.objects_live=" << stats.objects_live
                      << " stats.max_live_objects=" << stats.max_live_objects
                      << " stats.bytes_live=" << stats.bytes_live
                      << " stats.bytes_resident=" << stats.bytes_resident
                      << " stats.pages_spilled=" << stats.pages_spilled
                      << " stats.bytes_backing=" << stats.bytes_backing
                      << " stats.max_live_handles=" << stats.max_live_handles
                      << " stats.cleanup_skipped_rollback_only=" << stats.cleanup_skipped_rollback_only
                      << " stats.cleanup_failed=" << stats.cleanup_failed;
        } else {
            std::cerr << " store=not_opened stats=unavailable successfully_live=unavailable";
        }
        if (error) {
            const auto failure = classify_run_failure(*error);
            std::cerr << " exception_class=" << exception_class(*error)
                      << " exception_rtti=" << typeid(*error).name()
                      << " classified_code=" << pineforge::run_failure_code_name(failure.code)
                      << " classified_args=" << (failure.args ? *failure.args : "{}");
            const auto* capacity = dynamic_cast<const ScratchCapacityError*>(error);
            std::cerr << " exception_category="
                      << (capacity && is_cohort_refusal(*capacity)
                              ? "typed_cohort_refusal"
                              : "not_cohort_policy_evidence");
            if (const auto* system = dynamic_cast<const std::system_error*>(error))
                std::cerr << " system_code=" << system->code().value()
                          << " system_category=" << system->code().category().name();
            std::cerr << " what=[" << error->what() << "]";
        } else {
            std::cerr << " exception_class=none classified_code=none classified_args={}";
        }
        std::cerr << '\n';
    }
};

void require_unchanged(const Stats& before, const Stats& after) {
    require(after.objects_live == before.objects_live, "refusal changed live count");
    require(after.bytes_live == before.bytes_live, "refusal changed logical bytes");
    require(after.bytes_resident == before.bytes_resident, "refusal changed resident bytes");
    require(after.pages_spilled == before.pages_spilled, "refusal changed backing pages");
    require(after.bytes_backing == before.bytes_backing, "refusal changed backing bytes");
    require(after.max_live_handles == before.max_live_handles, "refusal consumed a Tx slot");
    require(after.cleanup_skipped_rollback_only == before.cleanup_skipped_rollback_only,
            "refusal changed cleanup diagnostics");
    require(after.cleanup_failed == before.cleanup_failed, "refusal caused cleanup failure");
    require(after.max_live_objects == before.max_live_objects, "refusal changed policy");
}

std::uint64_t object_bytes(std::uint64_t index, std::uint64_t requested) {
    // A real zero-payload header counts too; even a one-object workload also
    // has marker data. Every other object stores its own index-derived marker.
    return index == 0 && requested > 1 ? 0 : sizeof(std::uint64_t);
}

std::uint64_t marker(std::uint64_t index) {
    return UINT64_C(0xd38a91e65b7402cf) ^ ((index + 1) * UINT64_C(0x9e3779b97f4a7c15));
}

void write_marker(Store& store, Id id, std::uint64_t index, std::uint64_t requested) {
    if (object_bytes(index, requested) == 0) return;
    const auto value = marker(index);
    require(store.write(id, 0, &value, sizeof(value)) == sizeof(value), "short marker write");
}

void verify_retained(Observation& observation) {
    observation.invalidate_readback();
    Store& store = *observation.store;
    std::uint64_t logical_bytes = 0;
    for (std::size_t index = 0; index < observation.ids.size(); ++index) {
        const Id id = observation.ids[index];
        const auto bytes = object_bytes(index, observation.input.objects);
        require(store.alive(id), "retained ID is not alive");
        require(store.size(id) == bytes, "retained header has the wrong logical size");
        logical_bytes += bytes;
        if (bytes != 0) {
            std::uint64_t actual = 0;
            require(store.read(id, 0, &actual, sizeof(actual)) == sizeof(actual), "short marker read");
            require(actual == marker(index), "stored marker differs from the written data");
            observation.checksum = observation.checksum * UINT64_C(1099511628211) ^ actual;
        }
        ++observation.verified_ids;
    }
    require(store.stats().objects_live == observation.ids.size(), "live count differs from retained IDs");
    require(store.stats().bytes_live == logical_bytes, "logical-byte count differs from actual objects");
    observation.readback_complete = true;
}

void populate(Observation& observation, std::vector<Id>& ids) {
    Store& store = *observation.store;
    require(observation.input.objects <= ids.max_size(), "ID retention size is not representable");
    ids.reserve(static_cast<std::size_t>(observation.input.objects));
    for (std::uint64_t index = 0; index < observation.input.objects; ++index) {
        observation.phase = "population-create";
        observation.invalidate_readback();
        const Stats before = store.stats();
        const auto pages_before = observation.backing->page_count();
        Id id = kNullId;
        try {
            id = store.create(object_bytes(index, observation.input.objects), 1);
        } catch (const ScratchCapacityError& error) {
            // Log the actual exception before any diagnostic read can fail.
            // A cohort refusal remains a failing admission, even if its prefix
            // reads back perfectly. Other failures never become policy RED.
            observation.report("unexpected_refusal", &error);
            if (is_cohort_refusal(error)) {
                require_unchanged(before, store.stats());
                require(observation.backing->page_count() == pages_before,
                        "refused admission extended the file backing");
                if (!ids.empty())
                    require(!store.alive(ids.back() + 1), "refused admission left a partial object");
                observation.phase = "refused-prefix-readback";
                verify_retained(observation);
                observation.report("admission_failed_prefix_verified", &error);
            }
            throw;
        }
        require(id != kNullId, "create returned the null ID");
        require(ids.empty() || id > ids.back(), "population IDs are not strictly increasing");
        ids.push_back(id);
        observation.phase = "population-write";
        write_marker(store, id, index, observation.input.objects);
    }
    observation.phase = "population-readback";
    verify_retained(observation);
    require(store.stats().objects_live == observation.input.objects, "requested population not live");
    observation.report("population_verified");
}

void expect_refusal(Observation& observation, std::vector<Id>& ids) {
    Store& store = *observation.store;
    observation.invalidate_readback();
    const Stats before = store.stats();
    const auto pages_before = observation.backing->page_count();
    const auto ids_before = ids.size();
    bool refused = false;
    try {
        const Id unexpected = store.create(sizeof(std::uint64_t), 1);
        ids.push_back(unexpected);
    } catch (const ScratchCapacityError& error) {
        observation.report("boundary_exception", &error);
        require(is_cohort_refusal(error),
                "boundary exception must carry scratch_capacity_exceeded and resource cohort");
        refused = true;
    }
    require(refused, "finite policy admitted N+1");
    require_unchanged(before, store.stats());
    require(observation.backing->page_count() == pages_before, "refusal extended backing");
    require(ids.size() == ids_before, "refusal changed retained IDs");
}

Id check_finite_boundary(Observation& observation, std::vector<Id>& ids) {
    Store& store = *observation.store;
    const auto count = observation.input.objects;
    const Id next = ids.back() + 1;
    observation.phase = "finite-N-plus-one";
    expect_refusal(observation, ids);
    require(!store.alive(next), "refusal published a partial header");
    verify_retained(observation);

    observation.phase = "finite-release";
    observation.invalidate_readback();
    const Id original_first = ids.front();
    const auto bytes_before = store.stats().bytes_live;
    store.release(original_first);
    require(!store.alive(original_first), "release left the original alive");
    require(store.stats().objects_live == count - 1, "release did not reclaim one live slot");
    require(store.stats().bytes_live == bytes_before - object_bytes(0, count),
            "release did not reclaim logical bytes");
    observation.report("released");
    const Id replacement = store.create(object_bytes(0, count), 1);
    require(replacement == next, "refusal consumed an ID");
    ids.front() = replacement;
    write_marker(store, replacement, 0, count);
    observation.phase = "finite-replacement";
    verify_retained(observation);
    observation.report("replacement_verified");
    return replacement;
}

void check_lower(Observation& observation, std::vector<Id>& ids) {
    Store& store = *observation.store;
    const auto count = observation.input.objects;
    const Id replacement = check_finite_boundary(observation, ids);
    const Id last = ids.back();
    const auto last_index = count - 1;
    observation.phase = "lower-release-rollback";
    observation.invalidate_readback();
    {
        auto transaction = store.begin();
        store.release(last);
        require(!store.alive(last), "transactional release did not remove the ID");
        require(store.stats().objects_live == count - 1, "staged release count is wrong");
        observation.report("release_staged");
        transaction->rollback();
    }
    verify_retained(observation);
    observation.report("release_rollback_verified");

    observation.phase = "lower-create-rollback";
    observation.invalidate_readback();
    store.release(last);
    ids.pop_back();
    verify_retained(observation);
    require(store.stats().objects_live == count - 1, "committed release count is wrong");
    Id staged = kNullId;
    {
        auto transaction = store.begin();
        observation.invalidate_readback();
        staged = store.create(object_bytes(last_index, count), 1);
        require(staged == replacement + 1, "release/rollback consumed an unexpected ID");
        ids.push_back(staged);
        write_marker(store, staged, last_index, count);
        verify_retained(observation);
        observation.report("create_staged");
        observation.phase = "lower-transaction-N-plus-one";
        expect_refusal(observation, ids);
        require(!store.alive(staged + 1), "transactional refusal left a partial object");
        transaction->rollback();
    }
    ids.pop_back();
    observation.phase = "lower-create-rollback-readback";
    require(!store.alive(staged), "rolled-back create left its header alive");
    verify_retained(observation);
    require(store.stats().objects_live == count - 1, "create rollback did not restore live count");
    observation.report("create_rollback_verified");

    observation.phase = "lower-after-rollback-replacement";
    observation.invalidate_readback();
    const Id final_id = store.create(object_bytes(last_index, count), 1);
    require(final_id == staged + 1, "rollback must burn issued IDs; refusal must not consume one");
    ids.push_back(final_id);
    write_marker(store, final_id, last_index, count);
    require(!store.alive(last), "committed release was resurrected");
    verify_retained(observation);
}

int run_store_row(const Options& input) {
    Shape shape = a5u_test::shape(256);
    std::vector<Id> ids;
    Observation observation{input, shape, ids};
    // Keep the real fixture alive through failure reporting.
    std::unique_ptr<a5u_test::StoreFixture> fixture;
    try {
        // expectation corrected: helper default trusted -> compared with Shape,
        // because a fixture override must not hide the native default policy.
        require(shape.max_live_objects == Shape{}.max_live_objects,
                "fixture changed the native Shape default policy");
        if (input.row != "default") shape.max_live_objects = input.limit;
        else if (input.zero_policy) shape.max_live_objects = 0;
        observation.phase = "open";
        observation.report("opening");
        fixture = std::make_unique<a5u_test::StoreFixture>(shape);
        observation.backing = &fixture->backing;
        observation.store = fixture->store.get();
        observation.report("opened");
        require(observation.store->stats().objects_live == 0, "fresh store already contains objects");
        populate(observation, ids);
        // Check metadata after doing the work: the baseline's clamp must not
        // short-circuit the requested above-cap allocation attempt.
        observation.phase = "policy-stats";
        require(observation.store->stats().max_live_objects == input.limit,
                "Store did not preserve the finite limit or report unbounded policy as zero");
        if (input.row == "lower") check_lower(observation, ids);
        // expectation corrected: raised admission only -> finite N+1 refusal,
        // because a raised limit must enforce its boundary as well as admit N.
        else if (input.row == "raised" && input.objects == input.limit)
            check_finite_boundary(observation, ids);
        observation.phase = "complete";
        observation.report("passed");
        return 0;
    } catch (const std::exception& error) {
        observation.report("failed", &error);
        return 1;
    }
}

int run_invalid(const Options& input) {
    Shape shape = a5u_test::shape(256);
    shape.max_live_objects = 8;
    const std::uint64_t original_limit = shape.max_live_objects;
    std::int64_t negative = -1;
    std::vector<Id> ids;
    Observation observation{input, shape, ids};
    observation.signed_input = negative;
    std::unique_ptr<a5u_test::FileBacking> backing;
    std::unique_ptr<Store> store;
    try {
        observation.phase = "prepare-empty-backing";
        backing = std::make_unique<a5u_test::FileBacking>();
        observation.backing = backing.get();
        const auto pages_before = backing->page_count();
        require(pages_before == 0, "invalid-input backing must start empty");
        bool rejected = false;
        bool assigning = true;
        try {
            observation.phase = "signed-assignment";
            shape.max_live_objects = negative;
            assigning = false;
            observation.phase = "signed-ingress-open";
            store = Store::open(shape, *backing);
            observation.store = store.get();
        } catch (const std::invalid_argument& error) {
            // This is a native typed catch. No CLI rejection, invented wire
            // code, or reference to a proposed checked-field type is involved.
            observation.report("native_invalid_argument", &error);
            // expectation corrected: assignment or open rejection -> assignment
            // only, because rejecting an unsigned wrap at open is too late.
            require(assigning, "negative input was rejected only after assignment");
            require(std::string(error.what()).find("max_live_objects") != std::string::npos,
                    "negative assignment error did not name max_live_objects");
            require(!store, "negative input published a Store");
            require(backing->page_count() == pages_before,
                    "negative input mutated the backing before rejection");
            require(shape.max_live_objects == original_limit,
                    "rejected assignment changed the previous native value");
            rejected = true;
        }
        require(rejected, "signed -1 assignment and Store::open both accepted negative input");
        observation.phase = "complete";
        observation.report("passed");
        return 0;
    } catch (const std::exception& error) {
        observation.report("failed", &error);
        return 1;
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options input = parse_options(argc, argv);
        return input.row == "invalid" ? run_invalid(input) : run_store_row(input);
    } catch (const HarnessInputError& error) {
        std::cerr << "outcome=harness_input_error not_native_validation=1 what=["
                  << error.what() << "]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "outcome=failed_before_row exception_class=" << exception_class(error)
                  << " what=[" << error.what() << "]\n";
        return 1;
    } catch (...) {
        std::cerr << "outcome=failed exception_class=unknown\n";
        return 1;
    }
}
