#pragma once
//
// PineForge drawing-objects-as-data runtime.
//
// Spec: docs/drawing-objects-as-data.md §3 ("C++ Runtime").
//
// This is a HEADLESS backtester: drawing objects (line/box/label/linefill) are
// pure DATA the strategy can create, mutate and read back into trading logic.
// There is NO rendering, NO graphics — only geometry. The header is fully
// self-contained and header-only (every function is `inline`); codegen emits
// calls to the `pf_*` free functions below. Nothing here is exported via the C
// ABI — these are codegen-emitted in-process calls, not `c_abi.cpp` symbols.
//
// The header is gated into the generated include block behind `self._uses_drawing`
// exactly like `matrix.hpp` is gated behind `_uses_matrix`. The arenas are members
// of the generated strategy subclass, so reset-per-run is automatic (a fresh
// strategy instance is constructed per backtest).
//
#include <vector>
#include <deque>
#include <string>
#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <cmath>
#include <utility>
#include <pineforge/na.hpp>

namespace pineforge {

// ---- enums (spec §3.2) -----------------------------------------------------
enum class XLoc : int { bar_index = 0, bar_time = 1 };           // default bar_index
enum class YLoc : int { price = 0, abovebar = 1, belowbar = 2 }; // default price

// ---- value-view handles (spec §3.2) ----------------------------------------
// Stored in vars / array<T> / UDT fields. id < 0 == na.
struct Line     { int32_t id = -1; };
struct Box      { int32_t id = -1; };
struct Label    { int32_t id = -1; };
struct Linefill { int32_t id = -1; };

inline bool is_na(const Line& h)     { return h.id < 0; }
inline bool is_na(const Box& h)      { return h.id < 0; }
inline bool is_na(const Label& h)    { return h.id < 0; }
inline bool is_na(const Linefill& h) { return h.id < 0; }

// ---- pure value type: chart.point (spec §3.2) ------------------------------
// No arena, no identity, no delete. Lowered inline as aggregate literals by
// codegen; the engine only needs the struct + its is_na overload.
struct ChartPoint {
    int64_t index = na<int64_t>();   // x-coord as bar index (na sentinel INT64_MIN)
    int64_t time  = na<int64_t>();   // x-coord as UNIX ms
    double  price = na<double>();    // y-coord (NaN sentinel)
};
inline bool is_na(const ChartPoint& p) {
    return is_na(p.index) && is_na(p.time) && is_na(p.price);
}

// ---- arena records (spec §3.3) ---------------------------------------------
// Geometry only — every visual field (color/style/width/...) is dropped at
// lowering. x-coords are int64_t (xloc.bar_time stores UNIX-ms, overflows
// int32; na sentinel INT64_MIN); y-coords/prices are double (NaN sentinel).
// NOTE: BoxRec has NO extend and NO text fields (unlike LineRec / LabelRec).
struct LineRec     { int64_t x1, x2; double y1, y2; XLoc xloc; bool ext_l, ext_r; bool alive; };
struct BoxRec      { int64_t left, right; double top, bottom; XLoc xloc; bool alive; };
struct LabelRec    { int64_t x; double y; XLoc xloc; YLoc yloc; std::string text; bool alive; };
struct LinefillRec { int32_t line1, line2; bool alive; };

// ---- error type (spec §3.7) ------------------------------------------------
// Derives from std::runtime_error so BacktestEngine::run()'s existing
// `catch (const std::exception& e)` (src/engine_run.cpp) halts the backtest
// exactly like TradingView when a drawing op touches a dead/na handle.
// Drawing is ALWAYS-ON and ALWAYS TV-faithful: no compile flag, no lenient mode.
struct pine_drawing_error : std::runtime_error {
    explicit pine_drawing_error(const std::string& msg) : std::runtime_error(msg) {}
};

// dead/na handle access is ALWAYS an error — no flag, no lenient mode (spec §3.7)
#define PF_DRAW_DEAD(arena, handle) throw pine_drawing_error("drawing access on na/deleted handle")

// ---- arena template (spec §3.4) --------------------------------------------
// ids are monotonic and never reused (dead slots are tombstoned, not recycled)
// -> no ABA, reproducible across runs. Exact-cap FIFO eviction (oldest first).
template <class Rec>
class DrawingArena {
    std::vector<Rec>     recs_;   // index == id; dead slots retained so ids are stable & monotonic
    std::deque<int32_t>  order_;  // live ids, oldest -> newest == the *.all order; drives FIFO eviction
    int                  cap_;
public:
    explicit DrawingArena(int cap = 50) : cap_(std::max(1, cap)) {}

