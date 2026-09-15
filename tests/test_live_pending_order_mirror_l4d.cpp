// A29 CHECK-parity native-route twin. Base body copied from ab9714be;
// rewrite only owner-private drives/reads while retaining literal checks.
#include "l4d_native_route_guard.hpp"
#include "oracle_fixture_config_shim.hpp"
#define PineStrategyHost L4dPineHost
#define PendingOrder L4dPendingOrder
#define pending_orders_ l4d_pending_rows()
#define OrderType L4dOrderType
#define ShortSeedCollisionRole L4dShortSeedRole
#define is_first_tick_ is_first_tick()
#define coof_fill_recalc_active_ l4d_coof_fill_recalc_active()
#define coof_cursor_is_bar_close_ l4d_coof_cursor_is_bar_close()

// ABI v4 live-runtime surface (task 7): the generated POD mirror of
// pineforge::PendingOrder (include/pineforge/pending_order_mirror.hpp,
// src/pending_order_mirror.cpp -- scripts/gen_pending_order_mirror.py) and
// the strategy_pending_orders_len / strategy_pending_order_get /
// strategy_pending_order_layout accessors that expose the resting book
// through <pineforge/pineforge.h>.
//
// Include order is load-bearing (same as src/c_abi.cpp): pineforge.h BEFORE
// engine.hpp keeps the extern "C" prototypes visible so the calls below are
// prototype-checked against the public header.
#include <pineforge/pineforge.h>
#include <pineforge/bar.hpp>
#include <pineforge/engine.hpp>
#include <pineforge/source/pine_strategy_host.hpp>
#include <pineforge/pending_order_mirror.hpp>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>
using namespace pineforge;
namespace {
int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

Bar flat_bar(double p, int64_t ts) { return Bar{p, p, p, p, 1.0, ts}; }

uint64_t fnv1a64(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char ch : s) { h ^= ch; h *= 1099511628211ULL; }
    return h;
}

const std::string kLongId(70, 'x');   // > 63 bytes: exercises truncation + hash64

class Probe final : public pineforge::source::PineStrategyHost {
public:
    void on_source_bar(const Bar&) override {
        if (bar_index_ == 0) strategy_entry("L", true);
        // bar 1: the MARKET entry filled at this bar's open; rest a stop-only
        // exit with an over-long id so the mirror's char[64] truncates.
        if (bar_index_ == 1) strategy_exit(kLongId, "L", na<double>(), 95.0);
    }
};

