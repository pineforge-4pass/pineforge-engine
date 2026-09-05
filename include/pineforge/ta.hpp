#pragma once
#include "na.hpp"
#include "series.hpp"
#include "window_sum.hpp"
#include <deque>
#include <cmath>
#include <vector>
#include <string>

namespace pineforge {

namespace ta {

// Opt-in TradingView-built-in EMA warmup toggle (thread-local, default false).
//
// TradingView's *built-in* ``ta.ema`` returns ``na`` until ``length`` values
// have accumulated, then seeds with the SMA of those first ``length`` values —
// unlike the documented ``pine_ema`` reference impl (and this engine's default),
// which seed ``ema := src`` on the first bar and are therefore never ``na``.
// The difference matters for range-start-truncated chart and
// ``request.security`` series (KI-55): the relevant ``ta.ema`` must be ``na``
// for its whole warmup window to match TV, exactly as ``ta.rma`` / ``ta.sma``
// already are.
//
// An ``EMA`` instance latches this flag on its first ``compute()`` and keeps
// that mode for life. The engine scopes the flag independently around chart
// ``on_bar`` dispatch (``chart_ema_na_warmup``) and request.security evaluation
// (``security_range_start_na_warmup``), so either context can opt in without
// contaminating the other. When neither run flag is set, every EMA is
// byte-identical to the prior src-seed behavior.
bool& ema_na_warmup_flag();

// Per-context bar index for bar-addressed window state (thread-local).
//
// TradingView keeps the source history of a window call site (``ta.highest``,
// ``ta.lowest``, ``ta.highestbars``, ``ta.lowestbars``) in a ring of
// ``K = length + 1`` slots addressed by ``bar_index % K``. A call on bar ``b``
// writes ``slot[b % K] = src`` and returns the extremum over the WRITTEN slots
// ``(b - k) % K``, ``k in [0, length)``. For a call that executes every bar
// that is the plain positional window. For a call that executes
// CONDITIONALLY (inside an ``if`` block that does not run every bar) the slots
// not rewritten keep their stale values from earlier executions, never-written
// slots are skipped, and the result is na iff ``bar_index < length - 1`` — NOT
// until ``length`` executions have accumulated. Pinned 2026-09-03 with
// ``lab tv`` on NYSE:F 1D (``if bar_index % 7 == 3: v = ta.highest(high, 5)``
// 39/39 entry sizes exact, cadence 9 30/30; production: colasbreugnon-sma
// F@1D 31/31 only with K = 6). ``ta.sma`` inside ``if`` is NOT ring-addressed
// (per-execution buffer, pinned separately) and is untouched.
//
// The engine installs the executing context's bar index around every dispatch
// of generated code: the chart's Pine ``bar_index`` around ``on_bar`` and the
// requested context's evaluated-bar index around each ``evaluate_security``
// (HTF / lower-tf / auxiliary feeds). ``origin`` is the first bar index the
// context has data for — the warmup reference — so a feed that starts after
// TradingView's hidden first chart bar (``bar_index_offset``) still warms up
// over its own first ``length`` bars, exactly as before. With no context
// installed (the default — unit tests, standalone use) every ``compute()`` is
// its own bar and the objects are byte-identical to the previous positional
// deque.
//
// The ring is read through a cached extremum with an implied bar. Each call
// site keeps ``{value, bar}``: the first call caches ``(src, b)``; a call on
// bar ``b`` with ``b - bar < length`` only compares ``src`` against the cache
// (strict — ties never displace the cached member); the new-extremum test
// comes FIRST, so a bar whose ``src`` beats the cache takes it without
// reading the slots even when the cache has aged out; otherwise, once the
// cached extremum has aged out (``b - bar >= length`` — by bars, not calls)
// the slots are rescanned, oldest first, and the cache becomes
// ``(best, b - k_best)``; otherwise the cache is kept. The output is the
// cached value (the *Bars offset is ``b - bar``). Pinned 2026-09-04 with 8
// ``lab tv`` tapes on NYSE:F 1D (696/696 entries), the BINANCE:ETHUSDT.P
// geometry pin (82/82) and the jayentriken BBWP ETH replay (593/593):
// ``m = bar_index % 49; if m < 4 or 37 <= m < 47 or m == 48: v =
// ta.lowest(low, 10)`` answers 9.88 (cached from bar 42) on bars 48..51
// although bar 3's stale 9.20 sits one slot behind bar 48 — the plain ring
// would say 9.20 — and rescans to 9.20 on bar 52 once the cache has aged;
// on ETH the run-start bar 4990 (low 2648.62 < the expired cache 2650.47)
// answers its own low and never reads the aliased 08:15 slot the plain ring
// returned. For an every-bar caller the cache is exactly the positional
// window, so nothing changes there.
//
// Never-written slots read 0 in a rescan, for every source kind (pinned
// 2026-09-04 on BINANCE:BTCUSDT 1D and OANDA:XAUUSD 1D, cadence 13 / length
// 10: ``low``, ``close - 2000``, ``sma(close, 5)``, ``low[1]``, ``hl2`` and
// the sign-mixed ``sma(close, 5) - close`` each 13/13). A slot WRITTEN with
// na POISONS the ring (pinned 2026-09-04 on OANDA:XAUUSD 1D, scratchpad
// r6/pins/ringkind: leading-na sites 7 x 13/13, the market-logic replica
// 32/32; and the every-bar NYSE:F 1D gap tape pin-lowest-na-everybar, 20/20;
// ledger log-20260904t112527z-ea13dfcd): the rescan walks oldest first and a
// written-na slot resets the running extremum, so the answer is the extremum
// over the slots NEWER than the newest na slot in the window (never-written
// 0 slots among them included) and na when the newest slot is itself na; the
// na write also poisons the cache to ``(na, b)``, so the next valid call
// restarts from its own src without reading the slots. ``ta.lowest(
// sma(close, 14), 10)`` at cadence 13 answers na (bar 5 writes na), 2998
// (bar 18: restart on src, the never-written slot behind it unread), 0, 0,
// 0, 0 (rescans: never-written slots newer than the na slot), 3357.6 (bar
// 83: the na slot re-entered at k = 1, src is the only live member), 3327.4
// (new minimum), 2998, 2998, 2998, 2998 (the stale first valid write, once
// newer than the na slot), 3112. Every-bar ``x = bar_index % 4 == 0 ? na :
// low``: ``ta.lowest(x, 5) == ta.highest(x, 5)`` on the bar after every na
// bar — a single live member — where the old "skip the gap" reading kept
// the pre-gap extremum (bar 9: TV 9.20, the untouched cache 8.44).
struct BarContext {
    bool installed = false;
    long long bar_index = 0;   // ring address: the context's current bar index
    long long origin = 0;      // warmup reference: the context's first bar index
};
BarContext& bar_context();

// RAII installer: sets the thread-local BarContext for the enclosed dispatch
// and restores the previous one on exit (also during stack unwinding), so
// chart and request.security evaluation never see each other's index.
class BarContextScope {
    BarContext previous_;

public:
    BarContextScope(long long bar_index, long long origin);
    ~BarContextScope();
    BarContextScope(const BarContextScope&) = delete;
    BarContextScope& operator=(const BarContextScope&) = delete;
};

// Bar-addressed extremum ring shared by Highest / Lowest / HighestBars /
// LowestBars — the TradingView rule documented at bar_context() above.
class ExtremeRing {
public:
    struct Result {
        double value;   // na: window not formed yet, or no written non-na member
        int bars_back;  // k of the extremum slot (0 = the current bar)
    };
    explicit ExtremeRing(int length);
    // advance = true  -> compute():   records src for the context's current bar
    // advance = false -> recompute(): rewrites the current bar's slot
    // An na src is WRITTEN: it answers na, poisons the cache and, in every
    // later rescan, resets the running extremum (the poison rule documented
    // at bar_context()). The composites (Stoch, WPR, Range) share the ring
    // and the rule: their only oracle (finding 331, a stochRSI whose RSI is
    // na for its warmup only) is a leading-na series, on which the poison
    // answers exactly what "live over the non-na members" did.
    // Ties resolve to the OLDEST member (strict comparison, oldest slot first).
    Result update(double src, bool advance, bool want_max);

private:
    int length_;
    std::vector<double> values_;
    std::vector<unsigned char> written_;
    long long own_bar_ = -1;   // cadence when no context is installed
    bool has_written_ = false;
    // TradingView's cached extremum with an implied bar (bar_context() docs):
    // the value the site last answered and the bar it is attributed to.
    bool cached_ = false;
    double cval_ = 0.0;
    long long cbar_ = 0;
    // The cache as it stood BEFORE the current bar's first tick, so a
    // recompute() (same bar, advance=false) restores it and re-applies.
    long long cache_seen_bar_ = -1;
    bool saved_cached_ = false;
    double saved_cval_ = 0.0;
    long long saved_cbar_ = 0;
};

class RMA {
    double output_val;
    double sum;
    int length;
    int bar_count;