    int32_t alloc(Rec r) {
        while ((int)order_.size() >= cap_) {        // exact-cap FIFO eviction (oldest first)
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

    void erase(int32_t id) {                         // delete(): silent no-op on na/dead
        if (!alive(id)) return;
        recs_[id].alive = false;
        order_.erase(std::find(order_.begin(), order_.end(), id));
    }

    bool alive(int32_t id) const { return id >= 0 && id < (int)recs_.size() && recs_[id].alive; }

    Rec&       at(int32_t id)       { return recs_[id]; }
    const Rec& at(int32_t id) const { return recs_[id]; }

    const std::deque<int32_t>& order() const { return order_; }  // backs line.all/box.all if ever needed
};

// ---- shared helpers --------------------------------------------------------
// Returns a reference to the live record for a handle, or THROWS pine_drawing_error
// (spec §3.7: any getter/setter on a dead/na handle halts, unconditionally).
template <class Rec, class Handle>
inline Rec& pf_require_live(DrawingArena<Rec>& a, Handle h) {
    if (!a.alive(h.id)) throw pine_drawing_error("drawing access on na/deleted handle");
    return a.at(h.id);
}

// Selects the x-coordinate a ChartPoint contributes for a given xloc.
inline int64_t pf_point_x(const ChartPoint& p, XLoc xloc) {
    return (xloc == XLoc::bar_time) ? p.time : p.index;
}

// ============================ line ==========================================
inline Line pf_line_new(DrawingArena<LineRec>& a, int64_t x1, double y1, int64_t x2, double y2,
                        XLoc xloc = XLoc::bar_index, bool el = false, bool er = false) {
    // *_new(na,na,na,na) allocates a LIVE record with na coords (NOT a na handle).
    return Line{ a.alloc(LineRec{ x1, x2, y1, y2, xloc, el, er, true }) };
}
inline Line pf_line_new_pts(DrawingArena<LineRec>& a, ChartPoint p1, ChartPoint p2,
                            XLoc xloc = XLoc::bar_index) {
    return Line{ a.alloc(LineRec{ pf_point_x(p1, xloc), pf_point_x(p2, xloc),
                                  p1.price, p2.price, xloc, false, false, true }) };
}

inline int64_t pf_line_get_x1(DrawingArena<LineRec>& a, Line h) { return pf_require_live(a, h).x1; }
inline int64_t pf_line_get_x2(DrawingArena<LineRec>& a, Line h) { return pf_require_live(a, h).x2; }
inline double  pf_line_get_y1(DrawingArena<LineRec>& a, Line h) { return pf_require_live(a, h).y1; }
inline double  pf_line_get_y2(DrawingArena<LineRec>& a, Line h) { return pf_require_live(a, h).y2; }

inline void pf_line_set_x1(DrawingArena<LineRec>& a, Line h, int64_t v) { pf_require_live(a, h).x1 = v; }
inline void pf_line_set_x2(DrawingArena<LineRec>& a, Line h, int64_t v) { pf_require_live(a, h).x2 = v; }
inline void pf_line_set_y1(DrawingArena<LineRec>& a, Line h, double v)  { pf_require_live(a, h).y1 = v; }
inline void pf_line_set_y2(DrawingArena<LineRec>& a, Line h, double v)  { pf_require_live(a, h).y2 = v; }
inline void pf_line_set_xy1(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    LineRec& r = pf_require_live(a, h); r.x1 = x; r.y1 = y;
}
inline void pf_line_set_xy2(DrawingArena<LineRec>& a, Line h, int64_t x, double y) {
    LineRec& r = pf_require_live(a, h); r.x2 = x; r.y2 = y;
}
inline void pf_line_set_first_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    LineRec& r = pf_require_live(a, h); r.x1 = pf_point_x(p, r.xloc); r.y1 = p.price;
}
inline void pf_line_set_second_point(DrawingArena<LineRec>& a, Line h, ChartPoint p) {
    LineRec& r = pf_require_live(a, h); r.x2 = pf_point_x(p, r.xloc); r.y2 = p.price;
}
inline void pf_line_set_xloc(DrawingArena<LineRec>& a, Line h, int64_t x1, int64_t x2, XLoc xloc) {
    // REINTERPRET: stores new numbers + flag; does NOT convert coord spaces (TV-faithful, spec §3.9).
    LineRec& r = pf_require_live(a, h); r.x1 = x1; r.x2 = x2; r.xloc = xloc;
}

// real computation: infinite line, ignores extend; valid for xloc.bar_index (spec §3.6).
inline double pf_line_get_price(DrawingArena<LineRec>& a, Line h, int64_t x) {
    if (!a.alive(h.id)) PF_DRAW_DEAD(a, h);             // dead/na line -> throw -> halt (like TV)
    const LineRec& r = a.at(h.id);
    if (r.xloc == XLoc::bar_time)
        throw pine_drawing_error("line.get_price requires xloc.bar_index"); // TV errors on bar_time lines
    if (r.x1 == r.x2) return na<double>();              // degenerate vertical -> na (matches TV)
    return r.y1 + (r.y2 - r.y1) / (double)(r.x2 - r.x1) * (double)(x - r.x1); // infinite line, ignores extend
}

inline Line pf_line_copy(DrawingArena<LineRec>& a, Line h) {
    if (!a.alive(h.id)) return Line{};                 // copy of na/dead -> na handle (silent)
    LineRec r = a.at(h.id);                             // deep copy = independent new arena record
    return Line{ a.alloc(r) };
}
inline void pf_line_delete(DrawingArena<LineRec>& a, Line h) { a.erase(h.id); } // silent no-op on na/dead

// ============================ box ===========================================
inline Box pf_box_new(DrawingArena<BoxRec>& a, int64_t left, double top, int64_t right, double bottom,
                      XLoc xloc = XLoc::bar_index) {
    return Box{ a.alloc(BoxRec{ left, right, top, bottom, xloc, true }) };
}
inline Box pf_box_new_pts(DrawingArena<BoxRec>& a, ChartPoint top_left, ChartPoint bottom_right,
                          XLoc xloc = XLoc::bar_index) {
    return Box{ a.alloc(BoxRec{ pf_point_x(top_left, xloc), pf_point_x(bottom_right, xloc),
                                top_left.price, bottom_right.price, xloc, true }) };
}

inline int64_t pf_box_get_left  (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(a, h).left; }
inline int64_t pf_box_get_right (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(a, h).right; }
inline double  pf_box_get_top   (DrawingArena<BoxRec>& a, Box h) { return pf_require_live(a, h).top; }
inline double  pf_box_get_bottom(DrawingArena<BoxRec>& a, Box h) { return pf_require_live(a, h).bottom; }

inline void pf_box_set_left  (DrawingArena<BoxRec>& a, Box h, int64_t v) { pf_require_live(a, h).left = v; }
inline void pf_box_set_right (DrawingArena<BoxRec>& a, Box h, int64_t v) { pf_require_live(a, h).right = v; }
inline void pf_box_set_top   (DrawingArena<BoxRec>& a, Box h, double v)  { pf_require_live(a, h).top = v; }
inline void pf_box_set_bottom(DrawingArena<BoxRec>& a, Box h, double v)  { pf_require_live(a, h).bottom = v; }
inline void pf_box_set_lefttop(DrawingArena<BoxRec>& a, Box h, int64_t left, double top) {
    BoxRec& r = pf_require_live(a, h); r.left = left; r.top = top;
}
inline void pf_box_set_rightbottom(DrawingArena<BoxRec>& a, Box h, int64_t right, double bottom) {
    BoxRec& r = pf_require_live(a, h); r.right = right; r.bottom = bottom;
}
inline void pf_box_set_top_left_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    BoxRec& r = pf_require_live(a, h); r.left = pf_point_x(p, r.xloc); r.top = p.price;
}
inline void pf_box_set_bottom_right_point(DrawingArena<BoxRec>& a, Box h, ChartPoint p) {
    BoxRec& r = pf_require_live(a, h); r.right = pf_point_x(p, r.xloc); r.bottom = p.price;
}
inline void pf_box_set_xloc(DrawingArena<BoxRec>& a, Box h, int64_t left, int64_t right, XLoc xloc) {
    BoxRec& r = pf_require_live(a, h); r.left = left; r.right = right; r.xloc = xloc;  // reinterpret only
}

inline Box pf_box_copy(DrawingArena<BoxRec>& a, Box h) {
    if (!a.alive(h.id)) return Box{};
    BoxRec r = a.at(h.id);
    return Box{ a.alloc(r) };
}
inline void pf_box_delete(DrawingArena<BoxRec>& a, Box h) { a.erase(h.id); }

// ============================ label =========================================
// NOTE: yloc.abovebar / yloc.belowbar should auto-place y at the bar high/low
// (spec §3.9). The spec's pf_label_new signature is intentionally decoupled from
// the engine and does NOT receive bar high/low, so y is stored AS-IS and yloc is
// store-only here. yloc abovebar/belowbar auto-place needs bar high/low at the
// call site — codegen would pass close()/high()/low() if ever required. Inert:
// no corpus label reads get_y back (spec note 4), so no parity claim.
inline Label pf_label_new(DrawingArena<LabelRec>& a, int64_t x, double y, std::string text,
                          XLoc xloc = XLoc::bar_index, YLoc yloc = YLoc::price) {
    return Label{ a.alloc(LabelRec{ x, y, xloc, yloc, std::move(text), true }) };
}
inline Label pf_label_new_pt(DrawingArena<LabelRec>& a, ChartPoint point, std::string text,
                             YLoc yloc = YLoc::price) {
    // xloc carried by the ChartPoint: bar_time only when index is na but time is set.
    XLoc xloc = (is_na(point.index) && !is_na(point.time)) ? XLoc::bar_time : XLoc::bar_index;
    return Label{ a.alloc(LabelRec{ pf_point_x(point, xloc), point.price, xloc, yloc,
                                    std::move(text), true }) };
}

inline int64_t     pf_label_get_x(DrawingArena<LabelRec>& a, Label h)    { return pf_require_live(a, h).x; }
inline double      pf_label_get_y(DrawingArena<LabelRec>& a, Label h)    { return pf_require_live(a, h).y; }
inline std::string pf_label_get_text(DrawingArena<LabelRec>& a, Label h) { return pf_require_live(a, h).text; }

inline void pf_label_set_x(DrawingArena<LabelRec>& a, Label h, int64_t v) { pf_require_live(a, h).x = v; }
inline void pf_label_set_y(DrawingArena<LabelRec>& a, Label h, double v)  { pf_require_live(a, h).y = v; }
inline void pf_label_set_xy(DrawingArena<LabelRec>& a, Label h, int64_t x, double y) {
    LabelRec& r = pf_require_live(a, h); r.x = x; r.y = y;
}
inline void pf_label_set_point(DrawingArena<LabelRec>& a, Label h, ChartPoint p) {
    LabelRec& r = pf_require_live(a, h); r.x = pf_point_x(p, r.xloc); r.y = p.price;
}
inline void pf_label_set_xloc(DrawingArena<LabelRec>& a, Label h, int64_t x, XLoc xloc) {
    LabelRec& r = pf_require_live(a, h); r.x = x; r.xloc = xloc;  // sets x AND xloc
}
inline void pf_label_set_yloc(DrawingArena<LabelRec>& a, Label h, YLoc yloc) {
    pf_require_live(a, h).yloc = yloc;
}
inline void pf_label_set_text(DrawingArena<LabelRec>& a, Label h, std::string text) {
    pf_require_live(a, h).text = std::move(text);
}

inline Label pf_label_copy(DrawingArena<LabelRec>& a, Label h) {
    if (!a.alive(h.id)) return Label{};
    LabelRec r = a.at(h.id);                            // deep copy (incl. std::string text)
    return Label{ a.alloc(std::move(r)) };
}
inline void pf_label_delete(DrawingArena<LabelRec>& a, Label h) { a.erase(h.id); }

// ============================ linefill ======================================
inline Linefill pf_linefill_new(DrawingArena<LinefillRec>& a, Line l1, Line l2) {
    // Stores the two Line handle ids inertly (color dropped). linefill.new(na,na,na)
    // is legal -> a LIVE record over na line ids (spec note 1).
    return Linefill{ a.alloc(LinefillRec{ l1.id, l2.id, true }) };
}
inline Line pf_linefill_get_line1(DrawingArena<LinefillRec>& a, Linefill h) {
    return Line{ pf_require_live(a, h).line1 };
}
inline Line pf_linefill_get_line2(DrawingArena<LinefillRec>& a, Linefill h) {
    return Line{ pf_require_live(a, h).line2 };
}
inline void pf_linefill_delete(DrawingArena<LinefillRec>& a, Linefill h) { a.erase(h.id); }

// ============================ visual no-op sink (spec §3.6) ==================
// Evaluates + discards args (so side effects/typechecking still happen), returns void.
// Backs every VISUAL setter/ctor-kwarg: line/box/label set_color/set_style/...
template <class... A> inline void pf_noop(A&&...) {}

} // namespace pineforge
