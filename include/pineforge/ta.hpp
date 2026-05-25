#pragma once
#include "na.hpp"
#include "series.hpp"
#include <deque>
#include <cmath>
#include <vector>
#include <string>

namespace pineforge {

namespace ta {

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
    DynamicRingBuffer<double> buffer;
    int length;
    int bar_count;
    double running_sum;

    double recalculate_exact_sum() const;

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
    int length;
    int bar_count;

    // saved state for recompute
    double saved_output_val_;
    double saved_sum_;
    int saved_bar_count_;

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
    std::deque<double> buffer;
    int length;

public:
    explicit Highest(int length);
    double compute(double src);
    double recompute(double src);
};

// --- Lowest ---

class Lowest {
    std::deque<double> buffer;
    int length;

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
};

// --- Stoch ---

class Stoch {
    Highest highest;
    Lowest lowest;
    int length;

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
    std::deque<double> buffer_;

public:
    explicit WMA(int length);
    double compute(double src);
    double recompute(double src);
};

// --- HMA (Hull Moving Average) ---

class HMA {
    int length_;
    WMA wma_half_;
    WMA wma_full_;
    WMA wma_sqrt_;
    std::deque<double> diff_buffer_;

public:
    explicit HMA(int length);
    double compute(double src);
    double recompute(double src);
};

// --- StdDev (Standard Deviation) ---

class StdDev {
    int length_;
    std::deque<double> buffer_;

public:
    explicit StdDev(int length);
    double compute(double src);
    double recompute(double src);
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
    int di_length_;
    int adx_smoothing_;
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
    int length_;
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
    int length_;
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
    std::deque<double> buffer_;

public:
    explicit Variance(int length);
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
    int length_;
    std::deque<double> buffer_;

public:
    explicit HighestBars(int length);
    double compute(double src);
    double recompute(double src);
};

// --- LowestBars ---

class LowestBars {
    int length_;
    std::deque<double> buffer_;

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
    int length_;
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

    double saved_sum_, saved_prev_close_;
    int saved_bar_count_;

public:
    OBV() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class AccDist {
    double sum_ = 0.0;

    double saved_sum_;

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

    double saved_nvi_, saved_prev_close_, saved_prev_volume_;
    int saved_bar_count_;

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

    double saved_pvi_, saved_prev_close_, saved_prev_volume_;
    int saved_bar_count_;

public:
    PVI() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class PVT {
    double pvt_ = 0.0;
    double prev_close_ = na<double>();

    double saved_pvt_, saved_prev_close_;

public:
    PVT() = default;
    double compute(double close, double volume);
    double recompute(double close, double volume);
};

class WAD {
    double wad_ = 0.0;
    double prev_close_ = na<double>();

    double saved_wad_, saved_prev_close_;

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
    // Anchor day index (Unix-day = timestamp_ms / 86_400_000). On the
    // first compute() call we record this from the bar timestamp; on
    // every subsequent compute() the cumulator is reset whenever the
    // day index advances. Pine v6 `ta.vwap(source)` defaults to a Daily
    // anchor (`anchor = timeframe.change("1D")`); engine matches that.
    int64_t anchor_day_ = std::numeric_limits<int64_t>::min();

    double saved_cum_pv_, saved_cum_vol_, saved_cum_pv_sq_;
    int64_t saved_anchor_day_ = std::numeric_limits<int64_t>::min();

public:
    VWAP() = default;
    double compute(double src, double volume, int64_t timestamp_ms);
    double recompute(double src, double volume, int64_t timestamp_ms);
    VWAPBandsResult compute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult);
    VWAPBandsResult recompute_bands(double src, double volume, int64_t timestamp_ms, double stdev_mult);
};

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
