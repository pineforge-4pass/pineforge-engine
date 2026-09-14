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
using pineforge::source::PendingOrder;
namespace pineforge {
void fill_pending_order_mirror(const source::PendingOrder&, pf_pending_order_v1_t*);
const pf_field_desc_t* pending_order_layout(int*);
}
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
    const std::vector<source::PendingOrder>& book() const { return pending_orders_; }
};

Probe Build2Bars() {
    const std::vector<Bar> bars = {flat_bar(100, 0), flat_bar(100, 60'000)};
    Probe s; s.run(bars.data(), 2);
    return s;
}

const pf_field_desc_t* find_field(const pf_field_desc_t* layout, int n, const char* name) {
    for (int i = 0; i < n; ++i) if (std::strcmp(layout[i].name, name) == 0) return &layout[i];
    return nullptr;
}
}  // namespace

int main() {
    Probe s = Build2Bars();
    CHECK(s.book().size() == 1);
    if (s.book().empty()) return 1;
    const source::PendingOrder& o = s.book()[0];

    // --- fill_pending_order_mirror: value semantics -----------------------
    pf_pending_order_v1_t m;
    std::memset(&m, 0xAB, sizeof m);
    fill_pending_order_mirror(o, &m);
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
    CHECK(m.type == (int32_t)o.type);
    CHECK(m.created_bar == o.created_bar && m.created_seq == o.created_seq);
    CHECK(m.incarnation == o.incarnation && m.incarnation != 0);
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
        fill_pending_order_mirror(o, &m2);
        CHECK(std::memcmp(&m, &m2, sizeof m) == 0);
    }
    {
        source::PendingOrder cancelled = o;
        CancellationTarget target{cancelled.legs.target().incarnation,
                                  cancelled.legs.target().owner,
                                  cancelled.legs.revision()};
        if (target.incarnation == 0) target.incarnation = cancelled.incarnation;
        CHECK(cancelled.cancellation.bind_close_claim(2.5, 0.25));
        CHECK(cancelled.cancellation.cancel(CancellationCause::Dependency,
              7001, 4, target, target) == CancellationResult::Applied);
        pf_pending_order_v1_t cm;
        fill_pending_order_mirror(cancelled, &cm);
        CHECK(cm.cancellation_cause == static_cast<int32_t>(CancellationCause::Dependency));
        CHECK(cm.cancellation_state == static_cast<int32_t>(CancellationState::Cancelled));
        CHECK(cm.cancellation_close_claim_release == static_cast<int32_t>(CloseClaimRelease::Pending));
        CHECK(cm.cancellation_source_incarnation == 7001);
        CHECK(cm.cancellation_source_sequence == 4);
        CHECK(cm.cancellation_target_incarnation == target.incarnation);
        CHECK(cm.cancellation_target_owner == target.owner);
        CHECK(cm.cancellation_target_revision == target.revision);
        CHECK(cm.cancellation_close_claim_consumed == 2.5);
        CHECK(cm.cancellation_close_claim_retired == 0.25);
    }

    // --- pending_order_layout: self-describing, ordered, in-bounds ----------
    int n = 0;
    const pf_field_desc_t* layout = pending_order_layout(&n);
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
    CHECK(&s.pending_order_at(0) == &o);

    return failures == 0 ? 0 : 1;
}
