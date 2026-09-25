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
constexpr std::uint64_t kTranscriptDigests[] = {
    14377401231487041547ull,  // reversals seed 104729, 131 trades, 397 commands, 435 rows placed
    8277084373591058696ull,  // reversals seed 209458, 123 trades, 379 commands, 419 rows placed
    2936193272230691901ull,  // reversals seed 314187, 168 trades, 628 commands, 582 rows placed
    6549991687676950761ull,  // reversals seed 418916, 107 trades, 381 commands, 383 rows placed
    9953188267307049090ull,  // reversals seed 523645, 104 trades, 417 commands, 379 rows placed
    16185846816223020088ull,  // reversals seed 628374, 126 trades, 412 commands, 399 rows placed
    4216355035770739779ull,  // reversals seed 733103, 138 trades, 387 commands, 413 rows placed
    6363178433100572906ull,  // reversals seed 837832, 91 trades, 401 commands, 366 rows placed
    1473820887994227184ull,  // reversals seed 942561, 98 trades, 408 commands, 366 rows placed
    9193554944507238062ull,  // reversals seed 1047290, 133 trades, 395 commands, 392 rows placed
    6981925030785360377ull,  // reversals seed 1152019, 151 trades, 397 commands, 421 rows placed
    5305403161869337627ull,  // reversals seed 1256748, 118 trades, 402 commands, 374 rows placed
    9475766261620659540ull,  // reversals seed 1361477, 106 trades, 411 commands, 389 rows placed
    4927715982682604365ull,  // reversals seed 1466206, 151 trades, 372 commands, 413 rows placed
    12202956723937736421ull,  // reversals seed 1570935, 69 trades, 344 commands, 273 rows placed
    1308277235911883001ull,  // reversals seed 1675664, 118 trades, 389 commands, 409 rows placed
    15749505417408196912ull,  // reversals seed 1780393, 196 trades, 702 commands, 665 rows placed
    16795999522017645296ull,  // reversals seed 1885122, 127 trades, 396 commands, 391 rows placed
    6779215636140632786ull,  // reversals seed 1989851, 173 trades, 429 commands, 449 rows placed
    15115396572137765888ull,  // reversals seed 2094580, 92 trades, 384 commands, 350 rows placed
    1053394249578594394ull,  // reversals seed 2199309, 124 trades, 398 commands, 378 rows placed
    314047600622510661ull,  // reversals seed 2304038, 149 trades, 412 commands, 440 rows placed
    3946903920860189031ull,  // reversals seed 2408767, 138 trades, 418 commands, 417 rows placed
    8490080330531136220ull,  // reversals seed 2513496, 103 trades, 392 commands, 393 rows placed
    12238138393722192163ull,  // reversals seed 2618225, 105 trades, 411 commands, 399 rows placed
    2863182994793154059ull,  // reversals seed 2722954, 152 trades, 394 commands, 448 rows placed
    13953002312094633174ull,  // reversals seed 2827683, 48 trades, 347 commands, 229 rows placed
    12936186547753035500ull,  // reversals seed 2932412, 96 trades, 360 commands, 330 rows placed
    11138119210196651599ull,  // reversals seed 3037141, 118 trades, 408 commands, 398 rows placed
    11186738496014205060ull,  // reversals seed 3141870, 116 trades, 402 commands, 404 rows placed
    7365380738509267810ull,  // reversals seed 3246599, 147 trades, 455 commands, 507 rows placed
    8856317689282633932ull,  // reversals seed 3351328, 108 trades, 414 commands, 363 rows placed
    1680641833169632427ull,  // reversals seed 3456057, 87 trades, 373 commands, 350 rows placed
    17785508903938760497ull,  // reversals seed 3560786, 147 trades, 397 commands, 435 rows placed
    17135221856069256209ull,  // reversals seed 3665515, 132 trades, 392 commands, 402 rows placed
    16348052548195448490ull,  // reversals seed 3770244, 53 trades, 339 commands, 267 rows placed
    14413849309182271769ull,  // brackets seed 104730, 82 trades, 521 commands, 568 rows placed
    17836979205695001440ull,  // brackets seed 209459, 76 trades, 487 commands, 569 rows placed
    6185985077652624317ull,  // brackets seed 314188, 144 trades, 818 commands, 919 rows placed
    3737662390130165609ull,  // brackets seed 418917, 94 trades, 557 commands, 632 rows placed
    10672250305191215856ull,  // brackets seed 523646, 71 trades, 517 commands, 451 rows placed
    1855891803821441106ull,  // brackets seed 628375, 101 trades, 524 commands, 724 rows placed
    12279391351944236297ull,  // brackets seed 733104, 91 trades, 520 commands, 628 rows placed
    5785846610847751202ull,  // brackets seed 837833, 88 trades, 482 commands, 547 rows placed
    7040999793872498538ull,  // brackets seed 942562, 85 trades, 490 commands, 457 rows placed
    14314528608775687405ull,  // brackets seed 1047291, 85 trades, 468 commands, 547 rows placed
    13142231282207717561ull,  // brackets seed 1152020, 76 trades, 478 commands, 687 rows placed
    15849491031983505500ull,  // brackets seed 1256749, 88 trades, 511 commands, 566 rows placed
    9918882490509451201ull,  // brackets seed 1361478, 81 trades, 540 commands, 556 rows placed
    16303049545172805242ull,  // brackets seed 1466207, 70 trades, 568 commands, 669 rows placed
    8808473759868459315ull,  // brackets seed 1570936, 69 trades, 475 commands, 647 rows placed
    15751662755974333447ull,  // brackets seed 1675665, 87 trades, 520 commands, 602 rows placed
    5435028051816123314ull,  // brackets seed 1780394, 95 trades, 697 commands, 705 rows placed
    355520822114888679ull,  // brackets seed 1885123, 89 trades, 542 commands, 583 rows placed
    5402310364412118994ull,  // brackets seed 1989852, 89 trades, 505 commands, 639 rows placed
    11372035822747614056ull,  // brackets seed 2094581, 85 trades, 496 commands, 575 rows placed
    14176875954190827756ull,  // brackets seed 2199310, 79 trades, 446 commands, 504 rows placed
    15236878832940726852ull,  // brackets seed 2304039, 94 trades, 577 commands, 623 rows placed
    4629113575017480307ull,  // brackets seed 2408768, 78 trades, 528 commands, 676 rows placed
    1774065971302770478ull,  // brackets seed 2513497, 86 trades, 524 commands, 508 rows placed
    14075994758817023751ull,  // brackets seed 2618226, 77 trades, 512 commands, 581 rows placed
    4051940090401488259ull,  // brackets seed 2722955, 97 trades, 510 commands, 704 rows placed
    11315777065199433571ull,  // brackets seed 2827684, 93 trades, 530 commands, 631 rows placed
    4716911134077760843ull,  // brackets seed 2932413, 75 trades, 469 commands, 515 rows placed
    9921951745340606853ull,  // brackets seed 3037142, 72 trades, 476 commands, 521 rows placed
    5531345307764558073ull,  // brackets seed 3141871, 101 trades, 537 commands, 665 rows placed
    13285723008256739950ull,  // brackets seed 3246600, 115 trades, 657 commands, 863 rows placed
    14331750725646803575ull,  // brackets seed 3351329, 85 trades, 504 commands, 520 rows placed
    7444517548780988427ull,  // brackets seed 3456058, 80 trades, 530 commands, 529 rows placed
    5589169095865416088ull,  // brackets seed 3560787, 101 trades, 500 commands, 583 rows placed
    3100862192505875197ull,  // brackets seed 3665516, 92 trades, 571 commands, 626 rows placed
    5122510577651000745ull,  // brackets seed 3770245, 78 trades, 503 commands, 560 rows placed
    11442749230602752319ull,  // chains seed 104731, 67 trades, 318 commands, 355 rows placed
    973054536545442904ull,  // chains seed 209460, 66 trades, 384 commands, 419 rows placed
    3920721464232107171ull,  // chains seed 314189, 62 trades, 465 commands, 490 rows placed
    13576086977686921552ull,  // chains seed 418918, 44 trades, 293 commands, 349 rows placed
    13312605232919440966ull,  // chains seed 523647, 47 trades, 314 commands, 327 rows placed
    6547333340447752844ull,  // chains seed 628376, 73 trades, 342 commands, 367 rows placed
    3389962382455065848ull,  // chains seed 733105, 65 trades, 369 commands, 421 rows placed
    10071629273929733187ull,  // chains seed 837834, 60 trades, 368 commands, 418 rows placed
    15248982654499518174ull,  // chains seed 942563, 56 trades, 343 commands, 361 rows placed
    6473049927299403237ull,  // chains seed 1047292, 34 trades, 363 commands, 400 rows placed
    5657155561465537738ull,  // chains seed 1152021, 64 trades, 338 commands, 409 rows placed
    140926706188109974ull,  // chains seed 1256750, 64 trades, 314 commands, 315 rows placed
    1884320088982623449ull,  // chains seed 1361479, 65 trades, 309 commands, 326 rows placed
    17488515754754645248ull,  // chains seed 1466208, 78 trades, 390 commands, 407 rows placed
    8944738996467660919ull,  // chains seed 1570937, 68 trades, 353 commands, 354 rows placed
    3579325126147100396ull,  // chains seed 1675666, 64 trades, 314 commands, 355 rows placed
    17646633536556052253ull,  // chains seed 1780395, 67 trades, 438 commands, 465 rows placed
    7799970005878023626ull,  // chains seed 1885124, 62 trades, 362 commands, 378 rows placed
    3042300852731654308ull,  // chains seed 1989853, 75 trades, 350 commands, 372 rows placed
    9104693962049720988ull,  // chains seed 2094582, 37 trades, 310 commands, 353 rows placed
    3043920897357867824ull,  // chains seed 2199311, 56 trades, 321 commands, 327 rows placed
    17550770482504522165ull,  // chains seed 2304040, 61 trades, 330 commands, 349 rows placed
    10315907993814478663ull,  // chains seed 2408769, 62 trades, 349 commands, 378 rows placed
    11887874226824834835ull,  // chains seed 2513498, 50 trades, 344 commands, 365 rows placed
    8569167112979511745ull,  // chains seed 2618227, 51 trades, 339 commands, 386 rows placed
    1052992062651979911ull,  // chains seed 2722956, 59 trades, 355 commands, 422 rows placed
    18246893670312018403ull,  // chains seed 2827685, 54 trades, 342 commands, 370 rows placed
    16270472390219306311ull,  // chains seed 2932414, 62 trades, 336 commands, 373 rows placed
    14669408219364885155ull,  // chains seed 3037143, 61 trades, 344 commands, 412 rows placed
    4441359501225912121ull,  // chains seed 3141872, 59 trades, 364 commands, 395 rows placed
    4493812043353694436ull,  // chains seed 3246601, 73 trades, 444 commands, 515 rows placed
    10753585802392999956ull,  // chains seed 3351330, 45 trades, 301 commands, 354 rows placed
    9038807849693065880ull,  // chains seed 3456059, 43 trades, 310 commands, 298 rows placed
    2170094277734349720ull,  // chains seed 3560788, 60 trades, 363 commands, 422 rows placed
    11220893490402964604ull,  // chains seed 3665517, 70 trades, 347 commands, 380 rows placed
    1535576416135278226ull,  // chains seed 3770246, 62 trades, 346 commands, 372 rows placed
};
