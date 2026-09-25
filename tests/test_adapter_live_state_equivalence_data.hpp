// R5 lane V19-E: the transcript digests test_adapter_live_state_equivalence
// pins, harvested on engine be372243 (the lane's base: every placement row
// retained, pineforge-source-adapter/v3) by that TU compiled with
// -DPINEFORGE_V19E_HARVEST. Generated data: rebuild it the same way, never by
// hand. Included inside that TU's anonymous namespace. One configuration
// (reversals seed 3246599) ends in the kernel's own "native current evaluated
// allowance mismatch" failure on the base too; the failure is part of what
// its transcript pins.
//
// R5 lane V19-D re-pinned every value once, on its tree: the adapter re-issues
// with a carried binding (ReplaceOptions::keep_binding), so the kernel records
// no CloseBoundEvent where a re-issued exit's position has not moved -- and
// those events are part of the transcript. The same TU with every re-issue a
// plain replace reproduces every value below's old one exactly, and
// test_adapter_reissue_binding holds the two runs' transcripts equal with the
// CloseBoundEvents left out; the trades, commands and rows placed in each
// label are unchanged.
#pragma once

// K-IDX Option A re-pins the v19 transcript digests once for the script/input coordinate fold.

constexpr std::uint64_t kTranscriptDigests[] = {
    12303832286788334682ull,  // reversals seed 104729, 131 trades, 396 commands, 420 rows placed
    3937583110602879389ull,  // reversals seed 209458, 126 trades, 382 commands, 409 rows placed
    9660742953292018077ull,  // reversals seed 314187, 208 trades, 684 commands, 653 rows placed
    14691229822702022375ull,  // reversals seed 418916, 105 trades, 378 commands, 361 rows placed
    201369272286234124ull,  // reversals seed 523645, 104 trades, 417 commands, 357 rows placed
    1718744356722272312ull,  // reversals seed 628374, 138 trades, 410 commands, 393 rows placed
    10868119957001212051ull,  // reversals seed 733103, 139 trades, 386 commands, 404 rows placed
    15494092140752158088ull,  // reversals seed 837832, 91 trades, 401 commands, 332 rows placed
    5702736951683229543ull,  // reversals seed 942561, 98 trades, 408 commands, 356 rows placed
    10200415260680638236ull,  // reversals seed 1047290, 134 trades, 402 commands, 393 rows placed
    15845567806271913335ull,  // reversals seed 1152019, 148 trades, 386 commands, 415 rows placed
    9826242374101611623ull,  // reversals seed 1256748, 118 trades, 402 commands, 362 rows placed
    4883135135199369800ull,  // reversals seed 1361477, 105 trades, 413 commands, 349 rows placed
    6980972530369447172ull,  // reversals seed 1466206, 151 trades, 372 commands, 393 rows placed
    419860461863827083ull,  // reversals seed 1570935, 77 trades, 357 commands, 297 rows placed
    8598226526726960377ull,  // reversals seed 1675664, 132 trades, 397 commands, 406 rows placed
    3952138674949420146ull,  // reversals seed 1780393, 197 trades, 704 commands, 645 rows placed
    4513324373166417315ull,  // reversals seed 1885122, 113 trades, 393 commands, 388 rows placed
    18393465585117555058ull,  // reversals seed 1989851, 180 trades, 430 commands, 448 rows placed
    6120235323624511436ull,  // reversals seed 2094580, 92 trades, 384 commands, 340 rows placed
    17071186865396902026ull,  // reversals seed 2199309, 114 trades, 375 commands, 341 rows placed
    7979846979225294549ull,  // reversals seed 2304038, 149 trades, 412 commands, 418 rows placed
    16092912242308174488ull,  // reversals seed 2408767, 139 trades, 415 commands, 404 rows placed
    1517737495207721277ull,  // reversals seed 2513496, 110 trades, 390 commands, 394 rows placed
    1610718425964291349ull,  // reversals seed 2618225, 105 trades, 411 commands, 371 rows placed
    16169955000323314060ull,  // reversals seed 2722954, 153 trades, 390 commands, 423 rows placed
    8640229404818843386ull,  // reversals seed 2827683, 72 trades, 339 commands, 277 rows placed
    8135938306046315211ull,  // reversals seed 2932412, 96 trades, 360 commands, 318 rows placed
    9738428349677663372ull,  // reversals seed 3037141, 114 trades, 405 commands, 377 rows placed
    2670083654842814989ull,  // reversals seed 3141870, 66 trades, 332 commands, 248 rows placed
    15380097802441451398ull,  // reversals seed 3246599, 135 trades, 415 commands, 422 rows placed, error: native current evaluated allowance mismatch
    14583498249706098574ull,  // reversals seed 3351328, 108 trades, 414 commands, 345 rows placed
    13691690510990974075ull,  // reversals seed 3456057, 88 trades, 373 commands, 334 rows placed
    14092552094636276530ull,  // reversals seed 3560786, 147 trades, 413 commands, 444 rows placed
    4203447471686931733ull,  // reversals seed 3665515, 133 trades, 394 commands, 397 rows placed
    11999293368236543422ull,  // reversals seed 3770244, 53 trades, 339 commands, 240 rows placed
    5125743962394059998ull,  // brackets seed 104730, 82 trades, 521 commands, 568 rows placed
    3732927941814356572ull,  // brackets seed 209459, 77 trades, 487 commands, 551 rows placed
    10835407349126156117ull,  // brackets seed 314188, 147 trades, 866 commands, 1036 rows placed
    3737662390130165609ull,  // brackets seed 418917, 94 trades, 557 commands, 632 rows placed
    12564349532258145101ull,  // brackets seed 523646, 71 trades, 517 commands, 451 rows placed
    16592088247586231707ull,  // brackets seed 628375, 100 trades, 536 commands, 662 rows placed
    9284251268869981574ull,  // brackets seed 733104, 91 trades, 534 commands, 668 rows placed
    5785846610847751202ull,  // brackets seed 837833, 88 trades, 482 commands, 547 rows placed
    15132040098433212256ull,  // brackets seed 942562, 85 trades, 490 commands, 457 rows placed
    9624518409170657431ull,  // brackets seed 1047291, 86 trades, 468 commands, 526 rows placed
    15597660030609647223ull,  // brackets seed 1152020, 80 trades, 487 commands, 637 rows placed
    15849491031983505500ull,  // brackets seed 1256749, 88 trades, 511 commands, 566 rows placed
    9918882490509451201ull,  // brackets seed 1361478, 81 trades, 540 commands, 556 rows placed
    6438015927310529334ull,  // brackets seed 1466207, 72 trades, 568 commands, 577 rows placed
    17986613720295818227ull,  // brackets seed 1570936, 69 trades, 475 commands, 535 rows placed
    15751662755974333447ull,  // brackets seed 1675665, 87 trades, 520 commands, 602 rows placed
    11565075807030336076ull,  // brackets seed 1780394, 95 trades, 697 commands, 696 rows placed
    2015779222221407240ull,  // brackets seed 1885123, 90 trades, 542 commands, 587 rows placed
    1872777722679096721ull,  // brackets seed 1989852, 88 trades, 514 commands, 572 rows placed
    11372035822747614056ull,  // brackets seed 2094581, 85 trades, 496 commands, 575 rows placed
    14176875954190827756ull,  // brackets seed 2199310, 79 trades, 446 commands, 504 rows placed
    1455827488198344686ull,  // brackets seed 2304039, 92 trades, 524 commands, 594 rows placed
    11979119011511214626ull,  // brackets seed 2408768, 94 trades, 559 commands, 694 rows placed
    1774065971302770478ull,  // brackets seed 2513497, 86 trades, 524 commands, 508 rows placed
    10830766203810780391ull,  // brackets seed 2618226, 77 trades, 512 commands, 581 rows placed
    6172869291970139916ull,  // brackets seed 2722955, 85 trades, 503 commands, 661 rows placed
    9104251795068008494ull,  // brackets seed 2827684, 74 trades, 506 commands, 570 rows placed
    4716911134077760843ull,  // brackets seed 2932413, 75 trades, 469 commands, 515 rows placed
    6708847418396898846ull,  // brackets seed 3037142, 72 trades, 476 commands, 521 rows placed
    7906131650209684341ull,  // brackets seed 3141871, 101 trades, 537 commands, 653 rows placed
    8662530819297017027ull,  // brackets seed 3246600, 132 trades, 700 commands, 868 rows placed
    14331750725646803575ull,  // brackets seed 3351329, 85 trades, 504 commands, 520 rows placed
    7444517548780988427ull,  // brackets seed 3456058, 80 trades, 530 commands, 529 rows placed
    6178143291077411923ull,  // brackets seed 3560787, 107 trades, 521 commands, 634 rows placed
    11302959202215131177ull,  // brackets seed 3665516, 86 trades, 536 commands, 566 rows placed
    5122510577651000745ull,  // brackets seed 3770245, 78 trades, 503 commands, 560 rows placed
    11244999202114714137ull,  // chains seed 104731, 59 trades, 301 commands, 286 rows placed
    1482558061860139386ull,  // chains seed 209460, 85 trades, 390 commands, 393 rows placed
    12647248509338936453ull,  // chains seed 314189, 107 trades, 526 commands, 446 rows placed
    3121930092179333450ull,  // chains seed 418918, 45 trades, 312 commands, 299 rows placed
    165784926829741990ull,  // chains seed 523647, 61 trades, 322 commands, 329 rows placed
    18133723332411023357ull,  // chains seed 628376, 95 trades, 364 commands, 391 rows placed
    15591750256092081664ull,  // chains seed 733105, 88 trades, 351 commands, 319 rows placed
    10108332716913767868ull,  // chains seed 837834, 64 trades, 359 commands, 358 rows placed
    11616873062904594822ull,  // chains seed 942563, 59 trades, 341 commands, 322 rows placed
    2647666278133997762ull,  // chains seed 1047292, 64 trades, 343 commands, 344 rows placed
    10659847217212312660ull,  // chains seed 1152021, 70 trades, 342 commands, 418 rows placed
    12725802557554099964ull,  // chains seed 1256750, 65 trades, 319 commands, 302 rows placed
    1798455623603086886ull,  // chains seed 1361479, 64 trades, 312 commands, 323 rows placed
    13767422546817055791ull,  // chains seed 1466208, 73 trades, 382 commands, 345 rows placed
    10446321169177622098ull,  // chains seed 1570937, 90 trades, 366 commands, 419 rows placed
    16738581318629067020ull,  // chains seed 1675666, 64 trades, 321 commands, 389 rows placed
    12765469987058303812ull,  // chains seed 1780395, 82 trades, 480 commands, 461 rows placed
    8033768331525115464ull,  // chains seed 1885124, 66 trades, 360 commands, 304 rows placed
    8033691744657508662ull,  // chains seed 1989853, 73 trades, 353 commands, 373 rows placed
    6926029722960384835ull,  // chains seed 2094582, 75 trades, 321 commands, 365 rows placed
    2330688028628663175ull,  // chains seed 2199311, 56 trades, 327 commands, 306 rows placed
    6167262563711331013ull,  // chains seed 2304040, 81 trades, 342 commands, 354 rows placed
    1057624106564667840ull,  // chains seed 2408769, 67 trades, 347 commands, 374 rows placed
    13079047485388200520ull,  // chains seed 2513498, 66 trades, 336 commands, 338 rows placed
    17296102854052744512ull,  // chains seed 2618227, 65 trades, 336 commands, 355 rows placed
    5539584074139432609ull,  // chains seed 2722956, 87 trades, 363 commands, 366 rows placed
    16845310385882942743ull,  // chains seed 2827685, 57 trades, 337 commands, 325 rows placed
    10884597100613452852ull,  // chains seed 2932414, 65 trades, 350 commands, 331 rows placed
    8164707371872766708ull,  // chains seed 3037143, 81 trades, 324 commands, 358 rows placed
    18120582394174365029ull,  // chains seed 3141872, 82 trades, 355 commands, 349 rows placed
    5433696715667756825ull,  // chains seed 3246601, 100 trades, 485 commands, 473 rows placed
    10570656349418425385ull,  // chains seed 3351330, 60 trades, 311 commands, 317 rows placed
    10524873942376059207ull,  // chains seed 3456059, 59 trades, 292 commands, 298 rows placed
    12441518321478214338ull,  // chains seed 3560788, 73 trades, 359 commands, 338 rows placed
    3084783482698433102ull,  // chains seed 3665517, 70 trades, 347 commands, 326 rows placed
    15996277680102595200ull,  // chains seed 3770246, 76 trades, 362 commands, 298 rows placed
};
