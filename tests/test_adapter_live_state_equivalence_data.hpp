// R5 lane V19-E: the transcript digests test_adapter_live_state_equivalence
// pins, harvested on engine be372243 (the lane's base: every placement row
// retained, pineforge-source-adapter/v3) by that TU compiled with
// -DPINEFORGE_V19E_HARVEST. Generated data: rebuild it the same way, never by
// hand. Included inside that TU's anonymous namespace. One configuration
// (reversals seed 3246599) ended in the kernel's own "native current evaluated
// allowance mismatch" failure on the base too, and the failure was part of
// what its transcript pinned, until R5 lane PAR-ORDERS-2: the kernel now
// refuses the current execution that failed (a request bound to a host
// roster), and the run completes.
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
//
// R5 lane PAR-MARGIN re-harvested three values the same way, on its tree:
// reversals seeds 1570935, 2827683 and 3456057, the leveraged (margin 25,
// default 700 %) configurations whose openings breach on their own entry bar.
// on_applied now admits the kernel's post-fill margin point there
// (TradingView's rule: tests/fixtures/margin_entry_bar), so each books that
// margin call on the entry bar instead of a bar later; the other 105 values
// are unchanged.
//
// R5 lane PAR-MARGIN-2 re-harvested five more the same way, on its tree, each
// a leveraged configuration whose margin call moves onto the bar its fill was
// on, or is re-sized there on the book that fill left -- TradingView's rule on
// its tapes (tests/fixtures/margin_entry_bar/pm2-*):
//   chains seed 1570937: a limit long filled on its way down is called on its
//     own bar at the low (46 @106 on bar 25, a bar earlier) -- the kernel's
//     post-fill point now measures from the fill's own waypoint;
//   chains seed 2199311 and reversals seed 2199309 (process_orders_on_close),
//   reversals seed 1570935 (adds to a carried book) and reversals seed 314187
//   (calc_on_order_fills; its trades are unchanged, one more request placed):
//     the adapter now admits the post-fill point on those fills' bars.
// The other 103 values are unchanged.

