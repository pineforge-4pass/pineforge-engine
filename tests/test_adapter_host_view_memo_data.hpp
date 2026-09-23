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
constexpr Pinned kPinned[] = {
    {"batch/A",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=9607052712954665547 error=''"},
    {"batch/B",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=10777439908775197412 error=''"},
    {"stream/A",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=13425120520971970084 error=''"},
    {"stream/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=8709754572180659129 error=''"},
    {"interleaved/A",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=13425120520971970084 error=''"},
    {"interleaved/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=8709754572180659129 error=''"},
    {"round_robin/A1",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=13425120520971970084 error=''"},
    {"round_robin/B",
     "trades=35 fnv=5247894006b9a51c net=-306.89379320283734 hash=8709754572180659129 error=''"},
    {"round_robin/A2",
     "trades=23 fnv=dc1bbc35b4b25a7f net=17.321792890020532 hash=13425120520971970084 error=''"},
    {"rebuilt/0",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=9607052712954665547 error=''"},
    {"rebuilt/1",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=10777439908775197412 error=''"},
    {"rebuilt/2",
     "trades=23 fnv=5e2bd1d9860df94c net=71.786509342348751 hash=9607052712954665547 error=''"},
    {"rebuilt/3",
     "trades=35 fnv=9fbd9e232cb9843a net=-268.32270548365625 hash=10777439908775197412 error=''"},
};