    // saved state for recompute
    double saved_output_val_;
    double saved_sum_;
    int saved_bar_count_;

public:
    explicit RMA(int length);
    double compute(double src);
    void save();
    void restore();
    double recompute(double src);
};

class RSI {
    RMA rma_up;
    RMA rma_down;
    double prev_src;
    int bar_count;

    // saved state for recompute
    double saved_prev_src_;
    int saved_bar_count_;

public:
    explicit RSI(int length);
    double compute(double src);
    double recompute(double src);
};

class Crossover {
    double prev_a;
    double prev_b;

    double saved_prev_a_, saved_prev_b_;

public:
    Crossover();
    bool compute(double a, double b);
    bool recompute(double a, double b);
};

class Crossunder {
    double prev_a;
    double prev_b;

    double saved_prev_a_, saved_prev_b_;

public:
    Crossunder();
    bool compute(double a, double b);
    bool recompute(double a, double b);
};

// --- SMA ---

class SMA {
    KahanWindowSum window_;
    int length;
    int bar_count;

public:
    explicit SMA(int length);
    double compute(double src);
    double recompute(double src);
};

// --- EMA ---

class EMA {
    double output_val;
    double alpha;
    double sum;
    int bar_count;
    // Retained only for the opt-in TradingView-built-in na-warmup path (below),
    // which must count `length` values before seeding with their SMA. The
    // default src-seed recursion uses `alpha` alone and never reads this.
    int length_;

