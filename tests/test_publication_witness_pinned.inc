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
// expectation corrected (INT27 v19 hash re-pin, 1 values over 1 rows), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane PAR-ORDERS-3's recalculation rules (findings 1-2, an exit placed on a fill at a bar's extreme reads that fill; finding 4, after the matcher's own-level fill on a bar's first extreme the recalculation's orders act from that extreme); old values are INT26's pins, which every INT27 pick before PAR-ORDERS-3 kept (0631e045 reproduces each), harvested with this TU's own switch on the integrated tree and at every PAR-ORDERS-3 commit; the source folds, closed trades and recorded rows did not move:
//   ConfigFlags103: digest 0x9e412b4fb357ae55ull -> 0x275174bdfd481a1bull [PAR-ORDERS-3 f1-2, PAR-ORDERS-3 f4]
// INT28 re-harvested it once on the integrated tree (main 962960b3 and the
// wave-J picks) with PINEFORGE_D2D_PUBLICATION_HARVEST; the same harvest
// against main reproduces every old row. 142 of 156 runs move, each
// marked with the pick(s) at whose boundary it moved (a harvest at every
// pick); an instrumented copy of the adapter (scratch only) shows that
// pick's rule firing in the run on the final tree, or, marked superseded,
// at the boundary before the named later pick and not after it:
//   Config01: digest 0xb668af594ea2f5d9 -> 0x2338d6688ca58837 [W3B F02 first id; W3B void exit]
//   ConfigFlags101: digest 0xe7f27c137f45ac4b -> 0x6b3262a158c0e86d [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config01M: digest 0x1efcbeef8aa1dc6d -> 0x8b63a249e4bcbf1f [W3B void exit]
//   ConfigFlags101M: digest 0xbee912e49e616f4e -> 0xae3b2b9fc4e23750 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config02: digest 0x341cf20ba6b96fc3 -> 0x9fc3f24e211e1726, trades 11 -> 10 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R-A]
//   ConfigFlags102: digest 0xef9792602fc551c1 -> 0x443b33f233df5591, trades 52 -> 49 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config02M: digest 0x4983b8719bbfd192 -> 0x8cddc71b29406412, trades 11 -> 10 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R-A]
//   ConfigFlags102M: digest 0xeed018756d1228e9 -> 0x000ba0de0f306cb9, trades 52 -> 49 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config03: digest 0x0fc93105580e996e -> 0xc5424169be207aa2, trades 48 -> 53 [W3B F02 first id; W3B void exit; W8A R-B]
//   ConfigFlags103: digest 0x275174bdfd481a1b -> 0x70b81ca88d79dcc3, folds 184 -> 182, trades 63 -> 61 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W5 MK; W4 F19c; R1-CONSOLIDATE R1]
//   Config03M: digest 0x2e2ff1ccdf841c8d -> 0xea3de174c93cccc5, trades 44 -> 49 [W3B F02 first id; W3B void exit; W8A R-B]
//   ConfigFlags103M: digest 0xa68e0a951db111dd -> 0x8f0124d4ee2a443c, folds 184 -> 182, trades 65 -> 63 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W5 MK; R1-CONSOLIDATE R1]
//   Config04: digest 0x63aa9da27d89b846 -> 0x5fc85948d1cc71eb [W3B F02 ledger booking; W3B void exit]
//   ConfigFlags104: digest 0xefbb1c6bfcb31f17 -> 0x97b7894a32af23f1, trades 13 -> 14 [W3B F02 first id; W3B void exit; W8A R-B]
//   Config04M: digest 0xba3069b5b515a320 -> 0x87cc9f67ed62f28e [W3B F02 ledger booking; W3B void exit]
//   ConfigFlags104M: digest 0x0b1d2d1d8096f6b9 -> 0x3850e6440128d893, trades 13 -> 14 [W3B F02 first id; W3B void exit; W8A R-B]
//   Config05: digest 0x705de70e2d70af67 -> 0x85c421ae2f21507f, trades 14 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   ConfigFlags105: digest 0x69dac2f5ec1d400c -> 0xa103dd79855f049c, trades 13 -> 12 [W3B F02 ledger booking; W3B void exit]
//   Config05M: digest 0x7f5b8acc384eb6da -> 0x8422d41a3aa8dee1, trades 14 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   ConfigFlags105M: digest 0x3e23588769a2f97b -> 0x43e37c3b91360e9a, trades 13 -> 12 [W3B F02 ledger booking; W3B void exit]
//   Config06: digest 0x9f061ce6918f93c5 -> 0xd365aa9c3705ae1f [W3B void exit]
//   ConfigFlags106: digest 0xb5e0aaa404ca93b3 -> 0x4fc0738f0271278c [W3B void exit; W5 C1; W4 F08]
//   Config06M: digest 0x1ef42058337147b8 -> 0x21e19f868e2ac1a1 [W3B void exit]
//   ConfigFlags106M: digest 0x3f82500e8ed30156 -> 0x83a9bde980d36054 [W3B void exit; W5 C1; W4 F08]
//   Config07: digest 0x86c366bc672888fa -> 0xa2e64f9925787e69 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags107: digest 0xbe9ea2687b1b417a -> 0xabda13a612cf944d, trades 20 -> 22 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Config07M: digest 0x0bdf3ca651b69092 -> 0x9a4813d5ba021f90 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags107M: digest 0x2a5b0d3d56b2cd03 -> 0xca421b83f6380179, trades 21 -> 23 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Config08: digest 0xa23953fc578a6b43 -> 0xde6e8977d6fccc6f [W3B void exit]
//   ConfigFlags108: digest 0x10e8434816e387af -> 0xe78d704b74e4f695 [W3B void exit; W4 F08]
//   Config08M: digest 0x1f60bb9c965465d8 -> 0xe9dabaa4abe5ce0d [W3B void exit]
//   ConfigFlags108M: digest 0x2271fc69af564e4c -> 0x75aac168f00f1801 [W3B void exit; W4 F08]
//   Config09: digest 0x9360d69c97c9db7b -> 0xe6ce749e6d011633 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags109: digest 0x30a8c80d3bd0ad61 -> 0x18457ecdd3b9059b, trades 42 -> 45 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8A R-B]
//   Config09M: digest 0x9e5b912398d35af8 -> 0xcad60c482f2dadb6 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags109M: digest 0x5a3876b995a66c76 -> 0xb8b2405ade532a1a, trades 42 -> 45 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8A R-B]
//   Config10: digest 0x18f9e53074280722 -> 0x89d9ee8fcfdb8d2b [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags110: digest 0x248ead6c9dbf66b7 -> 0xfd01004606da1f60, trades 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Config10M: digest 0x4be74baf8fc46644 -> 0x27b4e0d84ceb64c8 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags110M: digest 0x54a9f81cdce9d069 -> 0xff6fa6e9e28a9bdf, trades 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Config11: digest 0xb150c52332d168ff -> 0xb20c8cbfea616264 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags111: digest 0x32f4fb6b779cc554 -> 0xcc9cdc745a08b222, trades 19 -> 17 [W3B F02 first id; W3B void exit; W4 F08]
//   Config11M: digest 0xc8005b52c87cf782 -> 0x3c15e549c4ca8b3b [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags111M: digest 0xdb5300166665f9c1 -> 0xbb8e81d92e1268bc, trades 19 -> 17 [W3B F02 first id; W3B void exit; W4 F08]
//   Config12: digest 0xc51092de174909d7 -> 0x8f89d528e22725e5 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags112: digest 0x68af9f1a740c44e9 -> 0xeffa667f3295fb5d, trades 18 -> 19 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1]
//   Config12M: digest 0x3edbdba7792b3233 -> 0x7628205f68612a98 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags112M: digest 0xfd4565803d79a56b -> 0x16a09b30a0769034, trades 18 -> 19 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1]
//   Config13: digest 0x254b2221a515f9e7 -> 0x4404022d43781ea0 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags113: digest 0x6c6163d6eb022517 -> 0xbd64633fe83e7319 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config13M: digest 0x2e78fa9fbc049ded -> 0x2ae2c70980ac0fd2 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags113M: digest 0x6e9f5a9ece8a9c58 -> 0xeb38b6811f6a8bbf [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Config14: digest 0x523695406e13e23c -> 0xa3f9a925e91a4f7c [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags114: digest 0x65aeb7fa3c3ab6bf -> 0xb481578151a6b65b, trades 60 -> 66 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W4 MCSIZE; W8A R3; R1-CONSOLIDATE R1]
//   Config14M: digest 0x9a3809499b3623c6 -> 0x5289594882c86a97 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags114M: digest 0x63e140a82186a98d -> 0xe22c1fc0d8f6feea, trades 60 -> 66 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W4 MCSIZE; W8A R3; R1-CONSOLIDATE R1]
//   Config15: digest 0xad1c7175dc94f6bc -> 0x186642fe4a546b57 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   ConfigFlags115: digest 0x0141076a069b89e3 -> 0x2498f0b2c19691b0 [W3B void exit]
//   Config15M: digest 0x1344d28fa92876b9 -> 0xf26e418f92af8c2a [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   ConfigFlags115M: digest 0xd8441700cc46211d -> 0xf689ea3140af17e6 [W3B void exit]
//   Config16: digest 0x2533d8cb0bd677c4 -> 0x40c001863790e4bb [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags116: digest 0xf6100c192c1b1a47 -> 0x6ea5c86901105686, trades 19 -> 21 [W3B F02 first id; W3B void exit; W8A R-B]
//   Config16M: digest 0xfff00bd45398ff09 -> 0x06b36e850ad09548 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   ConfigFlags116M: digest 0x4e9b7658af4a3968 -> 0xf4f9194225b2a7af, trades 19 -> 21 [W3B F02 first id; W3B void exit; W8A R-B]
//   CloseAllThenEntry: digest 0xc9a6feca41e82e29 -> 0x279fa9da8fb40549 [W3B F02 first id]
//   CloseAllThenEntryM: digest 0xd0aa4f83cfd909c6 -> 0xc6baa19d8a587d60 [W3B F02 first id]
//   SameBarBatch: digest 0x67372c2ba662f6c8 -> 0x167ef4263f9ae286 [W3B F02 first id; W3B F02 ledger booking]
//   SameBarBatchM: digest 0x5517e412f630d8c8 -> 0x488225822bf669d0 [W3B F02 first id; W3B F02 ledger booking]
//   CloseOrderAdds: digest 0xe5d547fd7ea6eb99 -> 0x1b5718bfccc88d5b [W3B F02 ledger booking]
//   CloseOrderAddsM: digest 0x2fc90068db6aa341 -> 0xbab50be39f3d0b28 [W3B F02 ledger booking]
//   DelayedStops: digest 0xf698a0012daa63b3 -> 0x43bed101b5c70825 [W3B F02 first id; W8D]
//   DelayedStopsM: digest 0x046314c00141b54c -> 0xeebabaa7a380fb84 [W3B F02 first id]
//   Storm01: digest 0x0d6f6de3707c0f23 -> 0x4fcc1a18f96048be [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm01M: digest 0x17fdf11d7effc7f0 -> 0x00cdd69222a72577 [W3B F02 first id; W3B void exit]
//   Storm02: digest 0x5ab7e89bf0a32e17 -> 0xc147c03e2f6038fe [W3B void exit; W4 F19c]
//   Storm02M: digest 0xae05144418043f11 -> 0x745faf07c1b57317 [W3B void exit]
//   Storm03: digest 0x1734fbc73a88fdaf -> 0x662d5ebe04f3918f [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm03M: digest 0x87784b723c37ec66 -> 0x4b0d6e52556ad140 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm04: digest 0x33ceb62ad976d38a -> 0x07c17443f491fb30, trades 22 -> 24 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm04M: digest 0xb06e7fe199931e4a -> 0x74d239698dac5c24, trades 22 -> 24 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm05: digest 0x00106a73f4035d2b -> 0x8c67fec9d289937d, folds 81 -> 80 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F19c]
//   Storm05M: digest 0xdef761ffd71cac33 -> 0xfbdf760609e41925, trades 17 -> 18 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm06: digest 0x6bfcdec22c1a1c8f -> 0x64b975d6442847b8, trades 11 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm06M: digest 0x44fe7480122bc682 -> 0x7c34e84d996f62b1, trades 11 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm07: digest 0xacff98851a92d473 -> 0x8c50963e152ddba8 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm07M: digest 0x196a55d2091dc429 -> 0xa75412c0c52f6ac1 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm08: digest 0x03ba3cc50503307d -> 0x3c2a5b9c43c4fd89 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm08M: digest 0xcdb1ebd3491998e5 -> 0x7bfabcd35099442c [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm09: digest 0xc5746cc50cf41375 -> 0x8087e1c554317200, trades 21 -> 23 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm09M: digest 0x8aa89637b4d04cf0 -> 0x31d4f5fc5411c31f, trades 20 -> 22 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm10: digest 0x2697bb1c5389db54 -> 0xf7007d9d5ea3296e, folds 65 -> 63, trades 16 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm10M: digest 0x741a981667c1a302 -> 0x862537a926bed30e, folds 66 -> 64, trades 16 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm11: digest 0x7cfb0ba740c2806b -> 0x0780237b158be286, trades 22 -> 23 [W3B void exit; W4 F08]
//   Storm11M: digest 0xcfc4532d945da385 -> 0x1932b3271b2aaaba, trades 22 -> 23 [W3B void exit; W4 F08]
//   Storm12: digest 0x659852f60d3d358c -> 0x2f7a3a3d3b832e58, folds 86 -> 85, trades 20 -> 19 [W3B F02 first id; W3B F02 ledger booking, superseded by R1-CONSOLIDATE R1; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Storm12M: digest 0xdb3db1a969f1b59a -> 0x6a900d649bc55ad8, folds 86 -> 85, trades 20 -> 19 [W3B F02 first id; W3B F02 ledger booking, superseded by R1-CONSOLIDATE R1; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Storm13: digest 0xe613ff94ad1aad2a -> 0x97f4432edb19a7e9, trades 23 -> 25 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm13M: digest 0xf7793304d1dbd532 -> 0x1170d6964f4f90ea, trades 23 -> 25 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm14: digest 0x0b1595a2d45290f2 -> 0x5ac38849fb40f365, trades 18 -> 17 [W3B void exit; W4 F08]
//   Storm14M: digest 0x9e9a3044d9dbcad8 -> 0xc90b1227bf0f3b60, trades 18 -> 17 [W3B void exit; W4 F08]
//   Storm15: digest 0xc2912804f77858fb -> 0x86667ae71fa7953d [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm15M: digest 0xf32358c4a29e8066 -> 0x1d30c61ffbbb4270 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm16: digest 0x9f128f923d41d2e7 -> 0x73e4d05238964e31 [W3B void exit; W4 F08]
//   Storm16M: digest 0x72b90fec2e8871d7 -> 0x7e4c9a223abf773e [W3B void exit; W4 F08]
//   Storm17: digest 0xd62326206a38e5f5 -> 0xfdfa329cd436bbf9, folds 86 -> 83, trades 23 -> 21 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Storm17M: digest 0x9631182cfd1c6023 -> 0x535fe23b5fbfb5ea, folds 80 -> 78, trades 18 -> 17 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Storm18: digest 0x86e8b0cc2cbda4ae -> 0xf12b91d86413533c, folds 80 -> 79, trades 23 -> 22 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Storm18M: digest 0xd9fb27e46899ccd7 -> 0x979b7bbd26c38a56, folds 80 -> 79, trades 23 -> 22 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Storm19: digest 0xe92413fbdc464242 -> 0x2be2f691db8da9c4, folds 86 -> 81, trades 23 -> 20 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8E C1]
//   Storm19M: digest 0xa9a584e70d470321 -> 0x937badd205890225, folds 86 -> 82, trades 23 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8E C1]
//   Storm20: digest 0x93de366f51f2315d -> 0x604f8dc86a777322, folds 45 -> 43, trades 12 -> 10 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm20M: digest 0x201adc8ab4ea9bdb -> 0x08c037a58815f21e, folds 45 -> 43, trades 12 -> 10 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm21: digest 0xd4a8eda3db34c6ee -> 0xc35dd2c653db38ec [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm21M: digest 0xefaa36484eb9c7a1 -> 0xf770bc84f8b963b1 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Storm22: digest 0x686821a1adb13279 -> 0x6862e0fd009c1790 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm22M: digest 0x28a2c0fefdac67af -> 0xbf55a11e282da39f [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Storm23: digest 0xfac7dcb54dc93113 -> 0xb57c9d2a9d2d519f, folds 89 -> 87, trades 26 -> 28 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W4 F19c]
//   Storm23M: digest 0xe66e15ba3a360e59 -> 0x6b9e5e8f5194fdfb, folds 89 -> 87, trades 26 -> 28 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Storm24: digest 0x9a05f4cc2a37c8b8 -> 0x63f5f7e8631c6674 [W3B void exit]
//   Storm24M: digest 0xa1f1acfcca37ef51 -> 0x8ec5900090e03ba1 [W3B void exit]
//   LegsStops01: digest 0xe82893532dd3da24 -> 0xcce81db4d82a5c84 [W3B F02 ledger booking]
//   LegsStops01M: digest 0x40cde458e42af83d -> 0x26925b94c1b0b053 [W3B F02 ledger booking]
//   LegsStops02: digest 0x7aa8ab78f1f5ada3 -> 0xf8a76370e77c0288 [W3B F02 ledger booking]
//   LegsStops02M: digest 0xa1d4657aea1b015d -> 0x249639bbcf988992 [W3B F02 ledger booking]
//   Legs03: digest 0x4a813820bb3a558d -> 0xcc33c2bf5b9e3eb3 [W3B F02 ledger booking]
//   Legs03M: digest 0xc10d45c7cb3cbee5 -> 0xbb57583c687ffd41 [W3B F02 ledger booking]
//   LegsStops03: digest 0xc02aaa3f7c355e7a -> 0x6bfd3259a98f9a50 [W3B F02 ledger booking]
//   LegsStops03M: digest 0x55a7fe1ea0fc100a -> 0x649fef9865386a15 [W3B F02 ledger booking]
//   Legs04: digest 0x254b1a89ea0388dd -> 0x8fa244580a1b5d71 [W3B F02 ledger booking]
//   Legs04M: digest 0x079706336deb5323 -> 0xa9104fae5877598d [W3B F02 ledger booking]
//   LegsStops04: digest 0xc6a5620646c92b3c -> 0xd0b576fc1ba1e92a [W3B F02 ledger booking]
//   LegsStops04M: digest 0xbcad7c107485fbe8 -> 0x905b7c095270f8b8 [W3B F02 ledger booking]
//   LegsStops05: digest 0x94435fe6bea6a39d -> 0x7ee3974eb7d07a1e [W3B F02 ledger booking]
//   LegsStops05M: digest 0x2722fdb103c71c42 -> 0xf5c4797f33609847 [W3B F02 ledger booking]
//   LegsStops06: digest 0x198f7d7e224884c0 -> 0x0015322d33dc00be [W3B F02 ledger booking]
//   LegsStops06M: digest 0x507c89b50b4d4b2b -> 0x3014ef9c76c328ad [W3B F02 ledger booking]
//   TwoEntries: digest 0x06f2ecb7c1a3a480 -> 0xef5e45463539ecb2 [W3B F02 first id]
//   TwoEntriesM: digest 0x7c956e01ced10ccb -> 0xa5293744ba522c82 [W3B F02 first id]
//   LegBesideClose: digest 0xfd494f8d3e8228d4 -> 0x1559b022773e0bae [W3B F02 ledger booking]
//   LegBesideCloseM: digest 0x4a9e3df3ac353419 -> 0x2b0bb872ebf141d2 [W3B F02 ledger booking]
//   LegsApart: digest 0x64ff417b6bcf212f -> 0xaef3fc2d3d2e942d [W3B F02 ledger booking]
//   LegsApartM: digest 0xaa0c742ddeacc4d5 -> 0x650d1b75fc17cdb7 [W3B F02 ledger booking]
// INT28-FIX re-harvested it once on the integrated tree (INT28 4efcb8c5 and
// INT28-FIX's ten commits) with PINEFORGE_D2D_PUBLICATION_HARVEST; the same harvest against
// INT28 reproduces every old row. 27 of 156 runs move, each marked with the
// commit(s) at whose boundary it moved (a harvest at every commit): only the
// picked W5B-ENG-MARGIN-RESIDUAL rules SB and PC move any; CA, PA, SZ, FU,
// MS, SS, OU and OF move none. The rules are TradingView's (their lanes'
// tests/fixtures tapes):
//   ConfigFlags103: digest 0x70b81ca88d79dcc3ull -> 0xc2c52ca326f4f6d0ull, trades 61 -> 62 [W5B PC]
//   ConfigFlags105: digest 0xa103dd79855f049cull -> 0xebd79e5a335d3276ull [W5B PC]
//   ConfigFlags105M: digest 0x43e37c3b91360e9aull -> 0x5f5ee5744f67b56cull [W5B PC]
//   Config10: digest 0x89d9ee8fcfdb8d2bull -> 0x0a435dc90aec3bf9ull, trades 18 -> 17 [W5B SB]
//   Config10M: digest 0x27b4e0d84ceb64c8ull -> 0xa7b6beecb9aa3a97ull, trades 18 -> 17 [W5B SB]
//   ConfigFlags114: digest 0xb481578151a6b65bull -> 0x1cd02a838db42d9bull [W5B PC]
//   CloseOrderAdds: digest 0x1b5718bfccc88d5bull -> 0x88f567d058671ddfull [W5B PC]
//   CloseOrderAddsM: digest 0xbab50be39f3d0b28ull -> 0x3d5a1b4dc5db78baull [W5B PC]
//   Storm01M: digest 0x00cdd69222a72577ull -> 0x3dc7eab655ecce11ull, trades 14 -> 13 [W5B SB]
//   Storm03: digest 0x662d5ebe04f3918full -> 0x689afea66e651a47ull [W5B PC]
//   Storm03M: digest 0x4b0d6e52556ad140ull -> 0xc93ba02279910560ull [W5B PC]
//   Storm06: digest 0x64b975d6442847b8ull -> 0x6c7841c84c9be251ull [W5B PC]
//   Storm06M: digest 0x7c34e84d996f62b1ull -> 0x2d5832871b33599cull [W5B PC]
//   Storm09: digest 0x8087e1c554317200ull -> 0xf36667634a185ebdull [W5B PC]
//   Storm09M: digest 0x31d4f5fc5411c31full -> 0xce00b799a8f5fce2ull [W5B PC]
//   Storm11: digest 0x0780237b158be286ull -> 0xbaf0b6cf451e3aa8ull [W5B PC]
//   Storm11M: digest 0x1932b3271b2aaabaull -> 0x67cb9e94d9a557faull [W5B PC]
//   Storm14: digest 0x5ac38849fb40f365ull -> 0x774e0adda81bef05ull [W5B PC]
//   Storm14M: digest 0xc90b1227bf0f3b60ull -> 0xdddcaf20cc1357c0ull [W5B PC]
//   Storm15M: digest 0x1d30c61ffbbb4270ull -> 0xec2886ecd5bbd6fdull, trades 14 -> 13 [W5B SB]
//   Storm16: digest 0x73e4d05238964e31ull -> 0x22efc7988df63201ull [W5B PC]
//   Storm16M: digest 0x7e4c9a223abf773eull -> 0x3a4c9fb2446c31c7ull [W5B PC]
//   Storm17: digest 0xfdfa329cd436bbf9ull -> 0x882383bc431065adull [W5B SB]
//   Storm17M: digest 0x535fe23b5fbfb5eaull -> 0xee2c56f66947ceacull [W5B SB]
//   Storm20: digest 0x604f8dc86a777322ull -> 0x411a4fdcd5c11299ull, folds 43 -> 44, trades 10 -> 11 [W5B PC]
//   Storm21: digest 0xc35dd2c653db38ecull -> 0xf7b8d2e1e5dbff53ull [W5B PC]
//   Storm21M: digest 0xf770bc84f8b963b1ull -> 0x261cd4c819b89bccull [W5B PC]
// INT29 re-harvested it once on the integrated tree (main 74491fa1, lane
// W10-DIAG-UNKNOWN's ten picks, lane W11-ENG-TIME-COLOR's eleven, INT29's own
// three commits, lane W12-ENG-TIME's thirteen and lane W13-ENG-MARGIN-OPP's
// four) with PINEFORGE_D2D_PUBLICATION_HARVEST; the same harvest against main
// reproduces every old row. One run moves, and a harvest at each side of the
// W13 CP pick places it there: the tree just before it reproduces main's row,
// the tree at it already has the head's -- the value lane W13 harvested on its
// own tree. Config03 calls its whole short at a bar's close, where the path
// after the fill is adverse only at that close; the call is now taken there
// after the script, which reads the short and places its strategy.close_all(),
// and the flat purge retires that order once the call empties the book. The
// closed trades and every count keep their values; the digest takes in the
// placed and retired order:
//   Config03: digest 0xc5424169be207aa2ull -> 0xdf3025153dce2e2bull [W13 CP]
// Lane TAIL-D re-harvested it once on its tree (8988faff and the lane's ten
// rule commits) with PINEFORGE_D2D_PUBLICATION_HARVEST; the same harvest
// against 8988faff reproduces every old row. 4 of 156 runs move, digest only
// (every trade and count unchanged), and a harvest after each rule commit
// places all four at FP17 (an exit limit the recalculation of an entry's open
// fill places at or through the open print fills there,
// tests/fixtures/coof_open_limit):
//   Storm02: digest 0xc147c03e2f6038feull -> 0x425da535d3f7f803ull [TAIL-D FP17]
//   Storm05: digest 0x8c67fec9d289937dull -> 0x15759d5d500e7b69ull [TAIL-D FP17]
//   Storm19: digest 0x2be2f691db8da9c4ull -> 0x1141e20bcecc13caull [TAIL-D FP17]
//   Storm23: digest 0xb57c9d2a9d2d519full -> 0x613145193da5fe25ull [TAIL-D FP17]
// R5 lane TAIL-C re-harvested it once on its tree (r5/tail-c: lane W8E-EXITS's
// rule A picked onto INT29 8988faff, then TAIL-C's six rules) with
// PINEFORGE_D2D_PUBLICATION_HARVEST; the same harvest against 8988faff
// reproduces every old row. 4 of 156 runs move, each marked with the commit at
// whose boundary it moved (a harvest at every commit); their closed trades and
// every count keep their values (W2 = the refill after a fill forced onto a
// leg's end, OPEN = the exit stop born through by a later open fill, POOC =
// the close pass's priced adds):
//   ConfigFlags103: digest 0xc2c52ca326f4f6d0ull -> 0x405c89f8d0c4f0a8ull [TAIL-C OPEN]
//   Storm08: digest 0x3c2a5b9c43c4fd89ull -> 0x3fbbfd35e31f33f4ull [TAIL-C OPEN]
//   Storm12: digest 0x2f7a3a3d3b832e58ull -> 0x7530d5bfdf947e25ull [TAIL-C OPEN]
//   Storm22: digest 0x6862e0fd009c1790ull -> 0x699bd1943a7cf263ull [TAIL-C OPEN]
// INT30 harvested it once more on the integrated tree (engine main 7239ab37
// with lanes TAIL-A, TAIL-B, TAIL-G, TAIL-D, TAIL-H, TAIL-E and TAIL-C picked,
// in that order) and at each lane boundary of that history: main reproduces
// every old row, the 4 TAIL-D rows move at TAIL-D's picks and the 4 TAIL-C
// rows at TAIL-C's, each to its lane's value above; no other pick moves one.
// Ordinary opening-stop audit: Storm02 has long and short callbacks, slippage 0;
// Storm05 and Storm23 have short callbacks, slippage 0. The long Storm02 callback
// is outside the short-only coof_marketable_stop tapes; their short callbacks
// are margin-backed shapes, outside those margin-free tapes. Activation and
// Market conversion are narrowed accordingly; all six ordinary/magnified Storm
// digits remain at baseline. No publication expectation is refreshed.
// The global exit children change re-harvested it the same way on its tree (124 of 156 runs move), because the source adapter's state now folds TradingView's entry-id table once it holds two entry ids or its peak passes 13 keys (TradingView's global exit children, pineforge-source-adapter/v4).
// With the three hash inputs taken out (the host-extension fold, the recorded
// broker-state hash rows, the final hash) this TU prints the same table against
// main 700c5d24 and this tree except Config16: its 18th trade, a named stop already
// through when its short limit entry fills, now fills at that fill plus one tick of
// slippage (107.75) instead of the level the bar crossed before the entry (105.5),
// as the pin's eth tapes show (tests/fixtures/global_exit_children). Every count
// is unchanged:
//   Config01: digest 0x2338d6688ca58837ull -> 0x31d2128500b8c810ull
//   ConfigFlags101: digest 0x6b3262a158c0e86dull -> 0x182b39aa6a8c8b09ull
//   Config01M: digest 0x8b63a249e4bcbf1full -> 0xda523bf6f1ff5da9ull
//   ConfigFlags101M: digest 0xae3b2b9fc4e23750ull -> 0x797804281338616eull
//   Config02: digest 0x9fc3f24e211e1726ull -> 0xa7c6dc31b184c062ull
//   ConfigFlags102: digest 0x443b33f233df5591ull -> 0x77c5d6a390151224ull
//   Config02M: digest 0x8cddc71b29406412ull -> 0xe56da321d367a98cull
//   ConfigFlags102M: digest 0x000ba0de0f306cb9ull -> 0x3ddbee2abb471b98ull
//   Config03: digest 0x8528140cf26cf73cull -> 0x08dc07e410fc80b9ull
//   ConfigFlags103: digest 0xc5c10186ccb406e9ull -> 0x98bd980334fb3389ull
//   Config03M: digest 0x01f7acf1f8c58dddull -> 0x5a61078d2cbfcc29ull
//   ConfigFlags103M: digest 0x1a1c983bde0e4d96ull -> 0x8159521b2ffa0ca2ull
//   Config04: digest 0x5fc85948d1cc71ebull -> 0x8f2f44270196ded6ull
//   ConfigFlags104: digest 0x97b7894a32af23f1ull -> 0xd01d770f131d65b7ull
//   Config04M: digest 0x87cc9f67ed62f28eull -> 0x3e8e13fda49449b8ull
//   ConfigFlags104M: digest 0x3850e6440128d893ull -> 0x43bc541d78c89e31ull
//   Config05: digest 0x85c421ae2f21507full -> 0x9d89db71dda55713ull
//   ConfigFlags105: digest 0xebd79e5a335d3276ull -> 0x9ca36cf70fbe46dfull
//   Config05M: digest 0x8422d41a3aa8dee1ull -> 0xc6ab3f6b28bc38beull
//   ConfigFlags105M: digest 0x5f5ee5744f67b56cull -> 0x7be4baad25309c76ull
//   Config06: digest 0xd365aa9c3705ae1full -> 0xc5c0924d42de3c09ull
//   ConfigFlags106: digest 0x4fc0738f0271278cull -> 0x61fbb144d4a4d992ull
//   Config06M: digest 0x21e19f868e2ac1a1ull -> 0x2d43d3b138cf9301ull
//   ConfigFlags106M: digest 0x83a9bde980d36054ull -> 0x743fe463843c0c27ull
//   Config07: digest 0xa2e64f9925787e69ull -> 0x674b81005f9768cfull
//   ConfigFlags107: digest 0xabda13a612cf944dull -> 0x1c2e8788412933f8ull
//   Config07M: digest 0x9a4813d5ba021f90ull -> 0x308f2486fa771f30ull
//   ConfigFlags107M: digest 0xca421b83f6380179ull -> 0xdd08094886a56aa1ull
//   Config08: digest 0xde6e8977d6fccc6full -> 0x74a00141ead9ff5bull
//   ConfigFlags108: digest 0xe78d704b74e4f695ull -> 0x69bc8df63a3dae4cull
//   Config08M: digest 0xe9dabaa4abe5ce0dull -> 0xb0841837b9745ddbull
//   ConfigFlags108M: digest 0x75aac168f00f1801ull -> 0x6e124ef876ee1eb7ull
//   Config09: digest 0xe6ce749e6d011633ull -> 0xbf4f528b905854c2ull
//   ConfigFlags109: digest 0x18457ecdd3b9059bull -> 0xbec8c2f2a1e3355bull
//   Config09M: digest 0xcad60c482f2dadb6ull -> 0x5b46dbc518eab274ull
//   ConfigFlags109M: digest 0xb8b2405ade532a1aull -> 0x949d1bee3fb2e2eaull
//   Config10: digest 0x0a435dc90aec3bf9ull -> 0xc19c68c60fab33eaull
//   ConfigFlags110: digest 0xfd01004606da1f60ull -> 0x5459b04e304fe3bcull
//   Config10M: digest 0xa7b6beecb9aa3a97ull -> 0x122baa54dbf2aeecull
//   ConfigFlags110M: digest 0xff6fa6e9e28a9bdfull -> 0x442b1334470df248ull
//   Config11: digest 0xb20c8cbfea616264ull -> 0x331bd85f32f8d029ull
//   ConfigFlags111: digest 0xcc9cdc745a08b222ull -> 0xd5244f565a53d0e7ull
//   Config11M: digest 0x3c15e549c4ca8b3bull -> 0x92f629637cecda50ull
//   ConfigFlags111M: digest 0xbb8e81d92e1268bcull -> 0xf6a424933c89f0d7ull
//   Config12: digest 0x8f89d528e22725e5ull -> 0xe38f7c1d9184919dull
//   ConfigFlags112: digest 0xeffa667f3295fb5dull -> 0xd35d36a03e4c76e3ull
//   Config12M: digest 0x7628205f68612a98ull -> 0x80e51eb102d48540ull
//   ConfigFlags112M: digest 0x16a09b30a0769034ull -> 0xea2bd7a7ab3457dfull
//   Config13: digest 0x4404022d43781ea0ull -> 0xb5e16162f8b1aacdull
//   ConfigFlags113: digest 0xbd64633fe83e7319ull -> 0x2d185d8da34d06ecull
//   Config13M: digest 0x2ae2c70980ac0fd2ull -> 0xdd48ef714c57ab37ull
//   ConfigFlags113M: digest 0xeb38b6811f6a8bbfull -> 0x641a157cadbd4246ull
//   Config14: digest 0xa3f9a925e91a4f7cull -> 0xac4e8d4393fc5c8eull
//   ConfigFlags114: digest 0x0e228ef5bd65a4e8ull -> 0x5ba816d3035e5910ull
//   Config14M: digest 0x5289594882c86a97ull -> 0x33aa7823f004513full
//   ConfigFlags114M: digest 0xec23dc5623bdcbd7ull -> 0x26ae2e662c9d6e78ull
//   Config15: digest 0x186642fe4a546b57ull -> 0x0de23720d948e602ull
//   ConfigFlags115: digest 0x2498f0b2c19691b0ull -> 0xef49d545aa0f75beull
//   Config15M: digest 0xf26e418f92af8c2aull -> 0x16d87fe364d379a1ull
//   ConfigFlags115M: digest 0xf689ea3140af17e6ull -> 0x4f867143b7e68019ull
//   Config16: digest 0x40c001863790e4bbull -> 0x12111f17f0631e40ull
//   ConfigFlags116: digest 0x6ea5c86901105686ull -> 0xf062830378fc647dull
//   Config16M: digest 0x06b36e850ad09548ull -> 0xa8d2819dd16d4c53ull
//   ConfigFlags116M: digest 0xf4f9194225b2a7afull -> 0x7d87a7333c612748ull
//   CloseAllThenEntry: digest 0x279fa9da8fb40549ull -> 0xa62c30d5de652cf0ull
//   CloseAllThenEntryM: digest 0xc6baa19d8a587d60ull -> 0xf1e300c1cbee3ebeull
//   Reversals: digest 0x23a3b43853bb2e1aull -> 0x71094a9d0a55d56bull
//   ReversalsM: digest 0x29160b951dc12d18ull -> 0x82cf4d57ffc988fbull
//   SameBarBatch: digest 0x167ef4263f9ae286ull -> 0xf5d9032f4adc5537ull
//   SameBarBatchM: digest 0x488225822bf669d0ull -> 0xef2267ffe47d784aull
//   CloseOrderAdds: digest 0x88f567d058671ddfull -> 0x71edee52fb9f0463ull
//   CloseOrderAddsM: digest 0x3d5a1b4dc5db78baull -> 0xe397d2974beb7820ull
//   DelayedStops: digest 0x43bed101b5c70825ull -> 0xaeb87857787eefa2ull
//   DelayedStopsM: digest 0xeebabaa7a380fb84ull -> 0xa5372b833b3b72bbull
//   Storm01: digest 0x4fcc1a18f96048beull -> 0x3262e4227877873full
//   Storm01M: digest 0x3dc7eab655ecce11ull -> 0x81394cb53836347full
//   Storm02: digest 0x425da535d3f7f803ull -> 0x67a916d5e5654ac9ull
//   Storm02M: digest 0x745faf07c1b57317ull -> 0xb1c4092731c1aff0ull
//   Storm03: digest 0x689afea66e651a47ull -> 0x3882198431226d3bull
//   Storm03M: digest 0xc93ba02279910560ull -> 0xe924c116ac2f7071ull
//   Storm04: digest 0x07c17443f491fb30ull -> 0x32ae091f217496f5ull
//   Storm04M: digest 0x74d239698dac5c24ull -> 0xd0e4d02cddd487b6ull
//   Storm05: digest 0x15759d5d500e7b69ull -> 0x77b51e46753e641cull
//   Storm05M: digest 0xfbdf760609e41925ull -> 0xfa8c7bc549eb7603ull
//   Storm06: digest 0x6c7841c84c9be251ull -> 0x08bcb70951b13623ull
//   Storm06M: digest 0x2d5832871b33599cull -> 0x35fc80c9ac1e84dbull
//   Storm07: digest 0x8c50963e152ddba8ull -> 0xd2db6c6ccc4cc8aeull
//   Storm07M: digest 0xa75412c0c52f6ac1ull -> 0xdd24736fb641419cull
//   Storm08: digest 0x3fbbfd35e31f33f4ull -> 0xe494e6047d8d3c34ull
//   Storm08M: digest 0x7bfabcd35099442cull -> 0x6de07fb9ae58e38eull
//   Storm09: digest 0xf36667634a185ebdull -> 0x8ef242d38861dfbdull
//   Storm09M: digest 0xce00b799a8f5fce2ull -> 0x5811362ad56e9947ull
//   Storm10: digest 0xf7007d9d5ea3296eull -> 0xcf25a075e8eef342ull
//   Storm10M: digest 0x862537a926bed30eull -> 0xbb27a7d5326e7a78ull
//   Storm11: digest 0xbaf0b6cf451e3aa8ull -> 0x48b4c85e44eec7a5ull
//   Storm11M: digest 0x67cb9e94d9a557faull -> 0x0f201229fac1e34dull
//   Storm12: digest 0x7530d5bfdf947e25ull -> 0x23dba443bd54a21bull
//   Storm12M: digest 0x6a900d649bc55ad8ull -> 0xb4a31c0afa9608a6ull
//   Storm13: digest 0x97f4432edb19a7e9ull -> 0xac17cc6af3536e5aull
//   Storm13M: digest 0x1170d6964f4f90eaull -> 0x7f2244d9967e2e6aull
//   Storm14: digest 0x774e0adda81bef05ull -> 0x881d26217cfff80dull
//   Storm14M: digest 0xdddcaf20cc1357c0ull -> 0xcff489d957517339ull
//   Storm15: digest 0x86667ae71fa7953dull -> 0xb328e1bd987feba1ull
//   Storm15M: digest 0xec2886ecd5bbd6fdull -> 0xf9c9756c338f6137ull
//   Storm16: digest 0x22efc7988df63201ull -> 0x77e2ccbbf316ed69ull
//   Storm16M: digest 0x3a4c9fb2446c31c7ull -> 0x149ce304657cb4beull
//   Storm17: digest 0x882383bc431065adull -> 0x5b185e3ee6e1536eull
//   Storm17M: digest 0xee2c56f66947ceacull -> 0xf8a5f5ebc1d0bbdbull
//   Storm18: digest 0xf12b91d86413533cull -> 0xca66435c9059f774ull
//   Storm18M: digest 0x979b7bbd26c38a56ull -> 0x39758f9c7c7b1b7eull
//   Storm19: digest 0x1141e20bcecc13caull -> 0x31665db6fec7e215ull
//   Storm19M: digest 0x937badd205890225ull -> 0xdb3d4be8a73d6a37ull
//   Storm20: digest 0x411a4fdcd5c11299ull -> 0x6553b52251409a50ull
//   Storm20M: digest 0x08c037a58815f21eull -> 0xa9eb81ea05cde681ull
//   Storm21: digest 0xf7b8d2e1e5dbff53ull -> 0x11106ad7d0d2096bull
//   Storm21M: digest 0x261cd4c819b89bccull -> 0xaea3f255e672f9baull
//   Storm22: digest 0x699bd1943a7cf263ull -> 0x8baa2f9d85197787ull
//   Storm22M: digest 0xbf55a11e282da39full -> 0x568459b941271e50ull
//   Storm23: digest 0x613145193da5fe25ull -> 0xbd87d9c4ab528ce8ull
//   Storm23M: digest 0x6b9e5e8f5194fdfbull -> 0xbaf875418c71afd6ull
//   Storm24: digest 0x63f5f7e8631c6674ull -> 0x556e0bf179e8561eull
//   Storm24M: digest 0x8ec5900090e03ba1ull -> 0xffa4d19d7e07cca7ull
//   TwoEntries: digest 0xef5e45463539ecb2ull -> 0x3c8710fd441285b6ull
//   TwoEntriesM: digest 0xa5293744ba522c82ull -> 0x02c530fc3ea1c8adull
// expectation corrected (G + L source money fold, 43 of 156 rows), because the source layer's fold (pineforge-source-adapter/v4) now folds the adapter's G + L source money (gains and losses, two realized-money accumulators) wherever their sum differs from the engine's running net profit; every closed trade is unchanged in every Trade field, as are the counts and net profits; harvested with this TU's own switch on the tree that adds the G + L money; the source folds, closed trades and recorded rows did not move:
//   Config01: digest 0x31d2128500b8c810ull -> 0xe752de2dd7010863ull
//   ConfigFlags101: digest 0x182b39aa6a8c8b09ull -> 0x19179db1a0eaed4dull
//   Config01M: digest 0xda523bf6f1ff5da9ull -> 0x3fac346cf0dfd611ull
//   ConfigFlags101M: digest 0x797804281338616eull -> 0x828c1a8486de7dbcull
//   ConfigFlags103: digest 0x98bd980334fb3389ull -> 0x3e57d9da83e37a90ull
//   ConfigFlags103M: digest 0x8159521b2ffa0ca2ull -> 0x093009bb7400f9e6ull
//   Config07: digest 0x674b81005f9768cfull -> 0xd9d3e307fbd37083ull
//   ConfigFlags107: digest 0x1c2e8788412933f8ull -> 0xa1c5304d09ed840full
//   Config07M: digest 0x308f2486fa771f30ull -> 0x39a5c46ca9dde53cull
//   ConfigFlags107M: digest 0xdd08094886a56aa1ull -> 0x309e017c2cec1d99ull
//   Config09: digest 0xbf4f528b905854c2ull -> 0x0e1130f27be90dc9ull
//   ConfigFlags109: digest 0xbec8c2f2a1e3355bull -> 0x5b094cce771a463dull
//   Config09M: digest 0x5b46dbc518eab274ull -> 0x8516eb5434e982c1ull
//   ConfigFlags109M: digest 0x949d1bee3fb2e2eaull -> 0xf5fedf3fd08d2537ull
//   Config10: digest 0xc19c68c60fab33eaull -> 0x3935ef7337848b09ull
//   Config10M: digest 0x122baa54dbf2aeecull -> 0x8c5b4dbb210ab0a4ull
//   Config11: digest 0x331bd85f32f8d029ull -> 0x20ebce3bc0c8f7e6ull
//   Config11M: digest 0x92f629637cecda50ull -> 0x081b828e33c83b35ull
//   Config14: digest 0xac4e8d4393fc5c8eull -> 0x0790948e51f47fe7ull
//   Config14M: digest 0x33aa7823f004513full -> 0xbe6ede6df7757dabull
//   Config15: digest 0x0de23720d948e602ull -> 0x50f4291e221846edull
//   Config15M: digest 0x16d87fe364d379a1ull -> 0x45c19d81b57dbc88ull
//   Config16: digest 0x12111f17f0631e40ull -> 0xa40dc13dfc6020daull
//   Config16M: digest 0xa8d2819dd16d4c53ull -> 0x77ab4693f93e9998ull
//   Storm04: digest 0x32ae091f217496f5ull -> 0x7fe213e69bacb416ull
//   Storm04M: digest 0xd0e4d02cddd487b6ull -> 0xcbf66004a097dd9aull
//   Storm05: digest 0x77b51e46753e641cull -> 0x2cbd035e654563adull
//   Storm05M: digest 0xfa8c7bc549eb7603ull -> 0x1d2d3cb30b645629ull
//   Storm06: digest 0x08bcb70951b13623ull -> 0x46fca021365eabbdull
//   Storm06M: digest 0x35fc80c9ac1e84dbull -> 0xdab2e58f2a967d63ull
//   Storm07: digest 0xd2db6c6ccc4cc8aeull -> 0xed87ebd53e8541faull
//   Storm07M: digest 0xdd24736fb641419cull -> 0x256345301bd94835ull
//   Storm09: digest 0x8ef242d38861dfbdull -> 0x5fa1c071ad028a37ull
//   Storm09M: digest 0x5811362ad56e9947ull -> 0x14a16ba1eb758cc5ull
//   Storm13: digest 0xac17cc6af3536e5aull -> 0x696fd99ab074a767ull
//   Storm13M: digest 0x7f2244d9967e2e6aull -> 0xc530a2795ae59349ull
//   Storm17: digest 0x5b185e3ee6e1536eull -> 0x0282eee14a50a3b2ull
//   Storm18: digest 0xca66435c9059f774ull -> 0x7f7d547d15124cbfull
//   Storm18M: digest 0x39758f9c7c7b1b7eull -> 0x2f28268b47beabafull
//   Storm20: digest 0x6553b52251409a50ull -> 0xc2e4766c4468e0d1ull
//   Storm20M: digest 0xa9eb81ea05cde681ull -> 0x50ef4cafe8ff7fbeull
//   Storm23: digest 0xbd87d9c4ab528ce8ull -> 0x3830c2ff0e888bdfull
//   Storm23M: digest 0xbaf875418c71afd6ull -> 0x7758f6756a947b75ull
constexpr Pinned kPinned[] = {
    {"Config01", 0xe752de2dd7010863ull, 144, 44, 144},
    {"ConfigFlags101", 0x19179db1a0eaed4dull, 145, 45, 144},
    {"Config01M", 0x3fac346cf0dfd611ull, 144, 42, 144},
    {"ConfigFlags101M", 0x828c1a8486de7dbcull, 145, 45, 144},
    {"Config02", 0xa7c6dc31b184c062ull, 48, 10, 48},
    {"ConfigFlags102", 0x77c5d6a390151224ull, 144, 49, 144},
    {"Config02M", 0xe56da321d367a98cull, 48, 10, 48},
    {"ConfigFlags102M", 0x3ddbee2abb471b98ull, 144, 49, 144},
    {"Config03", 0x08dc07e410fc80b9ull, 144, 55, 144},
    {"ConfigFlags103", 0x3e57d9da83e37a90ull, 182, 62, 144},
    {"Config03M", 0x5a61078d2cbfcc29ull, 144, 55, 144},
    {"ConfigFlags103M", 0x093009bb7400f9e6ull, 182, 64, 144},
    {"Config04", 0x8f2f44270196ded6ull, 144, 21, 144},
    {"ConfigFlags104", 0xd01d770f131d65b7ull, 48, 14, 48},
    {"Config04M", 0x3e8e13fda49449b8ull, 144, 21, 144},
    {"ConfigFlags104M", 0x43bc541d78c89e31ull, 48, 14, 48},
    {"Config05", 0x9d89db71dda55713ull, 48, 12, 48},
    {"ConfigFlags105", 0x9ca36cf70fbe46dfull, 48, 12, 48},
    {"Config05M", 0xc6ab3f6b28bc38beull, 48, 12, 48},
    {"ConfigFlags105M", 0x7be4baad25309c76ull, 48, 12, 48},
    {"Config06", 0xc5c0924d42de3c09ull, 48, 5, 48},
    {"ConfigFlags106", 0x61fbb144d4a4d992ull, 48, 6, 48},
    {"Config06M", 0x2d43d3b138cf9301ull, 48, 5, 48},
    {"ConfigFlags106M", 0x743fe463843c0c27ull, 48, 6, 48},
    {"Config07", 0xd9d3e307fbd37083ull, 144, 48, 144},
    {"ConfigFlags107", 0xa1c5304d09ed840full, 48, 22, 48},
    {"Config07M", 0x39a5c46ca9dde53cull, 144, 48, 144},
    {"ConfigFlags107M", 0x309e017c2cec1d99ull, 48, 23, 48},
    {"Config08", 0x74a00141ead9ff5bull, 48, 12, 48},
    {"ConfigFlags108", 0x69bc8df63a3dae4cull, 48, 15, 48},
    {"Config08M", 0xb0841837b9745ddbull, 48, 12, 48},
    {"ConfigFlags108M", 0x6e124ef876ee1eb7ull, 48, 15, 48},
    {"Config09", 0x0e1130f27be90dc9ull, 144, 66, 144},
    {"ConfigFlags109", 0x5b094cce771a463dull, 144, 45, 144},
    {"Config09M", 0x8516eb5434e982c1ull, 144, 66, 144},
    {"ConfigFlags109M", 0xf5fedf3fd08d2537ull, 144, 45, 144},
    {"Config10", 0x3935ef7337848b09ull, 48, 17, 48},
    {"ConfigFlags110", 0x5459b04e304fe3bcull, 48, 12, 48},
    {"Config10M", 0x8c5b4dbb210ab0a4ull, 48, 17, 48},
    {"ConfigFlags110M", 0x442b1334470df248ull, 48, 12, 48},
    {"Config11", 0x20ebce3bc0c8f7e6ull, 144, 60, 144},
    {"ConfigFlags111", 0xd5244f565a53d0e7ull, 48, 17, 48},
    {"Config11M", 0x081b828e33c83b35ull, 144, 60, 144},
    {"ConfigFlags111M", 0xf6a424933c89f0d7ull, 48, 17, 48},
    {"Config12", 0xe38f7c1d9184919dull, 144, 48, 144},
    {"ConfigFlags112", 0xd35d36a03e4c76e3ull, 144, 19, 144},
    {"Config12M", 0x80e51eb102d48540ull, 144, 48, 144},
    {"ConfigFlags112M", 0xea2bd7a7ab3457dfull, 144, 19, 144},
    {"Config13", 0xb5e16162f8b1aacdull, 48, 19, 48},
    {"ConfigFlags113", 0x2d185d8da34d06ecull, 48, 19, 48},
    {"Config13M", 0xdd48ef714c57ab37ull, 48, 19, 48},
    {"ConfigFlags113M", 0x641a157cadbd4246ull, 48, 19, 48},
    {"Config14", 0x0790948e51f47fe7ull, 144, 40, 144},
    {"ConfigFlags114", 0x5ba816d3035e5910ull, 144, 66, 144},
    {"Config14M", 0xbe6ede6df7757dabull, 144, 40, 144},
    {"ConfigFlags114M", 0x26ae2e662c9d6e78ull, 144, 66, 144},
    {"Config15", 0x50f4291e221846edull, 144, 48, 144},
    {"ConfigFlags115", 0xef49d545aa0f75beull, 48, 6, 48},
    {"Config15M", 0x45c19d81b57dbc88ull, 144, 48, 144},
    {"ConfigFlags115M", 0x4f867143b7e68019ull, 48, 6, 48},
    {"Config16", 0xa40dc13dfc6020daull, 144, 54, 144},
    {"ConfigFlags116", 0xf062830378fc647dull, 48, 21, 48},
    {"Config16M", 0x77ab4693f93e9998ull, 144, 54, 144},
    {"ConfigFlags116M", 0x7d87a7333c612748ull, 48, 21, 48},
    {"CloseAllThenEntry", 0xa62c30d5de652cf0ull, 40, 17, 40},
    {"CloseAllThenEntryM", 0xf1e300c1cbee3ebeull, 40, 17, 40},
    {"Reversals", 0x71094a9d0a55d56bull, 40, 16, 40},
    {"ReversalsM", 0x82cf4d57ffc988fbull, 40, 16, 40},
    {"SameBarBatch", 0xf5d9032f4adc5537ull, 36, 25, 36},
    {"SameBarBatchM", 0xef2267ffe47d784aull, 36, 25, 36},
    {"CloseOrderAdds", 0x71edee52fb9f0463ull, 36, 22, 36},
    {"CloseOrderAddsM", 0xe397d2974beb7820ull, 36, 22, 36},
    {"DelayedStops", 0xaeb87857787eefa2ull, 66, 20, 40},
    {"DelayedStopsM", 0xa5372b833b3b72bbull, 66, 20, 40},
    {"Storm01", 0x3262e4227877873full, 43, 13, 43},
    {"Storm01M", 0x81394cb53836347full, 43, 13, 43},
    {"Storm02", 0x67a916d5e5654ac9ull, 63, 12, 42},
    {"Storm02M", 0xb1c4092731c1aff0ull, 63, 12, 42},
    {"Storm03", 0x3882198431226d3bull, 41, 17, 41},
    {"Storm03M", 0xe924c116ac2f7071ull, 41, 17, 41},
    {"Storm04", 0x7fe213e69bacb416ull, 54, 24, 54},
    {"Storm04M", 0xcbf66004a097dd9aull, 54, 24, 54},
    {"Storm05", 0x2cbd035e654563adull, 80, 17, 53},
    {"Storm05M", 0x1d2d3cb30b645629ull, 82, 18, 53},
    {"Storm06", 0x46fca021365eabbdull, 40, 13, 40},
    {"Storm06M", 0xdab2e58f2a967d63ull, 40, 13, 40},
    {"Storm07", 0xed87ebd53e8541faull, 55, 13, 55},
    {"Storm07M", 0x256345301bd94835ull, 55, 13, 55},
    {"Storm08", 0xe494e6047d8d3c34ull, 81, 14, 58},
    {"Storm08M", 0x6de07fb9ae58e38eull, 81, 14, 58},
    {"Storm09", 0x5fa1c071ad028a37ull, 48, 23, 48},
    {"Storm09M", 0x14a16ba1eb758cc5ull, 48, 22, 48},
    {"Storm10", 0xcf25a075e8eef342ull, 63, 13, 57},
    {"Storm10M", 0xbb27a7d5326e7a78ull, 64, 13, 57},
    {"Storm11", 0x48b4c85e44eec7a5ull, 59, 23, 59},
    {"Storm11M", 0x0f201229fac1e34dull, 59, 23, 59},
    {"Storm12", 0x23dba443bd54a21bull, 85, 19, 56},
    {"Storm12M", 0xb4a31c0afa9608a6ull, 85, 19, 56},
    {"Storm13", 0x696fd99ab074a767ull, 46, 25, 46},
    {"Storm13M", 0xc530a2795ae59349ull, 46, 25, 46},
    {"Storm14", 0x881d26217cfff80dull, 46, 17, 46},
    {"Storm14M", 0xcff489d957517339ull, 46, 17, 46},
    {"Storm15", 0xb328e1bd987feba1ull, 43, 13, 43},
    {"Storm15M", 0xf9c9756c338f6137ull, 43, 13, 43},
    {"Storm16", 0x77e2ccbbf316ed69ull, 58, 18, 58},
    {"Storm16M", 0x149ce304657cb4beull, 58, 18, 58},
    {"Storm17", 0x0282eee14a50a3b2ull, 83, 21, 54},
    {"Storm17M", 0xf8a5f5ebc1d0bbdbull, 78, 17, 54},
    {"Storm18", 0x7f7d547d15124cbfull, 79, 22, 49},
    {"Storm18M", 0x2f28268b47beabafull, 79, 22, 49},
    {"Storm19", 0x31665db6fec7e215ull, 81, 20, 49},
    {"Storm19M", 0xdb3d4be8a73d6a37ull, 82, 21, 49},
    {"Storm20", 0xc2e4766c4468e0d1ull, 44, 11, 42},
    {"Storm20M", 0x50ef4cafe8ff7fbeull, 43, 10, 42},
    {"Storm21", 0x11106ad7d0d2096bull, 47, 14, 47},
    {"Storm21M", 0xaea3f255e672f9baull, 47, 14, 47},
    {"Storm22", 0x8baa2f9d85197787ull, 76, 19, 50},
    {"Storm22M", 0x568459b941271e50ull, 76, 19, 50},
    {"Storm23", 0x3830c2ff0e888bdfull, 87, 28, 54},
    {"Storm23M", 0x7758f6756a947b75ull, 87, 28, 54},
    {"Storm24", 0x556e0bf179e8561eull, 54, 25, 54},
    {"Storm24M", 0xffa4d19d7e07cca7ull, 54, 25, 54},
    {"Legs01", 0x2257c1f1b017aff6ull, 30, 6, 30},
    {"Legs01M", 0xdeeb42cf30042b94ull, 30, 6, 30},
    {"LegsStops01", 0xcce81db4d82a5c84ull, 30, 6, 30},
    {"LegsStops01M", 0x26925b94c1b0b053ull, 30, 6, 30},
    {"Legs02", 0x290854a540c0effdull, 30, 6, 30},
    {"Legs02M", 0xa31bffc389143eaaull, 30, 6, 30},
    {"LegsStops02", 0xf8a76370e77c0288ull, 30, 6, 30},
    {"LegsStops02M", 0x249639bbcf988992ull, 30, 6, 30},
    {"Legs03", 0xcc33c2bf5b9e3eb3ull, 30, 6, 30},
    {"Legs03M", 0xbb57583c687ffd41ull, 30, 6, 30},
    {"LegsStops03", 0x6bfd3259a98f9a50ull, 30, 6, 30},
    {"LegsStops03M", 0x649fef9865386a15ull, 30, 6, 30},
    {"Legs04", 0x8fa244580a1b5d71ull, 30, 6, 30},
    {"Legs04M", 0xa9104fae5877598dull, 30, 6, 30},
    {"LegsStops04", 0xd0b576fc1ba1e92aull, 30, 6, 30},
    {"LegsStops04M", 0x905b7c095270f8b8ull, 30, 6, 30},
    {"Legs05", 0x2b3d7db72415c3ecull, 30, 6, 30},
    {"Legs05M", 0x88fd0678c5020748ull, 30, 6, 30},
    {"LegsStops05", 0x7ee3974eb7d07a1eull, 30, 6, 30},
    {"LegsStops05M", 0xf5c4797f33609847ull, 30, 6, 30},
    {"Legs06", 0x6e7d43e693bec002ull, 30, 6, 30},
    {"Legs06M", 0x2fc1828cff3085f2ull, 30, 6, 30},
    {"LegsStops06", 0x0015322d33dc00beull, 30, 6, 30},
    {"LegsStops06M", 0x3014ef9c76c328adull, 30, 6, 30},
    {"TwoEntries", 0x3c8710fd441285b6ull, 30, 2, 30},
    {"TwoEntriesM", 0x02c530fc3ea1c8adull, 30, 2, 30},
    {"PyramidLots", 0x243689d4eeaf8d3bull, 30, 2, 30},
    {"PyramidLotsM", 0x7c425d1716c9a5a2ull, 30, 2, 30},
    {"LegBesideClose", 0x1559b022773e0baeull, 30, 2, 30},
    {"LegBesideCloseM", 0x2b0bb872ebf141d2ull, 30, 2, 30},
    {"TwoLegsOnly", 0x1e97c755ea7f6e5full, 30, 2, 30},
    {"TwoLegsOnlyM", 0x8b4283e07b576d5full, 30, 2, 30},
    {"LegsApart", 0xaef3fc2d3d2e942dull, 30, 2, 30},
    {"LegsApartM", 0x650d1b75fc17cdb7ull, 30, 2, 30},
};
