// Compare retained entry IDs against an in-place vector of real Trade rows.
#if !defined(__linux__)
#error "Entry ID cost comparisons require the Linux verification host"
#endif

#include <pineforge/detail/scratch_file.hpp>
#include <pineforge/run_failure.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include "../src/retired_trade_history_read.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace pineforge::detail {
struct TradeLedgerStorageProbe {
    static void retire(TradeLedger& ledger) { ledger.set_history_retirement(true); }
};
} // namespace pineforge::detail

namespace {
using namespace pineforge;
using Size = std::size_t;
// The actual row includes all four strings and every numeric/flag member.
using Row = Trade;
using Clock = std::chrono::steady_clock;
using Duration = std::chrono::duration<double>;

#if defined(PINEFORGE_ENTRY_ID_COST_SANITIZED) || defined(__SANITIZE_ADDRESS__) \
    || defined(__SANITIZE_THREAD__) || defined(__SANITIZE_UNDEFINED__)
constexpr bool kSanitized = true;
#elif defined(__has_feature)
constexpr bool kSanitized = __has_feature(address_sanitizer)
    || __has_feature(thread_sanitizer) || __has_feature(undefined_behavior_sanitizer);
#else
constexpr bool kSanitized = false;
#endif

Size checks = 0, failures = 0;
bool check(bool ok, const char* expression, int line) {
    ++checks;
    if (!ok) {
        ++failures;
        std::fprintf(stderr, "FAIL entry ID comparison line=%d: %s\n", line, expression);
    }
    return ok;
}
#define CHECK(expression) check(bool(expression), #expression, __LINE__)

struct Reader final : source::PineStrategyHost {
    void on_source_bar(const Bar&) override {}
    TradeLedger& ledger() { return trades_; }
    bool equals(int index, std::string_view rhs) const {
        return closed_trade_entry_id_equals(index, rhs);
    }
    using source::PineStrategyHost::closed_trade_exit_id_equals;
    using source::PineStrategyHost::closed_trade_entry_comment_equals;
    using source::PineStrategyHost::closed_trade_exit_comment_equals;
};

void compare(Reader& host, Size index, const std::string& id,
             const std::string& rhs, const char* context) {
    // Both operands belong to the input oracle; no ledger getter supplies it.
    const bool expected = id == rhs;
    const std::string_view view(rhs.data(), rhs.size());
    const bool direct = host.ledger().entry_id_equals(index, view);
    const bool wrapped = host.equals(static_cast<int>(index), view);
    if (!CHECK(direct == expected && wrapped == expected)) {
        std::fprintf(stderr,
            "context=%s index=%zu id_bytes=%zu rhs_bytes=%zu direct=%d wrapper=%d expected=%d\n",
            context, index, id.size(), rhs.size(), direct, wrapped, expected);
    }
}

void variants(Reader& host, Size index, const std::string& id, const char* context) {
    compare(host, index, id, id, context);
    compare(host, index, id, "", context);
    compare(host, index, id, id + "!", context); // RHS longer than the input.
    if (!id.empty()) {
        compare(host, index, id, id.substr(0, id.size() - 1), context);
        auto changed = id;
        changed.back() ^= 1;
        compare(host, index, id, changed, context); // Equal lengths, different tail.
    }
    // First seven bytes equal, then differences at word and chunk boundaries.
    for (const Size offset : {Size{7}, Size{8}, Size{16},
                              detail::ScratchFile::kCacheBytes}) {
        if (offset < id.size()) {
            auto changed = id;
            changed[offset] ^= 1;
            compare(host, index, id, changed, context);
        }
    }
}

template<class Action> void refuses(Action action, bool retired = false) {
    bool caught = false;
    try { action(); }
    catch (const std::logic_error& error) {
        caught = true;
        CHECK(dynamic_cast<const RunFailureInfo*>(&error) != nullptr);
        const auto failure = classify_run_failure(error);
        CHECK(failure.code == RunFailureCode::engine_invariant);
        if (retired) {
            CHECK(dynamic_cast<const detail::RetiredTradeHistoryRead*>(&error) != nullptr);
            CHECK(!failure.args || *failure.args == "{}");
        }
    }
    CHECK(caught);
}

void invalid_wrapper(Reader& host, int index) {
    const std::string empty;
    for (const auto& rhs : {std::string{}, std::string{"Other"}, std::string{"\0", 1}}) {
        CHECK(host.equals(index, std::string_view(rhs.data(), rhs.size())) == (empty == rhs));
    }
}

void equivalence_rows() {
    Reader host;
    auto& ledger = host.ledger();
    ledger.set_scratch_directory(".");
    const Size packed = TradeLedger::kCacheStringBytes;
    const Size chunk = detail::ScratchFile::kCacheBytes;
    std::vector<std::string> oracle{
        "Long", "Short", "Other", std::string("Lo\0ng", 5), "",
        "abcdefg", "abcdefgh", "abcdefghi", "abcdefghijklmnop", "abcdefghijklmnopq",
        std::string(packed, 'p'), std::string(packed + 1, 'q'),
        std::string(2 * chunk + 1, 'r')};
    // Three chunks, with NULs straddling a chunk boundary, exercise byte equality.
    oracle.back()[chunk - 1] = '\0';
    oracle.back()[chunk] = '\0';
    for (const auto& id : oracle) {
        Row row{};
        row.entry_id = id;
        row.entry_comment = "entry-note";
        row.exit_comment = "exit-note";
        row.exit_id = "exit-tag";
        ledger.push_back(row);
    }
    // The second round repeats already-read keys, including the packed cases.
    for (int round = 0; round < 2; ++round) {
        for (Size i = 0; i < oracle.size(); ++i)
            variants(host, i, oracle[i], round == 0 ? "initial" : "warm");
        compare(host, 3, oracle[3], "Lo", "embedded NUL versus prefix");
        compare(host, 3, oracle[3], std::string("Lo\0nx", 5), "embedded NUL versus tail");
    }

    const auto old_id = oracle.front();
    // Same-length replacement first: a length-only cache tag cannot catch this.
    for (const auto& id : {std::string{"Lark"}, std::string(packed + 1, 's'),
                           std::string{"Replacement"}}) {
        Row replacement{};
        replacement.entry_id = id;
        ledger.replace(0, replacement);
        oracle[0] = id;
        variants(host, 0, oracle[0], "replace");
        compare(host, 0, oracle[0], old_id, "replace invalidates old bytes");
    }

    const Size removed = oracle.size() - 1;
    const auto removed_id = oracle.back();
    ledger.resize(removed);
    oracle.pop_back();
    refuses([&] { (void)ledger.entry_id_equals(removed, removed_id); });
    refuses([&] { (void)ledger.entry_id_equals(removed, ""); });
    invalid_wrapper(host, static_cast<int>(removed));
    Row reappended{};
    reappended.entry_id = "ReusedKey";
    ledger.push_back(reappended);
    oracle.push_back(reappended.entry_id);
    variants(host, removed, oracle.back(), "rewind and reuse");
    compare(host, removed, oracle.back(), removed_id, "rewind invalidates old bytes");
    invalid_wrapper(host, -1);
    invalid_wrapper(host, static_cast<int>(oracle.size()));
    invalid_wrapper(host, std::numeric_limits<int>::max());

    Reader retiring;
    auto& retired = retiring.ledger();
    retired.set_scratch_directory(".");
    detail::TradeLedgerStorageProbe::retire(retired);
    const std::array<std::string, 3> retired_ids{"", "RetiredKey", "LiveKey"};
    for (const auto& id : retired_ids) {
        Row row{};
        row.entry_id = id;
        retired.push_back(row);
    }
    for (Size i = 0; i < retired_ids.size(); ++i)
        variants(retiring, i, retired_ids[i], "before retirement");
    // Retire both the cached empty ID and the cached nonempty ID; keep one live.
    retired.seal(retired_ids.size() - 1);
    CHECK(retired.base() == retired_ids.size() - 1);
    for (Size i = 0; i < retired_ids.size() - 1; ++i) {
        for (const auto& rhs : {retired_ids[i], std::string{}, std::string{"Mismatch"}}) {
            refuses([&] { (void)retired.entry_id_equals(i, rhs); }, true);
            refuses([&] { (void)retiring.equals(static_cast<int>(i), rhs); }, true);
        }
    }
    variants(retiring, retired_ids.size() - 1, retired_ids.back(), "after retirement");
}

struct TextMethod {
    const char* name;
    std::string Row::*field;
    bool (TradeLedger::*matches)(int, std::string_view) const;
    bool (TradeLedger::*equals)(Size, std::string_view) const;
    bool (Reader::*wrapped)(int, std::string_view) const;
};
const std::array<TextMethod, 4> text_methods{{
    {"entry_id", &Row::entry_id, &TradeLedger::entry_id_matches,
     &TradeLedger::entry_id_equals, &Reader::equals},
    {"exit_id", &Row::exit_id, &TradeLedger::exit_id_matches,
     &TradeLedger::exit_id_equals, &Reader::closed_trade_exit_id_equals},
    {"entry_comment", &Row::entry_comment, &TradeLedger::entry_comment_matches,
     &TradeLedger::entry_comment_equals, &Reader::closed_trade_entry_comment_equals},
    {"exit_comment", &Row::exit_comment, &TradeLedger::exit_comment_matches,
     &TradeLedger::exit_comment_equals, &Reader::closed_trade_exit_comment_equals}
}};

void matching_variants(Reader& host, int index, const Row& oracle, const char* context) {
    for (const auto& method : text_methods) {
        const auto& text = oracle.*method.field;
        std::vector<std::string> operands{text, "", text + "!"};
        if (!text.empty()) {
            operands.push_back(text.substr(0, text.size() - 1));
            auto changed = text;
            changed.back() ^= 1;
            operands.push_back(changed);
        }
        for (const Size offset : {Size{0}, Size{3}, Size{7}, Size{8}, Size{16}}) {
            if (offset < text.size()) {
                auto changed = text;
                changed[offset] ^= 1;
                operands.push_back(changed);
            }
        }
        for (const auto& operand : operands) {
            const std::string_view rhs(operand.data(), operand.size());
            const bool expected = std::string_view(text.data(), text.size()) == rhs;
            // The new entry point gets the first read, including a cache miss.
            const bool matched = (host.ledger().*method.matches)(index, rhs);
            const bool direct = (host.ledger().*method.equals)(static_cast<Size>(index), rhs);
            const bool wrapped = (host.*method.wrapped)(index, rhs);
            if (!CHECK(matched == expected && direct == expected && wrapped == expected)) {
                std::fprintf(stderr,
                    "context=%s field=%s index=%d text_bytes=%zu rhs_bytes=%zu "
                    "matched=%d direct=%d wrapper=%d expected=%d\n",
                    context, method.name, index, text.size(), operand.size(),
                    matched, direct, wrapped, expected);
            }
        }
        CHECK((host.ledger().*method.matches)(index, std::string_view{}) == text.empty());
        CHECK((host.ledger().*method.equals)(static_cast<Size>(index), std::string_view{})
              == text.empty());
        CHECK((host.*method.wrapped)(index, std::string_view{}) == text.empty());
    }
}

void invalid_text_matches(Reader& host, int index) {
    const std::string empty;
    for (const auto& method : text_methods) {
        for (const auto& operand : {std::string{}, std::string{"Other"}, std::string{"\0", 1}}) {
            const std::string_view rhs(operand.data(), operand.size());
            const bool expected = std::string_view(empty.data(), empty.size()) == rhs;
            CHECK((host.ledger().*method.matches)(index, rhs) == expected);
            CHECK((host.*method.wrapped)(index, rhs) == expected);
        }
        CHECK((host.ledger().*method.matches)(index, std::string_view{}));
        CHECK((host.*method.wrapped)(index, std::string_view{}));
    }
}

void matching_rows() {
    Reader host;
    auto& ledger = host.ledger();
    ledger.set_scratch_directory(".");
    const Size chunk = detail::ScratchFile::kCacheBytes;
    std::vector<std::string> texts{
        "", "a", "ab", "abc", "abcd", "abcde", "abcdef", "abcdefg", "abcdefgh",
        "abcdefghi", std::string("ab\0defg", 7), std::string("ab\0defgh", 8),
        std::string("ab\0defghi", 9), std::string(TradeLedger::kCacheStringBytes + 1, 'p'),
        std::string(2 * chunk + 1, 'q')};
    texts.back()[chunk - 1] = '\0';
    texts.back()[chunk] = '\0';
    const Size packed_texts = texts.size() - 2;
    std::vector<Row> oracle(texts.size());
    for (Size i = 0; i < oracle.size(); ++i) {
        // Distinct field inputs catch both wrong members and packed offsets.
        for (Size field = 0; field < text_methods.size(); ++field)
            oracle[i].*text_methods[field].field =
                texts[i < packed_texts ? (i + field) % packed_texts : i];
        ledger.push_back(oracle[i]);
    }
    for (int round = 0; round < 2; ++round)
        for (Size i = 0; i < oracle.size(); ++i)
            matching_variants(host, static_cast<int>(i), oracle[i],
                              round == 0 ? "initial text matches" : "warm text matches");
    invalid_text_matches(host, -1);
    invalid_text_matches(host, static_cast<int>(oracle.size()));
    invalid_text_matches(host, static_cast<int>(oracle.size() + 1));
    invalid_text_matches(host, std::numeric_limits<int>::max());

    // Same-length replacements must invalidate cached bytes for every member.
    Row replacement = oracle[7];
    for (const auto& method : text_methods)
        (replacement.*method.field).back() ^= 1;
    ledger.replace(7, replacement);
    matching_variants(host, 7, replacement, "replace text matches");
    const Size removed = oracle.size() - 1;
    ledger.resize(removed);
    invalid_text_matches(host, static_cast<int>(removed));
    ledger.push_back(replacement);
    matching_variants(host, static_cast<int>(removed), replacement, "reused text key");

    Reader retiring;
    auto& retired = retiring.ledger();
    retired.set_scratch_directory(".");
    detail::TradeLedgerStorageProbe::retire(retired);
    for (const auto& row : {oracle[0], oracle[7], oracle[8]}) retired.push_back(row);
    matching_variants(retiring, 0, oracle[0], "before retiring empty text");
    matching_variants(retiring, 1, oracle[7], "before retiring text");
    matching_variants(retiring, 2, oracle[8], "before retiring live text");
    retired.seal(2);
    CHECK(retired.base() == 2);
    for (Size i = 0; i < 2; ++i) {
        const Row& row = i == 0 ? oracle[0] : oracle[7];
        for (const auto& method : text_methods) {
            for (const auto& rhs : {row.*method.field, std::string{}, std::string{"Mismatch"}}) {
                refuses([&] { (void)(retired.*method.equals)(i, rhs); }, true);
                refuses([&] { (void)(retired.*method.matches)(static_cast<int>(i), rhs); }, true);
                refuses([&] { (void)(retiring.*method.wrapped)(static_cast<int>(i), rhs); }, true);
            }
        }
    }
    matching_variants(retiring, 2, oracle[8], "retained live text");
    invalid_text_matches(retiring, -1);
    invalid_text_matches(retiring, 3);
    invalid_text_matches(retiring, 4);
}

// Half the default cache gives the requested 32,768 distinct warm row keys.
constexpr Size kRows = 32768;
static_assert(kRows % 2 == 0 && kRows < TradeLedger::kDefaultCacheRows);
// Seven opportunities for an undisturbed sequence per loop; take their minimum.
constexpr Size kSamples = 7;
// This is an integration floor for timer/noise rejection, never a cost budget.
constexpr auto kSequenceFloor = std::chrono::milliseconds{50};
volatile std::uint64_t hit_sink = 0;

struct Sample {
    Duration elapsed{};
    std::uint64_t hits = 0;
};

template<class Loop> Sample sequence(const void* storage, Size passes, Loop loop) {
    std::uint64_t hits = 0;
    const auto begin = Clock::now();
    for (Size pass = 0; pass < passes; ++pass) {
        // Escape the storage and clobber memory once per full traversal so the
        // compiler cannot reuse/fold a vector pass. Both loops pay this barrier.
        asm volatile("" : : "g"(storage) : "memory");
        hits += loop();
    }
    hit_sink = hits;
    const auto end = Clock::now();
    return {end - begin, hits};
}

void print_samples(const std::array<Sample, kSamples>& ledger,
                   const std::array<Sample, kSamples>& baseline) {
    for (Size i = 0; i < kSamples; ++i) {
        std::fprintf(stderr,
            "sample=%zu ledger_ms=%.6f baseline_ms=%.6f ledger_hits=%llu baseline_hits=%llu\n",
            i, std::chrono::duration<double, std::milli>(ledger[i].elapsed).count(),
            std::chrono::duration<double, std::milli>(baseline[i].elapsed).count(),
            static_cast<unsigned long long>(ledger[i].hits),
            static_cast<unsigned long long>(baseline[i].hits));
    }
}

void wrapper_cost_row(Size population, const char* label) {
    Reader host;
    auto& ledger = host.ledger();
    ledger.set_scratch_directory(".");
    std::uint64_t expected_per_pass = 0;
    for (Size i = 0; i < population; ++i) {
        Row row{};
        row.entry_id = i % 2 == 0 ? "Long" : "Short";
        ledger.push_back(row);
        expected_per_pass += row.entry_id == "Long";
    }
    const auto loop = [&] {
        std::uint64_t hits = 0;
        for (Size i = 0; i < population; ++i)
            hits += host.equals(static_cast<int>(i), "Long");
        return hits;
    };
    hit_sink = loop(); // Populate every final key before any timer runs.
    // Each pass adds at most population hits; also respect the loop index width.
    const auto max_passes = std::min<std::uint64_t>(
        std::numeric_limits<Size>::max(), std::numeric_limits<std::uint64_t>::max() / population);
    Size passes = 1;
    for (;;) {
        const auto sample = sequence(&host, passes, loop);
        if (sample.elapsed >= kSequenceFloor) break;
        if (passes > max_passes / 2) {
            std::printf("%s calibration exceeds accumulator capacity\n", label);
            return;
        }
        passes *= 2;
    }
    for (;;) {
        Sample best{Duration::max(), 0};
        for (Size i = 0; i < kSamples; ++i) {
            const auto sample = sequence(&host, passes, loop);
            if (sample.elapsed < best.elapsed) best = sample;
        }
        if (best.elapsed < kSequenceFloor) {
            if (passes > max_passes / 2) {
                std::printf("%s calibration exceeds accumulator capacity\n", label);
                return;
            }
            passes *= 2;
            continue;
        }
        const double calls = static_cast<double>(population) * static_cast<double>(passes);
        std::printf("%s T=%zu P=%zu K=%zu ns_per_call=%.6f hits=%llu expected_hits=%llu\n",
            label, population, passes, kSamples, best.elapsed.count() * 1e9 / calls,
            static_cast<unsigned long long>(best.hits),
            static_cast<unsigned long long>(expected_per_pass * passes));
        return;
    }
}

void wrapper_cost_rows() {
    if constexpr (kSanitized) {
        std::puts("SKIP wrapper cost diagnostics: sanitizer instrumentation");
        return;
    }
    wrapper_cost_row(kRows, "diag-wrapper-ordered");
    wrapper_cost_row(1024, "diag-wrapper-small");
}

void cost_row() {
    if constexpr (kSanitized) {
        std::puts("SKIP entry ID cost ratio: sanitizer instrumentation; equivalence rows executed");
        return;
    }
    TradeLedger ledger;
    ledger.set_scratch_directory(".");
    std::vector<Row> rows(kRows);
    std::uint64_t expected_per_pass = 0;
    for (Size i = 0; i < kRows; ++i) {
        auto& row = rows[i];
        row.entry_id = i % 2 == 0 ? "Long" : "Short";
        row.is_long = i % 2 == 0;
        row.entry_bar_index = static_cast<int>(i);
        row.exit_bar_index = static_cast<int>(i + 1);
        const Size text_bytes = row.entry_id.size() + row.entry_comment.size()
            + row.exit_comment.size() + row.exit_id.size();
        if (!CHECK(text_bytes <= TradeLedger::kCacheStringBytes)) return;
        ledger.push_back(row);
        expected_per_pass += row.entry_id == "Long";
    }
    if (!CHECK(ledger.size() == kRows && ledger.cache_rows() > kRows)) return;
    // Appending can grow/invalidate the cache. Populate every final key before
    // any timer runs, independently checking every result against vector input.
    for (Size i = 0; i < kRows; ++i) {
        if (!CHECK(ledger.entry_id_equals(i, "Long") == (rows[i].entry_id == "Long"))) return;
    }
    if (!CHECK(expected_per_pass == kRows / 2)) return;
    const auto ledger_loop = [&] {
        std::uint64_t hits = 0;
        for (Size i = 0; i < kRows; ++i) hits += ledger.entry_id_equals(i, "Long");
        return hits;
    };
    const auto baseline_loop = [&] {
        std::uint64_t hits = 0;
        for (Size i = 0; i < kRows; ++i) hits += rows[i].entry_id == "Long";
        return hits;
    };
    // Bound P by the accumulator and index widths, rather than a timing guess.
    const auto max_passes = std::min<std::uint64_t>(
        std::numeric_limits<Size>::max(), std::numeric_limits<std::uint64_t>::max() / kRows);
    Size passes = 1;
    for (;;) {
        const auto a = sequence(&ledger, passes, ledger_loop);
        const auto b = sequence(rows.data(), passes, baseline_loop);
        if (!CHECK(a.hits == expected_per_pass * passes
                   && b.hits == expected_per_pass * passes)) return;
        if (std::min(a.elapsed, b.elapsed) >= kSequenceFloor) break;
        if (!CHECK(passes <= max_passes / 2)) return;
        passes *= 2;
    }

    for (;;) {
        std::array<Sample, kSamples> ledger_samples{}, baseline_samples{};
        for (Size i = 0; i < kSamples; ++i) {
            // Alternate order to avoid always favoring one side of a pair.
            if (i % 2 == 0) {
                ledger_samples[i] = sequence(&ledger, passes, ledger_loop);
                baseline_samples[i] = sequence(rows.data(), passes, baseline_loop);
            } else {
                baseline_samples[i] = sequence(rows.data(), passes, baseline_loop);
                ledger_samples[i] = sequence(&ledger, passes, ledger_loop);
            }
        }
        Duration ledger_min = Duration::max(), baseline_min = Duration::max();
        for (Size i = 0; i < kSamples; ++i) {
            if (!CHECK(ledger_samples[i].hits == expected_per_pass * passes
                       && baseline_samples[i].hits == expected_per_pass * passes)) {
                print_samples(ledger_samples, baseline_samples);
                return;
            }
            ledger_min = std::min(ledger_min, ledger_samples[i].elapsed);
            baseline_min = std::min(baseline_min, baseline_samples[i].elapsed);
        }
        // A faster sampled sequence than calibration requires a larger shared P.
        // Discard the whole batch; every retained sample must meet the floor.
        if (std::min(ledger_min, baseline_min) < kSequenceFloor) {
            if (!CHECK(passes <= max_passes / 2)) return;
            passes *= 2;
            continue;
        }
        const double ratio = ledger_min.count() / baseline_min.count();
        // Diagnostic: warm instruction cost does not capture cache-miss prefetch gains.
        // Acceptance uses the measurement-box timing row; equivalence stays asserted.
        std::printf("entry ID cost T=%zu P=%zu K=%zu row_bytes=%zu "
                    "ledger_min_ms=%.6f baseline_min_ms=%.6f ratio=%.6f\n",
            kRows, passes, kSamples, sizeof(Row),
            std::chrono::duration<double, std::milli>(ledger_min).count(),
            std::chrono::duration<double, std::milli>(baseline_min).count(),
            ratio);
        print_samples(ledger_samples, baseline_samples);
        return;
    }
}
} // namespace

int main() {
    try {
        equivalence_rows();
        matching_rows();
        std::printf("entry ID equivalence checks=%zu failures=%zu\n", checks, failures);
        if (failures == 0) wrapper_cost_rows();
        if (failures == 0) cost_row();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL entry ID comparison exception: %s\n", error.what());
        return EXIT_FAILURE;
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