    // saved state for recompute
    double saved_output_val_;
    double saved_sum_;
    int saved_bar_count_;

    // TradingView-built-in warmup latch (see ema_na_warmup_flag above). Latched
    // once on the first compute() and never revisited, so recompute()'s
    // restore() (which only rewinds output_val/sum/bar_count) cannot flip the
    // mode mid-series. Deliberately NOT part of save()/restore().
    bool na_warmup_ = false;
    bool warmup_latched_ = false;

public:
    explicit EMA(int length);
    double compute(double src);
    void save();
    void restore();
    double recompute(double src);
};

// --- MACD ---

struct MACDResult {
    double macd_line;
    double signal_line;
    double histogram;
};

class MACD {
    EMA fast_ema;
    EMA slow_ema;
    EMA signal_ema;

public:
    MACD(int fast_length, int slow_length, int signal_length);
    MACDResult compute(double src);
    MACDResult recompute(double src);
};

// --- Highest ---

class Highest {
    ExtremeRing ring_;

public:
    explicit Highest(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Lowest ---

class Lowest {
    ExtremeRing ring_;

public:
    explicit Lowest(int length);
    double compute(double src);
    double recompute(double src);
};

// --- ATR ---

class ATR {
    RMA rma;
    double prev_close;
    int bar_count;

    double saved_prev_close_;
    int saved_bar_count_;

public:
    explicit ATR(int length);
    double compute(double high, double low, double close);
    double recompute(double high, double low, double close);
    // issue #178 (pinned 2026-09-06, lab tv i178-sparse-atr-sense,
    // BINANCE:BTCUSDT 60, 398/398 executions): TradingView's ta.atr called
    // inside a block that does not execute every bar advances its RMA on
    // the EXECUTIONS only, but the true range always reads the previous
    // CHART bar's close (close[1]), never the close of the previous
    // execution. The 3-arg path keeps the per-call prev_close for callers
    // that run every bar (identical there); a sparse call site must pass
    // the chart's previous close (BacktestEngine::prev_chart_close()).
    double compute(double high, double low, double close, double prev_chart_close);
    double recompute(double high, double low, double close, double prev_chart_close);
};

// --- Stoch ---

class Stoch {
    Highest highest;
    Lowest lowest;

public:
    explicit Stoch(int length);
    double compute(double src, double high, double low);
    double recompute(double src, double high, double low);
};

// --- Change ---

class Change {
    std::deque<double> history;
    int max_length_;

public:
    explicit Change(int max_length = 1);
    double compute(double src, int length = 1);
    double recompute(double src, int length = 1);
};

// --- Cross ---

class Cross {
    double prev_a;
    double prev_b;
    // Skip-tie state: TV's `ta.cross` tracks the last NON-TIED sign of
    // (a - b) so that intermediate "tied" bars (a == b) are transparent.
    // ta.crossover/ta.crossunder use the simpler immediate-prev rule and
    // need no skip-tie state.
    int last_nonzero_sign_;  // -1, 0 (uninitialised), +1

    double saved_prev_a_, saved_prev_b_;
    int saved_last_nonzero_sign_;

public:
    Cross();
    bool compute(double a, double b);
    bool recompute(double a, double b);
};

// --- WMA (Weighted Moving Average) ---

class WMA {
    int length_;
    // Ring buffer (capacity == length_) instead of std::deque to avoid
    // per-bar node allocation. Newest sample at offset 0, oldest at
    // offset length_-1. The per-bar O(length) weighted-sum recompute is
    // unchanged — see WMA::compute for the oldest→newest weight 1..length
    // accumulation order that parity depends on.
    DynamicRingBuffer<double> buffer_;

public:
    explicit WMA(int length);
    double compute(double src);
    double recompute(double src);
};

// --- HMA (Hull Moving Average) ---

class HMA {
    WMA wma_half_;
    WMA wma_full_;
    WMA wma_sqrt_;

public:
    explicit HMA(int length);
    double compute(double src);
    double recompute(double src);
};

// --- StdDev (Standard Deviation) ---

class StdDev {
    int length_;
    bool biased_;
    std::deque<double> buffer_;

public:
    explicit StdDev(int length, bool biased = true);
    double compute(double src);
    double recompute(double src);

private:
    double held_stdev() const;
};

// --- Supertrend ---

struct SupertrendResult {
    double value;
    double direction;
};

class Supertrend {
    double factor_;
    ATR atr_;
    double prev_upper_, prev_lower_, prev_st_;
    double prev_direction_;
    double prev_close_;
    bool initialized_;

    // saved state
    double saved_prev_upper_, saved_prev_lower_, saved_prev_st_;
    double saved_prev_direction_, saved_prev_close_;
    bool saved_initialized_;

public:
    Supertrend(double factor, int atr_period);
    SupertrendResult compute(double high, double low, double close);
    SupertrendResult recompute(double high, double low, double close);
};

// --- DMI (Directional Movement Index) ---

struct DMIResult {
    double diplus;
    double diminus;
    double adx;
};

class DMI {
    RMA rma_plus_, rma_minus_, rma_tr_;
    RMA rma_adx_;
    double prev_high_, prev_low_, prev_close_;
    bool first_bar_;

    // saved state
    double saved_prev_high_, saved_prev_low_, saved_prev_close_;
    bool saved_first_bar_;

public:
    DMI(int di_length, int adx_smoothing);
    DMIResult compute(double high, double low, double close);
    DMIResult recompute(double high, double low, double close);
};

// --- SAR (Parabolic SAR) ---

class SAR {
    double start_, increment_, maximum_;
    double af_, ep_, sar_;
    bool is_long_;
    bool initialized_;
    double prev_high_, prev_low_, prev_close_;
    double prev_prev_high_, prev_prev_low_;

    // saved state
    double saved_af_, saved_ep_, saved_sar_;
    bool saved_is_long_, saved_initialized_;
    double saved_prev_high_, saved_prev_low_, saved_prev_close_;
    double saved_prev_prev_high_, saved_prev_prev_low_;

public:
    SAR(double start, double increment, double maximum);
    double compute(double high, double low, double close);
    double recompute(double high, double low, double close);
};

// --- BB (Bollinger Bands) ---

struct BBResult {
    double middle;
    double upper;
    double lower;
};

class BB {
    double mult_;
    SMA sma_;
    StdDev stdev_;

public:
    BB(int length, double mult);
    BBResult compute(double src);
    BBResult recompute(double src);
};

// --- KC (Keltner Channels) ---

struct KCResult {
    double middle;
    double upper;
    double lower;
};

class KC {
    double mult_;
    EMA ema_;
    EMA range_ema_;
    double prev_close_ = na<double>();
    double saved_prev_close_ = na<double>();

public:
    KC(int length, double mult);
    KCResult compute(double src, double high, double low, double close);
    KCResult recompute(double src, double high, double low, double close);
};

// --- PivotHigh ---

class PivotHigh {
    int left_bars_, right_bars_;
    std::deque<double> buffer_;

public:
    PivotHigh(int left_bars, int right_bars);
    double compute(double src);
    double recompute(double src);
};

// --- PivotLow ---

class PivotLow {
    int left_bars_, right_bars_;
    std::deque<double> buffer_;

public:
    PivotLow(int left_bars, int right_bars);
    double compute(double src);
    double recompute(double src);
};

// --- Linreg (Linear Regression) ---

class Linreg {
    int length_;
    std::deque<double> buffer_;

public:
    explicit Linreg(int length);
    double compute(double src, double offset);
    double recompute(double src, double offset);
};

// --- PercentRank ---

class PercentRank {
    int length_;
    std::deque<double> buffer_;

public:
    explicit PercentRank(int length);
    double compute(double src);
    double recompute(double src);
};

// --- VWMA (Volume-Weighted Moving Average) ---

class VWMA {
    int length_;
    std::deque<double> sv_buffer_;
    std::deque<double> v_buffer_;
    double sv_sum_, v_sum_;

public:
    explicit VWMA(int length);
    double compute(double src, double vol);
    double recompute(double src, double vol);
};

// --- Mom (Momentum) ---

class Mom {
    int length_;
    std::deque<double> buffer_;

public:
    explicit Mom(int length);
    double compute(double src);
    double recompute(double src);
};

// --- ROC (Rate of Change) ---

class ROC {
    int length_;
    std::deque<double> buffer_;

public:
    explicit ROC(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Rising ---

class Rising {
    int length_;
    std::deque<double> buffer_;

public:
    explicit Rising(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Falling ---

class Falling {
    int length_;
    std::deque<double> buffer_;

public:
    explicit Falling(int length);
    double compute(double src);
    double recompute(double src);
};

// --- CCI (Commodity Channel Index) ---

class CCI {
    int length_;
    std::deque<double> buffer_;

public:
    explicit CCI(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Cum (Cumulative Sum) ---

class Cum {
    double sum_;

    double saved_sum_;

public:
    Cum();
    double compute(double src);
    double recompute(double src);
};

// --- Chart all-time max/min of a series (ta.max / ta.min single-arg) ---

class AllTimeMax {
    double max_;
    bool has_;

    double saved_max_;
    bool saved_has_;

public:
    AllTimeMax();
    double compute(double src);
    double recompute(double src);
};

class AllTimeMin {
    double min_;
    bool has_;

    double saved_min_;
    bool saved_has_;

public:
    AllTimeMin();
    double compute(double src);
    double recompute(double src);
};

// --- RCI (Rank Correlation Index, Spearman × 100) ---

class RCI {
    int length_;
    std::deque<double> buffer_;

public:
    explicit RCI(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Variance ---

class Variance {
    int length_;
    bool biased_;
    std::deque<double> buffer_;

public:
    explicit Variance(int length, bool biased = true);
    double compute(double src);
    double recompute(double src);
};

// --- Median ---

class Median {
    int length_;
    std::deque<double> buffer_;

public:
    explicit Median(int length);
    double compute(double src);
    double recompute(double src);
};

// --- HighestBars ---

class HighestBars {
    ExtremeRing ring_;

public:
    explicit HighestBars(int length);
    double compute(double src);
    double recompute(double src);
};

// --- LowestBars ---

class LowestBars {
    ExtremeRing ring_;

public:
    explicit LowestBars(int length);
    double compute(double src);
    double recompute(double src);
};

// --- ALMA (Arnaud Legoux Moving Average) ---

class ALMA {
    int length_;
    double offset_, sigma_;
    std::deque<double> buffer_;

public:
    ALMA(int length, double offset = 0.85, double sigma = 6.0);
    double compute(double src);
    double recompute(double src);
};

// --- SWMA (Symmetrically Weighted Moving Average, period=4) ---

class SWMA {
    std::deque<double> buffer_;

public:
    SWMA();
    double compute(double src);
    double recompute(double src);
};

// --- MFI (Money Flow Index) ---

class MFI {
    int length_;
    std::deque<double> pos_buffer_, neg_buffer_;
    double prev_src_;
    int bar_count_;

    // saved state for recompute
    double saved_prev_src_;
    int saved_bar_count_;
    std::deque<double> saved_pos_buffer_, saved_neg_buffer_;

public:
    explicit MFI(int length);
    double compute(double src, double vol);
    double recompute(double src, double vol);
};

// --- CMO (Chande Momentum Oscillator) ---

class CMO {
    int length_;
    std::deque<double> up_buffer_, down_buffer_;
    double prev_src_;
    int bar_count_;

    // saved state for recompute
    double saved_prev_src_;
    int saved_bar_count_;
    std::deque<double> saved_up_buffer_, saved_down_buffer_;

public:
    explicit CMO(int length);
    double compute(double src);
    double recompute(double src);
};

// --- TSI (True Strength Index) ---

class TSI {
    EMA ema_long_;
    EMA ema_short_;
    EMA ema_abs_long_;
    EMA ema_abs_short_;
    double prev_src_;
    int bar_count_;

    double saved_prev_src_;
    int saved_bar_count_;

public:
    TSI(int short_length, int long_length);
    double compute(double src);
    double recompute(double src);
};

// --- WPR (Williams %R) ---

class WPR {
    Highest highest_;
    Lowest lowest_;

public:
    explicit WPR(int length);
    double compute(double close, double high, double low);
    double recompute(double close, double high, double low);
};

// --- COG (Center of Gravity) ---

class COG {
    int length_;
    std::deque<double> buffer_;

public:
    explicit COG(int length);
    double compute(double src);
    double recompute(double src);
};

// --- BBW (Bollinger Bands Width) ---

class BBW {
    BB bb_;

public:
    BBW(int length, double mult);
    double compute(double src);
    double recompute(double src);
};

// --- KCW (Keltner Channel Width) ---

class KCW {
    KC kc_;

public:
    KCW(int length, double mult);
    double compute(double src, double high, double low, double close);
    double recompute(double src, double high, double low, double close);
};

// --- BarsSince ---

class BarsSince {
    int count_;
    bool ever_true_;

    int saved_count_;
    bool saved_ever_true_;

public:
    BarsSince();
    double compute(bool condition);
    double recompute(bool condition);
};

// --- ValueWhen ---

class ValueWhen {
    std::deque<double> values_;
    int max_occurrence_;

    std::deque<double> saved_values_;

public:
    explicit ValueWhen(int max_occurrence = 1);
    double compute(bool condition, double source, int occurrence);
    double recompute(bool condition, double source, int occurrence);
};

// --- Correlation ---

class Correlation {
    int length_;
    std::deque<double> x_buffer_, y_buffer_;

public:
    explicit Correlation(int length);
    double compute(double src1, double src2);
    double recompute(double src1, double src2);
};

// --- TR (True Range as function) ---

class TR {
    double prev_close_;
    int bar_count_;
    bool handle_na_;

    double saved_prev_close_;
    int saved_bar_count_;

public:
    explicit TR(bool handle_na = false);
    double compute(double high, double low, double close);
    double recompute(double high, double low, double close);
    // issue #178: the sparse-call form — true range against the previous
    // CHART bar's close (na on the first chart bar: handle_na decides).
    double compute(double high, double low, double close, double prev_chart_close);
    double recompute(double high, double low, double close, double prev_chart_close);
};

// --- PercentileNearestRank ---

class PercentileNearestRank {
    int length_;
    std::deque<double> buffer_;

public:
    explicit PercentileNearestRank(int length);
    double compute(double src, double percentage);
    double recompute(double src, double percentage);
};

// --- PercentileLinearInterpolation ---

class PercentileLinearInterpolation {
    int length_;
    std::deque<double> buffer_;

public:
    explicit PercentileLinearInterpolation(int length);
    double compute(double src, double percentage);
    double recompute(double src, double percentage);
};

// --- Volume indicators ---
class OBV {
    double sum_ = 0.0;
    double prev_close_ = na<double>();
    int bar_count_ = 0;

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_sum_ = 0.0, saved_prev_close_ = na<double>();
    int saved_bar_count_ = 0;

public:
    OBV() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class AccDist {
    double sum_ = 0.0;

    // Mirror the initial committed sum_ (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_sum_ = 0.0;

public:
    AccDist() = default;
    double compute(double high, double low, double close, double volume);
    double recompute(double high, double low, double close, double volume);
};

class NVI {
    double nvi_ = 1.0;
    double prev_close_ = na<double>();
    double prev_volume_ = na<double>();
    int bar_count_ = 0;

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_nvi_ = 1.0, saved_prev_close_ = na<double>(),
           saved_prev_volume_ = na<double>();
    int saved_bar_count_ = 0;

public:
    NVI() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class PVI {
    double pvi_ = 1.0;
    double prev_close_ = na<double>();
    double prev_volume_ = na<double>();
    int bar_count_ = 0;

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_pvi_ = 1.0, saved_prev_close_ = na<double>(),
           saved_prev_volume_ = na<double>();
    int saved_bar_count_ = 0;

public:
    PVI() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class PVT {
    double pvt_ = 0.0;
    double prev_close_ = na<double>();

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_pvt_ = 0.0, saved_prev_close_ = na<double>();

public:
    PVT() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class WAD {
    double wad_ = 0.0;
    double prev_close_ = na<double>();

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_wad_ = 0.0, saved_prev_close_ = na<double>();

public:
    WAD() = default;
    double compute(double high, double low, double close);
    double recompute(double high, double low, double close);
};

class WVAD {
public:
    WVAD() = default;
    double compute(double open, double high, double low, double close, double volume);
    double recompute(double open, double high, double low, double close, double volume);
};

class III {
public:
    III() = default;
    double compute(double high, double low, double close, double volume);
    double recompute(double high, double low, double close, double volume);
};

// --- VWAP Bands result (3-tuple: vwap, upper_band, lower_band) ---
struct VWAPBandsResult {
    double vwap;
    double upper;
    double lower;
};

class VWAP {
    double cum_pv_ = 0.0;
    double cum_vol_ = 0.0;
    // Sum of (price^2 * volume) for running variance computation used by
    // compute_bands(). Variance = cum_pv_sq_ / cum_vol_ - mean^2.
    double cum_pv_sq_ = 0.0;
    // Anchor day index: the SESSION day of the bar (session_day_index —
    // timestamp_ms / 86_400_000 on a UTC/24x7 symbol, the 17:00-ET-keyed
    // trading day on forex, the 09:30 RTH day on equities). On the first
    // compute() call we record this from the bar timestamp; on every
    // subsequent compute() the cumulator is reset whenever the day index
    // advances. Pine v6 `ta.vwap(source)` defaults to a Daily anchor
    // (`anchor = timeframe.change("1D")` on the SYMBOL's daily bar); the
    // engine matches that when the codegen threads syminfo tz + session.
    int64_t anchor_day_ = std::numeric_limits<int64_t>::min();

    // Mirror the initial committed state (see RMA::RMA) so a recompute()
    // before the first compute() restores a well-defined pristine state.
    double saved_cum_pv_ = 0.0, saved_cum_vol_ = 0.0, saved_cum_pv_sq_ = 0.0;
    int64_t saved_anchor_day_ = std::numeric_limits<int64_t>::min();

    void roll_anchor(int64_t day);

public:
    VWAP() = default;
    // tz-less forms key the anchor on the UTC day (the 24x7 corpus regime);
    // the tz/session forms key it on the symbol's session day and reduce to
    // the same integer math for tz="UTC" + empty/"24x7" session.
    // Feature macro PF_VWAP_HAS_SESSION_ANCHOR (below the class) lets
    // generated code compile the (tz, session) tail out on older engines.
    double compute(double src, double volume, int64_t timestamp_ms);
    double recompute(double src, double volume, int64_t timestamp_ms);
    double compute(double src, double volume, int64_t timestamp_ms,
                   const std::string& tz, const std::string& session);
    double recompute(double src, double volume, int64_t timestamp_ms,
                     const std::string& tz, const std::string& session);
    VWAPBandsResult compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult);
    VWAPBandsResult recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult);
    VWAPBandsResult compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult,
                                  const std::string& tz, const std::string& session);
    VWAPBandsResult recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult,
                                    const std::string& tz, const std::string& session);
};

// Feature macro: the emitted prelude's PF_VWAP_SESSION_ANCHOR_ARGS(tz, s)
// expands to `, tz, s` only when this is defined (see VWAP overloads above).
#define PF_VWAP_HAS_SESSION_ANCHOR 1

// --- VWAP Bands wrapper class (3-tuple form: ta.vwap(src, anchor, stdev_mult)) ---
// Wraps VWAP and routes compute/recompute to compute_bands/recompute_bands with
// a fixed stdev_mult supplied at construction time. This lets the codegen use
// the standard .compute()/.recompute() dispatch pattern for tuple returns.
class VWAPBands {
    VWAP vwap_;
    double stdev_mult_;
public:
    explicit VWAPBands(double stdev_mult) : stdev_mult_(stdev_mult) {}
    VWAPBandsResult compute(double src, double volume, int64_t timestamp_ms) {
        return vwap_.compute_bands(src, volume, timestamp_ms, stdev_mult_);
    }
    VWAPBandsResult recompute(double src, double volume, int64_t timestamp_ms) {
        return vwap_.recompute_bands(src, volume, timestamp_ms, stdev_mult_);
    }
    VWAPBandsResult compute(double src, double volume, int64_t timestamp_ms,
                            const std::string& tz, const std::string& session) {
        return vwap_.compute_bands(src, volume, timestamp_ms, stdev_mult_, tz, session);
    }
    VWAPBandsResult recompute(double src, double volume, int64_t timestamp_ms,
                              const std::string& tz, const std::string& session) {
        return vwap_.recompute_bands(src, volume, timestamp_ms, stdev_mult_, tz, session);
    }
};

// --- Statistical ---
class Mode {
    int length_;
    std::deque<double> buffer_;
public:
    explicit Mode(int length) : length_(length) {}
    double compute(double src);
    double recompute(double src);
};

class Range {
    Highest highest_;
    Lowest lowest_;
public:
    explicit Range(int length) : highest_(length), lowest_(length) {}
    double compute(double src);
    double recompute(double src);
};

class Dev {
    int length_;
    std::deque<double> buffer_;
public:
    explicit Dev(int length) : length_(length) {}
    double compute(double src);
    double recompute(double src);
};

// --- pivot_point_levels (free function) ---

std::vector<double> pivot_point_levels(const std::string& method,
                                       double high, double low, double close);

} // namespace ta

} // namespace pineforge
