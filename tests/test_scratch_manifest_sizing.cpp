// Written before the manifest change. On 81d0f632c, policy fails because U
// remains 65,536 at 200,000 bars; --admission fails at distinct id 65,537.
#include <pineforge/detail/disk_resident_id_table.hpp>
#include <pineforge/detail/scratch_defaults.hpp>
#include <pineforge/detail/scratch_slab.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {
namespace detail = pineforge::detail;
using Word = std::uint64_t;

void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

std::array<Word, 11> fields(const detail::ScratchManifest& m) {
    return {{m.K_q, m.B_q, m.K_e, m.B_e, m.K_c, m.B_c,
             m.U, m.B_U, m.V, m.B_V, m.R}};
}

void typed_resource(const detail::ScratchCapacityError& error, const char* resource) {
    const auto failure = pineforge::classify_run_failure(error);
    require(failure.code == pineforge::RunFailureCode::scratch_capacity_exceeded
                && failure.args
                && *failure.args == std::string("{\"resource\":\"") + resource + "\"}",
            "capacity refusal must carry the typed resource");
}

void sized_fields(const detail::ScratchManifest& m, Word rows, Word bars) {
    require(m.U == rows, "manifest U must follow resident sizing above the floor");
    require(m.K_e == 4 * rows && m.K_c == rows / 2, "sized key-count ratios");
    require(m.B_U == 64 * rows && m.B_e == 96 * m.K_e
                && m.B_c == 129 * m.K_c, "sized byte budgets");
    require(m.K_q == 0 && m.B_q == 0 && m.V == 27 && m.B_V == 6912
                && m.R == 2 * bars, "q, rank and finite root semantics stay fixed");
}

void floor_and_sized_policy() {
    for (const Word bars : {Word{0}, Word{1000}, Word{65536}}) {
        const auto m = detail::default_scratch_manifest(bars);
        require(fields(m) == std::array<Word, 11>{{
                    0, 0, 262144, 25165824, 32768, 4227072,
                    65536, 4194304, 27, 6912, 2 * bars}},
                "every floor field must retain its historical value");
    }
    auto stream_expected = fields(detail::default_scratch_manifest(0));
    stream_expected.back() = 65536;
    require(fields(detail::default_scratch_manifest()) == stream_expected,
            "stream keeps the fixed floor tier, including roots");

    const auto mid_sizing = detail::resident_cohort_sizing(200000);
    require(mid_sizing.limit == 200000, "resident sizing selects the mid tier");
    sized_fields(detail::default_scratch_manifest(200000), mid_sizing.limit, 200000);
    const auto mid = detail::default_scratch_manifest(200000);
    require(mid.U == 200000 && mid.K_e == 800000 && mid.K_c == 100000,
            "actual mid manifest has the requested counts");

    const Word cap = detail::kMaxResidentCohortsCap;
    const auto m = detail::default_scratch_manifest(cap);
    sized_fields(m, detail::resident_cohort_sizing(cap).limit, cap);
    require(m.U == 1048576 && m.K_e == 4194304 && m.K_c == 524288,
            "actual cap manifest has the requested counts");
    sized_fields(detail::default_scratch_manifest(cap + 1), cap, cap + 1);
    // Odd rows intentionally truncate K_c=S/2; no rounding up of admission.
    sized_fields(detail::default_scratch_manifest(65537), 65537, 65537);
}

// A dependent call lets this whole test compile on the frozen one-argument
// base. Missing demand support is a runtime failure, never a skipped test.
struct MakeManifest {
    template<class... Args>
    auto operator()(Args... args) const
        -> decltype(detail::default_scratch_manifest(args...)) {
        return detail::default_scratch_manifest(args...);
    }
};

template<class Make>
void declared_and_saturating_demand(Make make) {
    if constexpr (std::is_invocable_v<Make, Word, Word, Word, Word, bool>) {
        const Word cap = detail::kMaxResidentCohortsCap;
        const auto sizing = detail::resident_cohort_sizing(100000, 2, 7, 11, true);
        require(sizing.need == 200018, "all four demand terms contribute");
        sized_fields(make(Word{100000}, Word{2}, Word{7}, Word{11}, true),
                     sizing.limit, 100000);
        // Explicit reuse can ask for the floor even on a longer finite run.
        sized_fields(make(cap, Word{0}, Word{7}, Word{11}, true), 65536, cap);
        bool refused = false;
        try {
            (void)make(cap + 1, Word{1}, Word{0}, Word{0}, true);
        } catch (const detail::ScratchCapacityError& error) {
            refused = true;
            typed_resource(error, "cohort");
            const std::string text = error.what();
            require(text.find(detail::scratch_limit_number(cap)) != std::string::npos
                        && text.find(detail::scratch_limit_number(cap + 1))
                            != std::string::npos, "declared refusal names limit and need");
        }
        require(refused, "declared cap-plus-one must not silently clamp");

        const auto maximum = std::numeric_limits<Word>::max();
        const Word bars = maximum / 2;
        const auto saturated = detail::resident_cohort_sizing(bars, maximum, 7, 11);
        require(saturated.need == maximum && saturated.limit == cap,
                "resident demand saturates without wrapping");
        sized_fields(make(bars, maximum, Word{7}, Word{11}, false), cap, bars);
        refused = false;
        try {
            (void)make(bars, maximum, Word{7}, Word{11}, true);
        } catch (const detail::ScratchCapacityError& error) {
            refused = true;
            typed_resource(error, "cohort");
            require(std::string(error.what()).find(detail::scratch_limit_number(maximum))
                        != std::string::npos, "saturated declared refusal retains need");
        }
        require(refused, "saturated declared demand must refuse");
    } else {
        require(false, "default_scratch_manifest must accept resident sizing inputs");
    }
}