void Build2Bars(Probe& s) {
    const std::vector<Bar> bars = {flat_bar(100, 0), flat_bar(100, 60'000)};
    s.run(bars.data(), 2);
}

const pf_field_desc_t* find_field(const pf_field_desc_t* layout, int n, const char* name) {
    for (int i = 0; i < n; ++i) if (std::strcmp(layout[i].name, name) == 0) return &layout[i];
    return nullptr;
}
}  // namespace

int main() {
    Probe s;
    Build2Bars(s);
    CHECK(strategy_pending_orders_len(&s) == 1);
    if (strategy_pending_orders_len(&s) != 1) return 1;

    // --- fill_pending_order_mirror: value semantics -----------------------
    pf_pending_order_v1_t m;
    std::memset(&m, 0xAB, sizeof m);
    if (strategy_pending_order_get(&s, 0, &m, sizeof m) != 0) return 1;
    CHECK(m.struct_version == 1 && m.size == sizeof(m));
    CHECK(m.struct_version == PF_PENDING_ORDER_STRUCT_VERSION);
    CHECK(m.id_truncated == 1 && std::strlen(m.id) == 63);
    CHECK(std::string(m.id) == kLongId.substr(0, 63));
    CHECK(m.id_hash64 == fnv1a64(kLongId));                    // hash of the FULL string
    CHECK(std::strcmp(m.from_entry, "L") == 0 && m.from_entry_truncated == 0);
    CHECK(m.from_entry_hash64 == fnv1a64("L"));
    CHECK(m.oca_name[0] == 0 && m.oca_name_truncated == 0 && m.oca_name_hash64 == fnv1a64(""));
    CHECK(m.stop_price == 95.0 && m.is_long == 0);
    CHECK(m.limit_price != m.limit_price);                    // NaN copied by value
    CHECK(m.type == static_cast<int32_t>(L4dOrderType::EXIT));
    CHECK(m.created_bar == 1 && m.created_seq > 0);
    CHECK(m.incarnation != 0);
    CHECK(m.created_position_side == (int32_t)PositionSide::LONG);
    CHECK(m.short_seed_collision_role == (int32_t)ShortSeedCollisionRole::NONE);
    CHECK(m.coof_cascade_seg_i == -1);                        // int8_t widened to int32_t
    CHECK(m.dormant_hold_bar == -1 && m.same_id_stop_deferred_close_all_bar == -1);
    // The v1 396-field prefix remains byte-stable; cancellation leaves are
    // appended after the final admission receipt field.
    CHECK(offsetof(pf_pending_order_v1_t, cancellation_cause)
          > offsetof(pf_pending_order_v1_t, market_admission_sizing_revision_target_command));
    // The whole struct is defined: no 0xAB byte survives outside the string
    // payloads (padding is memset to 0 by the filler).
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&m);
        size_t ab = 0;
        for (size_t i = 0; i < sizeof m; ++i) ab += p[i] == 0xAB;
        CHECK(ab == 0);
    }
    // Deterministic: two fills of the same order are byte-identical.
    {
        pf_pending_order_v1_t m2;
        std::memset(&m2, 0x5C, sizeof m2);
        if (strategy_pending_order_get(&s, 0, &m2, sizeof m2) != 0) return 1;
        CHECK(std::memcmp(&m, &m2, sizeof m) == 0);
    }
    {
        // Cancellation receipt is a generic, value-owned native record. Its
        // fields have no post-retirement pending-row projection, so exercise
        // the public receipt accessors directly rather than mutating a copied
        // source-book row.
        OrderCancellationReceipt cancelled;
        CancellationTarget target{m.incarnation, m.created_position_cycle_seq, 1};
        CHECK(cancelled.bind_close_claim(2.5, 0.25));
        CHECK(cancelled.cancel(CancellationCause::Dependency,
              7001, 4, target, target) == CancellationResult::Applied);
        CHECK(cancelled.cause() == CancellationCause::Dependency);
        CHECK(cancelled.state() == CancellationState::Cancelled);
        CHECK(cancelled.close_claim_release() == CloseClaimRelease::Pending);
        CHECK(cancelled.source_incarnation() == 7001);
        CHECK(cancelled.source_sequence() == 4);
        CHECK(cancelled.target_incarnation() == target.incarnation);
        CHECK(cancelled.target_owner() == target.owner);
        CHECK(cancelled.target_revision() == target.revision);
        CHECK(cancelled.close_claim_consumed() == 2.5);
        CHECK(cancelled.close_claim_retired() == 0.25);
    }

    // --- pending_order_layout: self-describing, ordered, in-bounds ----------
    int n = 0;
    const pf_field_desc_t* layout = strategy_pending_order_layout(&n);
    CHECK(layout != nullptr && n > 10);
    CHECK(std::strcmp(layout[0].name, "struct_version") == 0 && layout[0].offset == 0 && layout[0].size == 4);
    CHECK(std::strcmp(layout[1].name, "size") == 0 && layout[1].offset == 4 && layout[1].size == 4);
    const pf_field_desc_t* f_stop = find_field(layout, n, "stop_price");
    CHECK(f_stop && f_stop->offset == offsetof(pf_pending_order_v1_t, stop_price)
          && f_stop->size == sizeof(double) && std::strcmp(f_stop->type, "double") == 0);
    const pf_field_desc_t* f_id = find_field(layout, n, "id");
    CHECK(f_id && f_id->offset == offsetof(pf_pending_order_v1_t, id) && f_id->size == 64
          && std::strcmp(f_id->type, "char[64]") == 0);
    CHECK(find_field(layout, n, "id_truncated") && find_field(layout, n, "id_hash64"));
    CHECK(find_field(layout, n, "comment") && find_field(layout, n, "from_entry") && find_field(layout, n, "oca_name"));
    const pf_field_desc_t* f_side = find_field(layout, n, "created_position_side");
    CHECK(f_side && std::strcmp(f_side->type, "int32_t") == 0
          && f_side->offset == offsetof(pf_pending_order_v1_t, created_position_side));
    {
        static const std::set<std::string> kTypes = {
            "uint8_t", "int32_t", "int64_t", "uint64_t", "uint32_t", "double", "char[64]"};
        std::set<std::string> names;
        uint32_t prev_end = 0;
        for (int i = 0; i < n; ++i) {
            CHECK(kTypes.count(layout[i].type) == 1);
            CHECK(names.insert(layout[i].name).second);            // unique names
            CHECK(layout[i].offset >= prev_end);                   // declaration order, no overlap
            CHECK(layout[i].offset + layout[i].size <= sizeof(pf_pending_order_v1_t));
            prev_end = layout[i].offset + layout[i].size;
        }
        CHECK(prev_end <= sizeof(pf_pending_order_v1_t) && prev_end + 8 > sizeof(pf_pending_order_v1_t));
    }

    // --- C ABI: strategy_pending_orders_len / _get / _layout ------------------
    pf_strategy_t h = &s;
    CHECK(strategy_pending_orders_len(h) == 1);
    CHECK(strategy_pending_orders_len(nullptr) == 0);
    {
        pf_pending_order_v1_t v;
        std::memset(&v, 0x11, sizeof v);
        CHECK(strategy_pending_order_get(h, 0, &v, sizeof v) == 0);
        CHECK(std::memcmp(&v, &m, sizeof v) == 0);               // identical to the direct fill
        CHECK(strategy_pending_order_get(h, 1, &v, sizeof v) == -1);     // index past the book
        CHECK(strategy_pending_order_get(h, -1, &v, sizeof v) == -1);
        CHECK(strategy_pending_order_get(nullptr, 0, &v, sizeof v) == -1);
        CHECK(strategy_pending_order_get(h, 0, nullptr, sizeof v) == -1);
    }
    {
        // Older-reader contract: a caller with a smaller struct receives a
        // prefix copy of exactly size_in bytes and nothing beyond it.
        pf_pending_order_v1_t v;
        std::memset(&v, 0x11, sizeof v);
        CHECK(strategy_pending_order_get(h, 0, &v, 8) == 0);
        CHECK(v.struct_version == 1 && v.size == sizeof(pf_pending_order_v1_t));
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
        bool untouched = true;
        for (size_t i = 8; i < sizeof v; ++i) untouched = untouched && p[i] == 0x11;
        CHECK(untouched);
    }
    {
        // size_in < 8 cannot hold struct_version + size: rejected, nothing
        // written. size_in == 8 is the smallest honoured prefix.
        pf_pending_order_v1_t v;
        std::memset(&v, 0x33, sizeof v);
        CHECK(strategy_pending_order_get(h, 0, &v, 0) == -1);
        CHECK(strategy_pending_order_get(h, 0, &v, 7) == -1);
        const unsigned char* p = reinterpret_cast<const unsigned char*>(&v);
        bool untouched = true;
        for (size_t i = 0; i < sizeof v; ++i) untouched = untouched && p[i] == 0x33;
        CHECK(untouched);
        CHECK(strategy_pending_order_get(h, 0, &v, 8) == 0);
        CHECK(v.struct_version == 1 && v.size == sizeof(pf_pending_order_v1_t));
        CHECK(p[8] == 0x33);
    }
    {
        // Newer-reader contract: an over-sized buffer gets sizeof(v1) bytes;
        // the tail is left to the caller.
        unsigned char big[sizeof(pf_pending_order_v1_t) + 32];
        std::memset(big, 0x22, sizeof big);
        CHECK(strategy_pending_order_get(h, 0, big, sizeof big) == 0);
        CHECK(std::memcmp(big, &m, sizeof m) == 0);
        bool tail_untouched = true;
        for (size_t i = sizeof m; i < sizeof big; ++i) tail_untouched = tail_untouched && big[i] == 0x22;
        CHECK(tail_untouched);
    }
    {
        int n2 = -1;
        const pf_field_desc_t* l2 = strategy_pending_order_layout(&n2);
        CHECK(l2 == layout && n2 == n);
        CHECK(strategy_pending_order_layout(nullptr) == layout);    // count pointer optional
    }

    // --- the accessors track the live book -------------------------------------
    {
        const std::vector<Bar> bars = {flat_bar(100, 0), flat_bar(100, 60'000)};
        class Empty final : public pineforge::source::PineStrategyHost {
        public:
            void on_source_bar(const Bar&) override {}
        } e;
        e.run(bars.data(), 2);
        CHECK(strategy_pending_orders_len(&e) == 0);
        pf_pending_order_v1_t v;
        CHECK(strategy_pending_order_get(&e, 0, &v, sizeof v) == -1);
        CHECK(e.pending_order_count() == 0);
    }
    CHECK(s.pending_order_count() == 1);
    CHECK(strategy_pending_order_get(&s, 0, &m, sizeof m) == 0);

    return failures == 0 ? 0 : 1;
}

#undef coof_cursor_is_bar_close_
#undef coof_fill_recalc_active_
#undef is_first_tick_
#undef ShortSeedCollisionRole
#undef OrderType
#undef pending_orders_
#undef PendingOrder
#undef PineStrategyHost
