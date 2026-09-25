// R5 lane V19-B: one value for a run's whole native_events() record, row by
// row, so a readback can be held equal to the one a tree without the journal
// window returned. Every row folds its kind and ordinal; a command row folds
// its alternative, the fields common to the alternatives that carry them
// (definition identity, reason, cursor) and the payload of the ones a reader
// acts on (applied fills, activations, replaces, resolved terms); a driver row
// folds its coordinate, price and flags; an account row its four numbers. It
// reads nothing a v18 or earlier v19 tree lacks, so the same header compiles
// against 6211dc94 to harvest the pins (V19-B-scratch/failbefore).
//
// Source-free.
#pragma once

#include <pineforge/native_host.hpp>

#include <cstdint>
#include <cstring>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace event_rows {

namespace no = pineforge::native_order;

struct Fold {
    std::uint64_t h = 1469598103934665603ull;
    void u(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            h ^= (v >> (8 * i)) & 0xffu;
            h *= 1099511628211ull;
        }
    }
    void d(double x) {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &x, sizeof bits);
        u(bits);
    }
};

template <class T, class = void>
struct has_definition : std::false_type {};
template <class T>
struct has_definition<T, std::void_t<decltype(std::declval<const T&>().definition)>>
    : std::true_type {};
template <class T, class = void>
struct has_reason : std::false_type {};
template <class T>
struct has_reason<T, std::void_t<decltype(std::declval<const T&>().reason)>>
    : std::true_type {};
template <class T, class = void>
struct has_cursor : std::false_type {};
template <class T>
struct has_cursor<T, std::void_t<decltype(std::declval<const T&>().cursor)>>
    : std::true_type {};

inline void fold_command(Fold& f, const no::CommandEvent& event) {
    f.u(event.index());
    std::visit([&](const auto& e) {
        using E = std::decay_t<decltype(e)>;
        f.u(e.ordinal);
        if constexpr (has_definition<E>::value) {
            f.u(e.definition ? 1u : 0u);
            if (e.definition) {
                f.u(e.definition->handle.incarnation);
                f.u(e.definition->birth.acceptance_ordinal);
                f.u(e.definition->request.intent.index());
                f.u(e.definition->request.trigger.index());
                f.u(e.definition->request.label.size());
                for (const char c : e.definition->request.label) f.u(static_cast<unsigned char>(c));
            }
        }
        if constexpr (has_reason<E>::value) f.u(static_cast<std::uint64_t>(e.reason));
        if constexpr (has_cursor<E>::value) {
            f.u(e.cursor.point.ordinal);
            f.d(e.cursor.t);
        }
        if constexpr (std::is_same_v<E, no::ExecutionAppliedEvent>) {
            f.d(e.raw_price);
            f.d(e.resolved_price);
            f.d(e.closed_units);
            f.d(e.opened_units);
            f.u(e.first_trade_index);
            f.u(e.closed_trade_count);
            f.u(e.terminal ? 1u : 0u);
            f.u(static_cast<std::uint64_t>(e.cycle_before));
            f.u(static_cast<std::uint64_t>(e.cycle_after));
        }
        if constexpr (std::is_same_v<E, no::ActivatedEvent>) {
            f.u(static_cast<std::uint64_t>(e.kind));
            f.d(e.reached_price);
            f.u(e.before.index());
            f.u(e.after.index());
        }
        if constexpr (std::is_same_v<E, no::ReplacedEvent>) {
            f.u(e.predecessor().incarnation);
            f.u(e.successor().incarnation);
        }
        if constexpr (std::is_same_v<E, no::TermsResolvedEvent>) {
            f.d(e.input.terms.resolved_price);
            f.u(e.prior_adjustment_ids.size());
        }
        if constexpr (std::is_same_v<E, no::MarginCallEvent>) {
            f.u(e.applied.ordinal);
            f.d(e.mark);
            f.d(e.units);
        }
    }, event);
}

inline void fold_row(Fold& f, const pineforge::NativeMarketEvent& row) {
    f.u(static_cast<std::uint64_t>(row.kind));
    f.u(row.ordinal);
    if (row.command) fold_command(f, *row.command);
    if (row.driver) {
        const auto& c = row.driver->coordinate;
        f.u(c.ordinal);
        f.u(static_cast<std::uint64_t>(c.effective_time_ms));
        f.u(static_cast<std::uint64_t>(c.open_ms));
        f.u(static_cast<std::uint64_t>(c.interval_index));
        f.u(static_cast<std::uint64_t>(c.input_interval_index));
        f.u(static_cast<std::uint64_t>(c.provenance));
        f.u(static_cast<std::uint64_t>(c.path_phase));
        f.d(row.driver->raw_price);
        f.u(row.driver->matching ? 1u : 0u);
        f.u(row.driver->excursion ? 1u : 0u);
    }
    if (row.account) {
        f.u(static_cast<std::uint64_t>(row.account->effective_time_ms));
        f.d(row.account->marked_equity);
        f.d(row.account->realized_balance);
        f.d(row.account->signed_units);
    }
}

// The whole record: its count and the chained fold of its rows, in order.
struct Record {
    std::uint64_t rows = 0;
    std::uint64_t commands = 0;
    std::uint64_t drivers = 0;
    std::uint64_t accounts = 0;
    std::uint64_t digest = 0;
};

inline Record record(const std::vector<pineforge::NativeMarketEvent>& rows) {
    Record out;
    Fold f;
    for (const auto& row : rows) {
        fold_row(f, row);
        ++out.rows;
        if (row.command) ++out.commands;
        if (row.driver) ++out.drivers;
        if (row.account) ++out.accounts;
    }
    out.digest = f.h;
    return out;
}

// One row's own fold, for comparing single rows across two reads.
inline std::uint64_t row_value(const pineforge::NativeMarketEvent& row) {
    Fold f;
    fold_row(f, row);
    return f.h;
}

}  // namespace event_rows
