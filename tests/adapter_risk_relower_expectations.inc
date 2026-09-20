// Harvested from the Pine adapter as it stood on main
// 683a82f ("Docs: the subscription pump's chronology against the script
// interval (R5 lane L6d, item 3)"), the base of audit lane N12, BEFORE this
// lane: no part of TradingView's strategy.risk.* moved onto the kernel's
// generic risk limits (none can — see docs/design/native-feature-parity.md
// §3.6), so the adapter after the lane reproduces every `adapter` literal bit
// for bit and the `kernel` literals are the kernel's own outcome on the same
// tape, pinned so the measured divergence cannot drift silently.
//
// Provenance: this same translation unit was compiled against that tree's
// build-on/lib/libpineforge.a (snapshot /tmp/n12-base-lib, corpus 442d497b)
// with the project's own test flags
//   c++ -O3 -DNDEBUG -std=c++17 -arch arm64 -ffp-contract=off -UNDEBUG
// and run with PF_DUMP=1; the rows below are that run's output transcribed
// verbatim, to seventeen significant digits. Re-harvest with PF_DUMP=1 on
// this binary if a scenario is ever added.
//
// Row spelling: side units @entry_hour:entry_price->exit_hour:exit_price
// pnl exit_id exit_comment close_cause, then the final signed book. Hours
// count from 2025-03-31 00:00 UTC; the kernel stamps a fill with the next
// interval's time, the adapter with the bar it filled on.

namespace {

const Expected kDrawdownCadence{
    "MG10-a",
    "L 100 @0:100->3:99 pnl=-100 id='' c='' cause=6 | L 100 @2:99->3:99 pnl=0 id='' c='' cause=6 | pos=200",
    "pos=100"};

const Expected kDrawdownReversal{
    "MG10-b",
    "L 100 @0:100->2:94 pnl=-600 id='S' c='' cause=1 | S 100 @2:94->3:94 pnl=0 id='L' c='' cause=1 | L 100 @3:94->4:94 pnl=0 id='' c='' cause=6 | pos=100",
    "pos=100"};

const Expected kLossDaysOrdered{
    "MG11-a",
    "L 100 @0:100->1:99 pnl=-100 id='__close__L' c='' cause=1 | L 100 @24:100->25:102 pnl=200 id='__close__L' c='' cause=1 | L 100 @26:100->27:99 pnl=-100 id='__close__L' c='' cause=1 | L 100 @48:100->49:99 pnl=-100 id='__close__L' c='' cause=1 | pos=0",
    "L 100 @1:100->2:99 pnl=-100 id='X' c='' cause=1 | L 100 @25:100->26:102 pnl=200 id='X' c='' cause=1 | L 100 @27:100->28:99 pnl=-100 id='X' c='' cause=1 | L 100 @49:100->50:99 pnl=-100 id='X' c='' cause=1 | L 100 @51:100->52:99 pnl=-100 id='X' c='' cause=1 | L 100 @73:100->74:101 pnl=100 id='X' c='' cause=1 | pos=0"};

const Expected kLossDaysNetted{
    "MG11-b",
    "L 100 @0:100->1:99 pnl=-100 id='__close__L' c='' cause=1 | L 100 @2:100->3:100.5 pnl=50 id='__close__L' c='' cause=1 | L 100 @24:100->25:99 pnl=-100 id='__close__L' c='' cause=1 | L 100 @26:100->27:100.5 pnl=50 id='__close__L' c='' cause=1 | L 100 @48:100->49:99 pnl=-100 id='__close__L' c='' cause=1 | pos=0",
    "L 100 @1:100->2:99 pnl=-100 id='X' c='' cause=1 | L 100 @3:100->4:100.5 pnl=50 id='X' c='' cause=1 | L 100 @25:100->26:99 pnl=-100 id='X' c='' cause=1 | L 100 @27:100->28:100.5 pnl=50 id='X' c='' cause=1 | pos=0"};

const Expected kIntradayPath{
    "MG12-a",
    "L 100 @0:100->24:85 pnl=-1500 id='' c='Close Position (Max intraday Loss)' cause=4 | L 100 @48:95->49:95 pnl=0 id='' c='' cause=6 | pos=100",
    "pos=300"};

const Expected kIntradayOpen{
    "MG12-b",
    "L 100 @0:100->25:88 pnl=-1200 id='' c='Close Position (Max intraday Loss)' cause=4 | pos=0",
    "L 100 @1:100->26:88 pnl=-1200 id='__kernel_risk__' c='Risk limit' cause=4 | pos=100"};

const Expected kFillCap{
    "MG13",
    "L 1 @0:100->1:103 pnl=3 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | L 1 @1:101->1:103 pnl=2 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | L 1 @24:101->25:101 pnl=0 id='__close__L' c='' cause=1 | pos=0",
    "L 1 @1:100->2:101 pnl=1 id='__kernel_risk__' c='Risk limit' cause=4 | L 1 @2:101->2:101 pnl=0 id='__kernel_risk__' c='Risk limit' cause=4 | L 1 @25:101->26:101 pnl=0 id='X' c='' cause=1 | pos=0"};

const Expected kFillCapChartDay{
    "MG14",
    "L 1 @0:100->1:100 pnl=0 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | L 1 @1:100->1:100 pnl=0 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | L 1 @4:100->24:100 pnl=0 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | L 1 @24:100->24:100 pnl=0 id='' c='Close Position (Max number of filled orders in one day)' cause=5 | pos=0",
    "L 1 @1:100->2:100 pnl=0 id='__kernel_risk__' c='Risk limit' cause=4 | L 1 @2:100->2:100 pnl=0 id='__kernel_risk__' c='Risk limit' cause=4 | pos=1"};

const Expected kPositionSize{
    "PS",
    "L 2 @0:100->3:100 pnl=0 id='' c='' cause=6 | L 2 @1:100->3:100 pnl=0 id='' c='' cause=6 | pos=4",
    "pos=2"};

} // namespace