void monotonicity_and_root_overflow() {
    const Word cap = detail::kMaxResidentCohortsCap;
    const auto maximum = std::numeric_limits<Word>::max();
    const std::array<Word, 11> bars{{
        0, 1, 65535, 65536, 65537, 100000, 200000, cap, cap + 1,
        maximum / 2 - 1, maximum / 2}};
    auto previous = fields(detail::default_scratch_manifest(0));
    for (const auto count : bars) {
        const auto m = detail::default_scratch_manifest(count);
        sized_fields(m, detail::resident_cohort_sizing(count).limit, count);
        const auto current = fields(m);
        for (std::size_t i = 0; i < current.size(); ++i)
            require(current[i] >= previous[i], "no manifest field may decrease with bars");
        previous = current;
    }
    for (const Word count : {maximum / 2 + 1, maximum - 1, maximum}) {
        bool refused = false;
        try {
            (void)detail::default_scratch_manifest(count);
        } catch (const detail::ScratchCapacityError& error) {
            refused = true;
            typed_resource(error, "layout");
        }
        require(refused, "root overflow must refuse rather than wrap");
    }
}

struct LiveCohort {
    struct Record {
        Word handle;
        std::int64_t cycle;
        Word count, digest, sides;
    };
    Word handle = 0;
    bool live = true;
    Record retired_record() const noexcept { return {handle, 0, 0, 0, 0}; }
    static LiveCohort restore_record(const Record& row) { return {row.handle, false}; }
    bool spillable() const noexcept { return !live; }
};

void real_admission() {
    constexpr Word bars = 100000;
    const auto manifest = detail::default_scratch_manifest(bars);
    auto slab = std::make_shared<detail::ScratchSlab>(manifest);
    detail::DiskResidentIdTable<LiveCohort> table;
    table.bind_scratch(slab, detail::ScratchResource::cohort,
                       detail::resident_cohort_sizing(bars));
    for (Word i = 0; i < bars; ++i) {
        const auto inserted = table.emplace("manifest-" + std::to_string(i),
                                            LiveCohort{i + 1, true});
        require(inserted.second && !inserted.first->second.spillable(),
                "every sized live id must be admitted without spilling");
    }
    require(table.size() == bars && table.resident_size() == bars,
            "the sized slab retains all 100,000 distinct live ids");
    const auto extent = slab->region(detail::ScratchResource::cohort).logical_extent;
    bool refused = false;
    try {
        (void)table.emplace("manifest-overflow", LiveCohort{bars + 1, true});
    } catch (const detail::ScratchCapacityError& error) {
        refused = true;
        typed_resource(error, "cohort");
        require(std::string(error.what()).find(detail::scratch_limit_number(bars))
                    != std::string::npos, "next-id refusal names the sized bound");
    }
    require(refused && table.size() == bars && table.resident_size() == bars
                && slab->region(detail::ScratchResource::cohort).logical_extent == extent
                && table.work_counters().rows_spilled == 0,
            "next distinct id must refuse typed without changing occupancy or spilling");
}

void emit_native_manifests() {
    // Python compares all eleven fields for these same three pinned sizes.
    for (const Word bars : {Word{0}, Word{200000}, Word{1048576}}) {
        const auto values = fields(detail::default_scratch_manifest(bars));
        std::cout << '[' << bars;
        for (const auto value : values) std::cout << ',' << value;
        std::cout << "]\n";
    }
}
} // namespace

int main(int argc, char** argv) {
    try {
        const std::string mode = argc == 2 ? argv[1] : "";
        if (mode == "--admission") {
            real_admission();
        } else {
            require(mode.empty() || mode == "--manifest-values", "unknown test mode");
            floor_and_sized_policy();
            declared_and_saturating_demand(MakeManifest{});
            monotonicity_and_root_overflow();
            if (mode == "--manifest-values") {
                emit_native_manifests();
                return 0;
            }
        }
        std::puts("scratch manifest sizing: OK");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "scratch manifest sizing: %s\n", error.what());
        return 1;
    }
}
