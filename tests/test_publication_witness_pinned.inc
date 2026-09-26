// R5 lane D2-D: the values tests/test_publication_witness.cpp pins, one
// row per run of its battery (name, digest, source folds, closed trades,
// recorded broker-state hash rows).
// Harvested on 6c081f5d, re-harvested once for V19-D's v4 fold (INT23),
// once for K-ULP3's whole-lot close, once for V19-FIX's hash step and once
// for PAR-MARGIN-2's post-fill margin path: see the provenance note in the
// test.
// Generated -- never edit a row by hand.
// R5 lane PAR-ORDERS re-harvested it the same way on its tree (21 of 156 runs move;
// see the lane's hash commit for why each moves):
//   ConfigFlags113: digest 0x2a4b7804a0a3e27e -> 0xb0d7186d612e1063
//   ConfigFlags113M: digest 0x88c14690db445933 -> 0x066c20032094d9bf
//   Storm01: digest 0x75c523aefe67f41a -> 0x2940f65451a3189f
//   Storm01M: digest 0x5ea8235c86419d28 -> 0xbfc8780fbe24d51c
//   Storm06: digest 0x52fe595453aed8f6 -> 0x4053ebf00e230b53
//   Storm06M: digest 0x570cecdbe9d2d0c0 -> 0x4d4c9f8c75e1901b
//   Storm09: digest 0xe0004089dbf5f4ae -> 0x09195d4f14ec28dd
//   Storm09M: digest 0x977b07e1c905757a -> 0x5a6cd7deec2033ff
//   Storm10: digest 0xe7a0f75a29b2bd10 -> 0x7ae3261a9cbdeff3
//   Storm10M: digest 0xf3739b7278c799f2 -> 0x03595faccb3309cd
//   Storm11: digest 0x144d68a22896e3bf -> 0xf46684d86365208a, trades 24 -> 22
//   Storm11M: digest 0xcbb7769e3a9bf7c6 -> 0xb5205907e08e01bf, trades 24 -> 22
//   Storm13: digest 0x327d8e77ac1de02c -> 0xde3f322bf0c59a98
//   Storm13M: digest 0x6d1a8b0e3f3199b3 -> 0xf7d902eba9385f60
//   Storm17M: digest 0x00de1819d109b846 -> 0x46e2d77c04f174e3
//   Storm18: digest 0xcb801349463a0b9a -> 0xe7e874c21bf9c5fc
//   Storm18M: digest 0xc661ff254ed1d0b0 -> 0x75b39ccdb9b4542d
//   Storm19: digest 0x33d7bba6e955b14a -> 0x8c49334fb012a382
//   Storm19M: digest 0x6e51f913b73007d5 -> 0x83ea12d967fea7e7
//   Storm22: digest 0xce56d5badb05638c -> 0xda5de455f464504d
//   Storm22M: digest 0xa8b971a9116eb495 -> 0xfdaa191a97b50614
// R5 lane H-THIN's P10 moves the closed trades of 12 runs and the source folds
// of Storm17 and Storm17M (INT26: the behaviour half of its hash step ab297364,
// harvested on the integrated tree, every value equal to the lane's); every
// digest moves once, in the INT26 v19 hash re-pin:
//   Storm04: trades 20 -> 22
//   Storm04M: trades 20 -> 22
//   Storm07: trades 15 -> 13
//   Storm07M: trades 15 -> 13
//   Storm17: folds 82 -> 86, trades 21 -> 23
//   Storm17M: folds 77 -> 80, trades 17 -> 18
//   Storm18: trades 25 -> 23
//   Storm18M: trades 25 -> 23
//   Storm19: trades 22 -> 23
//   Storm19M: trades 22 -> 23
//   Storm23: trades 27 -> 26
//   Storm23M: trades 27 -> 26
// R5 lane PAR-CASHFEE moves the closed trades of five runs (INT26: the behaviour
// half of its hash step 214c082c, harvested on the integrated tree, each value
// equal to the lane's; its Config03 50 -> 48 was already pinned with R5 lane
// PAR-MARGIN-2); every digest moves once, in the INT26 v19 hash re-pin:
//   ConfigFlags103: trades 65 -> 63
//   Config03M: trades 48 -> 44
//   ConfigFlags103M: trades 66 -> 65
//   ConfigFlags104: trades 12 -> 13
//   ConfigFlags104M: trades 12 -> 13
// expectation corrected (INT26 v19 hash re-pin, 156 values over 156 rows), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; the source folds, closed trades and recorded rows did not move:
//   Config01: digest 0x13599d44631228e2ull -> 0xb668af594ea2f5d9ull [H-THIN]
//   ConfigFlags101: digest 0x9d75aa3c41307c6aull -> 0xe7f27c137f45ac4bull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Config01M: digest 0x3ebbd6d78581edeaull -> 0x1efcbeef8aa1dc6dull [H-THIN]
//   ConfigFlags101M: digest 0x347646d359e21807ull -> 0xbee912e49e616f4eull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Config02: digest 0x6dde6b5c5440d715ull -> 0x341cf20ba6b96fc3ull [H-THIN]
//   ConfigFlags102: digest 0x91619a8b469ea6aeull -> 0xef9792602fc551c1ull [H-THIN]
//   Config02M: digest 0xd04764a2d7139486ull -> 0x4983b8719bbfd192ull [H-THIN]
//   ConfigFlags102M: digest 0x854413a4a59d0d33ull -> 0xeed018756d1228e9ull [H-THIN]
//   Config03: digest 0x461a919ebdcd41e9ull -> 0x0fc93105580e996eull [PAR-MARGIN-2, H-THIN, PAR-CASHFEE]
//   ConfigFlags103: digest 0xcf60de51b3acfcc3ull -> 0x9e412b4fb357ae55ull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Config03M: digest 0x4511d5cd864e706full -> 0x2e2ff1ccdf841c8dull [H-THIN, PAR-CASHFEE]
//   ConfigFlags103M: digest 0xdd7937eb65073a91ull -> 0xa68e0a951db111ddull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Config04: digest 0xa0a03448564c9be0ull -> 0x63aa9da27d89b846ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags104: digest 0xaf23be135bf66e22ull -> 0xefbb1c6bfcb31f17ull [H-THIN, PAR-CASHFEE]
//   Config04M: digest 0xe0f138c9c848b160ull -> 0xba3069b5b515a320ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags104M: digest 0xbf637ed723b215beull -> 0x0b1d2d1d8096f6b9ull [H-THIN, PAR-CASHFEE]
//   Config05: digest 0x17fc80e7cecd9737ull -> 0x705de70e2d70af67ull [H-THIN]
//   ConfigFlags105: digest 0x270ebbdd8cfa9d63ull -> 0x69dac2f5ec1d400cull [H-THIN]
//   Config05M: digest 0x3e5a9fcbb1fdcd5eull -> 0x7f5b8acc384eb6daull [H-THIN]
//   ConfigFlags105M: digest 0x680bdc701517be1aull -> 0x3e23588769a2f97bull [H-THIN]
//   Config06: digest 0x19f57666c4c34f11ull -> 0x9f061ce6918f93c5ull [H-THIN]
//   ConfigFlags106: digest 0x006c443d5ac275d3ull -> 0xb5e0aaa404ca93b3ull [H-THIN]
//   Config06M: digest 0x9b6058dc837818a5ull -> 0x1ef42058337147b8ull [H-THIN]
//   ConfigFlags106M: digest 0xdd38d808b0b2005full -> 0x3f82500e8ed30156ull [H-THIN]
//   Config07: digest 0xd91ff183f38d87ceull -> 0x86c366bc672888faull [H-THIN, PAR-CASHFEE]
//   ConfigFlags107: digest 0x579a2bc891f60711ull -> 0xbe9ea2687b1b417aull [H-THIN, PAR-CASHFEE]
//   Config07M: digest 0x4225ca6a6ba7619eull -> 0x0bdf3ca651b69092ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags107M: digest 0xca1facefefeb3fceull -> 0x2a5b0d3d56b2cd03ull [H-THIN, PAR-CASHFEE]
//   Config08: digest 0x904781b92a51efd9ull -> 0xa23953fc578a6b43ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags108: digest 0xccc9a3e471b37be3ull -> 0x10e8434816e387afull [H-THIN]
//   Config08M: digest 0xf227bccff87ad1d6ull -> 0x1f60bb9c965465d8ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags108M: digest 0xf38c8ecf14fb7075ull -> 0x2271fc69af564e4cull [H-THIN]
//   Config09: digest 0xc4943ac1cca48c3eull -> 0x9360d69c97c9db7bull [H-THIN, PAR-CASHFEE]
//   ConfigFlags109: digest 0x33cf69335b29701full -> 0x30a8c80d3bd0ad61ull [H-THIN, PAR-CASHFEE]
//   Config09M: digest 0xd60f90586ad5c873ull -> 0x9e5b912398d35af8ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags109M: digest 0xc446a8edce72ed55ull -> 0x5a3876b995a66c76ull [H-THIN, PAR-CASHFEE]
//   Config10: digest 0xd207636458a7977dull -> 0x18f9e53074280722ull [H-THIN]
//   ConfigFlags110: digest 0x1be5ae2e695135fdull -> 0x248ead6c9dbf66b7ull [H-THIN, PAR-CASHFEE]
//   Config10M: digest 0x9e63b9317b0ee761ull -> 0x4be74baf8fc46644ull [H-THIN]
//   ConfigFlags110M: digest 0x7aabfb779282bb56ull -> 0x54a9f81cdce9d069ull [H-THIN, PAR-CASHFEE]
//   Config11: digest 0x777e1f3e0fba2d04ull -> 0xb150c52332d168ffull [H-THIN]
//   ConfigFlags111: digest 0x4722a0ba517a8698ull -> 0x32f4fb6b779cc554ull [H-THIN]
//   Config11M: digest 0x33ccb718a4da671eull -> 0xc8005b52c87cf782ull [H-THIN]
//   ConfigFlags111M: digest 0xd39fafb2fda05ff0ull -> 0xdb5300166665f9c1ull [H-THIN]
//   Config12: digest 0x4b461c21259a2e32ull -> 0xc51092de174909d7ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags112: digest 0x87321f2899dad267ull -> 0x68af9f1a740c44e9ull [H-THIN]
//   Config12M: digest 0xd27a9a9ee92fb498ull -> 0x3edbdba7792b3233ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags112M: digest 0xa9764e2ca9500901ull -> 0xfd4565803d79a56bull [H-THIN]
//   Config13: digest 0x5c4aa53ee5c2d4b9ull -> 0x254b2221a515f9e7ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags113: digest 0xb0d7186d612e1063ull -> 0x6c6163d6eb022517ull [H-THIN, PAR-CASHFEE]
//   Config13M: digest 0x5ba51d9728c119b8ull -> 0x2e78fa9fbc049dedull [H-THIN, PAR-CASHFEE]
//   ConfigFlags113M: digest 0x066c20032094d9bfull -> 0x6e9f5a9ece8a9c58ull [H-THIN, PAR-CASHFEE]
//   Config14: digest 0x6e95423cd9f2042aull -> 0x523695406e13e23cull [H-THIN, PAR-CASHFEE]
//   ConfigFlags114: digest 0x96a34f2bb0dc2df1ull -> 0x65aeb7fa3c3ab6bfull [H-THIN, PAR-CASHFEE]
//   Config14M: digest 0x3c6809a3326959b5ull -> 0x9a3809499b3623c6ull [H-THIN, PAR-CASHFEE]
//   ConfigFlags114M: digest 0xb11d614113e0ee96ull -> 0x63e140a82186a98dull [H-THIN, PAR-CASHFEE]
//   Config15: digest 0x9072bdfaf315f3b2ull -> 0xad1c7175dc94f6bcull [H-THIN]
//   ConfigFlags115: digest 0x9049c3bebf6874f2ull -> 0x0141076a069b89e3ull [H-THIN]
//   Config15M: digest 0x21730b1c555a3119ull -> 0x1344d28fa92876b9ull [H-THIN]
//   ConfigFlags115M: digest 0x2af33a20163cc455ull -> 0xd8441700cc46211dull [H-THIN]
//   Config16: digest 0x5b7b329438d68317ull -> 0x2533d8cb0bd677c4ull [H-THIN]
//   ConfigFlags116: digest 0x112cac461ad5e409ull -> 0xf6100c192c1b1a47ull [H-THIN, PAR-CASHFEE]
//   Config16M: digest 0x74174f7e187f36a9ull -> 0xfff00bd45398ff09ull [H-THIN]
//   ConfigFlags116M: digest 0xfee39f4d6acb5dd9ull -> 0x4e9b7658af4a3968ull [H-THIN, PAR-CASHFEE]
//   CloseAllThenEntry: digest 0x64d991ad87b59143ull -> 0xc9a6feca41e82e29ull [H-THIN]
//   CloseAllThenEntryM: digest 0x768b725ffcc7ca57ull -> 0xd0aa4f83cfd909c6ull [H-THIN]
//   Reversals: digest 0xf5db5c66ecd1dbd2ull -> 0x23a3b43853bb2e1aull [H-THIN]
//   ReversalsM: digest 0x36b59c9ef1737981ull -> 0x29160b951dc12d18ull [H-THIN]
//   SameBarBatch: digest 0x13e5941fa9151818ull -> 0x67372c2ba662f6c8ull [H-THIN]
//   SameBarBatchM: digest 0x10604224255b732aull -> 0x5517e412f630d8c8ull [H-THIN]
//   CloseOrderAdds: digest 0xa7d126e01939e06dull -> 0xe5d547fd7ea6eb99ull [H-THIN]
//   CloseOrderAddsM: digest 0x72762eb6d5e1230cull -> 0x2fc90068db6aa341ull [H-THIN]
//   DelayedStops: digest 0xa655cdf979a2d473ull -> 0xf698a0012daa63b3ull [PAR-ORDERS-2, H-THIN]
//   DelayedStopsM: digest 0x1d34635f60c4dbdbull -> 0x046314c00141b54cull [PAR-ORDERS-2, H-THIN]
//   Storm01: digest 0x2940f65451a3189full -> 0x0d6f6de3707c0f23ull [H-THIN]
//   Storm01M: digest 0xbfc8780fbe24d51cull -> 0x17fdf11d7effc7f0ull [H-THIN]
//   Storm02: digest 0x951345d2c3fa83a0ull -> 0x5ab7e89bf0a32e17ull [PAR-ORDERS-2, H-THIN]
//   Storm02M: digest 0x1521264d78122ebeull -> 0xae05144418043f11ull [PAR-ORDERS-2, H-THIN]
//   Storm03: digest 0x25f90b9eeb5f057aull -> 0x1734fbc73a88fdafull [H-THIN]
//   Storm03M: digest 0xe4da66913719f57aull -> 0x87784b723c37ec66ull [H-THIN]
//   Storm04: digest 0x3157bd59f491a789ull -> 0x33ceb62ad976d38aull [H-THIN]
//   Storm04M: digest 0xa1dd548b8f00457eull -> 0xb06e7fe199931e4aull [H-THIN]
//   Storm05: digest 0x8eacccdcac89765cull -> 0x00106a73f4035d2bull [PAR-ORDERS-2, H-THIN]
//   Storm05M: digest 0x67d9decf4dd40cb4ull -> 0xdef761ffd71cac33ull [PAR-ORDERS-2, H-THIN]
//   Storm06: digest 0x4053ebf00e230b53ull -> 0x6bfcdec22c1a1c8full [H-THIN]
//   Storm06M: digest 0x4d4c9f8c75e1901bull -> 0x44fe7480122bc682ull [H-THIN]
//   Storm07: digest 0xf7e280a190e60133ull -> 0xacff98851a92d473ull [H-THIN]
//   Storm07M: digest 0x3b29ea97a5b2dd0aull -> 0x196a55d2091dc429ull [H-THIN]
//   Storm08: digest 0x9302e1c04c2dfedfull -> 0x03ba3cc50503307dull [PAR-ORDERS-2, H-THIN]
//   Storm08M: digest 0x8944facb400c0524ull -> 0xcdb1ebd3491998e5ull [PAR-ORDERS-2, H-THIN]
//   Storm09: digest 0x09195d4f14ec28ddull -> 0xc5746cc50cf41375ull [H-THIN]
//   Storm09M: digest 0x5a6cd7deec2033ffull -> 0x8aa89637b4d04cf0ull [H-THIN]
//   Storm10: digest 0x7ae3261a9cbdeff3ull -> 0x2697bb1c5389db54ull [PAR-ORDERS-2, H-THIN]
//   Storm10M: digest 0x03595faccb3309cdull -> 0x741a981667c1a302ull [PAR-ORDERS-2, H-THIN]
//   Storm11: digest 0xf46684d86365208aull -> 0x7cfb0ba740c2806bull [H-THIN]
//   Storm11M: digest 0xb5205907e08e01bfull -> 0xcfc4532d945da385ull [H-THIN]
//   Storm12: digest 0x8b151167a2310837ull -> 0x659852f60d3d358cull [PAR-ORDERS-2, H-THIN]
//   Storm12M: digest 0xc49a83bf5709682bull -> 0xdb3db1a969f1b59aull [PAR-ORDERS-2, H-THIN]
//   Storm13: digest 0xde3f322bf0c59a98ull -> 0xe613ff94ad1aad2aull [H-THIN]
//   Storm13M: digest 0xf7d902eba9385f60ull -> 0xf7793304d1dbd532ull [H-THIN]
//   Storm14: digest 0x1ac3a18ac2e4ed4bull -> 0x0b1595a2d45290f2ull [H-THIN]
//   Storm14M: digest 0xd88e046854060dd6ull -> 0x9e9a3044d9dbcad8ull [H-THIN]
//   Storm15: digest 0x6229ca73e971d1c1ull -> 0xc2912804f77858fbull [H-THIN]
//   Storm15M: digest 0xa648d45a63e9a8afull -> 0xf32358c4a29e8066ull [H-THIN]
//   Storm16: digest 0x8918c41ad3b998bdull -> 0x9f128f923d41d2e7ull [H-THIN]
//   Storm16M: digest 0x6b216cf99a1880dfull -> 0x72b90fec2e8871d7ull [H-THIN]
//   Storm17: digest 0x644e0ad8e5fd4833ull -> 0xd62326206a38e5f5ull [PAR-ORDERS-2, H-THIN]
//   Storm17M: digest 0x46e2d77c04f174e3ull -> 0x9631182cfd1c6023ull [PAR-ORDERS-2, H-THIN]
//   Storm18: digest 0xe7e874c21bf9c5fcull -> 0x86e8b0cc2cbda4aeull [PAR-ORDERS-2, H-THIN]
//   Storm18M: digest 0x75b39ccdb9b4542dull -> 0xd9fb27e46899ccd7ull [PAR-ORDERS-2, H-THIN]
//   Storm19: digest 0x8c49334fb012a382ull -> 0xe92413fbdc464242ull [PAR-ORDERS-2, H-THIN]
//   Storm19M: digest 0x83ea12d967fea7e7ull -> 0xa9a584e70d470321ull [PAR-ORDERS-2, H-THIN]
//   Storm20: digest 0xe93eebdfc01f57b4ull -> 0x93de366f51f2315dull [PAR-ORDERS-2, H-THIN]
//   Storm20M: digest 0xfdb5a657600e2375ull -> 0x201adc8ab4ea9bdbull [PAR-ORDERS-2, H-THIN]
//   Storm21: digest 0xc0aad15a28128003ull -> 0xd4a8eda3db34c6eeull [H-THIN]
//   Storm21M: digest 0xbc3fbaacf5272ad1ull -> 0xefaa36484eb9c7a1ull [H-THIN]
//   Storm22: digest 0xda5de455f464504dull -> 0x686821a1adb13279ull [PAR-ORDERS-2, H-THIN]
//   Storm22M: digest 0xfdaa191a97b50614ull -> 0x28a2c0fefdac67afull [PAR-ORDERS-2, H-THIN]
//   Storm23: digest 0xb91e295d6da6778cull -> 0xfac7dcb54dc93113ull [PAR-ORDERS-2, H-THIN]
//   Storm23M: digest 0x78f7d0134456384full -> 0xe66e15ba3a360e59ull [PAR-ORDERS-2, H-THIN]
//   Storm24: digest 0x6f40ccb0266ca278ull -> 0x9a05f4cc2a37c8b8ull [H-THIN]
//   Storm24M: digest 0xa2eeb9f1d0e10619ull -> 0xa1f1acfcca37ef51ull [H-THIN]
//   Legs01: digest 0xb34653b540b649dfull -> 0x2257c1f1b017aff6ull [H-THIN]
//   Legs01M: digest 0xafbbc030265c824full -> 0xdeeb42cf30042b94ull [H-THIN]
//   LegsStops01: digest 0x622c768899d0febaull -> 0xe82893532dd3da24ull [H-THIN]
//   LegsStops01M: digest 0xd71b800ba07f1670ull -> 0x40cde458e42af83dull [H-THIN]
//   Legs02: digest 0xe6cecd70ee4e816cull -> 0x290854a540c0effdull [H-THIN]
//   Legs02M: digest 0x9932159d4daf8f18ull -> 0xa31bffc389143eaaull [H-THIN]
//   LegsStops02: digest 0x032c556130d84ee8ull -> 0x7aa8ab78f1f5ada3ull [H-THIN]
//   LegsStops02M: digest 0xb2d41090d3afed86ull -> 0xa1d4657aea1b015dull [H-THIN]
//   Legs03: digest 0xe484f43d3f03e46aull -> 0x4a813820bb3a558dull [H-THIN]
//   Legs03M: digest 0x5a7256077ac9caf7ull -> 0xc10d45c7cb3cbee5ull [H-THIN]
//   LegsStops03: digest 0xd1cb8cd8c6459831ull -> 0xc02aaa3f7c355e7aull [H-THIN]
//   LegsStops03M: digest 0x6f87644ed30f1f84ull -> 0x55a7fe1ea0fc100aull [H-THIN]
//   Legs04: digest 0x9405751b42b03526ull -> 0x254b1a89ea0388ddull [H-THIN]
//   Legs04M: digest 0xf80fabe52a3a310bull -> 0x079706336deb5323ull [H-THIN]
//   LegsStops04: digest 0x3b9823513ae000abull -> 0xc6a5620646c92b3cull [H-THIN]
//   LegsStops04M: digest 0x939d11e8a52ef4a6ull -> 0xbcad7c107485fbe8ull [H-THIN]
//   Legs05: digest 0xbe440458b5a03c6bull -> 0x2b3d7db72415c3ecull [H-THIN]
//   Legs05M: digest 0xab877ee06eeef9deull -> 0x88fd0678c5020748ull [H-THIN]
//   LegsStops05: digest 0x9dd37a12391bd5f1ull -> 0x94435fe6bea6a39dull [H-THIN]
//   LegsStops05M: digest 0x5895c3b5e20f05d2ull -> 0x2722fdb103c71c42ull [H-THIN]
//   Legs06: digest 0x8273cddc66300d47ull -> 0x6e7d43e693bec002ull [H-THIN]
//   Legs06M: digest 0x38b98f46b0793f2cull -> 0x2fc1828cff3085f2ull [H-THIN]
//   LegsStops06: digest 0x5eafd9de638208b3ull -> 0x198f7d7e224884c0ull [H-THIN]
//   LegsStops06M: digest 0x06d7e046fb01bdf4ull -> 0x507c89b50b4d4b2bull [H-THIN]
//   TwoEntries: digest 0x8c5be2d95d0be7e5ull -> 0x06f2ecb7c1a3a480ull [H-THIN]
//   TwoEntriesM: digest 0xfbbd9f232e1ccfbcull -> 0x7c956e01ced10ccbull [H-THIN]
//   PyramidLots: digest 0x196e1c6f6d80b3feull -> 0x243689d4eeaf8d3bull [H-THIN]
//   PyramidLotsM: digest 0xc771a9a6a70374c6ull -> 0x7c425d1716c9a5a2ull [H-THIN]
//   LegBesideClose: digest 0x805b56fa470a4dbeull -> 0xfd494f8d3e8228d4ull [H-THIN]
//   LegBesideCloseM: digest 0x8e0bb72d36ce8063ull -> 0x4a9e3df3ac353419ull [H-THIN]
//   TwoLegsOnly: digest 0x0ed47ce9f98c045aull -> 0x1e97c755ea7f6e5full [H-THIN]
//   TwoLegsOnlyM: digest 0xa713e15d8063372full -> 0x8b4283e07b576d5full [H-THIN]
//   LegsApart: digest 0xcfcd1f1d87803a44ull -> 0x64ff417b6bcf212full [H-THIN]
//   LegsApartM: digest 0x701aca7df9071fb5ull -> 0xaa0c742ddeacc4d5ull [H-THIN]
constexpr Pinned kPinned[] = {
    {"Config01", 0xb668af594ea2f5d9ull, 144, 44, 144},
    {"ConfigFlags101", 0xe7f27c137f45ac4bull, 145, 45, 144},
    {"Config01M", 0x1efcbeef8aa1dc6dull, 144, 42, 144},
    {"ConfigFlags101M", 0xbee912e49e616f4eull, 145, 45, 144},
    {"Config02", 0x341cf20ba6b96fc3ull, 48, 11, 48},
    {"ConfigFlags102", 0xef9792602fc551c1ull, 144, 52, 144},
    {"Config02M", 0x4983b8719bbfd192ull, 48, 11, 48},
    {"ConfigFlags102M", 0xeed018756d1228e9ull, 144, 52, 144},
    {"Config03", 0x0fc93105580e996eull, 144, 48, 144},
    {"ConfigFlags103", 0x9e412b4fb357ae55ull, 184, 63, 144},
    {"Config03M", 0x2e2ff1ccdf841c8dull, 144, 44, 144},
    {"ConfigFlags103M", 0xa68e0a951db111ddull, 184, 65, 144},
    {"Config04", 0x63aa9da27d89b846ull, 144, 21, 144},
    {"ConfigFlags104", 0xefbb1c6bfcb31f17ull, 48, 13, 48},
    {"Config04M", 0xba3069b5b515a320ull, 144, 21, 144},
    {"ConfigFlags104M", 0x0b1d2d1d8096f6b9ull, 48, 13, 48},
    {"Config05", 0x705de70e2d70af67ull, 48, 14, 48},
    {"ConfigFlags105", 0x69dac2f5ec1d400cull, 48, 13, 48},
    {"Config05M", 0x7f5b8acc384eb6daull, 48, 14, 48},
    {"ConfigFlags105M", 0x3e23588769a2f97bull, 48, 13, 48},
    {"Config06", 0x9f061ce6918f93c5ull, 48, 5, 48},
    {"ConfigFlags106", 0xb5e0aaa404ca93b3ull, 48, 6, 48},
    {"Config06M", 0x1ef42058337147b8ull, 48, 5, 48},
    {"ConfigFlags106M", 0x3f82500e8ed30156ull, 48, 6, 48},
    {"Config07", 0x86c366bc672888faull, 144, 48, 144},
    {"ConfigFlags107", 0xbe9ea2687b1b417aull, 48, 20, 48},
    {"Config07M", 0x0bdf3ca651b69092ull, 144, 48, 144},
    {"ConfigFlags107M", 0x2a5b0d3d56b2cd03ull, 48, 21, 48},
    {"Config08", 0xa23953fc578a6b43ull, 48, 12, 48},
    {"ConfigFlags108", 0x10e8434816e387afull, 48, 15, 48},
    {"Config08M", 0x1f60bb9c965465d8ull, 48, 12, 48},
    {"ConfigFlags108M", 0x2271fc69af564e4cull, 48, 15, 48},
    {"Config09", 0x9360d69c97c9db7bull, 144, 66, 144},
    {"ConfigFlags109", 0x30a8c80d3bd0ad61ull, 144, 42, 144},
    {"Config09M", 0x9e5b912398d35af8ull, 144, 66, 144},
    {"ConfigFlags109M", 0x5a3876b995a66c76ull, 144, 42, 144},
    {"Config10", 0x18f9e53074280722ull, 48, 18, 48},
    {"ConfigFlags110", 0x248ead6c9dbf66b7ull, 48, 11, 48},
    {"Config10M", 0x4be74baf8fc46644ull, 48, 18, 48},
    {"ConfigFlags110M", 0x54a9f81cdce9d069ull, 48, 11, 48},
    {"Config11", 0xb150c52332d168ffull, 144, 60, 144},
    {"ConfigFlags111", 0x32f4fb6b779cc554ull, 48, 19, 48},
    {"Config11M", 0xc8005b52c87cf782ull, 144, 60, 144},
    {"ConfigFlags111M", 0xdb5300166665f9c1ull, 48, 19, 48},
    {"Config12", 0xc51092de174909d7ull, 144, 48, 144},
    {"ConfigFlags112", 0x68af9f1a740c44e9ull, 144, 18, 144},
    {"Config12M", 0x3edbdba7792b3233ull, 144, 48, 144},
    {"ConfigFlags112M", 0xfd4565803d79a56bull, 144, 18, 144},
    {"Config13", 0x254b2221a515f9e7ull, 48, 19, 48},
    {"ConfigFlags113", 0x6c6163d6eb022517ull, 48, 19, 48},
    {"Config13M", 0x2e78fa9fbc049dedull, 48, 19, 48},
    {"ConfigFlags113M", 0x6e9f5a9ece8a9c58ull, 48, 19, 48},
    {"Config14", 0x523695406e13e23cull, 144, 40, 144},
    {"ConfigFlags114", 0x65aeb7fa3c3ab6bfull, 144, 60, 144},
    {"Config14M", 0x9a3809499b3623c6ull, 144, 40, 144},
    {"ConfigFlags114M", 0x63e140a82186a98dull, 144, 60, 144},
    {"Config15", 0xad1c7175dc94f6bcull, 144, 48, 144},
    {"ConfigFlags115", 0x0141076a069b89e3ull, 48, 6, 48},
    {"Config15M", 0x1344d28fa92876b9ull, 144, 48, 144},
    {"ConfigFlags115M", 0xd8441700cc46211dull, 48, 6, 48},
    {"Config16", 0x2533d8cb0bd677c4ull, 144, 54, 144},
    {"ConfigFlags116", 0xf6100c192c1b1a47ull, 48, 19, 48},
    {"Config16M", 0xfff00bd45398ff09ull, 144, 54, 144},
    {"ConfigFlags116M", 0x4e9b7658af4a3968ull, 48, 19, 48},
    {"CloseAllThenEntry", 0xc9a6feca41e82e29ull, 40, 17, 40},
    {"CloseAllThenEntryM", 0xd0aa4f83cfd909c6ull, 40, 17, 40},
    {"Reversals", 0x23a3b43853bb2e1aull, 40, 16, 40},
    {"ReversalsM", 0x29160b951dc12d18ull, 40, 16, 40},
    {"SameBarBatch", 0x67372c2ba662f6c8ull, 36, 25, 36},
    {"SameBarBatchM", 0x5517e412f630d8c8ull, 36, 25, 36},
    {"CloseOrderAdds", 0xe5d547fd7ea6eb99ull, 36, 22, 36},
    {"CloseOrderAddsM", 0x2fc90068db6aa341ull, 36, 22, 36},
    {"DelayedStops", 0xf698a0012daa63b3ull, 66, 20, 40},
    {"DelayedStopsM", 0x046314c00141b54cull, 66, 20, 40},
    {"Storm01", 0x0d6f6de3707c0f23ull, 43, 13, 43},
    {"Storm01M", 0x17fdf11d7effc7f0ull, 43, 14, 43},
    {"Storm02", 0x5ab7e89bf0a32e17ull, 63, 12, 42},
    {"Storm02M", 0xae05144418043f11ull, 63, 12, 42},
    {"Storm03", 0x1734fbc73a88fdafull, 41, 17, 41},
    {"Storm03M", 0x87784b723c37ec66ull, 41, 17, 41},
    {"Storm04", 0x33ceb62ad976d38aull, 54, 22, 54},
    {"Storm04M", 0xb06e7fe199931e4aull, 54, 22, 54},
    {"Storm05", 0x00106a73f4035d2bull, 81, 17, 53},
    {"Storm05M", 0xdef761ffd71cac33ull, 82, 17, 53},
    {"Storm06", 0x6bfcdec22c1a1c8full, 40, 11, 40},
    {"Storm06M", 0x44fe7480122bc682ull, 40, 11, 40},
    {"Storm07", 0xacff98851a92d473ull, 55, 13, 55},
    {"Storm07M", 0x196a55d2091dc429ull, 55, 13, 55},
    {"Storm08", 0x03ba3cc50503307dull, 81, 14, 58},
    {"Storm08M", 0xcdb1ebd3491998e5ull, 81, 14, 58},
    {"Storm09", 0xc5746cc50cf41375ull, 48, 21, 48},
    {"Storm09M", 0x8aa89637b4d04cf0ull, 48, 20, 48},
    {"Storm10", 0x2697bb1c5389db54ull, 65, 16, 57},
    {"Storm10M", 0x741a981667c1a302ull, 66, 16, 57},
    {"Storm11", 0x7cfb0ba740c2806bull, 59, 22, 59},
    {"Storm11M", 0xcfc4532d945da385ull, 59, 22, 59},
    {"Storm12", 0x659852f60d3d358cull, 86, 20, 56},
    {"Storm12M", 0xdb3db1a969f1b59aull, 86, 20, 56},
    {"Storm13", 0xe613ff94ad1aad2aull, 46, 23, 46},
    {"Storm13M", 0xf7793304d1dbd532ull, 46, 23, 46},
    {"Storm14", 0x0b1595a2d45290f2ull, 46, 18, 46},
    {"Storm14M", 0x9e9a3044d9dbcad8ull, 46, 18, 46},
    {"Storm15", 0xc2912804f77858fbull, 43, 13, 43},
    {"Storm15M", 0xf32358c4a29e8066ull, 43, 14, 43},
    {"Storm16", 0x9f128f923d41d2e7ull, 58, 18, 58},
    {"Storm16M", 0x72b90fec2e8871d7ull, 58, 18, 58},
    {"Storm17", 0xd62326206a38e5f5ull, 86, 23, 54},
    {"Storm17M", 0x9631182cfd1c6023ull, 80, 18, 54},
    {"Storm18", 0x86e8b0cc2cbda4aeull, 80, 23, 49},
    {"Storm18M", 0xd9fb27e46899ccd7ull, 80, 23, 49},
    {"Storm19", 0xe92413fbdc464242ull, 86, 23, 49},
    {"Storm19M", 0xa9a584e70d470321ull, 86, 23, 49},
    {"Storm20", 0x93de366f51f2315dull, 45, 12, 42},
    {"Storm20M", 0x201adc8ab4ea9bdbull, 45, 12, 42},
    {"Storm21", 0xd4a8eda3db34c6eeull, 47, 14, 47},
    {"Storm21M", 0xefaa36484eb9c7a1ull, 47, 14, 47},
    {"Storm22", 0x686821a1adb13279ull, 76, 19, 50},
    {"Storm22M", 0x28a2c0fefdac67afull, 76, 19, 50},
    {"Storm23", 0xfac7dcb54dc93113ull, 89, 26, 54},
    {"Storm23M", 0xe66e15ba3a360e59ull, 89, 26, 54},
    {"Storm24", 0x9a05f4cc2a37c8b8ull, 54, 25, 54},
    {"Storm24M", 0xa1f1acfcca37ef51ull, 54, 25, 54},
    {"Legs01", 0x2257c1f1b017aff6ull, 30, 6, 30},
    {"Legs01M", 0xdeeb42cf30042b94ull, 30, 6, 30},
    {"LegsStops01", 0xe82893532dd3da24ull, 30, 6, 30},
    {"LegsStops01M", 0x40cde458e42af83dull, 30, 6, 30},
    {"Legs02", 0x290854a540c0effdull, 30, 6, 30},
    {"Legs02M", 0xa31bffc389143eaaull, 30, 6, 30},
    {"LegsStops02", 0x7aa8ab78f1f5ada3ull, 30, 6, 30},
    {"LegsStops02M", 0xa1d4657aea1b015dull, 30, 6, 30},
    {"Legs03", 0x4a813820bb3a558dull, 30, 6, 30},
    {"Legs03M", 0xc10d45c7cb3cbee5ull, 30, 6, 30},
    {"LegsStops03", 0xc02aaa3f7c355e7aull, 30, 6, 30},
    {"LegsStops03M", 0x55a7fe1ea0fc100aull, 30, 6, 30},
    {"Legs04", 0x254b1a89ea0388ddull, 30, 6, 30},
    {"Legs04M", 0x079706336deb5323ull, 30, 6, 30},
    {"LegsStops04", 0xc6a5620646c92b3cull, 30, 6, 30},
    {"LegsStops04M", 0xbcad7c107485fbe8ull, 30, 6, 30},
    {"Legs05", 0x2b3d7db72415c3ecull, 30, 6, 30},
    {"Legs05M", 0x88fd0678c5020748ull, 30, 6, 30},
    {"LegsStops05", 0x94435fe6bea6a39dull, 30, 6, 30},
    {"LegsStops05M", 0x2722fdb103c71c42ull, 30, 6, 30},
    {"Legs06", 0x6e7d43e693bec002ull, 30, 6, 30},
    {"Legs06M", 0x2fc1828cff3085f2ull, 30, 6, 30},
    {"LegsStops06", 0x198f7d7e224884c0ull, 30, 6, 30},
    {"LegsStops06M", 0x507c89b50b4d4b2bull, 30, 6, 30},
    {"TwoEntries", 0x06f2ecb7c1a3a480ull, 30, 2, 30},
    {"TwoEntriesM", 0x7c956e01ced10ccbull, 30, 2, 30},
    {"PyramidLots", 0x243689d4eeaf8d3bull, 30, 2, 30},
    {"PyramidLotsM", 0x7c425d1716c9a5a2ull, 30, 2, 30},
    {"LegBesideClose", 0xfd494f8d3e8228d4ull, 30, 2, 30},
    {"LegBesideCloseM", 0x4a9e3df3ac353419ull, 30, 2, 30},
    {"TwoLegsOnly", 0x1e97c755ea7f6e5full, 30, 2, 30},
    {"TwoLegsOnlyM", 0x8b4283e07b576d5full, 30, 2, 30},
    {"LegsApart", 0x64ff417b6bcf212full, 30, 2, 30},
    {"LegsApartM", 0xaa0c742ddeacc4d5ull, 30, 2, 30},
};
