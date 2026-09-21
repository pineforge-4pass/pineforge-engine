// Differential expectations for tests/test_adapter_range_end_relower.cpp.
//
// `adapter` is HARVESTED: compiled from this lane's base, origin/main
// b8e7976e, with PF_DUMP=1, before the row loop was re-lowered onto the
// kernel's generic producer. The adapter after the lane reproduces every one
// of these bit for bit, which is this TU's byte-identity evidence.
//
// `kernel` is the kernel's own run-end producer
// (report_policy = KernelRecorded, report_open_position_at_end = true) on the
// same tape. The two halves agree on every RANGE-END ROW field and differ on
// exactly the report SHAPE the adapter keeps: see §3.7 of
// docs/design/native-feature-parity.md.
//
// One convention difference is not a range-end divergence and shows on the
// flat control RE4 too: the adapter dates a row on the script bar's OPEN and
// the bare host on its close-execution point, so every `@hour` label is one
// bar apart between the halves. The mark itself (the `->hour` of a range-end
// row) is the same bar on both.

const Expected kRE1{"RE1",
    "closed=0 report=2 | L 100 @0:100->3:105 pnl=500 comm=0 ru=500 dd=0 oae=1 | L 100 @2:102->3:105 pnl=300 comm=0 ru=300 dd=0 oae=1 | last[3] eq=100800 op=0 | maxeq=100800 mineq=100800 maxdd=0 maxru=0",
    "closed=0 report=2 | L 100 @1:100->3:105 pnl=500 comm=0 ru=500 dd=0 oae=1 | L 100 @3:102->3:105 pnl=300 comm=0 ru=300 dd=0 oae=1 | last[3] eq=100800 op=800 | maxeq=100800 mineq=100800 maxdd=0 maxru=0"};

const Expected kRE2{"RE2",
    "closed=0 report=2 | L 100 @0:100->3:105 pnl=479.5 comm=20.5 ru=490 dd=10 oae=1 | L 100 @2:102->3:105 pnl=279.30000000000001 comm=20.700000000000003 ru=289.80000000000001 dd=10.200000000000001 oae=1 | last[3] eq=100758.8 op=0 | maxeq=100758.8 mineq=100758.8 maxdd=0 maxru=0",
    "closed=0 report=2 | L 100 @1:100->3:105 pnl=479.5 comm=20.5 ru=490 dd=10 oae=1 | L 100 @3:102->3:105 pnl=279.30000000000001 comm=20.700000000000003 ru=289.80000000000001 dd=10.200000000000001 oae=1 | last[3] eq=100800 op=800 | maxeq=100800 mineq=100800 maxdd=0 maxru=0"};

const Expected kRE3{"RE3",
    "closed=0 report=2 | L 100 @0:100->5:105 pnl=479.5 comm=20.5 ru=790 dd=1010 oae=1 | L 100 @2:102->5:105 pnl=279.30000000000001 comm=20.700000000000003 ru=589.79999999999995 dd=610.20000000000005 oae=1 | last[5] eq=100758.8 op=0 | maxeq=101400 mineq=99000 maxdd=2400 maxru=1758.8000000000029",
    "closed=0 report=2 | L 100 @1:100->5:105 pnl=479.5 comm=20.5 ru=790 dd=1010 oae=1 | L 100 @3:102->5:105 pnl=279.30000000000001 comm=20.700000000000003 ru=589.79999999999995 dd=610.20000000000005 oae=1 | last[5] eq=100800 op=800 | maxeq=101400 mineq=99000 maxdd=2400 maxru=1800"};

const Expected kRE4{"RE4",
    "closed=1 report=1 | L 100 @0:100->2:102 pnl=200 comm=0 ru=200 dd=0 oae=0 | last[3] eq=100200 op=0 | maxeq=100200 mineq=100200 maxdd=0 maxru=0",
    "closed=1 report=1 | L 100 @1:100->3:102 pnl=200 comm=0 ru=200 dd=0 oae=0 | last[3] eq=100200 op=0 | maxeq=100200 mineq=100200 maxdd=0 maxru=0"};
