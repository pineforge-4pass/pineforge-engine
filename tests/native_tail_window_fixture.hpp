#pragma once

#include <pineforge/native_order.hpp>
#include <pineforge/detail/read_through_cache.hpp>
#if defined(PINEFORGE_TAIL_CACHE_ACCEPTANCE)
#include "cache_sweep_fixture.hpp"
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace native_tail_window_fixture {
namespace no = pineforge::native_order;
namespace ex = pineforge::execution;

inline void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct Driver {
    no::RunIdentity run{"tail-window-sweep", 1};
    no::WorkingRequestCore core;
    std::uint64_t incarnation = 1;
    std::uint64_t ordinal = 1;

    explicit Driver(no::TailWindowConfig windows) : core(run, windows) {}

    no::RequestHandle submit(no::Request request) {
        auto prepared = core.prepare_submit(request, {}, incarnation, ordinal);
        const auto prediction = prepared.predicted();
        require(prediction.handle.has_value(), "source command accepts its requested identity");
        auto installed = core.install_submit(std::move(prepared));
        require(std::holds_alternative<no::CommandInstalled<no::SubmitResult>>(installed),
                "source submit installs");
        ordinal += std::get<no::CommandInstalled<no::SubmitResult>>(installed).events.count;
        incarnation = prediction.handle->incarnation + 1;
        return *prediction.handle;
    }

    no::EvaluationContext evaluate(const no::RequestHandle& handle) {
        no::EvaluationContext context;
        context.cursor.point.ordinal = ordinal;
        context.cursor.point.effective_time_ms = 1000;
        context.driver_class = no::DriverEligibilityClass::ObservedPrint;
        context.existing_matching_bit = true;
        no::TargetObservation flat;
        flat.current_position = no::PositionFlat{};
        auto prepared = core.prepare_evaluation(handle, context, flat, ordinal);
        if (auto* mutation = std::get_if<no::PreparedMutation>(&prepared))
            install(std::move(*mutation));
        else require(std::holds_alternative<no::NoChange>(prepared), "evaluation is valid");
        return context;
    }

    void install(no::PreparedMutation mutation) {
        auto installed = core.install_mutation(std::move(mutation));
        require(std::holds_alternative<no::Installed>(installed), "mutation installs");
        ordinal += std::get<no::Installed>(installed).events.count;
    }

    no::EventId fill(const no::RequestHandle& handle, double units) {
        const auto context = evaluate(handle);
        no::ExecutionProposal proposal;
        proposal.cursor = context.cursor;
        proposal.raw_price = proposal.resolved_price = 100.0;
        proposal.physical_action = no::Transact{units};
        proposal.scope = ex::Book{};
        proposal.pre_fill = no::PositionFlat{};
        proposal.inspected_opened_units = units;
        proposal.inspected_current_ticket = 3.0;
        auto prepared = core.prepare_execution(handle, proposal, ordinal);
        require(std::holds_alternative<no::PreparedExecution>(prepared), "prepare source fill");
        no::CommittedExecutionFacts facts;
        facts.result.status = ex::Status::Applied;
        facts.result.opened_units = units;
        facts.result.current_ticket = 3.0;
        facts.result.first_trade_index = 7;
        facts.cycle_before = 1;
        facts.cycle_after = 2;
        facts.post_target.current_position = no::PositionNonflat{2, no::Side::Long};
        facts.committed_action = proposal.physical_action;
        auto installed = core.install_execution(
            std::get<no::PreparedExecution>(std::move(prepared)), facts);
        require(std::holds_alternative<no::Installed>(installed), "install source fill");
        ordinal += std::get<no::Installed>(installed).events.count;
        return {run, std::get<no::ExecutionAppliedEvent>(core.history().back()).ordinal};
    }
};

inline std::vector<std::byte> history(const no::WorkingRequestCore& core) {
    std::vector<std::byte> result;
    for (auto i = core.history_base(); i != core.history_end(); ++i) {
        const no::JournalRecordSource encoded(core.history_at(i));
        const auto offset = result.size();
        result.resize(offset + static_cast<std::size_t>(encoded.size()));
        require(encoded.read(0, {result.data() + offset,
                                 static_cast<std::size_t>(encoded.size())})
                    == pineforge::detail::ScratchStatus::ok,
                "encode every complete journal payload for byte comparison");
    }
    return result;
}

struct Output {
    std::vector<std::byte> events;
    std::vector<std::uint64_t> receipt_words;
    std::vector<std::uint64_t> adjustment_ids;
    std::vector<bool> membership;
    struct Cache {
        pineforge::detail::ReadThroughCacheStats stats;
        std::uint64_t inner_reads = 0, inner_bytes = 0;
        std::uint64_t cold_reads = 0, evicted_reread_reads = 0;
        std::size_t pending_bytes = 0;
        std::size_t control_bytes = 0;
    };
    std::array<Cache, 3> caches{};
};

inline Output run(no::TailWindowConfig windows) {
    Driver driver(windows);
    auto& core = driver.core;
    Output result;
    std::vector<no::EventId> resolutions;
    std::vector<std::vector<std::uint64_t>> reference_ids;
    // Thirty-three resolutions put 24 rows into backing at the default
    // 16-row half-flushed tail, exceeding its 16-line read cache as well.
    // Every resolution exceeds the semantic inline-id bound.
    constexpr auto cycles = std::size_t{33};
    constexpr auto links = no::WorkingRequestCore::kInlineAdjustmentIds + 1;
    for (std::size_t cycle = 0; cycle != cycles; ++cycle) {
        no::Request deferred{no::HostSized{no::HostSizedKind::Open, no::Side::Long}, "deferred", ""};
        deferred.group = no::Member{44, 2, no::GroupEffect::Reduce};
        const auto target = driver.submit(deferred);
        std::vector<std::uint64_t> expected;
        double total = 0.0;
        for (std::size_t i = 0; i != links; ++i) {
            const double units = i % 3 == 0 ? 0.1 : i % 3 == 1 ? 0.2 : 0.3;
            no::Request request{no::Transact{units}, "source", ""};
            request.group = no::Member{44, 1, no::GroupEffect::Reduce};
            const auto source = driver.submit(request);
            const auto cause = driver.fill(source, units);
            auto prepared = core.prepare_group_effect(cause, target, driver.ordinal);
            require(std::holds_alternative<no::PreparedMutation>(prepared), "prepare deferred receipt");
            driver.install(std::get<no::PreparedMutation>(std::move(prepared)));
            expected.push_back(core.last_ordinal());
            total += units;
        }
        const auto context = driver.evaluate(target);
        no::TermsResolvedInput terms;
        terms.price_kind = no::NativeCandidatePriceKind::PointPrice;
        terms.raw_price = terms.default_resolved_price = 100.0;
        terms.terms.resolved_price = 100.0;
        terms.terms.units = 1000000.0;
        terms.terms.shape = no::OpeningShape::Transact;
        const auto first = core.history_end();
        auto prepared = core.prepare_terms(target, context, terms, driver.ordinal);
        require(std::holds_alternative<no::PreparedMutation>(prepared), "prepare long resolution");
        driver.install(std::get<no::PreparedMutation>(std::move(prepared)));
        const auto& event = std::get<no::TermsResolvedEvent>(core.history_at(first));
        require(event.pending_total == total && event.effective_deduction == total
                    && event.prior_adjustment_ids.empty(),
                "long resolution keeps exact ordered arithmetic and paged ids");
        resolutions.push_back({driver.run, event.ordinal});
        reference_ids.push_back(std::move(expected));
    }
    const auto read_receipt = [&](std::size_t i) {
        const auto receipt = core.group_effect_receipt(i);
        require(receipt.cause.run == driver.run && receipt.recipient.run == driver.run,
                "receipt run identity survives readback");
        result.receipt_words.insert(result.receipt_words.end(),
            {receipt.cause.ordinal, receipt.recipient.incarnation,
             static_cast<std::uint64_t>(receipt.effect), receipt.outcome_ordinal});
    };
    const auto first_receipt = core.group_effect_receipt_base();
    const auto before_receipts = core.tail_window_stats().receipt_reads;
    read_receipt(first_receipt);
    read_receipt(first_receipt);
    for (auto i = first_receipt; i != core.group_effect_receipt_count(); ++i) read_receipt(i);
    result.caches[1].cold_reads = core.tail_window_stats().receipt_reads - before_receipts;
    const auto before_receipt_reread = core.tail_window_stats().receipt_reads;
    read_receipt(first_receipt);
    result.caches[1].evicted_reread_reads =
        core.tail_window_stats().receipt_reads - before_receipt_reread;
    require(core.tail_window_stats().receipt_reads > before_receipts,
            "retained old receipt read reaches its inner backing");
    const auto read_resolution = [&](std::size_t i) {
        std::vector<std::uint64_t> actual;
        require(core.visit_prior_adjustments(resolutions[i], [&](const no::EventId& id) {
                    require(id.run == driver.run, "adjustment id keeps its run");
                    actual.push_back(id.ordinal);
                    return true;
                }), "every resolution remains readable");
        require(actual == reference_ids[i], "complete adjustment list preserves command order");
        result.adjustment_ids.insert(result.adjustment_ids.end(), actual.begin(), actual.end());
    };
    const auto before_segments = core.tail_window_stats().chain_index_reads;
    read_resolution(0);
    read_resolution(0);
    for (std::size_t i = 0; i != resolutions.size(); ++i) read_resolution(i);
    result.caches[2].cold_reads = core.tail_window_stats().chain_index_reads - before_segments;
    const auto before_segment_reread = core.tail_window_stats().chain_index_reads;
    read_resolution(0);
    result.caches[2].evicted_reread_reads =
        core.tail_window_stats().chain_index_reads - before_segment_reread;
    require(core.tail_window_stats().chain_index_reads > before_segments,
            "old resolution lookup reaches the inner segment backing");

    no::Request moving{no::Transact{1.0}, "moving", ""};
    moving.trigger = no::Limit{90.0};
    const auto root = driver.submit(moving);
    auto tail = root;
    std::vector<no::RequestHandle> aliases{root};
    for (std::size_t i = 0; i != 129; ++i) {
        auto prepared = core.prepare_replace(tail, moving, {}, driver.incarnation, driver.ordinal);
        const auto prediction = prepared.predicted();
        require(prediction.successor.has_value(), "replacement predicts successor");
        auto installed = core.install_replace(std::move(prepared));
        require(std::holds_alternative<no::CommandInstalled<no::ReplaceResult>>(installed),
                "replacement installs");
        driver.ordinal += std::get<no::CommandInstalled<no::ReplaceResult>>(installed).events.count;
        tail = *prediction.successor;
        driver.incarnation = tail.incarnation + 1;
        aliases.push_back(tail);
    }
    const auto cohort = core.cohort_open();
    core.cohort_add(cohort, tail);
    const auto read_alias = [&](const no::RequestHandle& alias) {
        const bool member = core.cohort_contains(cohort, alias);
        require(member, "every successor alias resolves to the enrolled root");
        result.membership.push_back(member);
    };
    const auto before_roots = core.tail_window_stats().successor_root_reads;
    read_alias(aliases[1]);
    read_alias(aliases[1]);
    for (const auto& alias : aliases) read_alias(alias);
    result.caches[0].cold_reads = core.tail_window_stats().successor_root_reads - before_roots;
    const auto before_root_reread = core.tail_window_stats().successor_root_reads;
    read_alias(aliases[1]);
    result.caches[0].evicted_reread_reads =
        core.tail_window_stats().successor_root_reads - before_root_reread;
    require(core.tail_window_stats().successor_root_reads > before_roots,
            "evicted successor lookup reaches the inner root backing");
    const auto stats = core.tail_window_stats();
    result.caches[0].stats = core.tail_read_cache_stats(no::TailWindowKind::successor_roots);
    result.caches[1].stats = core.tail_read_cache_stats(no::TailWindowKind::receipts);
    result.caches[2].stats = core.tail_read_cache_stats(no::TailWindowKind::chain_segments);
    result.caches[0].inner_reads = stats.successor_root_reads;
    result.caches[0].inner_bytes = stats.successor_root_read_bytes;
    result.caches[0].pending_bytes = stats.successor_root_pending_bytes;
    result.caches[0].control_bytes = stats.successor_root_control_bytes;
    result.caches[1].inner_reads = stats.receipt_reads;
    result.caches[1].inner_bytes = stats.receipt_read_bytes;
    result.caches[1].pending_bytes = stats.receipt_pending_bytes;
    result.caches[1].control_bytes = stats.receipt_control_bytes;
    result.caches[2].inner_reads = stats.chain_index_reads;
    result.caches[2].inner_bytes = stats.chain_index_read_bytes;
    result.caches[2].pending_bytes = stats.chain_index_pending_bytes;
    result.caches[2].control_bytes = stats.chain_index_control_bytes;
    for (const auto& cache : result.caches)
        require(cache.stats.get_calls > 0 && cache.stats.misses > 0,
                "each committed native tail uses the shared cache, including bypass");
    std::printf("native_tail configured=%zu/%zu/%zu effective=%zu/%zu/%zu "
                "root_reads=%llu receipt_reads=%llu segment_reads=%llu id_reads=%llu\n",
                windows.successor_root_rows, windows.receipt_rows, windows.chain_segment_rows,
                stats.successor_root_capacity_rows, stats.receipt_capacity_rows,
                stats.chain_index_capacity_rows,
                static_cast<unsigned long long>(stats.successor_root_reads),
                static_cast<unsigned long long>(stats.receipt_reads),
                static_cast<unsigned long long>(stats.chain_index_reads),
                static_cast<unsigned long long>(stats.chain_id_reads));
    result.events = history(core);
    const auto count = core.group_effect_receipt_count();
    (void)core.retire_history(core.last_ordinal());
    require(core.group_effect_receipt_base() == count,
            "explicit history retirement advances the receipt lifetime");
    // Public receipt access asserts its range in Debug. Retired rows are
    // outside that contract; this sweep asks every retained row before retiring.
    no::WorkingRequestCore moved(std::move(core));
    require(moved.tail_window_stats().receipt_capacity_rows
                == (windows.receipt_rows > 1 ? windows.receipt_rows : 1),
            "moving a core preserves per-instance tail capacities");
    moved.reset({"tail-window-sweep", 2});
    const auto reset_windows = moved.tail_window_config();
    require(reset_windows.successor_root_rows == windows.successor_root_rows
                && reset_windows.receipt_rows == windows.receipt_rows
                && reset_windows.chain_segment_rows == windows.chain_segment_rows,
            "reset preserves construction policy for the next lazy stores");
    return result;
}

inline bool equal(const Output& a, const Output& b) {
    return a.events == b.events && a.receipt_words == b.receipt_words
        && a.adjustment_ids == b.adjustment_ids && a.membership == b.membership;
}

#if defined(PINEFORGE_TAIL_CACHE_ACCEPTANCE)
inline void acceptance() {
    static constexpr std::array<std::size_t, 3> widths{{16, 32, 24}};
    static constexpr std::array<std::size_t, 3> defaults{{64, 64, 16}};
    constexpr std::array<const char*, 3> names{{"successor_roots", "receipt_tail", "chain_segment"}};
    for (std::size_t kind = 0; kind != names.size(); ++kind) {
        cache_sweep::sweep(names[kind], 1, defaults[kind], defaults[kind],
            [kind](cache_sweep::Setting setting) {
                no::TailWindowConfig windows;
                if (!setting.defaults) {
                    if (kind == 0) windows.successor_root_rows = setting.units;
                    else if (kind == 1) windows.receipt_rows = setting.units;
                    else windows.chain_segment_rows = setting.units;
                }
                const auto output = run(windows);
                const auto& cache = output.caches[kind];
                cache_sweep::Result result;
                result.output.records.assign(reinterpret_cast<const char*>(output.events.data()),
                                             output.events.size());
                for (const auto word : output.receipt_words)
                    cache_sweep::scalar(result.output.records, word);
                for (const auto id : output.adjustment_ids)
                    cache_sweep::scalar(result.output.records, id);
                for (const bool member : output.membership)
                    cache_sweep::scalar(result.output.records, member);
                auto& s = result.cache;
                const auto width = widths[kind];
                const auto rows = setting.defaults ? defaults[kind] : setting.units;
                s.configured_payload = rows * width;
                s.configured_metadata = rows
                    * (pineforge::detail::ReadThroughCache::line_bytes(width) - width);
                s.payload = cache.stats.current_payload_bytes;
                s.metadata = cache.stats.current_metadata_bytes;
                s.peak_resident = cache.stats.peak_bytes;
                s.fixed_control = cache.control_bytes;
                s.workspace = s.fixed_workspace = cache.pending_bytes + width;
                s.hits = cache.stats.hits;
                s.misses = cache.stats.misses;
                s.evictions = cache.stats.evictions;
                s.inner_reads = cache.inner_reads;
                s.inner_bytes = cache.inner_bytes;
                s.cold_reads = cache.cold_reads;
                s.evicted_reread_reads = cache.evicted_reread_reads;
                return result;
            });
    }
}
#endif
} // namespace native_tail_window_fixture
