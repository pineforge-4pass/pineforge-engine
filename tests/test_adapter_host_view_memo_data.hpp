// Harvested by test_adapter_host_view_memo at the lane's base (fc7aad62) with
// PF_HOST_VIEW_MEMO_DUMP=1.
// expectation corrected: every run's broker-state hash moved once, because v19
// folds the continuation over live state word-wise (native-consumer/v9) and
// the broker-state hash folds a running closed-row digest
// (pineforge-broker-state/v19); trades, trade digests, net profits and errors
// did not move:
//   batch/A: hash=10993652456342934788 -> hash=9607052712954665547
//   batch/B: hash=9540537160828157475 -> hash=10777439908775197412
//   stream/A: hash=6159194991465133697 -> hash=13425120520971970084
//   stream/B: hash=17697507986854574129 -> hash=8709754572180659129
//   interleaved/A: hash=6159194991465133697 -> hash=13425120520971970084
//   interleaved/B: hash=17697507986854574129 -> hash=8709754572180659129
//   round_robin/A1: hash=6159194991465133697 -> hash=13425120520971970084
//   round_robin/B: hash=17697507986854574129 -> hash=8709754572180659129
//   round_robin/A2: hash=6159194991465133697 -> hash=13425120520971970084
//   rebuilt/0: hash=10993652456342934788 -> hash=9607052712954665547
//   rebuilt/1: hash=9540537160828157475 -> hash=10777439908775197412
//   rebuilt/2: hash=10993652456342934788 -> hash=9607052712954665547
//   rebuilt/3: hash=9540537160828157475 -> hash=10777439908775197412
// expectation corrected (v19-B): the batch runs' broker-state hash moved once
// more, because v19-B folds the instants of the last two driver points, the
// FX-roll check's previous point, for a run with a staged FX curve and a
// margin model (the driver log that held them is no longer kept); trades,
// trade digests, net profits and errors did not move, and the stream runs,
// with no staged curve, did not move at all:
//   batch/A: hash=9607052712954665547 -> hash=11441494716296226561
//   batch/B: hash=10777439908775197412 -> hash=2328126988218613423
//   rebuilt/0: hash=9607052712954665547 -> hash=11441494716296226561
//   rebuilt/1: hash=10777439908775197412 -> hash=2328126988218613423
//   rebuilt/2: hash=9607052712954665547 -> hash=11441494716296226561
//   rebuilt/3: hash=10777439908775197412 -> hash=2328126988218613423
// expectation corrected (v19-E): every run's broker-state hash moved once more, because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); harvested with PF_HOST_VIEW_MEMO_DUMP=1 against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272: the stream runs equal V19-E's tip, the batch runs also fold V19-B's FX-roll driver instants;
// trades, trade digests, net profits and errors did not move:
//   batch/A: hash=11441494716296226561 -> hash=10773891632206972342
//   batch/B: hash=2328126988218613423 -> hash=17469022339767313045
//   stream/A: hash=13425120520971970084 -> hash=9662791836503609096
//   stream/B: hash=8709754572180659129 -> hash=466321451949962607
//   interleaved/A: hash=13425120520971970084 -> hash=9662791836503609096
//   interleaved/B: hash=8709754572180659129 -> hash=466321451949962607
//   round_robin/A1: hash=13425120520971970084 -> hash=9662791836503609096
//   round_robin/B: hash=8709754572180659129 -> hash=466321451949962607
//   round_robin/A2: hash=13425120520971970084 -> hash=9662791836503609096
//   rebuilt/0: hash=11441494716296226561 -> hash=10773891632206972342
//   rebuilt/1: hash=2328126988218613423 -> hash=17469022339767313045
//   rebuilt/2: hash=11441494716296226561 -> hash=10773891632206972342
//   rebuilt/3: hash=2328126988218613423 -> hash=17469022339767313045
constexpr Pinned kPinned[] = {
    {"batch/A",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342 error=''"},
    {"batch/B",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=17469022339767313045 error=''"},
    {"stream/A",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096 error=''"},
    {"stream/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607 error=''"},
    {"interleaved/A",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096 error=''"},
    {"interleaved/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607 error=''"},
    {"round_robin/A1",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096 error=''"},
    {"round_robin/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607 error=''"},
    {"round_robin/A2",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096 error=''"},
    {"rebuilt/0",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342 error=''"},
    {"rebuilt/1",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=17469022339767313045 error=''"},
    {"rebuilt/2",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342 error=''"},
    {"rebuilt/3",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=17469022339767313045 error=''"},
};
