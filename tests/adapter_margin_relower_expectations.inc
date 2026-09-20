// Harvested from the Pine adapter as it stood on main
// b0cec54816fcc0bf8dc0b8500e784346e838dc8c ("Port the wave-2 lanes'
// hash-neutrality guards to native_run_spec_digest()"), BEFORE any part of
// the TradingView margin call moved onto the kernel's generic margin model.
//
// Provenance: this same translation unit was compiled against that library in
// a worktree detached at that sha (/private/tmp/pf-r5-margin-base, corpus
// 442d497b) with the project's own test flags
//   c++ -O3 -DNDEBUG -std=c++17 -arch arm64 -ffp-contract=off -UNDEBUG
// and run with PF_DUMP=1; the rows below are that run's output transcribed
// verbatim, to seventeen significant digits. Re-harvest with PF_DUMP=1 on
// this binary if a scenario is ever added.
//
// The four rows that overlap the L4b twin (tests/test_native_margin_hooks.cpp
// twin_leveraged_long / twin_short_adverse_high / twin_forced_execution_price
// / twin_rounded_money_and_whole_drop) carry the same values there, measured
// independently against a native host driving the kernel through the hooks.

namespace {

const double kNone[1] = {0.0};

// MG-F: 4x restore at the adverse waypoint 95.
const double kLeveragedLongUnits[1] = {4.2105263157894735};
const double kLeveragedLongPrices[1] = {95.0};
// MG-F2: the same rule on the short side, at the high.
const double kLeveragedShortUnits[1] = {3.8095238095238093};
const double kLeveragedShortPrices[1] = {105.0};
// MG-H2: the tick-rounded fire price minus two ticks of exit-side slippage.
const double kSlippedLongUnits[1] = {2.5599999999999885};
const double kSlippedLongPrices[1] = {94.980000000000004};
// MG-B/G: the rounded-money admission, sized by the whole-drop band.
const double kRoundedMoneyUnits[1] = {1.0};
const double kRoundedMoneyPrices[1] = {99.999999996};
// MG-E: the 1x-long money call, one contract at the 00:30Z low.
const double kMoneyLongUnits[1] = {1.0};
const double kMoneyLongPrices[1] = {1606.1700000000001};
// MG-I: one slice per script bar, each sized against the book the last left.
const double kRepeatedUnits[2] = {4.2105263157894735, 1.7069701280227589};
const double kRepeatedPrices[2] = {95.0, 74.0};

const char kOneCall[] = "__margin_call__\x1f" "Margin call\x1e";
const char kTwoCalls[] = "__margin_call__\x1f" "Margin call\x1e"
                         "__margin_call__\x1f" "Margin call\x1e";

const Expected kLeveragedLong{
    "MG-F", 1, kLeveragedLongUnits, kLeveragedLongPrices, kOneCall,
    15.789473684210527, 2};
const Expected kLeveragedShort{
    "MG-F2", 1, kLeveragedShortUnits, kLeveragedShortPrices, kOneCall,
    -6.1904761904761907, 2};
const Expected kSlippedLong{
    "MG-H2", 1, kSlippedLongUnits, kSlippedLongPrices, kOneCall,
    17.440000000000012, 2};
// MG-A: TradingView makes no call at all on this book.
const Expected kCashFeeLong{"MG-A", 0, kNone, kNone, "", 20.0, 1};
const Expected kRoundedMoneyLong{
    "MG-B/G", 1, kRoundedMoneyUnits, kRoundedMoneyPrices, kOneCall, 19.0, 2};
const Expected kMoneyLong{
    "MG-E", 1, kMoneyLongUnits, kMoneyLongPrices, kOneCall, 0.0, 2};
const Expected kRepeatedBreach{
    "MG-I", 2, kRepeatedUnits, kRepeatedPrices, kTwoCalls,
    14.082503556187769, 3};
// MG-K: the C setter still switches the whole call off.
const Expected kDisabledLong{"MG-K", 0, kNone, kNone, "", 20.0, 1};

} // namespace
