// K-IDX follow-up: re-pins v19 broker-state hashes after the coordinate fold.
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
// expectation corrected (R5 lane K-ULP3): host A's runs and host B's streamed
// runs close a pyramided book with strategy.close by exactly its own sum,
// whose last lot K-ULP3 closes whole (two such closes in batch/A and in
// stream/A, one in stream/B, none in batch/B). The closing row now books
// the lot's whole size, one ulp above the rest fl(U - C) it booked before,
// whose one-ulp dust lot the host then swept unbooked; the trade
// digest, the net profit's last digits and the broker-state hash move with
// that row. Trade counts and errors did not move, and batch/B and the
// rebuilt runs of host B did not move at all:
//   batch/A: fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342
//     -> fnv=e337022ac3d8b681 net=71.786509342348978 hash=11831117278087605757
//   stream/A: fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096
//     -> fnv=a0e7b82126f9e8de net=17.321792890020646 hash=1782579242963268431
//   stream/B: fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607
//     -> fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=3909006125715448229
//   interleaved/A: fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096
//     -> fnv=a0e7b82126f9e8de net=17.321792890020646 hash=1782579242963268431
//   interleaved/B: fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607
//     -> fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=3909006125715448229
//   round_robin/A1: fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096
//     -> fnv=a0e7b82126f9e8de net=17.321792890020646 hash=1782579242963268431
//   round_robin/B: fnv=5247894006b9a51c net=-306.89379320283734 hash=466321451949962607
//     -> fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=3909006125715448229
//   round_robin/A2: fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=9662791836503609096
//     -> fnv=a0e7b82126f9e8de net=17.321792890020646 hash=1782579242963268431
//   rebuilt/0: fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342
//     -> fnv=e337022ac3d8b681 net=71.786509342348978 hash=11831117278087605757
//   rebuilt/2: fnv=5e2bd1d9860df94c net=71.786509342348751 hash=10773891632206972342
//     -> fnv=e337022ac3d8b681 net=71.786509342348978 hash=11831117278087605757
// expectation corrected (V19-FIX): every run's broker-state hash moved once more, because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree;
// trades, trade digests, net profits and errors did not move:
//   batch/A: hash=11831117278087605757 -> hash=13831511007419341774
//   batch/B: hash=17469022339767313045 -> hash=16942810041893704776
//   stream/A: hash=1782579242963268431 -> hash=3001610611355826173
//   stream/B: hash=3909006125715448229 -> hash=8241673901617223988
//   interleaved/A: hash=1782579242963268431 -> hash=3001610611355826173
//   interleaved/B: hash=3909006125715448229 -> hash=8241673901617223988
//   round_robin/A1: hash=1782579242963268431 -> hash=3001610611355826173
//   round_robin/B: hash=3909006125715448229 -> hash=8241673901617223988
//   round_robin/A2: hash=1782579242963268431 -> hash=3001610611355826173
//   rebuilt/0: hash=11831117278087605757 -> hash=13831511007419341774
//   rebuilt/1: hash=17469022339767313045 -> hash=16942810041893704776
//   rebuilt/2: hash=11831117278087605757 -> hash=13831511007419341774
//   rebuilt/3: hash=17469022339767313045 -> hash=16942810041893704776
// expectation corrected (INT25): every run's broker-state hash moved once more, because the integrated tree folds V19-FIX's Pine hash inputs and K-IDX's script-bar coordinate together (each lane re-pinned on a tree without the other); harvested once on the INT25 tree with this TU's harvest switch;
// trades, trade digests, net profits and errors did not move:
//   batch/A: hash=13831511007419341774 -> hash=496593797863776142
//   batch/B: hash=16942810041893704776 -> hash=5316201723780989919
//   stream/A: hash=3001610611355826173 -> hash=4351362905685696693
//   stream/B: hash=8241673901617223988 -> hash=1033336853289890500
//   interleaved/A: hash=3001610611355826173 -> hash=4351362905685696693
//   interleaved/B: hash=8241673901617223988 -> hash=1033336853289890500
//   round_robin/A1: hash=3001610611355826173 -> hash=4351362905685696693
//   round_robin/B: hash=8241673901617223988 -> hash=1033336853289890500
//   round_robin/A2: hash=3001610611355826173 -> hash=4351362905685696693
//   rebuilt/0: hash=13831511007419341774 -> hash=496593797863776142
//   rebuilt/1: hash=16942810041893704776 -> hash=5316201723780989919
//   rebuilt/2: hash=13831511007419341774 -> hash=496593797863776142
//   rebuilt/3: hash=16942810041893704776 -> hash=5316201723780989919
constexpr Pinned kPinned[] = {
    {"batch/A",
     "trades=23 fnv=e337022ac3d8b681 net=71.786509342348978 hash=496593797863776142 error=''"},
    {"batch/B",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=5316201723780989919 error=''"},
    {"stream/A",
     "trades=23 fnv=a0e7b82126f9e8de net=17.321792890020646 hash=4351362905685696693 error=''"},
    {"stream/B",
     "trades=35 fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=1033336853289890500 error=''"},
    {"interleaved/A",
     "trades=23 fnv=a0e7b82126f9e8de net=17.321792890020646 hash=4351362905685696693 error=''"},
    {"interleaved/B",
     "trades=35 fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=1033336853289890500 error=''"},
    {"round_robin/A1",
     "trades=23 fnv=a0e7b82126f9e8de net=17.321792890020646 hash=4351362905685696693 error=''"},
    {"round_robin/B",
     "trades=35 fnv=3eaaaacd90998d64 net=-306.89379320283734 hash=1033336853289890500 error=''"},
    {"round_robin/A2",
     "trades=23 fnv=a0e7b82126f9e8de net=17.321792890020646 hash=4351362905685696693 error=''"},
    {"rebuilt/0",
     "trades=23 fnv=e337022ac3d8b681 net=71.786509342348978 hash=496593797863776142 error=''"},
    {"rebuilt/1",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=5316201723780989919 error=''"},
    {"rebuilt/2",
     "trades=23 fnv=e337022ac3d8b681 net=71.786509342348978 hash=496593797863776142 error=''"},
    {"rebuilt/3",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=5316201723780989919 error=''"},
};
