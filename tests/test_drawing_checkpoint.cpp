// K-DRAWSNAP: a drawing arena's checkpoint is exact and costs O(live + changes).
//
// Under calc_on_order_fills the source scheduler checkpoints the generated
// script state at every script bar (src/source/pine_scheduler_native.cpp:
// a snapshot at the bar open, a restore before the close execution, a commit
// after it, and two restores around every fill recalculation), and the
// generated hooks hold each DrawingArena by value: snapshot and commit
// copy-construct it, restore copy-assigns it. A record is never forgotten --
// delete and max_*_count eviction only tombstone it -- so while a copy cost
// O(every record ever allocated), a run that keeps drawing was quadratic in
// bars. The arena now holds its records in a vector until it is copied, and
// once it has been assigned from a copy it shares its records with its copies
// and clones only the blocks a write reaches (include/pineforge/drawing.hpp).
//
// Rows:
//   differential  every observable of the arena (size, alive, every field of
//                 every record, order()) and every pf_* getter, setter, copy
//                 and delete agree with a deep-copy reference -- the vector
//                 arena as it stood before this lane, verbatim below -- over
//                 random create / delete / eviction / set_* interleaved with
//                 snapshot, restore, commit and arbitrary copy graphs, for all
//                 four record kinds;
//   scheduler     the scheduler's own sequence (snapshot, restore, execute,
//                 commit, recalculations restoring twice), checked after every
//                 rollback and every commit, across a run reset that puts a
//                 fresh vector arena over a shared one;
//   deep trie     writes to old records of a 40,000-record arena after a
//                 checkpoint leave the checkpoint intact;
//   cost          records copied and heap blocks allocated by checkpointing:
//                 none but order_'s for an unchanged arena, whatever it holds,
//                 and linear in bars for the synth-label-coof shape;
//   bad_alloc     an allocation refused while a write unshares blocks --
//                 eviction's included -- or while a copy is built leaves the
//                 arena consistent and its checkpoint untouched, including a
//                 refusal partway through copying a block's records.
//
// Under AddressSanitizer (whose fake stack makes every call dear) the random
// rows run an eighth of their steps, the two long ones a thirty-second, and
// the cost row a third of its bars; the 40,000-record row still takes the
// arena past three block levels.
#define PINEFORGE_TEST_ALLOCATION_BYTES
#include "global_allocation_replacement.hpp"

#include <pineforge/drawing.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace pineforge;

#if defined(__SANITIZE_ADDRESS__)
constexpr int kSanitized = 1;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int kSanitized = 1;
#else
constexpr int kSanitized = 0;
#endif
#else
constexpr int kSanitized = 0;
#endif
constexpr int kStepDivisor = kSanitized ? 8 : 1;
constexpr int kDeepDivisor = kSanitized ? 32 : 1;
constexpr int kCostDivisor = kSanitized ? 3 : 1;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            ++g_fail;                                                          \
        } else {                                                               \
            ++g_pass;                                                          \
        }                                                                      \
    } while (0)

namespace {

// ---- the deep-copy reference: DrawingArena before K-DRAWSNAP, verbatim ------
template <class Rec>
class RefArena {
    std::vector<Rec>     recs_;
    std::deque<int32_t>  order_;
    int                  cap_;
public:
    explicit RefArena(int cap = 50) : cap_(std::max(1, cap)) {}

    int32_t alloc(Rec r) {
        while ((int)order_.size() >= cap_) {
            int32_t old = order_.front();
            order_.pop_front();
            recs_[old].alive = false;
        }
        int32_t id = (int32_t)recs_.size();
        r.alive = true;
        recs_.push_back(std::move(r));
        order_.push_back(id);
        return id;
    }

    void erase(int32_t id) {
        if (!alive(id)) return;
        recs_[id].alive = false;
        order_.erase(std::find(order_.begin(), order_.end(), id));
    }