// R5 lane PAR-ORDERS re-harvested it the same way on its tree (29 of 108 digests
// move; see the lane's hash commit for why each moves).
// R5 lane PAR-ORDERS-2 moves 16 of 108 digests (INT26: the behaviour half of
// the lane's hash commit 9f7a025c, which says why each moves; harvested the
// same way on the integrated tree, every value equal to the lane's):
//   reversals seed 104729: 8320704943709246891 -> 17062111550189209216 (131 trades, 397 commands, 426 rows placed -> 131 trades, 397 commands, 431 rows placed)
//   reversals seed 314187: 9660742953292018077 -> 4920319354091947109 (208 trades, 684 commands, 653 rows placed -> 208 trades, 684 commands, 651 rows placed)
//   reversals seed 628374: 1718744356722272312 -> 12651613190033426865 (138 trades, 410 commands, 393 rows placed -> 138 trades, 410 commands, 399 rows placed)
//   reversals seed 1152019: 6104491581818825501 -> 4811666956682998059 (145 trades, 386 commands, 411 rows placed -> 151 trades, 397 commands, 409 rows placed)
//   reversals seed 1675664: 9695271777348687666 -> 15023244533711012002 (125 trades, 398 commands, 400 rows placed -> 124 trades, 398 commands, 405 rows placed)
//   reversals seed 1780393: 3952138674949420146 -> 3425020382181293193 (197 trades, 704 commands, 645 rows placed -> 196 trades, 702 commands, 629 rows placed)
//   reversals seed 2199309: 12243088919143375019 -> 12203651668804466516 (126 trades, 398 commands, 370 rows placed -> 127 trades, 398 commands, 375 rows placed)
//   reversals seed 2722954: 1694449538189143220 -> 4868757197565921069 (150 trades, 390 commands, 419 rows placed -> 150 trades, 390 commands, 428 rows placed)
//   reversals seed 3246599: 15380097802441451398 -> 14261337452491704800 (135 trades, 415 commands, 422 rows placed, error: native current evaluated allowance mismatch -> 156 trades, 469 commands, 484 rows placed)
//   reversals seed 3770244: 17742628010123667157 -> 16581004868667265691 (53 trades, 339 commands, 239 rows placed -> 53 trades, 339 commands, 245 rows placed)
//   brackets seed 314188: 10835407349126156117 -> 11664346520909115979 (147 trades, 866 commands, 1036 rows placed -> 145 trades, 850 commands, 981 rows placed)
//   brackets seed 1780394: 11565075807030336076 -> 1550055039614354387 (95 trades, 697 commands, 696 rows placed -> 95 trades, 697 commands, 705 rows placed)
//   brackets seed 3246600: 8662530819297017027 -> 2136070886124345893 (132 trades, 700 commands, 868 rows placed -> 115 trades, 657 commands, 847 rows placed)
//   chains seed 314189: 10134000431367652603 -> 5640170146031748940 (107 trades, 526 commands, 444 rows placed -> 107 trades, 526 commands, 454 rows placed)
//   chains seed 1780395: 632278533107602324 -> 11993708214650334332 (82 trades, 480 commands, 426 rows placed -> 82 trades, 480 commands, 436 rows placed)
//   chains seed 3246601: 17544117140908072910 -> 17828078233634427072 (98 trades, 485 commands, 468 rows placed -> 98 trades, 485 commands, 466 rows placed)
// R5 lane H-THIN moves 96 of 108 digests (INT26: the behaviour half of its hash
// step ab297364, harvested on the integrated tree with this TU's own switch;
// the transcript holds no hash value): P10 moves the trades of configurations
// with a FIFO exit from one of several entries, E19 the excursion fields every
// closed trade folds:
//   reversals seed 104729: 17062111550189209216 -> 14377401231487041547 (an earlier pick had moved it too) (reversals seed 104729, 131 trades, 397 commands, 431 rows placed -> reversals seed 104729, 131 trades, 397 commands, 435 rows placed)
//   reversals seed 209458: 3937583110602879389 -> 8277084373591058696 (reversals seed 209458, 126 trades, 382 commands, 409 rows placed -> reversals seed 209458, 123 trades, 379 commands, 419 rows placed)
//   reversals seed 314187: 9508064597864072295 -> 2936193272230691901 (an earlier pick had moved it too) (reversals seed 314187, 208 trades, 684 commands, 652 rows placed -> reversals seed 314187, 168 trades, 628 commands, 582 rows placed)
//   reversals seed 418916: 14691229822702022375 -> 6549991687676950761 (reversals seed 418916, 105 trades, 378 commands, 361 rows placed -> reversals seed 418916, 107 trades, 381 commands, 383 rows placed)
//   reversals seed 523645: 201369272286234124 -> 9953188267307049090 (reversals seed 523645, 104 trades, 417 commands, 357 rows placed -> reversals seed 523645, 104 trades, 417 commands, 379 rows placed)
//   reversals seed 628374: 12651613190033426865 -> 16185846816223020088 (an earlier pick had moved it too) (reversals seed 628374, 138 trades, 410 commands, 399 rows placed -> reversals seed 628374, 126 trades, 412 commands, 399 rows placed)
//   reversals seed 733103: 10868119957001212051 -> 4216355035770739779 (reversals seed 733103, 139 trades, 386 commands, 404 rows placed -> reversals seed 733103, 138 trades, 387 commands, 413 rows placed)
//   reversals seed 837832: 15494092140752158088 -> 6363178433100572906 (reversals seed 837832, 91 trades, 401 commands, 332 rows placed -> reversals seed 837832, 91 trades, 401 commands, 366 rows placed)
//   reversals seed 942561: 5702736951683229543 -> 1473820887994227184 (reversals seed 942561, 98 trades, 408 commands, 356 rows placed -> reversals seed 942561, 98 trades, 408 commands, 366 rows placed)
//   reversals seed 1047290: 10200415260680638236 -> 9193554944507238062 (reversals seed 1047290, 134 trades, 402 commands, 393 rows placed -> reversals seed 1047290, 133 trades, 395 commands, 392 rows placed)
//   reversals seed 1152019: 4811666956682998059 -> 6981925030785360377 (an earlier pick had moved it too) (reversals seed 1152019, 151 trades, 397 commands, 409 rows placed -> reversals seed 1152019, 151 trades, 397 commands, 421 rows placed)
//   reversals seed 1256748: 9826242374101611623 -> 5305403161869337627 (reversals seed 1256748, 118 trades, 402 commands, 362 rows placed -> reversals seed 1256748, 118 trades, 402 commands, 374 rows placed)
//   reversals seed 1361477: 4883135135199369800 -> 9475766261620659540 (reversals seed 1361477, 105 trades, 413 commands, 349 rows placed -> reversals seed 1361477, 106 trades, 411 commands, 389 rows placed)
//   reversals seed 1466206: 6980972530369447172 -> 4927715982682604365 (reversals seed 1466206, 151 trades, 372 commands, 393 rows placed -> reversals seed 1466206, 151 trades, 372 commands, 413 rows placed)
//   reversals seed 1570935: 17000758005854428354 -> 12202956723937736421 (an earlier pick had moved it too) (reversals seed 1570935, 92 trades, 362 commands, 329 rows placed -> reversals seed 1570935, 69 trades, 344 commands, 273 rows placed)
//   reversals seed 1675664: 15023244533711012002 -> 1308277235911883001 (an earlier pick had moved it too) (reversals seed 1675664, 124 trades, 398 commands, 405 rows placed -> reversals seed 1675664, 118 trades, 389 commands, 409 rows placed)
//   reversals seed 1780393: 3425020382181293193 -> 15749505417408196912 (an earlier pick had moved it too) (reversals seed 1780393, 196 trades, 702 commands, 629 rows placed -> reversals seed 1780393, 196 trades, 702 commands, 665 rows placed)
//   reversals seed 1885122: 4513324373166417315 -> 16795999522017645296 (reversals seed 1885122, 113 trades, 393 commands, 388 rows placed -> reversals seed 1885122, 127 trades, 396 commands, 391 rows placed)
//   reversals seed 1989851: 18393465585117555058 -> 6779215636140632786 (reversals seed 1989851, 180 trades, 430 commands, 448 rows placed -> reversals seed 1989851, 173 trades, 429 commands, 449 rows placed)
//   reversals seed 2094580: 6120235323624511436 -> 15115396572137765888 (reversals seed 2094580, 92 trades, 384 commands, 340 rows placed -> reversals seed 2094580, 92 trades, 384 commands, 350 rows placed)
//   reversals seed 2199309: 9639971099840596709 -> 1053394249578594394 (an earlier pick had moved it too) (reversals seed 2199309, 127 trades, 398 commands, 375 rows placed -> reversals seed 2199309, 124 trades, 398 commands, 378 rows placed)
//   reversals seed 2304038: 7979846979225294549 -> 314047600622510661 (reversals seed 2304038, 149 trades, 412 commands, 418 rows placed -> reversals seed 2304038, 149 trades, 412 commands, 440 rows placed)
//   reversals seed 2408767: 16092912242308174488 -> 3946903920860189031 (reversals seed 2408767, 139 trades, 415 commands, 404 rows placed -> reversals seed 2408767, 138 trades, 418 commands, 417 rows placed)
//   reversals seed 2513496: 1517737495207721277 -> 8490080330531136220 (reversals seed 2513496, 110 trades, 390 commands, 394 rows placed -> reversals seed 2513496, 103 trades, 392 commands, 393 rows placed)
//   reversals seed 2618225: 1610718425964291349 -> 12238138393722192163 (reversals seed 2618225, 105 trades, 411 commands, 371 rows placed -> reversals seed 2618225, 105 trades, 411 commands, 399 rows placed)
//   reversals seed 2722954: 4868757197565921069 -> 2863182994793154059 (an earlier pick had moved it too) (reversals seed 2722954, 150 trades, 390 commands, 428 rows placed -> reversals seed 2722954, 152 trades, 394 commands, 448 rows placed)
//   reversals seed 2827683: 13294227884245356190 -> 13953002312094633174 (an earlier pick had moved it too) (reversals seed 2827683, 48 trades, 347 commands, 215 rows placed -> reversals seed 2827683, 48 trades, 347 commands, 229 rows placed)
//   reversals seed 2932412: 8135938306046315211 -> 12936186547753035500 (reversals seed 2932412, 96 trades, 360 commands, 318 rows placed -> reversals seed 2932412, 96 trades, 360 commands, 330 rows placed)
//   reversals seed 3037141: 9738428349677663372 -> 11138119210196651599 (reversals seed 3037141, 114 trades, 405 commands, 377 rows placed -> reversals seed 3037141, 118 trades, 408 commands, 398 rows placed)
//   reversals seed 3141870: 2670083654842814989 -> 11186738496014205060 (reversals seed 3141870, 66 trades, 332 commands, 248 rows placed -> reversals seed 3141870, 116 trades, 402 commands, 404 rows placed)
//   reversals seed 3246599: 14261337452491704800 -> 7365380738509267810 (an earlier pick had moved it too) (reversals seed 3246599, 156 trades, 469 commands, 484 rows placed -> reversals seed 3246599, 147 trades, 455 commands, 507 rows placed)
//   reversals seed 3351328: 14583498249706098574 -> 8856317689282633932 (reversals seed 3351328, 108 trades, 414 commands, 345 rows placed -> reversals seed 3351328, 108 trades, 414 commands, 363 rows placed)
//   reversals seed 3456057: 7833467542887485857 -> 1680641833169632427 (an earlier pick had moved it too) (reversals seed 3456057, 87 trades, 373 commands, 334 rows placed -> reversals seed 3456057, 87 trades, 373 commands, 350 rows placed)
//   reversals seed 3560786: 14092552094636276530 -> 17785508903938760497 (reversals seed 3560786, 147 trades, 413 commands, 444 rows placed -> reversals seed 3560786, 147 trades, 397 commands, 435 rows placed)
//   reversals seed 3665515: 4203447471686931733 -> 17135221856069256209 (reversals seed 3665515, 133 trades, 394 commands, 397 rows placed -> reversals seed 3665515, 132 trades, 392 commands, 402 rows placed)
//   reversals seed 3770244: 16581004868667265691 -> 16348052548195448490 (an earlier pick had moved it too) (reversals seed 3770244, 53 trades, 339 commands, 245 rows placed -> reversals seed 3770244, 53 trades, 339 commands, 267 rows placed)
//   brackets seed 104730: 5125743962394059998 -> 14413849309182271769
//   brackets seed 209459: 3732927941814356572 -> 17836979205695001440 (brackets seed 209459, 77 trades, 487 commands, 551 rows placed -> brackets seed 209459, 76 trades, 487 commands, 569 rows placed)
//   brackets seed 314188: 11664346520909115979 -> 6185985077652624317 (an earlier pick had moved it too) (brackets seed 314188, 145 trades, 850 commands, 981 rows placed -> brackets seed 314188, 144 trades, 818 commands, 919 rows placed)
//   brackets seed 523646: 12564349532258145101 -> 10672250305191215856
//   brackets seed 628375: 16592088247586231707 -> 1855891803821441106 (brackets seed 628375, 100 trades, 536 commands, 662 rows placed -> brackets seed 628375, 101 trades, 524 commands, 724 rows placed)
//   brackets seed 733104: 9284251268869981574 -> 12279391351944236297 (brackets seed 733104, 91 trades, 534 commands, 668 rows placed -> brackets seed 733104, 91 trades, 520 commands, 628 rows placed)
//   brackets seed 942562: 15132040098433212256 -> 7040999793872498538
//   brackets seed 1047291: 9624518409170657431 -> 14314528608775687405 (brackets seed 1047291, 86 trades, 468 commands, 526 rows placed -> brackets seed 1047291, 85 trades, 468 commands, 547 rows placed)
//   brackets seed 1152020: 15597660030609647223 -> 13142231282207717561 (brackets seed 1152020, 80 trades, 487 commands, 637 rows placed -> brackets seed 1152020, 76 trades, 478 commands, 687 rows placed)
//   brackets seed 1466207: 6438015927310529334 -> 16303049545172805242 (brackets seed 1466207, 72 trades, 568 commands, 577 rows placed -> brackets seed 1466207, 70 trades, 568 commands, 669 rows placed)
//   brackets seed 1570936: 17986613720295818227 -> 8808473759868459315 (brackets seed 1570936, 69 trades, 475 commands, 535 rows placed -> brackets seed 1570936, 69 trades, 475 commands, 647 rows placed)
//   brackets seed 1780394: 1550055039614354387 -> 5435028051816123314 (an earlier pick had moved it too)
//   brackets seed 1885123: 2015779222221407240 -> 355520822114888679 (brackets seed 1885123, 90 trades, 542 commands, 587 rows placed -> brackets seed 1885123, 89 trades, 542 commands, 583 rows placed)
//   brackets seed 1989852: 1872777722679096721 -> 5402310364412118994 (brackets seed 1989852, 88 trades, 514 commands, 572 rows placed -> brackets seed 1989852, 89 trades, 505 commands, 639 rows placed)
//   brackets seed 2304039: 1455827488198344686 -> 15236878832940726852 (brackets seed 2304039, 92 trades, 524 commands, 594 rows placed -> brackets seed 2304039, 94 trades, 577 commands, 623 rows placed)
//   brackets seed 2408768: 11979119011511214626 -> 4629113575017480307 (brackets seed 2408768, 94 trades, 559 commands, 694 rows placed -> brackets seed 2408768, 78 trades, 528 commands, 676 rows placed)
//   brackets seed 2618226: 10830766203810780391 -> 14075994758817023751
//   brackets seed 2722955: 6172869291970139916 -> 4051940090401488259 (brackets seed 2722955, 85 trades, 503 commands, 661 rows placed -> brackets seed 2722955, 97 trades, 510 commands, 704 rows placed)
//   brackets seed 2827684: 9104251795068008494 -> 11315777065199433571 (brackets seed 2827684, 74 trades, 506 commands, 570 rows placed -> brackets seed 2827684, 93 trades, 530 commands, 631 rows placed)
//   brackets seed 3037142: 6708847418396898846 -> 9921951745340606853
//   brackets seed 3141871: 7906131650209684341 -> 5531345307764558073 (brackets seed 3141871, 101 trades, 537 commands, 653 rows placed -> brackets seed 3141871, 101 trades, 537 commands, 665 rows placed)
//   brackets seed 3246600: 2136070886124345893 -> 13285723008256739950 (an earlier pick had moved it too) (brackets seed 3246600, 115 trades, 657 commands, 847 rows placed -> brackets seed 3246600, 115 trades, 657 commands, 863 rows placed)
//   brackets seed 3560787: 6178143291077411923 -> 5589169095865416088 (brackets seed 3560787, 107 trades, 521 commands, 634 rows placed -> brackets seed 3560787, 101 trades, 500 commands, 583 rows placed)
//   brackets seed 3665516: 11302959202215131177 -> 3100862192505875197 (brackets seed 3665516, 86 trades, 536 commands, 566 rows placed -> brackets seed 3665516, 92 trades, 571 commands, 626 rows placed)
//   chains seed 104731: 11244999202114714137 -> 11442749230602752319 (bisected by pick: H-THIN's source on K-OCA-KEEP's tip gives the lane's own value, on PAR-ORDERS' E14 commit a6e6a005 this one -- P10's path meets E14's pending-entry trail) (chains seed 104731, 59 trades, 301 commands, 286 rows placed -> chains seed 104731, 67 trades, 318 commands, 355 rows placed)
//   chains seed 209460: 11413128619516782387 -> 973054536545442904 (an earlier pick had moved it too) (chains seed 209460, 85 trades, 390 commands, 386 rows placed -> chains seed 209460, 66 trades, 384 commands, 419 rows placed)
//   chains seed 314189: 5640170146031748940 -> 3920721464232107171 (an earlier pick had moved it too) (chains seed 314189, 107 trades, 526 commands, 454 rows placed -> chains seed 314189, 62 trades, 465 commands, 490 rows placed)
//   chains seed 418918: 3121930092179333450 -> 13576086977686921552 (chains seed 418918, 45 trades, 312 commands, 299 rows placed -> chains seed 418918, 44 trades, 293 commands, 349 rows placed)
//   chains seed 523647: 165784926829741990 -> 13312605232919440966 (bisected by pick: H-THIN's source on K-OCA-KEEP's tip gives the lane's own value, on PAR-ORDERS' E14 commit a6e6a005 this one -- P10's path meets E14's pending-entry trail) (chains seed 523647, 61 trades, 322 commands, 329 rows placed -> chains seed 523647, 47 trades, 314 commands, 327 rows placed)
//   chains seed 628376: 10122741367483366791 -> 6547333340447752844 (an earlier pick had moved it too) (chains seed 628376, 90 trades, 358 commands, 415 rows placed -> chains seed 628376, 73 trades, 342 commands, 367 rows placed)
//   chains seed 733105: 13574785125744972298 -> 3389962382455065848 (an earlier pick had moved it too) (chains seed 733105, 88 trades, 351 commands, 318 rows placed -> chains seed 733105, 65 trades, 369 commands, 421 rows placed)
//   chains seed 837834: 10108332716913767868 -> 10071629273929733187 (chains seed 837834, 64 trades, 359 commands, 358 rows placed -> chains seed 837834, 60 trades, 368 commands, 418 rows placed)
//   chains seed 942563: 12836024126514275333 -> 15248982654499518174 (an earlier pick had moved it too) (chains seed 942563, 59 trades, 341 commands, 322 rows placed -> chains seed 942563, 56 trades, 343 commands, 361 rows placed)
//   chains seed 1047292: 16444976724508807594 -> 6473049927299403237 (an earlier pick had moved it too) (chains seed 1047292, 64 trades, 343 commands, 341 rows placed -> chains seed 1047292, 34 trades, 363 commands, 400 rows placed)
//   chains seed 1152021: 6715343122935757718 -> 5657155561465537738 (an earlier pick had moved it too) (chains seed 1152021, 70 trades, 342 commands, 384 rows placed -> chains seed 1152021, 64 trades, 338 commands, 409 rows placed)
//   chains seed 1256750: 12725802557554099964 -> 140926706188109974 (chains seed 1256750, 65 trades, 319 commands, 302 rows placed -> chains seed 1256750, 64 trades, 314 commands, 315 rows placed)
//   chains seed 1361479: 1798455623603086886 -> 1884320088982623449 (chains seed 1361479, 64 trades, 312 commands, 323 rows placed -> chains seed 1361479, 65 trades, 309 commands, 326 rows placed)
//   chains seed 1466208: 9973593188851025006 -> 17488515754754645248 (an earlier pick had moved it too) (chains seed 1466208, 73 trades, 382 commands, 338 rows placed -> chains seed 1466208, 78 trades, 390 commands, 407 rows placed)
//   chains seed 1570937: 16038855060654239956 -> 8944738996467660919 (an earlier pick had moved it too) (chains seed 1570937, 90 trades, 366 commands, 370 rows placed -> chains seed 1570937, 68 trades, 353 commands, 354 rows placed)
//   chains seed 1675666: 4161494051921551918 -> 3579325126147100396 (an earlier pick had moved it too) (chains seed 1675666, 64 trades, 321 commands, 339 rows placed -> chains seed 1675666, 64 trades, 314 commands, 355 rows placed)
//   chains seed 1780395: 11993708214650334332 -> 17646633536556052253 (an earlier pick had moved it too) (chains seed 1780395, 82 trades, 480 commands, 436 rows placed -> chains seed 1780395, 67 trades, 438 commands, 465 rows placed)
//   chains seed 1885124: 8033768331525115464 -> 7799970005878023626 (chains seed 1885124, 66 trades, 360 commands, 304 rows placed -> chains seed 1885124, 62 trades, 362 commands, 378 rows placed)
//   chains seed 1989853: 8033691744657508662 -> 3042300852731654308 (chains seed 1989853, 73 trades, 353 commands, 373 rows placed -> chains seed 1989853, 75 trades, 350 commands, 372 rows placed)
//   chains seed 2094582: 10675979410645833515 -> 9104693962049720988 (an earlier pick had moved it too) (chains seed 2094582, 75 trades, 321 commands, 355 rows placed -> chains seed 2094582, 37 trades, 310 commands, 353 rows placed)
//   chains seed 2199311: 13103985213974940416 -> 3043920897357867824 (an earlier pick had moved it too) (chains seed 2199311, 54 trades, 326 commands, 285 rows placed -> chains seed 2199311, 56 trades, 321 commands, 327 rows placed)
//   chains seed 2304040: 9463870180619581974 -> 17550770482504522165 (an earlier pick had moved it too) (chains seed 2304040, 81 trades, 342 commands, 353 rows placed -> chains seed 2304040, 61 trades, 330 commands, 349 rows placed)
//   chains seed 2408769: 1057624106564667840 -> 10315907993814478663 (chains seed 2408769, 67 trades, 347 commands, 374 rows placed -> chains seed 2408769, 62 trades, 349 commands, 378 rows placed)
//   chains seed 2513498: 1646636678506043505 -> 11887874226824834835 (an earlier pick had moved it too) (chains seed 2513498, 66 trades, 336 commands, 332 rows placed -> chains seed 2513498, 50 trades, 344 commands, 365 rows placed)
//   chains seed 2618227: 15055331467095475975 -> 8569167112979511745 (an earlier pick had moved it too) (chains seed 2618227, 65 trades, 336 commands, 331 rows placed -> chains seed 2618227, 51 trades, 339 commands, 386 rows placed)
//   chains seed 2722956: 18266136563684836716 -> 1052992062651979911 (an earlier pick had moved it too) (chains seed 2722956, 62 trades, 345 commands, 344 rows placed -> chains seed 2722956, 59 trades, 355 commands, 422 rows placed)
//   chains seed 2827685: 16845310385882942743 -> 18246893670312018403 (chains seed 2827685, 57 trades, 337 commands, 325 rows placed -> chains seed 2827685, 54 trades, 342 commands, 370 rows placed)
//   chains seed 2932414: 6451466112212038981 -> 16270472390219306311 (an earlier pick had moved it too) (chains seed 2932414, 65 trades, 350 commands, 330 rows placed -> chains seed 2932414, 62 trades, 336 commands, 373 rows placed)
//   chains seed 3037143: 6296605148496367564 -> 14669408219364885155 (an earlier pick had moved it too) (chains seed 3037143, 81 trades, 324 commands, 328 rows placed -> chains seed 3037143, 61 trades, 344 commands, 412 rows placed)
//   chains seed 3141872: 18120582394174365029 -> 4441359501225912121 (chains seed 3141872, 82 trades, 355 commands, 349 rows placed -> chains seed 3141872, 59 trades, 364 commands, 395 rows placed)
//   chains seed 3246601: 17828078233634427072 -> 4493812043353694436 (an earlier pick had moved it too) (chains seed 3246601, 98 trades, 485 commands, 466 rows placed -> chains seed 3246601, 73 trades, 444 commands, 515 rows placed)
//   chains seed 3351330: 9531902743311875506 -> 10753585802392999956 (an earlier pick had moved it too) (chains seed 3351330, 60 trades, 311 commands, 310 rows placed -> chains seed 3351330, 45 trades, 301 commands, 354 rows placed)
//   chains seed 3456059: 1817828500136052373 -> 9038807849693065880 (an earlier pick had moved it too) (chains seed 3456059, 59 trades, 292 commands, 289 rows placed -> chains seed 3456059, 43 trades, 310 commands, 298 rows placed)
//   chains seed 3560788: 5397962162622300398 -> 2170094277734349720 (an earlier pick had moved it too) (chains seed 3560788, 73 trades, 359 commands, 337 rows placed -> chains seed 3560788, 60 trades, 363 commands, 422 rows placed)
//   chains seed 3665517: 3084783482698433102 -> 11220893490402964604 (chains seed 3665517, 70 trades, 347 commands, 326 rows placed -> chains seed 3665517, 70 trades, 347 commands, 380 rows placed)
//   chains seed 3770246: 15996277680102595200 -> 1535576416135278226 (chains seed 3770246, 76 trades, 362 commands, 298 rows placed -> chains seed 3770246, 62 trades, 346 commands, 372 rows placed)
// R5 lane PAR-ORDERS-3 moves 6 of the 108 digests on the integrated tree (INT27:
// the behaviour half of the lane's hash commit b65cee22, which says why each
// moves -- finding 4, bc5f7a21). Harvested with PINEFORGE_V19E_HARVEST; the
// lane's seventh, chains seed 1780395 (a digest-only move on its tree), does
// not move here, and every count is this tree's (INT26's picks had moved each
// of these seeds before):
//   reversals seed 314187: 2936193272230691901 -> 8480529943005853423 (168 trades, 628 commands, 582 rows placed -> 196 trades, 673 commands, 657 rows placed)
//   reversals seed 1780393: 15749505417408196912 -> 17449904882900142298
//   reversals seed 3246599: 7365380738509267810 -> 14507000991737484187 (147 trades, 455 commands, 507 rows placed -> 149 trades, 455 commands, 514 rows placed)
//   brackets seed 314188: 6185985077652624317 -> 15155965389917690170 (144 trades, 818 commands, 919 rows placed -> 134 trades, 837 commands, 932 rows placed)
//   brackets seed 1780394: 5435028051816123314 -> 12091946260420883994 (95 trades, 697 commands, 705 rows placed -> 96 trades, 721 commands, 761 rows placed)
//   chains seed 314189: 3920721464232107171 -> 14595591561869877759 (62 trades, 465 commands, 490 rows placed -> 75 trades, 498 commands, 527 rows placed)
// INT28 re-harvested it once on the integrated tree (main 962960b3 and the
// wave-J picks) with PINEFORGE_V19E_HARVEST; the same harvest against main
// reproduces every old value. 108 of 108 digests move, each marked with
// the pick(s) at whose boundary it moved (a harvest at every pick); an
// instrumented copy of the adapter (scratch only) shows the rule of each
// such pick firing in the run on the final tree; a pick marked superseded
// moved the run at its boundary, and its rule fires in the run at the
// boundary before the named later pick, not after it:
//   reversals seed 104729: 14377401231487041547 -> 12925325283399332139 (131 trades, 397 commands, 435 rows placed -> 131 trades, 397 commands, 438 rows placed) [W3B void exit; W4 F08]
//   reversals seed 209458: 8277084373591058696 -> 10024640008279748788 (123 trades, 379 commands, 419 rows placed -> 124 trades, 379 commands, 418 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 314187: 8480529943005853423 -> 2794849691207223034 (196 trades, 673 commands, 657 rows placed -> 235 trades, 723 commands, 742 rows placed) [W3B void exit; W8D; W8A R-B; W8A R3]
//   reversals seed 418916: 6549991687676950761 -> 16010591667054285812 (107 trades, 381 commands, 383 rows placed -> 107 trades, 372 commands, 373 rows placed) [W3B void exit]
//   reversals seed 523645: 9953188267307049090 -> 16125481158763653444 (104 trades, 417 commands, 379 rows placed -> 104 trades, 417 commands, 380 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 628374: 16185846816223020088 -> 4132505341042249934 (126 trades, 412 commands, 399 rows placed -> 124 trades, 404 commands, 394 rows placed) [W3B void exit; W5 C1]
//   reversals seed 733103: 4216355035770739779 -> 10949395400367693376 (138 trades, 387 commands, 413 rows placed -> 138 trades, 387 commands, 412 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 837832: 6363178433100572906 -> 2687164286151755975 (91 trades, 401 commands, 366 rows placed -> 90 trades, 401 commands, 369 rows placed) [W3B void exit]
//   reversals seed 942561: 1473820887994227184 -> 6131133790994226055 (98 trades, 408 commands, 366 rows placed -> 98 trades, 408 commands, 367 rows placed) [W3B void exit; W8A R3; R1-CONSOLIDATE R-A]
//   reversals seed 1047290: 9193554944507238062 -> 12699224912122031164 (133 trades, 395 commands, 392 rows placed -> 137 trades, 390 commands, 399 rows placed) [W3B void exit; W8E C1]
//   reversals seed 1152019: 6981925030785360377 -> 10462491692893518576 (151 trades, 397 commands, 421 rows placed -> 152 trades, 397 commands, 426 rows placed) [W3B void exit; W4 F08; W8A R-B; R1-CONSOLIDATE R1]
//   reversals seed 1256748: 5305403161869337627 -> 10671261055934450290 (118 trades, 402 commands, 374 rows placed -> 117 trades, 401 commands, 373 rows placed) [W3B void exit]
//   reversals seed 1361477: 9475766261620659540 -> 3043805792067546930 (106 trades, 411 commands, 389 rows placed -> 100 trades, 409 commands, 390 rows placed) [W3B void exit; W4 DORM]
//   reversals seed 1466206: 4927715982682604365 -> 11441719398179537978 (151 trades, 372 commands, 413 rows placed -> 156 trades, 387 commands, 440 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 1570935: 12202956723937736421 -> 18024148252823409663 (69 trades, 344 commands, 273 rows placed -> 69 trades, 344 commands, 269 rows placed) [W3B void exit; W8A R3]
//   reversals seed 1675664: 1308277235911883001 -> 10035800005689134556 (118 trades, 389 commands, 409 rows placed -> 111 trades, 379 commands, 394 rows placed) [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   reversals seed 1780393: 17449904882900142298 -> 3777516334616247576 (196 trades, 702 commands, 665 rows placed -> 192 trades, 691 commands, 650 rows placed) [W3B void exit; W8D; W8A R-B]
//   reversals seed 1885122: 16795999522017645296 -> 2275854726471319605 (127 trades, 396 commands, 391 rows placed -> 127 trades, 396 commands, 402 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B; R1-CONSOLIDATE R-A]
//   reversals seed 1989851: 6779215636140632786 -> 11265413284249527184 (173 trades, 429 commands, 449 rows placed -> 171 trades, 429 commands, 451 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   reversals seed 2094580: 15115396572137765888 -> 8444874965488324792 [W3B void exit]
//   reversals seed 2199309: 1053394249578594394 -> 5727820387840827666 (124 trades, 398 commands, 378 rows placed -> 122 trades, 400 commands, 378 rows placed) [W3B void exit; W4 F08; W8A R-B; R1-CONSOLIDATE R1]
//   reversals seed 2304038: 314047600622510661 -> 16340996993447103239 (149 trades, 412 commands, 440 rows placed -> 162 trades, 409 commands, 437 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 2408767: 3946903920860189031 -> 2756298568903753126 (138 trades, 418 commands, 417 rows placed -> 138 trades, 418 commands, 419 rows placed) [W3B void exit]
//   reversals seed 2513496: 8490080330531136220 -> 8654109025937810515 (103 trades, 392 commands, 393 rows placed -> 102 trades, 393 commands, 399 rows placed) [W3B void exit]
//   reversals seed 2618225: 12238138393722192163 -> 12615617675046204564 (105 trades, 411 commands, 399 rows placed -> 105 trades, 412 commands, 400 rows placed) [W3B void exit]
//   reversals seed 2722954: 2863182994793154059 -> 9207129964188117780 (152 trades, 394 commands, 448 rows placed -> 154 trades, 400 commands, 456 rows placed) [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   reversals seed 2827683: 13953002312094633174 -> 11406439758950114475 (48 trades, 347 commands, 229 rows placed -> 45 trades, 340 commands, 197 rows placed) [W3B void exit; W8A R3]
//   reversals seed 2932412: 12936186547753035500 -> 11829483237718104981 (96 trades, 360 commands, 330 rows placed -> 92 trades, 366 commands, 327 rows placed) [W3B void exit]
//   reversals seed 3037141: 11138119210196651599 -> 4678338477382770081 (118 trades, 408 commands, 398 rows placed -> 114 trades, 405 commands, 409 rows placed) [W3B void exit]
//   reversals seed 3141870: 11186738496014205060 -> 15598437293592346454 (116 trades, 402 commands, 404 rows placed -> 66 trades, 347 commands, 260 rows placed) [W3B void exit; W8A R3; R1-CONSOLIDATE R-A]
//   reversals seed 3246599: 14507000991737484187 -> 10688861328799387616 (149 trades, 455 commands, 514 rows placed -> 168 trades, 485 commands, 555 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   reversals seed 3351328: 8856317689282633932 -> 14703653017612176717 [W3B void exit]
//   reversals seed 3456057: 1680641833169632427 -> 12370377506510123879 (87 trades, 373 commands, 350 rows placed -> 87 trades, 373 commands, 351 rows placed) [W3B void exit; R1-CONSOLIDATE R-A]
//   reversals seed 3560786: 17785508903938760497 -> 4523288504224647910 (147 trades, 397 commands, 435 rows placed -> 153 trades, 403 commands, 452 rows placed) [W3B void exit; W8A R-B; R1-CONSOLIDATE R-A]
//   reversals seed 3665515: 17135221856069256209 -> 14246593818834397991 (132 trades, 392 commands, 402 rows placed -> 136 trades, 393 commands, 424 rows placed) [W3B void exit; W8A R-B]
//   reversals seed 3770244: 16348052548195448490 -> 920766308841330226 (53 trades, 339 commands, 267 rows placed -> 34 trades, 321 commands, 223 rows placed) [W3B void exit; W5 C1; W8A R3]
//   brackets seed 104730: 14413849309182271769 -> 7562978076140294353 (82 trades, 521 commands, 568 rows placed -> 84 trades, 472 commands, 586 rows placed) [W3B void exit; W4 F08]
//   brackets seed 209459: 17836979205695001440 -> 918119817585072101 (76 trades, 487 commands, 569 rows placed -> 79 trades, 588 commands, 642 rows placed) [W3 F05 rule A; W3B void exit; W8E C1]
//   brackets seed 314188: 15155965389917690170 -> 9866841135197940589 (134 trades, 837 commands, 932 rows placed -> 120 trades, 788 commands, 950 rows placed) [W3 F05 rule A; W3B void exit; W4 F19c]
//   brackets seed 418917: 3737662390130165609 -> 345551147133871958 (94 trades, 557 commands, 632 rows placed -> 94 trades, 524 commands, 629 rows placed) [W3B void exit]
//   brackets seed 523646: 10672250305191215856 -> 3598556121825612327 (71 trades, 517 commands, 451 rows placed -> 73 trades, 524 commands, 523 rows placed) [W3B void exit]
//   brackets seed 628375: 1855891803821441106 -> 10787524866046025632 (101 trades, 524 commands, 724 rows placed -> 92 trades, 580 commands, 741 rows placed) [TVDEF-DROPS R3; W3 F05 rule A; W3B void exit; W4 F08]
//   brackets seed 733104: 12279391351944236297 -> 12647837381492911624 (91 trades, 520 commands, 628 rows placed -> 105 trades, 546 commands, 689 rows placed) [W3 F05 rule A; W3B void exit; W8E C1]
//   brackets seed 837833: 5785846610847751202 -> 9142292326310325449 (88 trades, 482 commands, 547 rows placed -> 60 trades, 471 commands, 504 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   brackets seed 942562: 7040999793872498538 -> 1575526034554365613 (85 trades, 490 commands, 457 rows placed -> 96 trades, 541 commands, 581 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   brackets seed 1047291: 14314528608775687405 -> 3479520250185906772 (85 trades, 468 commands, 547 rows placed -> 84 trades, 508 commands, 578 rows placed) [W3 F05 rule A; W3B void exit]
//   brackets seed 1152020: 13142231282207717561 -> 14362220529398004157 (76 trades, 478 commands, 687 rows placed -> 71 trades, 559 commands, 681 rows placed) [W3B void exit; W4 F08]
//   brackets seed 1256749: 15849491031983505500 -> 18185683915043225509 (88 trades, 511 commands, 566 rows placed -> 85 trades, 510 commands, 578 rows placed) [W3B void exit]
//   brackets seed 1361478: 9918882490509451201 -> 12383757188086100027 (81 trades, 540 commands, 556 rows placed -> 87 trades, 538 commands, 598 rows placed) [W3B void exit]
//   brackets seed 1466207: 16303049545172805242 -> 13395291126983886538 (70 trades, 568 commands, 669 rows placed -> 86 trades, 571 commands, 692 rows placed) [W3B void exit]
//   brackets seed 1570936: 8808473759868459315 -> 17661091441404936538 (69 trades, 475 commands, 647 rows placed -> 66 trades, 475 commands, 665 rows placed) [W3B void exit]
//   brackets seed 1675665: 15751662755974333447 -> 12371414418607433001 (87 trades, 520 commands, 602 rows placed -> 74 trades, 501 commands, 551 rows placed) [TVDEF-DROPS R3, superseded by W3B void exit; W3B void exit; W4 F08]
//   brackets seed 1780394: 12091946260420883994 -> 7524564435896309120 (96 trades, 721 commands, 761 rows placed -> 93 trades, 692 commands, 780 rows placed) [W3 F05 rule A; W3B void exit; W4 F19c]
//   brackets seed 1885123: 355520822114888679 -> 17554269931279056335 (89 trades, 542 commands, 583 rows placed -> 93 trades, 540 commands, 738 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8E C1]
//   brackets seed 1989852: 5402310364412118994 -> 14174662451420240772 (89 trades, 505 commands, 639 rows placed -> 77 trades, 558 commands, 683 rows placed) [W3 F05 rule A; W3B void exit; W4 F19a]
//   brackets seed 2094581: 11372035822747614056 -> 4473743503459375456 (85 trades, 496 commands, 575 rows placed -> 73 trades, 520 commands, 601 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   brackets seed 2199310: 14176875954190827756 -> 5399518682810858268 (79 trades, 446 commands, 504 rows placed -> 76 trades, 452 commands, 575 rows placed) [TVDEF-DROPS R3, superseded by W3B void exit; W3B void exit; W4 F08]
//   brackets seed 2304039: 15236878832940726852 -> 12383750543640558931 (94 trades, 577 commands, 623 rows placed -> 89 trades, 548 commands, 663 rows placed) [W3B void exit]
//   brackets seed 2408768: 4629113575017480307 -> 5478903712594868650 (78 trades, 528 commands, 676 rows placed -> 71 trades, 527 commands, 614 rows placed) [W3B void exit]
//   brackets seed 2513497: 1774065971302770478 -> 5629628733882308382 (86 trades, 524 commands, 508 rows placed -> 72 trades, 507 commands, 525 rows placed) [W3B void exit]
//   brackets seed 2618226: 14075994758817023751 -> 14281581543217611485 (77 trades, 512 commands, 581 rows placed -> 73 trades, 508 commands, 558 rows placed) [W3B void exit]
//   brackets seed 2722955: 4051940090401488259 -> 6533341922887312293 (97 trades, 510 commands, 704 rows placed -> 93 trades, 490 commands, 657 rows placed) [W3 F05 rule A; W3B void exit; W4 F08]
//   brackets seed 2827684: 11315777065199433571 -> 11549075198333019681 (93 trades, 530 commands, 631 rows placed -> 66 trades, 557 commands, 594 rows placed) [W3B void exit]
//   brackets seed 2932413: 4716911134077760843 -> 15594624435547718078 (75 trades, 469 commands, 515 rows placed -> 73 trades, 509 commands, 552 rows placed) [W3B void exit]
//   brackets seed 3037142: 9921951745340606853 -> 8625746513086268806 (72 trades, 476 commands, 521 rows placed -> 64 trades, 507 commands, 528 rows placed) [W3B void exit]
//   brackets seed 3141871: 5531345307764558073 -> 9112443912224141062 (101 trades, 537 commands, 665 rows placed -> 95 trades, 558 commands, 645 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   brackets seed 3246600: 13285723008256739950 -> 15164668017906893228 (115 trades, 657 commands, 863 rows placed -> 124 trades, 633 commands, 797 rows placed) [W3 F05 rule A; W3B void exit; W4 F08]
//   brackets seed 3351329: 14331750725646803575 -> 8445431132239256351 (85 trades, 504 commands, 520 rows placed -> 86 trades, 543 commands, 615 rows placed) [W3B void exit]
//   brackets seed 3456058: 7444517548780988427 -> 17096340679869233820 (80 trades, 530 commands, 529 rows placed -> 73 trades, 530 commands, 558 rows placed) [W3B void exit]
//   brackets seed 3560787: 5589169095865416088 -> 5173892073438342031 (101 trades, 500 commands, 583 rows placed -> 96 trades, 506 commands, 588 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   brackets seed 3665516: 3100862192505875197 -> 13707670575754008369 (92 trades, 571 commands, 626 rows placed -> 75 trades, 559 commands, 694 rows placed) [W3B void exit]
//   brackets seed 3770245: 5122510577651000745 -> 11684879077355395000 (78 trades, 503 commands, 560 rows placed -> 79 trades, 510 commands, 573 rows placed) [W3B void exit; W4 F08]
//   chains seed 104731: 11442749230602752319 -> 4961901654803425155 (67 trades, 318 commands, 355 rows placed -> 67 trades, 318 commands, 348 rows placed) [W3B void exit; W4 F08]
//   chains seed 209460: 973054536545442904 -> 11415452759840164455 (66 trades, 384 commands, 419 rows placed -> 71 trades, 379 commands, 424 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B]
//   chains seed 314189: 14595591561869877759 -> 800157839082870006 (75 trades, 498 commands, 527 rows placed -> 83 trades, 491 commands, 516 rows placed) [W3 F05 rule A; W3B void exit; W8D, superseded by W8A R-B; W4 F19c; W8A R-B]
//   chains seed 418918: 13576086977686921552 -> 4820568811517812619 [W3B void exit]
//   chains seed 523647: 13312605232919440966 -> 146053814318818740 [W3B void exit]
//   chains seed 628376: 6547333340447752844 -> 17614793864897068161 (73 trades, 342 commands, 367 rows placed -> 73 trades, 342 commands, 350 rows placed) [W3B void exit; W4 F08]
//   chains seed 733105: 3389962382455065848 -> 4167100239005267285 (65 trades, 369 commands, 421 rows placed -> 67 trades, 369 commands, 415 rows placed) [W3B void exit; W8A R-B]
//   chains seed 837834: 10071629273929733187 -> 1658255311856021412 [W3B void exit]
//   chains seed 942563: 15248982654499518174 -> 6339945337346473638 [W3B void exit]
//   chains seed 1047292: 6473049927299403237 -> 730910484327473289 (34 trades, 363 commands, 400 rows placed -> 35 trades, 363 commands, 402 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   chains seed 1152021: 5657155561465537738 -> 7776152202237345240 (64 trades, 338 commands, 409 rows placed -> 72 trades, 351 commands, 401 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W4 F08; W8A R-B]
//   chains seed 1256750: 140926706188109974 -> 15009504967454947641 [W3B void exit]
//   chains seed 1361479: 1884320088982623449 -> 14955372577578561066 [W3B void exit]
//   chains seed 1466208: 17488515754754645248 -> 13676647888332124949 (78 trades, 390 commands, 407 rows placed -> 79 trades, 388 commands, 405 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B]
//   chains seed 1570937: 8944738996467660919 -> 202435186057658374 (68 trades, 353 commands, 354 rows placed -> 76 trades, 358 commands, 358 rows placed) [W3B void exit; W8A R-B; W8A R3]
//   chains seed 1675666: 3579325126147100396 -> 3770602179437410615 (64 trades, 314 commands, 355 rows placed -> 64 trades, 314 commands, 346 rows placed) [W3B void exit; W4 F08]
//   chains seed 1780395: 17646633536556052253 -> 14674860695032071620 (67 trades, 438 commands, 465 rows placed -> 67 trades, 440 commands, 482 rows placed) [W3B void exit; W8D]
//   chains seed 1885124: 7799970005878023626 -> 6942996273005923496 (62 trades, 362 commands, 378 rows placed -> 60 trades, 362 commands, 336 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R3]
//   chains seed 1989853: 3042300852731654308 -> 7007249540920133426 (75 trades, 350 commands, 372 rows placed -> 76 trades, 351 commands, 381 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B]
//   chains seed 2094582: 9104693962049720988 -> 9887369544729338515 [W3B void exit]
//   chains seed 2199311: 3043920897357867824 -> 4082722409939848804 (56 trades, 321 commands, 327 rows placed -> 56 trades, 321 commands, 328 rows placed) [W3B void exit; W4 F08; W8A R3]
//   chains seed 2304040: 17550770482504522165 -> 8018172237702962449 (61 trades, 330 commands, 349 rows placed -> 64 trades, 326 commands, 339 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit]
//   chains seed 2408769: 10315907993814478663 -> 10601587719066905856 (62 trades, 349 commands, 378 rows placed -> 63 trades, 349 commands, 377 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B]
//   chains seed 2513498: 11887874226824834835 -> 10758446752353699061 (50 trades, 344 commands, 365 rows placed -> 50 trades, 344 commands, 354 rows placed) [W3B void exit; W8A R3]
//   chains seed 2618227: 8569167112979511745 -> 5354971660748769171 [W3B void exit]
//   chains seed 2722956: 1052992062651979911 -> 2693529084803416692 (59 trades, 355 commands, 422 rows placed -> 59 trades, 355 commands, 391 rows placed) [W3B void exit; W4 F08; W8A R-B]
//   chains seed 2827685: 18246893670312018403 -> 8753155635371523673 (54 trades, 342 commands, 370 rows placed -> 74 trades, 341 commands, 400 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8E C1; W8A R-B]
//   chains seed 2932414: 16270472390219306311 -> 18215172146061067714 [W3B void exit]
//   chains seed 3037143: 14669408219364885155 -> 1905765453874616921 [W3B void exit]
//   chains seed 3141872: 4441359501225912121 -> 15456267325861807530 (59 trades, 364 commands, 395 rows placed -> 64 trades, 366 commands, 416 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W8A R-B]
//   chains seed 3246601: 4493812043353694436 -> 4424154706609241721 (73 trades, 444 commands, 515 rows placed -> 96 trades, 464 commands, 500 rows placed) [W3 F05 rule A, superseded by W3B void exit; W3B void exit; W4 F08; W4 F19c; W8A R-B]
//   chains seed 3351330: 10753585802392999956 -> 16665891776065465266 [W3B void exit]
//   chains seed 3456059: 9038807849693065880 -> 8183628840974573929 (43 trades, 310 commands, 298 rows placed -> 43 trades, 310 commands, 268 rows placed) [W3B void exit; W8A R3]
//   chains seed 3560788: 2170094277734349720 -> 3614987093622666609 (60 trades, 363 commands, 422 rows placed -> 61 trades, 363 commands, 426 rows placed) [W3B void exit; W8A R-B]
//   chains seed 3665517: 11220893490402964604 -> 11464826053545175105 (70 trades, 347 commands, 380 rows placed -> 72 trades, 347 commands, 376 rows placed) [W3B void exit; W8A R-B]
//   chains seed 3770246: 1535576416135278226 -> 6257524543885771248 (62 trades, 346 commands, 372 rows placed -> 63 trades, 346 commands, 364 rows placed) [W3B void exit; W5 C1; W4 F08]
// INT28-FIX re-harvested it once on the integrated tree (INT28 4efcb8c5 and
// INT28-FIX's ten commits) with PINEFORGE_V19E_HARVEST; the same harvest against
// INT28 reproduces every old row. 4 of 108 digests move, each marked with the
// commit(s) at whose boundary it moved (a harvest at every commit): only the
// picked W5B-ENG-MARGIN-RESIDUAL rules SB and PC move any; CA, PA, SZ, FU,
// MS, SS, OU and OF move none. The rules are TradingView's (their lanes'
// tests/fixtures tapes):
//   reversals seed 418916: 16010591667054285812 -> 8603512707567609453 (107 trades, 372 commands, 373 rows placed -> 106 trades, 372 commands, 373 rows placed) [W5B SB]
//   reversals seed 1675664: 10035800005689134556 -> 9885010638859166194 (111 trades, 379 commands, 394 rows placed -> 110 trades, 379 commands, 394 rows placed) [W5B SB]
//   reversals seed 2199309: 5727820387840827666 -> 3773617840179934238 (122 trades, 400 commands, 378 rows placed -> 122 trades, 400 commands, 380 rows placed) [W5B PC]
//   reversals seed 3351328: 14703653017612176717 -> 6358143307834022586 (108 trades, 414 commands, 363 rows placed -> 106 trades, 414 commands, 363 rows placed) [W5B SB]
// R5 lane TAIL-D re-harvested it the same way on its tree (8988faff and the
// lane's ten rule commits); the same harvest against 8988faff reproduces every
// old value. 6 of 108 digests move, and a harvest after each rule commit places
// each move at the commits named in brackets: M1 (a close the recalculation of
// a fill inside a leg places fills at the end of that leg, `immediately` or
// not, tests/fixtures/coof_immediate_close), M1e (every exit one path point
// triggers fills there before that point recalculates,
// tests/fixtures/coof_same_point_exits) and FP17 (an exit limit the
// recalculation of an entry's open fill places at or through the open print
// fills there, tests/fixtures/coof_open_limit):
//   reversals seed 314187: 2794849691207223034 -> 6746884236832554282 (235 trades, 723 commands, 742 rows placed -> 235 trades, 723 commands, 742 rows placed) [TAIL-D FP17]
//   brackets seed 314188: 9866841135197940589 -> 12119427392368104304 (120 trades, 788 commands, 950 rows placed -> 120 trades, 788 commands, 950 rows placed) [TAIL-D FP17]
//   brackets seed 1780394: 7524564435896309120 -> 6621438182284286369 (93 trades, 692 commands, 780 rows placed -> 92 trades, 671 commands, 752 rows placed) [TAIL-D M1e, FP17]
//   brackets seed 3246600: 15164668017906893228 -> 15943260418411472413 (124 trades, 633 commands, 797 rows placed -> 98 trades, 629 commands, 829 rows placed) [TAIL-D M1e]
//   chains seed 314189: 800157839082870006 -> 16211312595913436604 (83 trades, 491 commands, 516 rows placed -> 83 trades, 491 commands, 518 rows placed) [TAIL-D M1, FP17]
//   chains seed 3246601: 4424154706609241721 -> 13884755031629811470 (96 trades, 464 commands, 500 rows placed -> 96 trades, 464 commands, 498 rows placed) [TAIL-D M1]
// Lane TAIL-H re-harvested it once on its tree (engine 9e6196ca and the
// lane's commits) with PINEFORGE_V19E_HARVEST: the harvest on the lane's rule-PS commit
// reproduces every old value and the one on its rule-PK commit every new
// one, so each move is rule PK's (a margin call hands the position it leaves
// to the exits in their queue order again, tests/fixtures/exit_reservation);
// no count moves:
//   reversals seed 314187: 2794849691207223034 -> 14528917797583302432 (235 trades, 723 commands, 742 rows placed) [TAIL-H PK]
//   reversals seed 3141870: 15598437293592346454 -> 9350461461386796741 (66 trades, 347 commands, 260 rows placed) [TAIL-H PK]
//   reversals seed 3770244: 920766308841330226 -> 10085504415528449775 (34 trades, 321 commands, 223 rows placed) [TAIL-H PK]
//   chains seed 1570937: 202435186057658374 -> 7515518570456744171 (76 trades, 358 commands, 358 rows placed) [TAIL-H PK]
//   chains seed 1885124: 6942996273005923496 -> 15441784586956253521 (60 trades, 362 commands, 336 rows placed) [TAIL-H PK]
//   chains seed 2199311: 4082722409939848804 -> 2275122436928983111 (56 trades, 321 commands, 328 rows placed) [TAIL-H PK]
//   chains seed 3456059: 8183628840974573929 -> 17080496727403308679 (43 trades, 310 commands, 268 rows placed) [TAIL-H PK]
// R5 lane TAIL-C re-harvested it once on its tree (r5/tail-c: lane W8E-EXITS's
// rule A picked onto INT29 8988faff, then TAIL-C's six rules) with
// PINEFORGE_V19E_HARVEST; the same harvest against 8988faff reproduces every
// old row. 13 of 108 digests move, each marked with the commit(s) at whose
// boundary it moved (a harvest at every commit; W8E's rule A and TAIL-C's
// magnifier, admission and infinite-quantity rules move none; W2 = the refill
// after a fill forced onto a leg's end, OPEN = the exit stop born through by a
// later open fill, POOC = the close pass's priced adds):
//   reversals seed 104729: 12925325283399332139 -> 1790576907302357908 (131 trades, 397 commands, 438 rows placed -> 124 trades, 397 commands, 444 rows placed) [TAIL-C POOC]
//   reversals seed 314187: 2794849691207223034 -> 6369004299673343988 (235 trades, 723 commands, 742 rows placed -> 220 trades, 713 commands, 709 rows placed) [TAIL-C W2; TAIL-C OPEN]
//   reversals seed 628374: 4132505341042249934 -> 11954619287366675874 (124 trades, 404 commands, 394 rows placed -> 132 trades, 411 commands, 403 rows placed) [TAIL-C POOC]
//   reversals seed 1152019: 10462491692893518576 -> 10245107961889113516 (152 trades, 397 commands, 426 rows placed -> 141 trades, 392 commands, 444 rows placed) [TAIL-C POOC]
//   reversals seed 1675664: 9885010638859166194 -> 13300180944349916279 (110 trades, 379 commands, 394 rows placed -> 120 trades, 404 commands, 439 rows placed) [TAIL-C POOC]
//   reversals seed 1780393: 3777516334616247576 -> 145486145448837641 (192 trades, 691 commands, 650 rows placed -> 178 trades, 660 commands, 613 rows placed) [TAIL-C W2; TAIL-C OPEN]
//   reversals seed 2199309: 3773617840179934238 -> 15378875764722406544 (122 trades, 400 commands, 380 rows placed -> 123 trades, 401 commands, 391 rows placed) [TAIL-C POOC]
//   reversals seed 2722954: 9207129964188117780 -> 13574645891582671747 (154 trades, 400 commands, 456 rows placed -> 139 trades, 398 commands, 445 rows placed) [TAIL-C POOC]
//   reversals seed 3246599: 10688861328799387616 -> 2179811162140710357 (168 trades, 485 commands, 555 rows placed -> 156 trades, 467 commands, 537 rows placed) [TAIL-C POOC]
//   reversals seed 3770244: 920766308841330226 -> 15875985722928681968 (34 trades, 321 commands, 223 rows placed -> 34 trades, 321 commands, 226 rows placed) [TAIL-C POOC]
//   brackets seed 1780394: 7524564435896309120 -> 14543837208249092685 (93 trades, 692 commands, 780 rows placed -> 93 trades, 692 commands, 781 rows placed) [TAIL-C OPEN]
//   chains seed 314189: 800157839082870006 -> 1760223447026107653 (83 trades, 491 commands, 516 rows placed -> 81 trades, 487 commands, 505 rows placed) [TAIL-C W2]
//   chains seed 1780395: 14674860695032071620 -> 17472316981750834312 (67 trades, 440 commands, 482 rows placed -> 63 trades, 430 commands, 475 rows placed) [TAIL-C W2]
// R5 lane TAIL-E re-harvested it once on its tree with PINEFORGE_V19E_HARVEST.
// 3 of 108 digests move, all at its commit "an immediate close the bar's
// calculation executes is final for the bar" (the table held at the lane's
// time() commit 1b67b6c6: its debug ctest ran this row green). Each is a
// calc_on_order_fills configuration without process_orders_on_close that
// closes immediately (strategy.close(immediately = true)): TradingView runs no
// fill recalculation after such a close and fills an entry sent behind it at
// the next bar's open (tests/fixtures/coof_immediate_close):
//   reversals seed 1780393: 3777516334616247576 -> 1465470659302029377 (192 trades, 691 commands, 650 rows placed -> 191 trades, 700 commands, 665 rows placed)
//   chains seed 314189: 800157839082870006 -> 15999827188627897905 (83 trades, 491 commands, 516 rows placed -> 94 trades, 526 commands, 543 rows placed)
//   chains seed 1780395: 14674860695032071620 -> 1417054795560865178 (67 trades, 440 commands, 482 rows placed -> 73 trades, 456 commands, 505 rows placed)
// INT30 harvested it once more on the integrated tree (engine main 7239ab37
// with lanes TAIL-A, TAIL-B, TAIL-G, TAIL-D, TAIL-H, TAIL-E and TAIL-C picked,
// in that order) and at each lane boundary of that history. main reproduces
// every old value; TAIL-A, TAIL-B, TAIL-G, TAIL-H's rule PS, TAIL-E's time()
// rule and TAIL-C's infinite-quantity rule move none; 6 digests move at
// TAIL-D's picks, 7 at TAIL-H's rule PK, 13 at TAIL-C's picks and 4 at
// TAIL-E's immediate-close pick. A digest moved at one lane's picks alone
// equals that lane's value above, but for reversals seed 2199309, which
// TAIL-C's picks move to its counts and to another transcript than on its
// own tree. The runs below move at several lanes' picks; with those rules in
// one run each is no single lane's value (main -> the integrated tree):
//   reversals seed 314187: 2794849691207223034 -> 15896989768062521734 (235 trades, 723 commands, 742 rows placed -> 220 trades, 710 commands, 708 rows placed) [TAIL-D then TAIL-H PK then TAIL-C then TAIL-E M5]
//   reversals seed 1780393: 3777516334616247576 -> 7352429137180502973 (192 trades, 691 commands, 650 rows placed -> 177 trades, 669 commands, 628 rows placed) [TAIL-C then TAIL-E M5]
//   reversals seed 2199309: 3773617840179934238 -> 7434878463000589134 (122 trades, 400 commands, 380 rows placed -> 123 trades, 401 commands, 391 rows placed) [TAIL-C]
//   reversals seed 3770244: 920766308841330226 -> 5669930245333977489 (34 trades, 321 commands, 223 rows placed -> 34 trades, 321 commands, 226 rows placed) [TAIL-H PK then TAIL-C]
//   brackets seed 1780394: 7524564435896309120 -> 7591142039497383648 (93 trades, 692 commands, 780 rows placed -> 92 trades, 671 commands, 752 rows placed) [TAIL-D then TAIL-C]
//   chains seed 314189: 800157839082870006 -> 4910930847136591687 (83 trades, 491 commands, 516 rows placed -> 79 trades, 477 commands, 480 rows placed) [TAIL-D then TAIL-C then TAIL-E M5]
//   chains seed 1780395: 14674860695032071620 -> 1070411562087501788 (67 trades, 440 commands, 482 rows placed -> 57 trades, 436 commands, 463 rows placed) [TAIL-C then TAIL-E M5]
//
// The resting-limit tick reach re-harvested 9 values the same way, on its tree: a
// calc_on_order_fills limit exit that closes the position held when it is placed, on
// the chart path and not yet reached where it is placed, rests at its tick-built
// threshold (RestingLimitTickReach, src/compat/pine/callback_lifecycle_rules.hpp).
// Every moved configuration runs calc_on_order_fills, and a working request's trigger
// price is part of the transcript; every closed trade is the same in every field, as
// are the trades, commands and rows placed in each label (the same harvest against
// main 44eab7b1 reproduces every old value):
//   reversals seed 314187: 15896989768062521734 -> 18108469526809334996
//   reversals seed 1780393: 7352429137180502973 -> 2813325518340450122
//   reversals seed 3246599: 2179811162140710357 -> 18025952178484281790
//   brackets seed 314188: 12119427392368104304 -> 5626204446991468979
//   brackets seed 1780394: 7591142039497383648 -> 5509523710559050159
//   brackets seed 3246600: 15943260418411472413 -> 1519353229550935462
//   chains seed 314189: 4910930847136591687 -> 18266502754563434276
//   chains seed 1780395: 1070411562087501788 -> 4910402023661469991
//   chains seed 3246601: 13884755031629811470 -> 13715974753572872612
//
// Dropping the calc_on_order_fills competing chart-tick shift re-harvested 2 values the
// same way, on its tree: a stop or limit leg of a strategy.exit staged in a fill
// recalculation beside another key of the book keeps the trigger exit() installed
// instead of one moved half a tick outward (TradingView has no such exclusion:
// tests/fixtures/coof_competing_tick). Both configurations run calc_on_order_fills, and
// a working request's trigger price is part of the transcript: the only folded values
// that move are 10 (brackets, legs X_B) and 34 (chains, legs rel) trigger prices of exit
// request definitions, and the per-bar digests they feed. Every fill, every other fact
// of every command event, every pending order row, the position, the equity and every
// closed trade are the same in every field, as are the trades, commands and rows placed
// in each label (the same harvest against main 7a1f01c0 reproduces every old value):
//   brackets seed 314188: 5626204446991468979 -> 13444318351679181243
//   chains seed 314189: 18266502754563434276 -> 300926759266208037
constexpr std::uint64_t kTranscriptDigests[] = {
    1790576907302357908ull,  // reversals seed 104729, 124 trades, 397 commands, 444 rows placed
    10024640008279748788ull,  // reversals seed 209458, 124 trades, 379 commands, 418 rows placed
    18108469526809334996ull,  // reversals seed 314187, 220 trades, 710 commands, 708 rows placed
    8603512707567609453ull,  // reversals seed 418916, 106 trades, 372 commands, 373 rows placed
    16125481158763653444ull,  // reversals seed 523645, 104 trades, 417 commands, 380 rows placed
    11954619287366675874ull,  // reversals seed 628374, 132 trades, 411 commands, 403 rows placed
    10949395400367693376ull,  // reversals seed 733103, 138 trades, 387 commands, 412 rows placed
    2687164286151755975ull,  // reversals seed 837832, 90 trades, 401 commands, 369 rows placed
    6131133790994226055ull,  // reversals seed 942561, 98 trades, 408 commands, 367 rows placed
    12699224912122031164ull,  // reversals seed 1047290, 137 trades, 390 commands, 399 rows placed
    10245107961889113516ull,  // reversals seed 1152019, 141 trades, 392 commands, 444 rows placed
    10671261055934450290ull,  // reversals seed 1256748, 117 trades, 401 commands, 373 rows placed
    3043805792067546930ull,  // reversals seed 1361477, 100 trades, 409 commands, 390 rows placed
    11441719398179537978ull,  // reversals seed 1466206, 156 trades, 387 commands, 440 rows placed
    18024148252823409663ull,  // reversals seed 1570935, 69 trades, 344 commands, 269 rows placed
    13300180944349916279ull,  // reversals seed 1675664, 120 trades, 404 commands, 439 rows placed
    2813325518340450122ull,  // reversals seed 1780393, 177 trades, 669 commands, 628 rows placed
    2275854726471319605ull,  // reversals seed 1885122, 127 trades, 396 commands, 402 rows placed
    11265413284249527184ull,  // reversals seed 1989851, 171 trades, 429 commands, 451 rows placed
    8444874965488324792ull,  // reversals seed 2094580, 92 trades, 384 commands, 350 rows placed
    7434878463000589134ull,  // reversals seed 2199309, 123 trades, 401 commands, 391 rows placed
    16340996993447103239ull,  // reversals seed 2304038, 162 trades, 409 commands, 437 rows placed
    2756298568903753126ull,  // reversals seed 2408767, 138 trades, 418 commands, 419 rows placed
    8654109025937810515ull,  // reversals seed 2513496, 102 trades, 393 commands, 399 rows placed
    12615617675046204564ull,  // reversals seed 2618225, 105 trades, 412 commands, 400 rows placed
    13574645891582671747ull,  // reversals seed 2722954, 139 trades, 398 commands, 445 rows placed
    11406439758950114475ull,  // reversals seed 2827683, 45 trades, 340 commands, 197 rows placed
    11829483237718104981ull,  // reversals seed 2932412, 92 trades, 366 commands, 327 rows placed
    4678338477382770081ull,  // reversals seed 3037141, 114 trades, 405 commands, 409 rows placed
    9350461461386796741ull,  // reversals seed 3141870, 66 trades, 347 commands, 260 rows placed
    18025952178484281790ull,  // reversals seed 3246599, 156 trades, 467 commands, 537 rows placed
    6358143307834022586ull,  // reversals seed 3351328, 106 trades, 414 commands, 363 rows placed
    12370377506510123879ull,  // reversals seed 3456057, 87 trades, 373 commands, 351 rows placed
    4523288504224647910ull,  // reversals seed 3560786, 153 trades, 403 commands, 452 rows placed
    14246593818834397991ull,  // reversals seed 3665515, 136 trades, 393 commands, 424 rows placed
    5669930245333977489ull,  // reversals seed 3770244, 34 trades, 321 commands, 226 rows placed
    7562978076140294353ull,  // brackets seed 104730, 84 trades, 472 commands, 586 rows placed
    918119817585072101ull,  // brackets seed 209459, 79 trades, 588 commands, 642 rows placed
    13444318351679181243ull,  // brackets seed 314188, 120 trades, 788 commands, 950 rows placed
    345551147133871958ull,  // brackets seed 418917, 94 trades, 524 commands, 629 rows placed
    3598556121825612327ull,  // brackets seed 523646, 73 trades, 524 commands, 523 rows placed
    10787524866046025632ull,  // brackets seed 628375, 92 trades, 580 commands, 741 rows placed
    12647837381492911624ull,  // brackets seed 733104, 105 trades, 546 commands, 689 rows placed
    9142292326310325449ull,  // brackets seed 837833, 60 trades, 471 commands, 504 rows placed
    1575526034554365613ull,  // brackets seed 942562, 96 trades, 541 commands, 581 rows placed
    3479520250185906772ull,  // brackets seed 1047291, 84 trades, 508 commands, 578 rows placed
    14362220529398004157ull,  // brackets seed 1152020, 71 trades, 559 commands, 681 rows placed
    18185683915043225509ull,  // brackets seed 1256749, 85 trades, 510 commands, 578 rows placed
    12383757188086100027ull,  // brackets seed 1361478, 87 trades, 538 commands, 598 rows placed
    13395291126983886538ull,  // brackets seed 1466207, 86 trades, 571 commands, 692 rows placed
    17661091441404936538ull,  // brackets seed 1570936, 66 trades, 475 commands, 665 rows placed
    12371414418607433001ull,  // brackets seed 1675665, 74 trades, 501 commands, 551 rows placed
    5509523710559050159ull,  // brackets seed 1780394, 92 trades, 671 commands, 752 rows placed
    17554269931279056335ull,  // brackets seed 1885123, 93 trades, 540 commands, 738 rows placed
    14174662451420240772ull,  // brackets seed 1989852, 77 trades, 558 commands, 683 rows placed
    4473743503459375456ull,  // brackets seed 2094581, 73 trades, 520 commands, 601 rows placed
    5399518682810858268ull,  // brackets seed 2199310, 76 trades, 452 commands, 575 rows placed
    12383750543640558931ull,  // brackets seed 2304039, 89 trades, 548 commands, 663 rows placed
    5478903712594868650ull,  // brackets seed 2408768, 71 trades, 527 commands, 614 rows placed
    5629628733882308382ull,  // brackets seed 2513497, 72 trades, 507 commands, 525 rows placed
    14281581543217611485ull,  // brackets seed 2618226, 73 trades, 508 commands, 558 rows placed
    6533341922887312293ull,  // brackets seed 2722955, 93 trades, 490 commands, 657 rows placed
    11549075198333019681ull,  // brackets seed 2827684, 66 trades, 557 commands, 594 rows placed
    15594624435547718078ull,  // brackets seed 2932413, 73 trades, 509 commands, 552 rows placed
    8625746513086268806ull,  // brackets seed 3037142, 64 trades, 507 commands, 528 rows placed
    9112443912224141062ull,  // brackets seed 3141871, 95 trades, 558 commands, 645 rows placed
    1519353229550935462ull,  // brackets seed 3246600, 98 trades, 629 commands, 829 rows placed
    8445431132239256351ull,  // brackets seed 3351329, 86 trades, 543 commands, 615 rows placed
    17096340679869233820ull,  // brackets seed 3456058, 73 trades, 530 commands, 558 rows placed
    5173892073438342031ull,  // brackets seed 3560787, 96 trades, 506 commands, 588 rows placed
    13707670575754008369ull,  // brackets seed 3665516, 75 trades, 559 commands, 694 rows placed
    11684879077355395000ull,  // brackets seed 3770245, 79 trades, 510 commands, 573 rows placed
    4961901654803425155ull,  // chains seed 104731, 67 trades, 318 commands, 348 rows placed
    11415452759840164455ull,  // chains seed 209460, 71 trades, 379 commands, 424 rows placed
    300926759266208037ull,  // chains seed 314189, 79 trades, 477 commands, 480 rows placed
    4820568811517812619ull,  // chains seed 418918, 44 trades, 293 commands, 349 rows placed
    146053814318818740ull,  // chains seed 523647, 47 trades, 314 commands, 327 rows placed
    17614793864897068161ull,  // chains seed 628376, 73 trades, 342 commands, 350 rows placed
    4167100239005267285ull,  // chains seed 733105, 67 trades, 369 commands, 415 rows placed
    1658255311856021412ull,  // chains seed 837834, 60 trades, 368 commands, 418 rows placed
    6339945337346473638ull,  // chains seed 942563, 56 trades, 343 commands, 361 rows placed
    730910484327473289ull,  // chains seed 1047292, 35 trades, 363 commands, 402 rows placed
    7776152202237345240ull,  // chains seed 1152021, 72 trades, 351 commands, 401 rows placed
    15009504967454947641ull,  // chains seed 1256750, 64 trades, 314 commands, 315 rows placed
    14955372577578561066ull,  // chains seed 1361479, 65 trades, 309 commands, 326 rows placed
    13676647888332124949ull,  // chains seed 1466208, 79 trades, 388 commands, 405 rows placed
    7515518570456744171ull,  // chains seed 1570937, 76 trades, 358 commands, 358 rows placed
    3770602179437410615ull,  // chains seed 1675666, 64 trades, 314 commands, 346 rows placed
    4910402023661469991ull,  // chains seed 1780395, 57 trades, 436 commands, 463 rows placed
    15441784586956253521ull,  // chains seed 1885124, 60 trades, 362 commands, 336 rows placed
    7007249540920133426ull,  // chains seed 1989853, 76 trades, 351 commands, 381 rows placed
    9887369544729338515ull,  // chains seed 2094582, 37 trades, 310 commands, 353 rows placed
    2275122436928983111ull,  // chains seed 2199311, 56 trades, 321 commands, 328 rows placed
    8018172237702962449ull,  // chains seed 2304040, 64 trades, 326 commands, 339 rows placed
    10601587719066905856ull,  // chains seed 2408769, 63 trades, 349 commands, 377 rows placed
    10758446752353699061ull,  // chains seed 2513498, 50 trades, 344 commands, 354 rows placed
    5354971660748769171ull,  // chains seed 2618227, 51 trades, 339 commands, 386 rows placed
    2693529084803416692ull,  // chains seed 2722956, 59 trades, 355 commands, 391 rows placed
    8753155635371523673ull,  // chains seed 2827685, 74 trades, 341 commands, 400 rows placed
    18215172146061067714ull,  // chains seed 2932414, 62 trades, 336 commands, 373 rows placed
    1905765453874616921ull,  // chains seed 3037143, 61 trades, 344 commands, 412 rows placed
    15456267325861807530ull,  // chains seed 3141872, 64 trades, 366 commands, 416 rows placed
    13715974753572872612ull,  // chains seed 3246601, 96 trades, 464 commands, 498 rows placed
    16665891776065465266ull,  // chains seed 3351330, 45 trades, 301 commands, 354 rows placed
    17080496727403308679ull,  // chains seed 3456059, 43 trades, 310 commands, 268 rows placed
    3614987093622666609ull,  // chains seed 3560788, 61 trades, 363 commands, 426 rows placed
    11464826053545175105ull,  // chains seed 3665517, 72 trades, 347 commands, 376 rows placed
    6257524543885771248ull,  // chains seed 3770246, 63 trades, 346 commands, 364 rows placed
};
