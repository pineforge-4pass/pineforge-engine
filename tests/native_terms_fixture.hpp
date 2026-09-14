#pragma once

#include "native_current_fixture.hpp"

// The A-T13 control needs to force the otherwise impossible post-binding
// NoChange branch without adding a product test hook.  The consumer layout is
// unchanged; this only gives the test fixture access to its private core.
#define private public
#include "../src/native_execution_consumer.hpp"
#undef private

#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <tuple>
#include <vector>

namespace r4_terms {

using namespace r4_test;

inline std::uint64_t bits(double value) {
    std::uint64_t out = 0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

inline double from_bits(std::uint64_t value) {
    double out = 0.0;
    std::memcpy(&out, &value, sizeof out);
    return out;
}

struct TermsHost : Host {
    mutable std::function<no::ExecutionTerms(const NativeExecutionTermsFacts&)> resolver;
    std::function<NativePrecommitVerdict(const NativePrecommitView&)> validator;
    mutable std::vector<NativeExecutionTermsFacts> resolved_facts;
    std::vector<NativePrecommitView> precommit_views;
    mutable int resolver_calls = 0;
    int validator_calls = 0;

    std::tuple<bool, std::size_t, double> fx_clock_state() const {
        return {account_currency_fx_broker_epoch_initialized_,
                account_currency_fx_broker_epoch_, account_currency_fx_broker_rate_};
    }
    std::int64_t engine_timestamp() const { return current_bar_.timestamp; }
    void poison_next_cycle() { next_position_cycle_seq_ = std::numeric_limits<std::int64_t>::max(); }
    void enable_trace_for_sink_test() { trace_enabled_ = true; }

    // Construct the deliberately absorbed second physical lot used by the
    // whole-book Flatten witness. Its aggregate contribution is intentionally
    // invisible in binary64, but the roster and per-entry accounting remain
    // authoritative.
    void append_absorbed_lot(double quantity, double price, std::uint64_t incarnation) {
        PyramidEntry lot{price, current_bar_.timestamp, quantity, "absorbed-tiny", 3};
        lot.entry_incarnation = incarnation;
        lot.entry_commission_account = 0.0;
        pyramid_entries_.push_back(lot);
        position_qty_ += quantity;
        ++position_entry_count_;
        id_unclosed_qty_[lot.entry_id] += quantity;
        cycle_filled_entry_ids_.insert(lot.entry_id);
    }

    std::uint64_t stream_hash_at_timestamp(std::int64_t timestamp) {
        const auto saved = current_bar_.timestamp;
        current_bar_.timestamp = timestamp;
        const auto hash = stream_state_hash();
        current_bar_.timestamp = saved;
        return hash;
    }

    std::optional<std::int64_t> emit_trace_timestamp(const char* name) {
        trace(name, 0.0);
        ReportC report{};
        fill_report(&report);
        std::optional<std::int64_t> timestamp;
        if (report.trace_len > 0) timestamp = report.trace[report.trace_len - 1].timestamp;
        BacktestEngine::free_report(&report);
        return timestamp;
    }

    void inject_post_binding_no_change(const no::RequestHandle& target) {
        auto& consumer = as_native_consumer(execution_consumer());
        const auto* found = consumer.requests_.find_live(target);
        REQUIRE(found);
        // prepare_terms copies this state into its binding mutation.  The
        // subsequent prepare_execution therefore takes its normal
        // NotEligible NoChange branch after the receipt is installed.
        auto* live = const_cast<no::LiveRequest*>(found);
        live->trigger_state = no::StopIdle{};
    }

    no::ExecutionTerms resolve_execution_terms(
            const NativeExecutionTermsFacts& facts) const override {
        ++resolver_calls;
        resolved_facts.push_back(facts);
        if (resolver) return resolver(facts);
        return {facts.default_resolved_price, std::nullopt, no::OpeningShape::Transact};
    }

    NativePrecommitVerdict validate_execution_precommit(
            const NativePrecommitView& view) const override {
        auto& self = const_cast<TermsHost&>(*this);
        ++self.validator_calls;
        self.precommit_views.push_back(view);
        if (validator) return validator(view);
        return NativePrecommitVerdict::Proceed;
    }
};

inline no::Request host_open(no::Side side = no::Side::Long,
                             const char* label = "host-open") {
    no::Request out;
    out.intent = no::HostSized{no::HostSizedKind::Open, side};
    out.label = label;
    return out;
}

inline no::Request host_close(const char* label = "host-close") {
    no::Request out;
    out.intent = no::HostSized{no::HostSizedKind::Close, std::nullopt};
    out.label = label;
    return out;
}

inline no::Request reverse(double signed_units, const char* label = "reverse") {
    no::Request out;
    out.intent = no::ReverseTo{signed_units};
    out.label = label;
    return out;
}

template <class Event>
inline std::optional<Event> last_event(const TermsHost& host) {
    const auto rows = events<Event>(host);
    if (rows.empty()) return std::nullopt;
    return rows.back();
}

inline void check_no_physical_change(const TermsHost& host, std::size_t lots,
                                     std::size_t rows, std::size_t accounts_before) {
    CHECK(host.lots().size() == lots);
    CHECK(host.rows().size() == rows);
    CHECK(accounts(host) == accounts_before);
}

}  // namespace r4_terms