    bool alive(int32_t id) const { return id >= 0 && id < (int)recs_.size() && recs_[id].alive; }
    int32_t size() const { return (int32_t)recs_.size(); }
    Rec&       at(int32_t id)       { return recs_[id]; }
    const Rec& at(int32_t id) const { return recs_[id]; }
    const std::deque<int32_t>& order() const { return order_; }
};

// The reference pf_* bodies, as drawing.hpp spelled them before this lane.
template <class Rec, class Handle>
Rec& ref_require(RefArena<Rec>& a, Handle h) {
    if (h.id < 0 || h.id >= a.size()) throw pine_drawing_error("drawing access on na handle");
    return a.at(h.id);
}

// ---- deterministic randomness (identical on every platform) ---------------
struct Rng {
    std::uint64_t s;
    std::uint64_t next() {
        std::uint64_t z = (s += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    bool coin(int percent) { return below(100) < percent; }
    int64_t x() {
        switch (below(8)) {
            case 0: return na<int64_t>();
            case 1: return 1'700'000'000'000LL + below(1'000'000);
            default: return below(4000) - 50;
        }
    }
    double y() {
        switch (below(8)) {
            case 0: return na<double>();
            case 1: return -0.0;
            default: return (below(2'000'000) - 1'000'000) / 64.0;
        }
    }
    XLoc xloc() { return coin(80) ? XLoc::bar_index : XLoc::bar_time; }
    YLoc yloc() { return static_cast<YLoc>(below(3)); }
    std::string text() {
        static const char* const words[] = {"", "x", "k", "1x1", "Gann 2x1 upper fan",
                                            "a label text long enough to leave the small-string buffer"};
        std::string t = words[below(6)];
        if (coin(20)) t += std::to_string(below(100000));
        return t;
    }
    ChartPoint point() { return ChartPoint{coin(85) ? x() : na<int64_t>(), x(), y()}; }
};

// ---- exact comparison ------------------------------------------------------
bool same_double(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

bool same_rec(const LineRec& a, const LineRec& b) {
    return a.x1 == b.x1 && a.x2 == b.x2 && same_double(a.y1, b.y1) && same_double(a.y2, b.y2)
        && a.xloc == b.xloc && a.ext_l == b.ext_l && a.ext_r == b.ext_r && a.alive == b.alive;
}
bool same_rec(const BoxRec& a, const BoxRec& b) {
    return a.left == b.left && a.right == b.right && same_double(a.top, b.top)
        && same_double(a.bottom, b.bottom) && a.xloc == b.xloc && a.alive == b.alive;
}
bool same_rec(const LabelRec& a, const LabelRec& b) {
    return a.x == b.x && same_double(a.y, b.y) && a.xloc == b.xloc && a.yloc == b.yloc
        && a.text == b.text && a.alive == b.alive;
}
bool same_rec(const LinefillRec& a, const LinefillRec& b) {
    return a.line1 == b.line1 && a.line2 == b.line2 && a.alive == b.alive;
}

// Everything a script can observe of an arena: the id range, every record
// (live or dead) with every field, liveness, and the *.all / eviction order.
template <class Rec>
bool same_arena(const DrawingArena<Rec>& got, const RefArena<Rec>& want) {
    if (got.size() != want.size()) return false;
    if (got.alive(-1) || got.alive(got.size())) return false;
    for (int32_t id = 0; id < want.size(); ++id) {
        if (got.alive(id) != want.alive(id)) return false;
        if (!same_rec(got.at(id), want.at(id))) return false;
    }
    return got.order() == want.order();
}

// The cheap per-step comparison: size, order and the records one op touched.
template <class Rec>
bool same_touched(const DrawingArena<Rec>& got, const RefArena<Rec>& want,
                  const std::vector<int32_t>& ids) {
    if (got.size() != want.size() || got.order() != want.order()) return false;
    for (int32_t id : ids) {
        if (id < 0 || id >= want.size()) continue;
        if (got.alive(id) != want.alive(id) || !same_rec(got.at(id), want.at(id))) return false;
    }
    return true;
}

// ---- one random pf_* operation, applied to an arena and to its reference ----
// Each returns false when the two disagree (value or exception); `ids`
// collects the records the op touched.
template <class F, class G>
bool both(F&& got, G&& want) {
    bool got_threw = false, want_threw = false;
    try { got(); } catch (const pine_drawing_error&) { got_threw = true; }
    try { want(); } catch (const pine_drawing_error&) { want_threw = true; }
    return got_threw == want_threw;
}

int32_t pick_id(Rng& r, int32_t size) {
    if (r.coin(6)) return -1;
    if (r.coin(4)) return size + r.below(3);
    if (size == 0) return -1;
    if (r.coin(50)) return size - 1 - r.below(std::min<int32_t>(size, 70));
    return r.below(size);
}

bool random_op(Rng& r, DrawingArena<LineRec>& a, RefArena<LineRec>& b, std::vector<int32_t>& ids) {
    const Line h{pick_id(r, b.size())};
    ids.push_back(h.id);
    bool ok = true;
    switch (r.below(18)) {
        case 0: case 1: case 2: case 3: {
            const int64_t x1 = r.x(), x2 = r.x();
            const double y1 = r.y(), y2 = r.y();
            const XLoc xl = r.xloc();
            const bool el = r.coin(30), er = r.coin(30);
            Line got = pf_line_new(a, x1, y1, x2, y2, xl, el, er);
            int32_t want = b.alloc(LineRec{x1, x2, y1, y2, xl, el, er, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 4: {
            const ChartPoint p1 = r.point(), p2 = r.point();
            const XLoc xl = r.xloc();
            Line got = pf_line_new_pts(a, p1, p2, xl);
            int32_t want = b.alloc(LineRec{pf_point_x(p1, xl), pf_point_x(p2, xl), p1.price,
                                           p2.price, xl, false, false, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 5: { const int64_t v = r.x();
            ok = both([&] { pf_line_set_x1(a, h, v); }, [&] { ref_require(b, h).x1 = v; }); break; }
        case 6: { const int64_t v = r.x();
            ok = both([&] { pf_line_set_x2(a, h, v); }, [&] { ref_require(b, h).x2 = v; }); break; }
        case 7: { const double v = r.y();
            ok = both([&] { pf_line_set_y1(a, h, v); }, [&] { ref_require(b, h).y1 = v; }); break; }
        case 8: { const double v = r.y();
            ok = both([&] { pf_line_set_y2(a, h, v); }, [&] { ref_require(b, h).y2 = v; }); break; }
        case 9: { const int64_t x = r.x(); const double y = r.y();
            ok = both([&] { pf_line_set_xy1(a, h, x, y); },
                      [&] { LineRec& q = ref_require(b, h); q.x1 = x; q.y1 = y; });
            break; }
        case 10: { const int64_t x = r.x(); const double y = r.y();
            ok = both([&] { pf_line_set_xy2(a, h, x, y); },
                      [&] { LineRec& q = ref_require(b, h); q.x2 = x; q.y2 = y; });
            break; }
        case 11: { const ChartPoint p = r.point();
            ok = both([&] { pf_line_set_first_point(a, h, p); },
                      [&] { LineRec& q = ref_require(b, h); q.x1 = pf_point_x(p, q.xloc); q.y1 = p.price; });
            break; }
        case 12: { const ChartPoint p = r.point();
            ok = both([&] { pf_line_set_second_point(a, h, p); },
                      [&] { LineRec& q = ref_require(b, h); q.x2 = pf_point_x(p, q.xloc); q.y2 = p.price; });
            break; }
        case 13: { const int64_t x1 = r.x(), x2 = r.x(); const XLoc xl = r.xloc();
            ok = both([&] { pf_line_set_xloc(a, h, x1, x2, xl); },
                      [&] { LineRec& q = ref_require(b, h); q.x1 = x1; q.x2 = x2; q.xloc = xl; });
            break; }
        case 14: {
            int64_t g[4] = {}, w[4] = {};
            double gy[2] = {}, wy[2] = {};
            ok = both([&] { g[0] = pf_line_get_x1(a, h); g[1] = pf_line_get_x2(a, h);
                            gy[0] = pf_line_get_y1(a, h); gy[1] = pf_line_get_y2(a, h); },
                      [&] { const LineRec& q = ref_require(b, h);
                            w[0] = q.x1; w[1] = q.x2; wy[0] = q.y1; wy[1] = q.y2; });
            ok = ok && g[0] == w[0] && g[1] == w[1] && same_double(gy[0], wy[0])
                && same_double(gy[1], wy[1]);
            break;
        }
        case 15: {
            // get_price's int64 arithmetic overflows on a na (INT64_MIN) x --
            // an engine limit this row does not probe -- so it asks only lines
            // with real x coordinates, at a real x.
            const int64_t x = r.below(4000) - 50;
            if (h.id >= 0 && h.id < b.size()
                && (is_na(b.at(h.id).x1) || is_na(b.at(h.id).x2))) break;
            double g = 0.0, w = 0.0;
            ok = both([&] { g = pf_line_get_price(a, h, x); },
                      [&] {
                          if (h.id < 0 || h.id >= b.size())
                              throw pine_drawing_error("drawing access on na handle");
                          const LineRec& q = b.at(h.id);
                          if (q.xloc == XLoc::bar_time)
                              throw pine_drawing_error("line.get_price requires xloc.bar_index");
                          w = q.x1 == q.x2 ? na<double>()
                                           : q.y1 + (q.y2 - q.y1) / (double)(q.x2 - q.x1) * (double)(x - q.x1);
                      });
            ok = ok && same_double(g, w);
            break;
        }
        case 16: {
            Line got = pf_line_copy(a, h);
            int32_t want = -1;
            if (b.alive(h.id)) { LineRec q = b.at(h.id); want = b.alloc(q); }
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        default:
            pf_line_delete(a, h);
            b.erase(h.id);
            break;
    }
    return ok;
}

bool random_op(Rng& r, DrawingArena<BoxRec>& a, RefArena<BoxRec>& b, std::vector<int32_t>& ids) {
    const Box h{pick_id(r, b.size())};
    ids.push_back(h.id);
    bool ok = true;
    switch (r.below(16)) {
        case 0: case 1: case 2: case 3: {
            const int64_t l = r.x(), rt = r.x();
            const double t = r.y(), bt = r.y();
            const XLoc xl = r.xloc();
            Box got = pf_box_new(a, l, t, rt, bt, xl);
            int32_t want = b.alloc(BoxRec{l, rt, t, bt, xl, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 4: {
            const ChartPoint tl = r.point(), br = r.point();
            const XLoc xl = r.xloc();
            Box got = pf_box_new_pts(a, tl, br, xl);
            int32_t want = b.alloc(BoxRec{pf_point_x(tl, xl), pf_point_x(br, xl), tl.price,
                                          br.price, xl, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 5: { const int64_t v = r.x();
            ok = both([&] { pf_box_set_left(a, h, v); }, [&] { ref_require(b, h).left = v; }); break; }
        case 6: { const int64_t v = r.x();
            ok = both([&] { pf_box_set_right(a, h, v); }, [&] { ref_require(b, h).right = v; }); break; }
        case 7: { const double v = r.y();
            ok = both([&] { pf_box_set_top(a, h, v); }, [&] { ref_require(b, h).top = v; }); break; }
        case 8: { const double v = r.y();
            ok = both([&] { pf_box_set_bottom(a, h, v); }, [&] { ref_require(b, h).bottom = v; }); break; }
        case 9: { const int64_t x = r.x(); const double y = r.y();
            ok = both([&] { pf_box_set_lefttop(a, h, x, y); },
                      [&] { BoxRec& q = ref_require(b, h); q.left = x; q.top = y; });
            break; }
        case 10: { const int64_t x = r.x(); const double y = r.y();
            ok = both([&] { pf_box_set_rightbottom(a, h, x, y); },
                      [&] { BoxRec& q = ref_require(b, h); q.right = x; q.bottom = y; });
            break; }
        case 11: { const ChartPoint p = r.point();
            ok = both([&] { pf_box_set_top_left_point(a, h, p); },
                      [&] { BoxRec& q = ref_require(b, h); q.left = pf_point_x(p, q.xloc); q.top = p.price; });
            break; }
        case 12: { const ChartPoint p = r.point();
            ok = both([&] { pf_box_set_bottom_right_point(a, h, p); },
                      [&] { BoxRec& q = ref_require(b, h); q.right = pf_point_x(p, q.xloc); q.bottom = p.price; });
            break; }
        case 13: {
            int64_t g[2] = {}, w[2] = {};
            double gy[2] = {}, wy[2] = {};
            ok = both([&] { g[0] = pf_box_get_left(a, h); g[1] = pf_box_get_right(a, h);
                            gy[0] = pf_box_get_top(a, h); gy[1] = pf_box_get_bottom(a, h); },
                      [&] { const BoxRec& q = ref_require(b, h);
                            w[0] = q.left; w[1] = q.right; wy[0] = q.top; wy[1] = q.bottom; });
            ok = ok && g[0] == w[0] && g[1] == w[1] && same_double(gy[0], wy[0])
                && same_double(gy[1], wy[1]);
            break;
        }
        case 14: {
            Box got = pf_box_copy(a, h);
            int32_t want = -1;
            if (b.alive(h.id)) { BoxRec q = b.at(h.id); want = b.alloc(q); }
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        default:
            if (r.coin(50)) {
                const int64_t l = r.x(), rt = r.x(); const XLoc xl = r.xloc();
                ok = both([&] { pf_box_set_xloc(a, h, l, rt, xl); },
                          [&] { BoxRec& q = ref_require(b, h); q.left = l; q.right = rt; q.xloc = xl; });
            } else {
                pf_box_delete(a, h);
                b.erase(h.id);
            }
            break;
    }
    return ok;
}

bool random_op(Rng& r, DrawingArena<LabelRec>& a, RefArena<LabelRec>& b, std::vector<int32_t>& ids) {
    const Label h{pick_id(r, b.size())};
    ids.push_back(h.id);
    bool ok = true;
    switch (r.below(17)) {
        case 0: case 1: case 2: case 3: {
            const int64_t x = r.x(); const double y = r.y();
            const std::string t = r.text();
            const XLoc xl = r.xloc(); const YLoc yl = r.yloc();
            Label got = pf_label_new(a, x, y, t, xl, yl);
            int32_t want = b.alloc(LabelRec{x, y, xl, yl, t, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 4: {
            const ChartPoint p = r.point();
            const std::string t = r.text();
            const YLoc yl = r.yloc();
            Label got = pf_label_new_pt(a, p, t, yl);
            const XLoc xl = (is_na(p.index) && !is_na(p.time)) ? XLoc::bar_time : XLoc::bar_index;
            int32_t want = b.alloc(LabelRec{pf_point_x(p, xl), p.price, xl, yl, t, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 5: { const int64_t v = r.x();
            ok = both([&] { pf_label_set_x(a, h, v); }, [&] { ref_require(b, h).x = v; }); break; }
        case 6: { const double v = r.y();
            ok = both([&] { pf_label_set_y(a, h, v); }, [&] { ref_require(b, h).y = v; }); break; }
        case 7: { const int64_t x = r.x(); const double y = r.y();
            ok = both([&] { pf_label_set_xy(a, h, x, y); },
                      [&] { LabelRec& q = ref_require(b, h); q.x = x; q.y = y; });
            break; }
        case 8: { const ChartPoint p = r.point();
            ok = both([&] { pf_label_set_point(a, h, p); },
                      [&] { LabelRec& q = ref_require(b, h); q.x = pf_point_x(p, q.xloc); q.y = p.price; });
            break; }
        case 9: { const int64_t x = r.x(); const XLoc xl = r.xloc();
            ok = both([&] { pf_label_set_xloc(a, h, x, xl); },
                      [&] { LabelRec& q = ref_require(b, h); q.x = x; q.xloc = xl; });
            break; }
        case 10: { const YLoc yl = r.yloc();
            ok = both([&] { pf_label_set_yloc(a, h, yl); }, [&] { ref_require(b, h).yloc = yl; }); break; }
        case 11: case 12: { const std::string t = r.text();
            ok = both([&] { pf_label_set_text(a, h, t); }, [&] { ref_require(b, h).text = t; }); break; }
        case 13: {
            int64_t gx = 0, wx = 0; double gy = 0.0, wy = 0.0; std::string gt, wt;
            ok = both([&] { gx = pf_label_get_x(a, h); gy = pf_label_get_y(a, h); gt = pf_label_get_text(a, h); },
                      [&] { const LabelRec& q = ref_require(b, h); wx = q.x; wy = q.y; wt = q.text; });
            ok = ok && gx == wx && same_double(gy, wy) && gt == wt;
            break;
        }
        case 14: {
            Label got = pf_label_copy(a, h);
            int32_t want = -1;
            if (b.alive(h.id)) { LabelRec q = b.at(h.id); want = b.alloc(std::move(q)); }
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        default:
            pf_label_delete(a, h);
            b.erase(h.id);
            break;
    }
    return ok;
}

bool random_op(Rng& r, DrawingArena<LinefillRec>& a, RefArena<LinefillRec>& b, std::vector<int32_t>& ids) {
    const Linefill h{pick_id(r, b.size())};
    ids.push_back(h.id);
    bool ok = true;
    switch (r.below(6)) {
        case 0: case 1: case 2: {
            const Line l1{r.below(500) - 10}, l2{r.below(500) - 10};
            Linefill got = pf_linefill_new(a, l1, l2);
            int32_t want = b.alloc(LinefillRec{l1.id, l2.id, true});
            ok = got.id == want;
            ids.push_back(want);
            break;
        }
        case 3: case 4: {
            int32_t g[2] = {}, w[2] = {};
            ok = both([&] { g[0] = pf_linefill_get_line1(a, h).id; g[1] = pf_linefill_get_line2(a, h).id; },
                      [&] { const LinefillRec& q = ref_require(b, h); w[0] = q.line1; w[1] = q.line2; });
            ok = ok && g[0] == w[0] && g[1] == w[1];
            break;
        }
        default:
            pf_linefill_delete(a, h);
            b.erase(h.id);
            break;
    }
    return ok;
}

// ---- 1. the differential over random copy graphs ---------------------------
// A live arena and three checkpoint slots on each side. Beyond the
// generated hooks' own moves (copy-construct into a slot, copy-assign back,
// re-snapshot) the graph also writes to checkpoints, copies checkpoints into
// each other, swaps, moves and self-assigns: every copy must stay an
// independent value.
template <class Rec>
struct Differential {
    DrawingArena<Rec> live;
    RefArena<Rec> ref_live;
    std::optional<DrawingArena<Rec>> slot[3];
    std::optional<RefArena<Rec>> ref_slot[3];
    int disagreements = 0;
    int full_checks = 0;
    int slot_checks = 0;

    int ckpt_percent;   // the share of steps that move checkpoints rather than write
    int burst;          // a writing step runs 1 .. burst pf_* operations

    Differential(int cap, int ckpt_percent_, int burst_)
        : live(cap), ref_live(cap), ckpt_percent(ckpt_percent_), burst(burst_) {}

    bool all_same() {
        ++full_checks;
        if (!same_arena(live, ref_live)) return false;
        for (int k = 0; k < 3; ++k) {
            if (slot[k].has_value() != ref_slot[k].has_value()) return false;
            if (slot[k] && !same_arena(*slot[k], *ref_slot[k])) return false;
        }
        return true;
    }

    // A write that leaked into a copy lands on the records it touched: after
    // every write, those records are compared in the live arena and in every
    // checkpoint alike.
    void touched_everywhere(const std::vector<int32_t>& ids) {
        if (!same_touched(live, ref_live, ids)) ++disagreements;
        for (int k = 0; k < 3; ++k)
            if (slot[k] && !same_touched(*slot[k], *ref_slot[k], ids)) ++disagreements;
    }

    // A checkpoint is compared whole before it is replaced, reset or moved
    // from, so no checkpoint leaves the graph unexamined.
    void slot_whole(int k) {
        if (!slot[k]) return;
        ++slot_checks;
        if (!same_arena(*slot[k], *ref_slot[k])) ++disagreements;
    }

    void step(Rng& r) {
        const int k = r.below(3), j = r.below(3);
        if (!r.coin(ckpt_percent)) {
            for (int op = 1 + r.below(burst); op > 0; --op) {
                std::vector<int32_t> ids;
                if (!random_op(r, live, ref_live, ids)) ++disagreements;
                touched_everywhere(ids);
            }
            return;
        }
        switch (r.below(12)) {
            case 0: case 1:            // snapshot / commit: copy-construct
                slot_whole(k);
                slot[k].emplace(live);
                ref_slot[k].emplace(ref_live);
                break;
            case 2: {                  // the generated form: a temporary moved in
                slot_whole(k);
                struct State { DrawingArena<Rec> arena; };
                struct RefState { RefArena<Rec> arena; };
                State s{live};
                RefState rs{ref_live};
                slot[k].emplace(std::move(s.arena));
                ref_slot[k].emplace(std::move(rs.arena));
                break;
            }
            case 3: case 4: case 5:    // restore: copy-assign
                if (slot[k]) { live = *slot[k]; ref_live = *ref_slot[k]; }
                break;
            case 6:
                slot_whole(k);
                slot[k].reset();
                ref_slot[k].reset();
                break;
            case 7:                    // checkpoint into checkpoint
                slot_whole(j);
                slot[j] = slot[k];
                ref_slot[j] = ref_slot[k];
                break;
            case 8:                    // a write to a checkpoint, not to the live arena
                if (slot[k]) {
                    std::vector<int32_t> touched;
                    if (!random_op(r, *slot[k], *ref_slot[k], touched)) ++disagreements;
                    touched_everywhere(touched);
                }
                break;
            case 9:                    // swap (move construct + move assign)
                if (slot[k]) {
                    std::swap(live, *slot[k]);
                    std::swap(ref_live, *ref_slot[k]);
                }
                break;
            case 10: {                 // move out of a checkpoint, then drop it
                if (slot[k]) {
                    slot_whole(k);
                    live = std::move(*slot[k]);
                    ref_live = std::move(*ref_slot[k]);
                    slot[k].reset();
                    ref_slot[k].reset();
                }
                break;
            }
            default: {                 // self-assignment
                DrawingArena<Rec>& alias = live;
                live = alias;
                RefArena<Rec>& ref_alias = ref_live;
                ref_live = ref_alias;
                break;
            }
        }
    }
};

template <class Rec>
void run_differential(const char* kind, int cap, std::uint64_t seed, int steps, int full_every,
                      int ckpt_percent = 30, int burst = 1, int divisor = kStepDivisor) {
    Differential<Rec> d(cap, ckpt_percent, burst);
    Rng r{seed};
    steps /= divisor;
    full_every = full_every / divisor > 0 ? full_every / divisor : 1;
    for (int i = 0; i < steps; ++i) {
        d.step(r);
        if ((i + 1) % full_every == 0 && !d.all_same()) {
            ++d.disagreements;
            std::printf("  %s cap %d seed %llu: arenas diverged by step %d\n", kind, cap,
                        (unsigned long long)seed, i + 1);
            break;
        }
    }
    const bool final_same = d.all_same();
    std::printf("  %-11s cap %3d: %6d steps, %6d records, %3d full checks, %4d checkpoints "
                "checked whole, %d disagreements\n",
                kind, cap, steps, d.live.size(), d.full_checks, d.slot_checks, d.disagreements);
    CHECK(d.disagreements == 0);
    CHECK(final_same);
}

void test_random_differential() {
    std::printf("test_random_differential\n");
    run_differential<LineRec>("LineRec", 1, 11, 6000, 97);
    run_differential<LineRec>("LineRec", 3, 12, 6000, 97);
    run_differential<LineRec>("LineRec", 50, 13, 20000, 251);
    run_differential<LineRec>("LineRec", 500, 14, 20000, 251);
    run_differential<BoxRec>("BoxRec", 3, 21, 6000, 97);
    run_differential<BoxRec>("BoxRec", 50, 22, 20000, 251);
    run_differential<LabelRec>("LabelRec", 1, 31, 6000, 97);
    run_differential<LabelRec>("LabelRec", 100, 32, 20000, 251);
    run_differential<LinefillRec>("LinefillRec", 2, 41, 6000, 97);
    run_differential<LinefillRec>("LinefillRec", 50, 42, 20000, 251);
}

// Long runs whose arenas grow past three block levels (32 * 32 * 32 records),
// checkpointed and rolled back throughout (past two under AddressSanitizer).
void test_random_differential_deep() {
    std::printf("test_random_differential_deep\n");
    run_differential<LineRec>("LineRec", 100, 51, 60000, 6007, 4, 12, kDeepDivisor);
    run_differential<LabelRec>("LabelRec", 500, 52, 50000, 5003, 4, 12, kDeepDivisor);
}

// ---- 2. the scheduler's sequence --------------------------------------------
// One script execution of the synth-label-coof / coof-drawing-rollback shape:
// a var line whose x2 is bumped, a label deleted and re-created, and on some
// bars a burst that evicts (cap 5) and edits a dead record.
struct Script {
    Line var_line;
    Label label;
    std::vector<Label> burst;
};

template <class LA, class LB>
void execute(LA& lines, LB& labels, Script& s, int bar, int execution,
             void (*line_set_x2)(LA&, Line, int64_t), int64_t (*line_get_x2)(LA&, Line),
             void (*label_del)(LB&, Label), Label (*label_new)(LB&, int64_t, double, std::string),
             void (*label_set_text)(LB&, Label, std::string)) {
    line_set_x2(lines, s.var_line, line_get_x2(lines, s.var_line) + 1);
    label_del(labels, s.label);
    s.label = label_new(labels, bar, 100.0 + bar + execution, "k");
    if (bar % 7 == 3) {
        for (int i = 0; i < 4; ++i)
            s.burst.push_back(label_new(labels, bar, i, "burst " + std::to_string(execution)));
        if (s.burst.size() > 9) {
            // an evicted (dead) label stays writable
            label_set_text(labels, s.burst[s.burst.size() - 9], "dead edit " + std::to_string(bar));
            label_del(labels, s.burst[s.burst.size() - 5]);
        }
    }
}

void real_line_set_x2(DrawingArena<LineRec>& a, Line h, int64_t v) { pf_line_set_x2(a, h, v); }
int64_t real_line_get_x2(DrawingArena<LineRec>& a, Line h) { return pf_line_get_x2(a, h); }
void real_label_del(DrawingArena<LabelRec>& a, Label h) { pf_label_delete(a, h); }
Label real_label_new(DrawingArena<LabelRec>& a, int64_t x, double y, std::string t) {
    return pf_label_new(a, x, y, std::move(t));
}
void real_label_set_text(DrawingArena<LabelRec>& a, Label h, std::string t) { pf_label_set_text(a, h, std::move(t)); }

void ref_line_set_x2(RefArena<LineRec>& a, Line h, int64_t v) { ref_require(a, h).x2 = v; }
int64_t ref_line_get_x2(RefArena<LineRec>& a, Line h) { return ref_require(a, h).x2; }
void ref_label_del(RefArena<LabelRec>& a, Label h) { a.erase(h.id); }
Label ref_label_new(RefArena<LabelRec>& a, int64_t x, double y, std::string t) {
    return Label{a.alloc(LabelRec{x, y, XLoc::bar_index, YLoc::price, std::move(t), true})};
}
void ref_label_set_text(RefArena<LabelRec>& a, Label h, std::string t) { ref_require(a, h).text = std::move(t); }

void test_scheduler_sequence() {
    std::printf("test_scheduler_sequence\n");
    DrawingArena<LineRec> lines(50);
    DrawingArena<LabelRec> labels(5);
    RefArena<LineRec> ref_lines(50);
    RefArena<LabelRec> ref_labels(5);
    Script s, rs;
    s.var_line = pf_line_new(lines, 0, 0.0, 0, 0.0);
    rs.var_line = Line{ref_lines.alloc(LineRec{0, 0, 0.0, 0.0, XLoc::bar_index, false, false, true})};

    struct State { Script script; DrawingArena<LineRec> lines; DrawingArena<LabelRec> labels; };
    struct RefState { Script script; RefArena<LineRec> lines; RefArena<LabelRec> labels; };
    std::optional<State> ckpt;
    std::optional<RefState> ref_ckpt;
    auto snapshot = [&] {
        ckpt.emplace(State{s, lines, labels});
        ref_ckpt.emplace(RefState{rs, ref_lines, ref_labels});
    };
    auto restore = [&] {
        s = ckpt->script; lines = ckpt->lines; labels = ckpt->labels;
        rs = ref_ckpt->script; ref_lines = ref_ckpt->lines; ref_labels = ref_ckpt->labels;
    };
    auto run = [&](int bar, int execution) {
        execute(lines, labels, s, bar, execution, real_line_set_x2, real_line_get_x2, real_label_del,
                real_label_new, real_label_set_text);
        execute(ref_lines, ref_labels, rs, bar, execution, ref_line_set_x2, ref_line_get_x2,
                ref_label_del, ref_label_new, ref_label_set_text);
    };
    auto same_now = [&] {
        return same_arena(lines, ref_lines) && same_arena(labels, ref_labels)
            && s.label.id == rs.label.id && s.burst.size() == rs.burst.size()
            && same_arena(ckpt->lines, ref_ckpt->lines) && same_arena(ckpt->labels, ref_ckpt->labels);
    };

    // A new run's prepare_script_run: the checkpoint dropped, each arena
    // move-assigned a fresh (vector) arena over the one it shares blocks with.
    auto reset_run = [&] {
        ckpt.reset();
        ref_ckpt.reset();
        lines = DrawingArena<LineRec>{50};
        labels = DrawingArena<LabelRec>{5};
        ref_lines = RefArena<LineRec>{50};
        ref_labels = RefArena<LabelRec>{5};
        s = Script{};
        rs = Script{};
        s.var_line = pf_line_new(lines, 0, 0.0, 0, 0.0);
        rs.var_line = Line{ref_lines.alloc(LineRec{0, 0, 0.0, 0.0, XLoc::bar_index, false, false, true})};
    };

    int after_rollback = 0, after_commit = 0, recalcs = 0;
    bool ok = true;
    for (int bar = 0; bar < 400 && ok; ++bar) {
        if (bar == 150) {
            ok = ok && pf_line_get_x2(lines, s.var_line) == 150;
            reset_run();
        }
        snapshot();                                    // bar open
        const int pre_close = bar % 3 == 0 ? 2 : 0;    // fill recalculations before the close
        for (int f = 0; f < pre_close; ++f) {
            restore();
            run(bar, 1 + f);
            ++recalcs;
            restore();                                 // the recalculation rolls back
            ok = ok && same_now();
            ++after_rollback;
        }
        restore();                                     // before the close execution
        run(bar, 0);
        snapshot();                                    // commit
        ok = ok && same_now();
        ++after_commit;
        if (bar % 5 == 1) {                            // a post-close recalculation
            restore();
            run(bar, 9);
            ++recalcs;
            restore();
            ok = ok && same_now();
            ++after_rollback;
        }
    }
    std::printf("  400 bars with a run reset at bar 150, %d recalculations, %d rollbacks and %d "
                "commits checked, x2 = %lld\n", recalcs, after_rollback, after_commit,
                (long long)pf_line_get_x2(lines, s.var_line));
    CHECK(ok);
    // x2 counts the run's committed executions only, like the tape's `var` counter n.
    CHECK(pf_line_get_x2(lines, s.var_line) == 250);
    CHECK(same_arena(labels, ref_labels));
    CHECK(labels.order().size() <= 5u);
}

// ---- 3. writes to old records after a checkpoint ----------------------------
void test_deep_trie_writes_after_checkpoint() {
    std::printf("test_deep_trie_writes_after_checkpoint\n");
    DrawingArena<LineRec> built(100);
    RefArena<LineRec> ref(100);
    for (int i = 0; i < 40000; ++i) {
        pf_line_new(built, i, i * 0.5, i + 1, i * 0.25);
        ref.alloc(LineRec{i, i + 1, i * 0.5, i * 0.25, XLoc::bar_index, false, false, true});
    }
    // A copy is held in blocks; the checkpoint of it shares every block, so
    // the writes below go through blocks another copy still holds.
    DrawingArena<LineRec> a = built;
    const DrawingArena<LineRec> checkpoint = a;
    const RefArena<LineRec> ref_checkpoint = ref;
    // Write every 97th record, dead or live, across the whole trie.
    for (int id = 0; id < 40000; id += 97) {
        pf_line_set_y1(a, Line{id}, -1.0 * id);
        ref.at(id).y1 = -1.0 * id;
    }
    for (int i = 0; i < 77; ++i) {
        pf_line_new(a, -i, 0.0, i, 1.0);
        ref.alloc(LineRec{-i, i, 0.0, 1.0, XLoc::bar_index, false, false, true});
    }
    CHECK(same_arena(a, ref));
    CHECK(same_arena(checkpoint, ref_checkpoint));   // the checkpoint saw none of it
    a = checkpoint;                                   // roll back
    CHECK(same_arena(a, ref_checkpoint));
    CHECK(a.size() == 40000);
    CHECK(pf_line_get_y1(a, Line{97}) == 48.5);
    CHECK(!a.alive(0) && a.alive(39999));
}

// A moved-from arena is empty, keeps its cap and is usable again.
void test_moved_from_arena() {
    std::printf("test_moved_from_arena\n");
    DrawingArena<LabelRec> a(2);
    for (int i = 0; i < 70; ++i) pf_label_new(a, i, i, "t");
    DrawingArena<LabelRec> b = std::move(a);
    CHECK(b.size() == 70);
    CHECK(a.size() == 0);
    CHECK(a.order().empty());
    Label h = pf_label_new(a, 1, 2.0, "again");
    CHECK(h.id == 0);
    pf_label_new(a, 2, 3.0, "and");
    pf_label_new(a, 3, 4.0, "evicts");
    CHECK(!a.alive(0) && a.alive(1) && a.alive(2));  // cap 2 survived the move
    a = std::move(b);
    CHECK(a.size() == 70 && pf_label_get_text(a, Label{69}) == "t");
    // A self-move keeps the arena, whether its records are in a vector or in
    // blocks shared with a copy.
    const std::deque<int32_t> order = a.order();
    DrawingArena<LabelRec>& alias = a;
    a = std::move(alias);
    CHECK(a.size() == 70 && a.order() == order && pf_label_get_text(a, Label{69}) == "t");
    DrawingArena<LabelRec> blocks = a;
    const DrawingArena<LabelRec> sharing = blocks;
    DrawingArena<LabelRec>& blocks_alias = blocks;
    blocks = std::move(blocks_alias);
    CHECK(blocks.size() == 70 && blocks.order() == order && sharing.size() == 70);
    CHECK(pf_label_get_text(blocks, Label{3}) == "t");
}

// ---- 4. cost: records copied by checkpointing -------------------------------
// A record kind whose copies are counted; moves are not copies.
struct CountedRec {
    int64_t x = 0;
    double y = 0.0;
    bool alive = false;
    static inline std::uint64_t copies = 0;

    CountedRec() = default;
    CountedRec(int64_t x_, double y_) : x(x_), y(y_) {}
    CountedRec(const CountedRec& o) : x(o.x), y(o.y), alive(o.alive) { ++copies; }
    CountedRec& operator=(const CountedRec& o) {
        x = o.x; y = o.y; alive = o.alive;
        ++copies;
        return *this;
    }
    CountedRec(CountedRec&&) noexcept = default;
    CountedRec& operator=(CountedRec&&) noexcept = default;
};

// Bytes the recording window asks for, freed or not.
std::size_t g_requested_bytes = 0;
void count_requested(std::size_t size) { g_requested_bytes += size; }

struct HeapCost {
    std::size_t allocations = 0;
    std::size_t bytes = 0;
};

template <class F>
HeapCost heap_cost(F&& f) {
    const std::size_t before = global_allocation::allocations;
    g_requested_bytes = 0;
    global_allocation::on_recorded = count_requested;
    global_allocation::recording = true;
    f();
    global_allocation::recording = false;
    global_allocation::on_recorded = nullptr;
    return {global_allocation::allocations - before, g_requested_bytes};
}

// An unchanged arena's checkpoint copies no record, and what it allocates --
// order_'s copy, at most cap ids -- does not grow with the records it holds.
// The first copy of an arena (still a vector) builds the checkpoint's blocks
// and copies each record once; the restore then shares them.
struct UnchangedCost {
    std::uint64_t first_copies = 0;
    std::uint64_t copies = 0;
    HeapCost heap;
};

UnchangedCost unchanged_checkpoint(int records) {
    DrawingArena<CountedRec> live(100);
    for (int i = 0; i < records; ++i) live.alloc(CountedRec{i, 1.0 * i});
    std::optional<DrawingArena<CountedRec>> checkpoint;
    UnchangedCost cost;
    CountedRec::copies = 0;
    checkpoint.emplace(live);   // the first bar: snapshot, restore, commit
    live = *checkpoint;
    checkpoint.emplace(live);
    cost.first_copies = CountedRec::copies;
    CountedRec::copies = 0;
    cost.heap = heap_cost([&] {
        for (int bar = 0; bar < 1000; ++bar) {
            checkpoint.emplace(live);   // snapshot
            live = *checkpoint;         // restore
            checkpoint.emplace(live);   // commit
        }
    });
    cost.copies = CountedRec::copies;
    return cost;
}

void test_unchanged_checkpoint_copies_nothing() {
    std::printf("test_unchanged_checkpoint_copies_nothing\n");
    const UnchangedCost small = unchanged_checkpoint(10000);
    const UnchangedCost large = unchanged_checkpoint(40000);
    std::printf("  the first bar copies %llu of 10,000 records and %llu of 40,000; then 1,000 bars "
                "of snapshot/restore/commit: 10,000 records -> %llu record copies, %zu allocations, "
                "%zu bytes; 40,000 records -> %llu, %zu, %zu\n",
                (unsigned long long)small.first_copies, (unsigned long long)large.first_copies,
                (unsigned long long)small.copies, small.heap.allocations, small.heap.bytes,
                (unsigned long long)large.copies, large.heap.allocations, large.heap.bytes);
    CHECK(small.first_copies <= 10000u);
    CHECK(large.first_copies <= 40000u);
    CHECK(small.copies == 0);
    CHECK(large.copies == 0);
    CHECK(large.heap.allocations == small.heap.allocations);
    CHECK(large.heap.bytes == small.heap.bytes);
}

// ---- 5. a refused allocation ------------------------------------------------
// Every id in order_ is alive, ascending and at most cap of them, and every
// live record is in order_: what eviction and *.all rely on.
template <class Rec>
bool consistent(const DrawingArena<Rec>& a, std::size_t cap) {
    const auto& order = a.order();
    if (order.size() > cap) return false;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (!a.alive(order[i])) return false;
        if (i > 0 && order[i - 1] >= order[i]) return false;
    }
    std::size_t live = 0;
    for (int32_t id = 0; id < a.size(); ++id) live += a.alive(id) ? 1 : 0;
    return live == order.size();
}

// Arenas at cap whose every block a checkpoint shares, so each write needs a
// copy first; with every allocation refused, each operation that allocates
// throws std::bad_alloc. The arena must stay consistent and the checkpoint
// must not change. (A copy holds its records in blocks, and the checkpoint of
// that copy shares them.)
void test_refused_allocation_keeps_arena_consistent() {
    std::printf("test_refused_allocation_keeps_arena_consistent\n");
    int attempts = 0, refusals = 0, failures = 0;
    for (int n = 1; n <= 140; ++n) {
        for (int op = 0; op < 5; ++op) {
            DrawingArena<LineRec> built(7);
            RefArena<LineRec> ref(7);
            for (int i = 0; i < n; ++i) {
                pf_line_new(built, i, 0.5 * i, i + 1, 1.0);
                ref.alloc(LineRec{i, i + 1, 0.5 * i, 1.0, XLoc::bar_index, false, false, true});
            }
            DrawingArena<LineRec> a = built;
            const DrawingArena<LineRec> checkpoint = a;
            bool threw = false;
            global_allocation::refusing = true;
            try {
                switch (op) {
                    case 0: pf_line_new(a, -1, 0.0, 1, 1.0); break;   // evicts at cap
                    case 1: pf_line_delete(a, Line{n - 1}); break;
                    case 2: pf_line_set_y1(a, Line{0}, 9.0); break;   // a dead record's block
                    case 3: { DrawingArena<LineRec> c = a; (void)c; break; }
                    default: pf_line_copy(a, Line{n - 1}); break;
                }
            } catch (const std::bad_alloc&) {
                threw = true;
            }
            global_allocation::refusing = false;
            ++attempts;
            refusals += threw ? 1 : 0;
            if (!consistent(a, 7) || !same_arena(checkpoint, ref)) ++failures;
        }
    }
    std::printf("  %d refused operations, %d threw std::bad_alloc, %d left an inconsistent arena "
                "or a changed checkpoint\n", attempts, refusals, failures);
    CHECK(failures == 0);
    CHECK(refusals > 0);
}

// A refusal partway through copying a shared block of labels, whose texts
// outgrow every small-string buffer so each record copy allocates: the copy
// has built some records when an allocation fails. It must destroy exactly
// those (AddressSanitizer's leak check sees any it strands), leave the writing
// arena and its checkpoint as they were, and let a later write through.
std::size_t g_allowed = 0;
void refuse_when_spent(std::size_t) {
    if (g_allowed > 0 && --g_allowed == 0) global_allocation::refusing = true;
}

void test_refused_block_copy_unwinds() {
    std::printf("test_refused_block_copy_unwinds\n");
    const std::string text(64, 'q');
    int attempts = 0, refusals = 0, failures = 0;
    for (std::size_t allowed = 0; allowed <= 40; ++allowed) {
        DrawingArena<LabelRec> built(100);
        RefArena<LabelRec> ref(100);
        for (int i = 0; i < 70; ++i) {   // blocks 0 and 1 in the tree, 6 in the tail
            pf_label_new(built, i, i, text + std::to_string(i));
            ref.alloc(LabelRec{i, 1.0 * i, XLoc::bar_index, YLoc::price, text + std::to_string(i), true});
        }
        DrawingArena<LabelRec> a = built;
        const DrawingArena<LabelRec> checkpoint = a;
        std::string edit = text + "edited";
        bool threw = false;
        g_allowed = allowed;
        global_allocation::on_recorded = refuse_when_spent;
        global_allocation::recording = true;
        global_allocation::refusing = allowed == 0;
        try {
            pf_label_set_text(a, Label{5}, std::move(edit));   // a record of block 0
        } catch (const std::bad_alloc&) {
            threw = true;
        }
        global_allocation::refusing = false;
        global_allocation::recording = false;
        global_allocation::on_recorded = nullptr;
        ++attempts;
        refusals += threw ? 1 : 0;
        if (!same_arena(checkpoint, ref)) ++failures;
        RefArena<LabelRec> want = ref;
        if (!threw) want.at(5).text = text + "edited";
        if (!same_arena(a, want)) ++failures;
        pf_label_set_text(a, Label{6}, "after");
        want.at(6).text = "after";
        if (!same_arena(a, want) || !same_arena(checkpoint, ref)) ++failures;
    }
    std::printf("  %d writes into a shared block of 32 long labels, %d refused at each allocation "
                "in turn, %d left an arena or its checkpoint changed\n", attempts, refusals, failures);
    CHECK(failures == 0);
    CHECK(refusals >= 34);   // the node, the block and each of its 32 records
    CHECK(refusals < attempts);
}

// synth-label-coof's body, `label.delete(live); live := label.new(...)`, under
// the scheduler's checkpoint sequence with a fill recalculation every fifth
// bar. Returns the records copied over the run; `executions` counts the
// script executions.
struct ChurnCost {
    std::uint64_t copies = 0;
    int executions = 0;
    int checkpoint_steps = 0;
    HeapCost heap;   // allocated by the checkpoint steps alone
};

ChurnCost label_churn(int bars) {
    DrawingArena<CountedRec> arena(100);
    int32_t live = -1;
    struct State { int32_t live; DrawingArena<CountedRec> arena; };
    std::optional<State> checkpoint;
    ChurnCost cost;
    auto body = [&](int bar) {
        arena.erase(live);
        live = arena.alloc(CountedRec{bar, 1.0 * bar});
        ++cost.executions;
    };
    auto step = [&](auto&& f) {
        const HeapCost h = heap_cost(f);
        cost.heap.allocations += h.allocations;
        cost.heap.bytes += h.bytes;
        ++cost.checkpoint_steps;
    };
    auto snapshot = [&] { step([&] { checkpoint.emplace(State{live, arena}); }); };
    auto restore = [&] { step([&] { live = checkpoint->live; arena = checkpoint->arena; }); };
    CountedRec::copies = 0;
    for (int bar = 0; bar < bars; ++bar) {
        snapshot();                                 // bar open
        if (bar % 5 == 0) { restore(); body(bar); restore(); }
        restore();
        body(bar);
        snapshot();                                 // commit
        if (bar % 5 == 2) { restore(); body(bar); restore(); }
    }
    cost.copies = CountedRec::copies;
    return cost;
}

// The bound: a script execution writes records in at most two blocks of 32
// that a checkpoint shares (the block of the record it deletes and the newest
// block it appends to), so checkpointing copies at most 64 records per
// execution -- independent of how many records the run has created -- and
// doubling the bars at most doubles the copies (ratio <= 2.25 with slack).
// The vector arena copied every record at every checkpoint: 3 * N^2 / 2
// records for N bars, a ratio of 4.
// The same bound holds for what the checkpoint steps allocate: order_'s copy
// (at most cap ids) and nothing that grows with the records, so N -> 2N at
// most doubles it. The vector arena allocated every record at every step.
void test_checkpoint_cost_is_linear() {
    std::printf("test_checkpoint_cost_is_linear\n");
    const int n = 3000 / kCostDivisor;
    const ChurnCost a = label_churn(n);
    const ChurnCost b = label_churn(2 * n);
    const double ratio = a.copies ? static_cast<double>(b.copies) / static_cast<double>(a.copies) : 0.0;
    const double byte_ratio = a.heap.bytes
        ? static_cast<double>(b.heap.bytes) / static_cast<double>(a.heap.bytes) : 0.0;
    std::printf("  %d bars: %llu record copies over %d executions; %d bars: %llu over %d; ratio %.3f\n",
                n, (unsigned long long)a.copies, a.executions, 2 * n, (unsigned long long)b.copies,
                b.executions, ratio);
    std::printf("  checkpoint steps allocate %zu blocks / %zu bytes over %d steps, then %zu / %zu over "
                "%d; byte ratio %.3f\n", a.heap.allocations, a.heap.bytes, a.checkpoint_steps,
                b.heap.allocations, b.heap.bytes, b.checkpoint_steps, byte_ratio);
    CHECK(a.copies <= 64ULL * static_cast<std::uint64_t>(a.executions));
    CHECK(b.copies <= 64ULL * static_cast<std::uint64_t>(b.executions));
    CHECK(a.copies > 0 && ratio <= 2.25);
    CHECK(a.heap.bytes > 0 && byte_ratio <= 2.25);
    // Each step copies one order_ of at most 100 ids: a few blocks, never the records.
    CHECK(b.heap.bytes <= 8192u * static_cast<std::size_t>(b.checkpoint_steps));
}

}  // namespace

int main() {
    test_random_differential();
    test_random_differential_deep();
    test_scheduler_sequence();
    test_deep_trie_writes_after_checkpoint();
    test_moved_from_arena();
    test_unchanged_checkpoint_copies_nothing();
    test_checkpoint_cost_is_linear();
    test_refused_allocation_keeps_arena_consistent();
    test_refused_block_copy_unwinds();
    std::printf("\ndrawing checkpoint tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
