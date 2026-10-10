/*
 * selected_window_plan_c.cpp - the C bridge to the selected primary planner.
 * include/pineforge/selected_window_plan.h holds the contract and
 * include/pineforge/selected_window_plan.hpp the planner's.
 *
 * The bridge only moves values across the boundary. It validates the two
 * descriptors, copies the five request strings into a SelectedPlanRequest, reads
 * the caller's pf_bar_t rows in place as pineforge::Bar (the c_abi.cpp
 * convention; no row is copied), calls pineforge::plan_selected_primary once and
 * maps the plan into a local, zeroed result that is committed to the caller with
 * one whole assignment. It keeps no state, constructs no strategy or engine,
 * calls no host or library callback and repeats no calendar, count or hash
 * logic. It is compiled only into the standalone window-plan shared
 * library, which links the kernel archive and nothing of the source layer: this
 * file is in neither PINEFORGE_KERNEL_SOURCES nor libpineforge.a, so no existing
 * artifact gains a symbol.
 */

#include <pineforge/selected_window_plan.h>
#include <pineforge/selected_window_plan.hpp>

#include <pineforge/bar.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <optional>
#include <string>

/* ── Layout ─────────────────────────────────────────────────────── */

/* pf_bar_t is read in place as pineforge::Bar, the convention every feed setter
 * in c_abi.cpp uses. The two are asserted layout-identical here as well, so this
 * library does not lean on that file's asserts, and against the documented
 * layout itself: five doubles, then the int64 timestamp. */
