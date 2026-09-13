#pragma once

#include "native_current_fixture.hpp"

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