static_assert(sizeof(pf_bar_t) == sizeof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar size mismatch");
static_assert(alignof(pf_bar_t) == alignof(pineforge::Bar),
              "pf_bar_t / pineforge::Bar alignment mismatch");
static_assert(offsetof(pf_bar_t, open) == offsetof(pineforge::Bar, open),
              "pf_bar_t::open offset mismatch");
static_assert(offsetof(pf_bar_t, high) == offsetof(pineforge::Bar, high),
              "pf_bar_t::high offset mismatch");
static_assert(offsetof(pf_bar_t, low) == offsetof(pineforge::Bar, low),
              "pf_bar_t::low offset mismatch");
static_assert(offsetof(pf_bar_t, close) == offsetof(pineforge::Bar, close),
              "pf_bar_t::close offset mismatch");
static_assert(offsetof(pf_bar_t, volume) == offsetof(pineforge::Bar, volume),
              "pf_bar_t::volume offset mismatch");
static_assert(offsetof(pf_bar_t, timestamp) == offsetof(pineforge::Bar, timestamp),
              "pf_bar_t::timestamp offset mismatch");
static_assert(sizeof(double) == 8 && sizeof(std::int64_t) == 8,
              "pf_bar_t is five 8-byte doubles then an 8-byte timestamp");
static_assert(sizeof(pf_bar_t) == 48, "pf_bar_t size moved");
static_assert(offsetof(pf_bar_t, open) == 0 && offsetof(pf_bar_t, high) == 8
                  && offsetof(pf_bar_t, low) == 16 && offsetof(pf_bar_t, close) == 24
                  && offsetof(pf_bar_t, volume) == 32 && offsetof(pf_bar_t, timestamp) == 40,
              "pf_bar_t field offsets moved");

/* The frozen 64-bit layout of the two descriptors. The executed C, C++ and
 * ctypes layout assertions are separate (tests/test_selected_window_plan_c.cpp). */
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
static_assert(sizeof(pf_selected_plan_request_v1) == 80,
              "pf_selected_plan_request_v1 size moved");
static_assert(offsetof(pf_selected_plan_request_v1, input_tf) == 40,
              "pf_selected_plan_request_v1::input_tf offset moved");
static_assert(sizeof(pf_selected_plan_result_v1) == 248,
              "pf_selected_plan_result_v1 size moved");
static_assert(offsetof(pf_selected_plan_result_v1, present_mask) == 200,
              "pf_selected_plan_result_v1::present_mask offset moved");
static_assert(offsetof(pf_selected_plan_result_v1, option) == 216,
              "pf_selected_plan_result_v1::option offset moved");
#endif
static_assert(sizeof(pf_selected_plan_result_v1::option) == 32,
              "pf_selected_plan_result_v1::option must be 32 bytes");

namespace {

constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kOptionCapacity = sizeof(pf_selected_plan_result_v1::option);

/* ── Descriptors ────────────────────────────────────────────────── */

/* Only reads the caller's output header; nothing is written before the result
 * is whole. */
bool output_header_valid(const pf_selected_plan_result_v1* out) noexcept {
    return out != nullptr
        && out->struct_size == sizeof(pf_selected_plan_result_v1)
        && out->version == kVersion;
}

bool request_valid(const pf_selected_plan_request_v1* request) noexcept {
    return request != nullptr
        && request->struct_size == sizeof(pf_selected_plan_request_v1)
        && request->version == kVersion
        && request->feed_tolerant <= 1u
        && request->input_tf != nullptr
        && request->script_tf != nullptr
        && request->chart_timezone != nullptr
        && request->engine_timezone != nullptr
        && request->session != nullptr;
}

/* The count must be representable as a size_t (always true on LP64, where the
 * round trip below is the identity). */
bool row_count_of(std::uint64_t count, std::size_t& rows) noexcept {
    rows = static_cast<std::size_t>(count);
    return static_cast<std::uint64_t>(rows) == count;
}

/* String conversion: each NUL-terminated string is copied into the C++ request.
 * An allocation failure propagates as std::bad_alloc. */
pineforge::SelectedPlanRequest planner_request_of(const pf_selected_plan_request_v1& request) {
    pineforge::SelectedPlanRequest planner{};
    planner.start_ms = request.start_ms;
    planner.end_ms = request.end_ms;
    planner.fed_start_ms = request.fed_start_ms;
    planner.preroll_bars = request.preroll_bars;
    planner.input_tf = request.input_tf;
    planner.script_tf = request.script_tf;
    planner.chart_timezone = request.chart_timezone;
    planner.engine_timezone = request.engine_timezone;
    planner.session = request.session;
    planner.feed_tolerant = request.feed_tolerant != 0u;
    return planner;
}

/* ── Result ─────────────────────────────────────────────────────── */

/* The C++ enumerators mapped explicitly to the frozen words (0 Ok, 1
 * RequestInvalid, 2 CalendarUnsupported, 3 BoundaryUnaligned, 4
 * FeedRangeInvalid, 5 RowsUnordered, 6 InternalError). The word is not inferred
 * from the enumerator's value or width, and a new enumerator is a compile error
 * here. A value outside the enumeration is a bridge failure. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
#endif
bool status_word_of(pineforge::SelectedPlanStatus status, std::int32_t& word) noexcept {
    switch (status) {
    case pineforge::SelectedPlanStatus::Ok:
        word = 0;
        return true;
    case pineforge::SelectedPlanStatus::RequestInvalid:
        word = 1;
        return true;
    case pineforge::SelectedPlanStatus::CalendarUnsupported:
        word = 2;
        return true;
    case pineforge::SelectedPlanStatus::BoundaryUnaligned:
        word = 3;
        return true;
    case pineforge::SelectedPlanStatus::FeedRangeInvalid:
        word = 4;
        return true;
    case pineforge::SelectedPlanStatus::RowsUnordered:
        word = 5;
        return true;
    case pineforge::SelectedPlanStatus::InternalError:
        word = 6;
        return true;
    }
    return false;
}
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* The option is ASCII, NUL-terminated and zero-padded in 32 bytes. One that is
 * longer than 31 characters, or holds a NUL or a non-ASCII byte, is a bridge
 * failure and is never truncated into another error. */
bool copy_option(const std::string& option, char (&target)[kOptionCapacity]) noexcept {
    if (option.size() >= kOptionCapacity) return false;
    for (const char ch : option) {
        const unsigned char byte = static_cast<unsigned char>(ch);
        if (byte == 0u || byte >= 0x80u) return false;
    }
    std::memset(target, 0, kOptionCapacity);
    std::memcpy(target, option.data(), option.size());
    return true;
}

/* An optional timestamp: absent leaves the zeroed slot and its mask bit clear. */
void put_timestamp(const std::optional<std::int64_t>& source, unsigned bit, std::int64_t& slot,
                    std::uint32_t& mask) noexcept {
    if (!source.has_value()) return;
    slot = *source;
    mask |= std::uint32_t{1} << bit;
}

/* Maps a plan into the zeroed local result. Returns false, leaving the caller to
 * report a bridge failure, when the plan breaks a bridge invariant. */
bool fill_result(const pineforge::SelectedPrimaryPlan& plan,
                 pf_selected_plan_result_v1& local) noexcept {
    std::memset(&local, 0, sizeof local);
    std::int32_t status = 0;
    if (!status_word_of(plan.status, status)) return false;
    if (plan.bound < -1 || plan.bound > 2) return false;
    if (!copy_option(plan.option, local.option)) return false;

    local.struct_size = static_cast<std::uint32_t>(sizeof local);
    local.version = kVersion;
    local.status = status;
    local.bound = static_cast<std::int32_t>(plan.bound);
    local.value_ms = plan.value_ms;
    local.previous_boundary_ms = plan.previous_boundary_ms;
    local.next_boundary_ms = plan.next_boundary_ms;
    local.supplied_input_bars = plan.supplied_input_bars;
    local.supplied_script_bars = plan.supplied_script_bars;
    local.available_script_bars = plan.available_script_bars;
    local.used_script_bars = plan.used_script_bars;
    local.trimmed_script_bars = plan.trimmed_script_bars;
    local.trim_index = plan.trim_index;
    local.fed_input_bars = plan.fed_input_bars;
    local.fed_script_bars = plan.fed_script_bars;
    local.preroll_input_bars = plan.preroll_input_bars;
    local.window_input_bars = plan.window_input_bars;
    local.window_script_bars = plan.window_script_bars;
    local.trim_start_ms = plan.trim_start_ms;

    /* Bits 0..7 are the eight optional timestamps in the result's field order. */
    std::uint32_t mask = 0;
    put_timestamp(plan.preroll_first_bar_ms, 0, local.preroll_first_bar_ms, mask);
    put_timestamp(plan.preroll_last_bar_ms, 1, local.preroll_last_bar_ms, mask);
    put_timestamp(plan.supplied_first_data_ms, 2, local.supplied_first_data_ms, mask);
    put_timestamp(plan.supplied_last_data_ms, 3, local.supplied_last_data_ms, mask);
    put_timestamp(plan.fed_first_data_ms, 4, local.fed_first_data_ms, mask);
    put_timestamp(plan.fed_last_data_ms, 5, local.fed_last_data_ms, mask);
    put_timestamp(plan.window_first_data_ms, 6, local.window_first_data_ms, mask);
    put_timestamp(plan.window_last_data_ms, 7, local.window_last_data_ms, mask);
    local.present_mask = mask;

    local.shortfall = plan.shortfall ? 1u : 0u;
    local.complete_pending_preroll_at_horizon =
        plan.complete_pending_preroll_at_horizon ? 1u : 0u;
    local.reserved = 0u;
    return true;
}

}  // namespace

/* ── Exports ────────────────────────────────────────────────────── */

PF_API uint32_t pf_selected_plan_version(void) { return kVersion; }

PF_API int pf_plan_selected_primary_v1(const pf_selected_plan_request_v1* request,
                                       const pf_bar_t* rows, uint64_t count,
                                       pf_selected_plan_result_v1* out) {
    /* Every refusal of the descriptors, and of the count, is -1 and happens
     * before anything could be written. */
    if (!output_header_valid(out) || !request_valid(request)) return -1;
    std::size_t row_count = 0;
    if (!row_count_of(count, row_count)) return -1;

    try {
        const pineforge::SelectedPlanRequest planner = planner_request_of(*request);
        /* The rows are never dereferenced here: a NULL pointer with a positive
         * count reaches the planner, which refuses it as RequestInvalid. */
        const auto* native = reinterpret_cast<const pineforge::Bar*>(rows);
        const pineforge::SelectedPrimaryPlan plan =
            pineforge::plan_selected_primary(native, row_count, planner);

        pf_selected_plan_result_v1 local;
        if (!fill_result(plan, local)) return -3;
        *out = local;  /* the one write: a whole result or nothing */
        return 0;
    } catch (const std::bad_alloc&) {
        return -2;
    } catch (...) {
        return -3;
    }
}
