// R5 lane PERF-L4: the values tests/test_adapter_quiet_bar.cpp pins, one
// row per run of its battery (scenario, magnifier, recording; the digests
// of the source folds, the recorded rows and the trades; the final
// broker-state hash; the three counts). Harvested on the base tree: see
// the provenance note in the test. Generated -- never edit a row by hand.
// expectation corrected (408 values), because v19's broker-state hash folds the closed rows as their count and a running digest, each row once when it is final (pineforge-broker-state/v19); this host projects one fixed execution hash, so the continuation does not enter; the source-fold digest (boundary), the trade digest and the three counts did not move; re-harvested the same way on INT19's tree; V19-A's tip (6211dc94) gives the same rows:
//   Quiet/0/1: rows 0xea4df9335a7c1f58ull -> 0xc8c5fef2737653d1ull, final_hash 0x0b62c904f7e3555aull -> 0x7fcbc2aeb4710017ull
//   Quiet/0/0: final_hash 0x0b62c904f7e3555aull -> 0x7fcbc2aeb4710017ull
//   Quiet/1/1: rows 0x513640861f244867ull -> 0x0eb8899e312a2d42ull, final_hash 0xc7b4ff38f4496eb8ull -> 0x9f59de4478395359ull
//   Quiet/1/0: final_hash 0xc7b4ff38f4496eb8ull -> 0x9f59de4478395359ull
//   HoldLongFull/0/1: rows 0x90e1b6bf5792bdaaull -> 0x89fbfe880001bea0ull, final_hash 0x7e9d454a144f3a95ull -> 0x314185ce5f198ed2ull
//   HoldLongFull/0/0: final_hash 0x7e9d454a144f3a95ull -> 0x314185ce5f198ed2ull
//   HoldLongFull/1/1: rows 0xa78c8518960e3a59ull -> 0x66f6cddf7a5a74e4ull, final_hash 0xfeb697a1c69c0b40ull -> 0x2dc5bb26f3b04821ull
//   HoldLongFull/1/0: final_hash 0xfeb697a1c69c0b40ull -> 0x2dc5bb26f3b04821ull
//   MarginCalls/0/1: rows 0x00637db4e50a3104ull -> 0xb1909807bff1764eull, final_hash 0x2db1298d20178ae9ull -> 0x2b840c18dd39f1c6ull
//   MarginCalls/0/0: final_hash 0x2db1298d20178ae9ull -> 0x2b840c18dd39f1c6ull
//   MarginCalls/1/1: rows 0xf90993ab5541ea2dull -> 0x513f1874b2ab78d7ull, final_hash 0xd83db0ab094928f0ull -> 0x6c821aa201775ed6ull
//   MarginCalls/1/0: final_hash 0xd83db0ab094928f0ull -> 0x6c821aa201775ed6ull
//   MarginCallCycles/0/1: rows 0x3fbb7e165da4bdc5ull -> 0x7bba702b498455c7ull, final_hash 0x805fe394fd981ca2ull -> 0xcd53400b82e5c5efull
//   MarginCallCycles/0/0: final_hash 0x805fe394fd981ca2ull -> 0xcd53400b82e5c5efull
//   MarginCallCycles/1/1: rows 0x434c44be89d39708ull -> 0xc177930f1e113f4dull, final_hash 0x14b09fd079999504ull -> 0x0f82109b17bf1707ull
//   MarginCallCycles/1/0: final_hash 0x14b09fd079999504ull -> 0x0f82109b17bf1707ull
//   Bracket/0/1: rows 0xab0620553b734e09ull -> 0x9c39620356824df1ull, final_hash 0x791cb37890b21997ull -> 0x1bc18c13b13001d5ull
//   Bracket/0/0: final_hash 0x791cb37890b21997ull -> 0x1bc18c13b13001d5ull
//   Bracket/1/1: rows 0x4deba48affb88a0aull -> 0x90e62865c5f88613ull, final_hash 0x7ed4ee96ee8bbebfull -> 0xdf15239ff6e52b85ull
//   Bracket/1/0: final_hash 0x7ed4ee96ee8bbebfull -> 0xdf15239ff6e52b85ull
//   Trail/0/1: rows 0x022e76cf27917e0cull -> 0x3890234a486c20fbull, final_hash 0xe5996123d5a521e5ull -> 0x1fb783b27fc69cfbull
//   Trail/0/0: final_hash 0xe5996123d5a521e5ull -> 0x1fb783b27fc69cfbull
//   Trail/1/1: rows 0xd5514f8553d17c2full -> 0xc8b7a3b36dbb4168ull, final_hash 0xee0af0aca586d6a9ull -> 0xca5f22b6b913959full
//   Trail/1/0: final_hash 0xee0af0aca586d6a9ull -> 0xca5f22b6b913959full
//   DualStops/0/1: rows 0x165baea266cae574ull -> 0x9bf10a8ebe9e9b42ull, final_hash 0x4b8216e2174e96b0ull -> 0x73d3bc6580643304ull
//   DualStops/0/0: final_hash 0x4b8216e2174e96b0ull -> 0x73d3bc6580643304ull
//   DualStops/1/1: rows 0xa2af403e318938d9ull -> 0x67aeaafbaca21d10ull, final_hash 0xe84043c8519e1a85ull -> 0xa7092be33e24f8f9ull
//   DualStops/1/0: final_hash 0xe84043c8519e1a85ull -> 0xa7092be33e24f8f9ull
//   InvertedStops/0/1: rows 0x90e1fe437bb964feull -> 0x3c74297c14f57223ull, final_hash 0x445f7c1144843088ull -> 0x861579348f792fb6ull
//   InvertedStops/0/0: final_hash 0x445f7c1144843088ull -> 0x861579348f792fb6ull
//   InvertedStops/1/1: rows 0x9477b58873d5932bull -> 0xd1e2c12219d76eb9ull, final_hash 0x9d222165eeadfd46ull -> 0xc98f7ac165556e38ull
//   InvertedStops/1/0: final_hash 0x9d222165eeadfd46ull -> 0xc98f7ac165556e38ull
//   PendingParent/0/1: rows 0x4222c1b9229612c2ull -> 0x6830a53c32326397ull, final_hash 0x54bff082cce9c098ull -> 0xd90bc7468c38a9ebull
//   PendingParent/0/0: final_hash 0x54bff082cce9c098ull -> 0xd90bc7468c38a9ebull
//   PendingParent/1/1: rows 0x40b1cc26f908f3c4ull -> 0x8178bdae0e1a3f9eull, final_hash 0x793b9ef825ec7b91ull -> 0x885afddb8d023efeull
//   PendingParent/1/0: final_hash 0x793b9ef825ec7b91ull -> 0x885afddb8d023efeull
//   GappedAdd/0/1: rows 0xf17afbf0bfc2e53full -> 0xe5c0cb20f459db08ull, final_hash 0x0b8c160999783d58ull -> 0x9ebb2f3cb5791396ull
//   GappedAdd/0/0: final_hash 0x0b8c160999783d58ull -> 0x9ebb2f3cb5791396ull
//   GappedAdd/1/1: rows 0x344f2bc03b0699e8ull -> 0xf6ee5af1d423e76dull, final_hash 0x04ce0c77144482e4ull -> 0xf41267d4b469b3e6ull
//   GappedAdd/1/0: final_hash 0x04ce0c77144482e4ull -> 0xf41267d4b469b3e6ull
//   DeclinedReversal/0/1: rows 0x41721ace073af54bull -> 0xbcc27c75c81f9cfcull, final_hash 0x7a821a116903785eull -> 0x0bb474c848b2df55ull
//   DeclinedReversal/0/0: final_hash 0x7a821a116903785eull -> 0x0bb474c848b2df55ull
//   DeclinedReversal/1/1: rows 0xce425a04522d3bc6ull -> 0xacfeb029ac5b58a2ull, final_hash 0xb684293b032bada3ull -> 0x1bd3ca07887fcbe8ull
//   DeclinedReversal/1/0: final_hash 0xb684293b032bada3ull -> 0x1bd3ca07887fcbe8ull
//   CalcOnFills/0/1: rows 0x547d551fd82cdd96ull -> 0x1ee0e8388da66ecdull, final_hash 0x833a3008970dac64ull -> 0xe2dca494134c93f1ull
//   CalcOnFills/0/0: final_hash 0x833a3008970dac64ull -> 0xe2dca494134c93f1ull
//   CalcOnFills/1/1: rows 0xff46c0055c0f3775ull -> 0xbc525727c762e4f8ull, final_hash 0xeedd3de8ed3f6efcull -> 0x7089025bc0e91262ull
//   CalcOnFills/1/0: final_hash 0xeedd3de8ed3f6efcull -> 0x7089025bc0e91262ull
//   CloseOrders/0/1: rows 0xd980c44075211046ull -> 0xeec40b1fff0b8920ull, final_hash 0xf551752e37229525ull -> 0x51f605644deea034ull
//   CloseOrders/0/0: final_hash 0xf551752e37229525ull -> 0x51f605644deea034ull
//   CloseOrders/1/1: rows 0x07840a6e9a7f5b99ull -> 0x9a52f0861ef94044ull, final_hash 0xa7dc0ee1d8f63ae5ull -> 0x10c3b0649de953bdull
//   CloseOrders/1/0: final_hash 0xa7dc0ee1d8f63ae5ull -> 0x10c3b0649de953bdull
//   CloseOrdersSlipped/0/1: rows 0xf8d801b478d2ee6bull -> 0xa2a9e3378f4b155aull, final_hash 0xb60ea150c637b760ull -> 0x2bd4d450a501719bull
//   CloseOrdersSlipped/0/0: final_hash 0xb60ea150c637b760ull -> 0x2bd4d450a501719bull
//   CloseOrdersSlipped/1/1: rows 0x9edc9c935ba42ea2ull -> 0x21b19021bfd0becdull, final_hash 0xe211f55f1c3c56a9ull -> 0x4f7dc4d8f1ff659aull
//   CloseOrdersSlipped/1/0: final_hash 0xe211f55f1c3c56a9ull -> 0x4f7dc4d8f1ff659aull
//   IntradayLoss/0/1: rows 0x5c8b7b96536ac7e0ull -> 0x1b8d617a36ed7938ull, final_hash 0x9bb44a4474f50220ull -> 0x29b318c0c276f639ull
//   IntradayLoss/0/0: final_hash 0x9bb44a4474f50220ull -> 0x29b318c0c276f639ull
//   IntradayLoss/1/1: rows 0x873487e4bc793e60ull -> 0x3feb98e03c9470ccull, final_hash 0x68eb07249f217197ull -> 0x4c28e687b7134e5aull
//   IntradayLoss/1/0: final_hash 0x68eb07249f217197ull -> 0x4c28e687b7134e5aull
//   IntradayCapDeferred/0/1: rows 0x3c41bb59f8aeaf08ull -> 0x2866b495085da45bull, final_hash 0x2ab4f592b7e54ec5ull -> 0xbc0f1bf5934650b3ull
//   IntradayCapDeferred/0/0: final_hash 0x2ab4f592b7e54ec5ull -> 0xbc0f1bf5934650b3ull
//   IntradayCapDeferred/1/1: rows 0x86b429198e7f8838ull -> 0xcfeaab75eb875edbull, final_hash 0x361bcc44abda3734ull -> 0xe597a163bba24ba0ull
//   IntradayCapDeferred/1/0: final_hash 0x361bcc44abda3734ull -> 0xe597a163bba24ba0ull
//   PreopenSlice/0/1: rows 0x15782374f95480c4ull -> 0x8c551e84339a2da2ull, final_hash 0x3569c43fd4f4474dull -> 0x66bf7f75f3719669ull
//   PreopenSlice/0/0: final_hash 0x3569c43fd4f4474dull -> 0x66bf7f75f3719669ull
//   PreopenSlice/1/1: rows 0xeb790e7d457e72d6ull -> 0xe5284fbfbf5fd9baull, final_hash 0xe2a874cb86816ed8ull -> 0xc6aff12be76ec96cull
//   PreopenSlice/1/0: final_hash 0xe2a874cb86816ed8ull -> 0xc6aff12be76ec96cull
//   SameBarBatch/0/1: rows 0xa44529b44bad7b16ull -> 0xe0e239162893b61eull, final_hash 0x8f22f0e3056201ceull -> 0x6e0321e00827705bull
//   SameBarBatch/0/0: final_hash 0x8f22f0e3056201ceull -> 0x6e0321e00827705bull
//   SameBarBatch/1/1: rows 0x79394abcc220f7dfull -> 0xbc52a698e423cf9full, final_hash 0x761d91b347bb13beull -> 0x0cf044723f142dabull
//   SameBarBatch/1/0: final_hash 0x761d91b347bb13beull -> 0x0cf044723f142dabull
//   CommissionedShort/0/1: rows 0x6b5189408abaa634ull -> 0x314377120173c3b2ull, final_hash 0xf1c6a20e98249e69ull -> 0xdd3b3e8c0e2c6b4full
//   CommissionedShort/0/0: final_hash 0xf1c6a20e98249e69ull -> 0xdd3b3e8c0e2c6b4full
//   CommissionedShort/1/1: rows 0xeadbb02815f9d86full -> 0xfa59d1a1daea5fa1ull, final_hash 0x9b40e34228a87c5full -> 0x21960f8f6bb14475ull
//   CommissionedShort/1/0: final_hash 0x9b40e34228a87c5full -> 0x21960f8f6bb14475ull
//   AccountFx/0/1: rows 0xdb9ade787204062bull -> 0x2fc1bbdf854e7bc2ull, final_hash 0xce5e94959aaa8b72ull -> 0x0f440a3fe5b1668cull
//   AccountFx/0/0: final_hash 0xce5e94959aaa8b72ull -> 0x0f440a3fe5b1668cull
//   Stream/0/1: rows 0x0f37db25fc1f512cull -> 0x5d07d71d7af45568ull, final_hash 0x43bb71c8398ee071ull -> 0x66dd516da3eb328aull
//   Stream/0/0: final_hash 0x43bb71c8398ee071ull -> 0x66dd516da3eb328aull
//   Random01/0/1: rows 0x1032e16efbe21da3ull -> 0xefa88fe4ae7387c2ull, final_hash 0x0037c3ad6c6ada35ull -> 0x4e616563c94d80a9ull
//   Random01/0/0: final_hash 0x0037c3ad6c6ada35ull -> 0x4e616563c94d80a9ull
//   Random01/1/1: rows 0x60c566d13c2c5ff0ull -> 0xba484319dc1a9917ull, final_hash 0xd1c984c0f087baf1ull -> 0x226dac1e01b42a77ull
//   Random01/1/0: final_hash 0xd1c984c0f087baf1ull -> 0x226dac1e01b42a77ull
//   Random02/0/1: rows 0x93a7b0e5fad15223ull -> 0xc71e38b3565b0432ull, final_hash 0xb6889d3c8ebe32fcull -> 0xd7cdc71d531a0847ull
//   Random02/0/0: final_hash 0xb6889d3c8ebe32fcull -> 0xd7cdc71d531a0847ull
//   Random02/1/1: rows 0xd2b8cae940a101ecull -> 0x1175a94a033f9de7ull, final_hash 0x5e03193469b90f2aull -> 0xde0935dcf7e6e909ull
//   Random02/1/0: final_hash 0x5e03193469b90f2aull -> 0xde0935dcf7e6e909ull
//   Random03/0/1: rows 0xddbd8d1d25a5fb6cull -> 0xb0f60628c32f69f7ull, final_hash 0x0b667080962c71bfull -> 0xa72ac6eceb11da99ull
//   Random03/0/0: final_hash 0x0b667080962c71bfull -> 0xa72ac6eceb11da99ull
//   Random03/1/1: rows 0x82fbf19432414cf4ull -> 0x27230b8676426ae3ull, final_hash 0x21d74506233adcabull -> 0xb154de44035844a6ull
//   Random03/1/0: final_hash 0x21d74506233adcabull -> 0xb154de44035844a6ull
//   Random04/0/1: rows 0x72a4810f35193c29ull -> 0xaf237f78a5d328e7ull, final_hash 0x280285e3c34def56ull -> 0x758619585ce0f06aull
//   Random04/0/0: final_hash 0x280285e3c34def56ull -> 0x758619585ce0f06aull
//   Random04/1/1: rows 0x1977e2e069b87ab9ull -> 0x214b4a4921156211ull, final_hash 0x4636126121e1f8efull -> 0x5b7f8ccdd3a6841bull
//   Random04/1/0: final_hash 0x4636126121e1f8efull -> 0x5b7f8ccdd3a6841bull
//   Random05/0/1: rows 0x6a9c6805651a011dull -> 0x91e0916588b1704cull, final_hash 0x4713b82b749584e6ull -> 0x2dfa41d9aa6a5fe6ull
//   Random05/0/0: final_hash 0x4713b82b749584e6ull -> 0x2dfa41d9aa6a5fe6ull
//   Random05/1/1: rows 0x605d165bad7081beull -> 0xe3e3928c248a40c7ull, final_hash 0x2f7dc7ed453a8420ull -> 0x877dd69817370d17ull
//   Random05/1/0: final_hash 0x2f7dc7ed453a8420ull -> 0x877dd69817370d17ull
//   Random06/0/1: rows 0xea19a55a3a006d02ull -> 0xf87e3026be4daf31ull, final_hash 0x92b969cdb8b9b8d8ull -> 0xd6641c5032ea5abbull
//   Random06/0/0: final_hash 0x92b969cdb8b9b8d8ull -> 0xd6641c5032ea5abbull
//   Random06/1/1: rows 0x7832183df1ee7bd0ull -> 0x5bd3fc90dad226dbull, final_hash 0x45edb5132fbf8181ull -> 0xa1e760452fa23398ull
//   Random06/1/0: final_hash 0x45edb5132fbf8181ull -> 0xa1e760452fa23398ull
//   Random07/0/1: rows 0x108f95129c32e65dull -> 0x8260a8ca13421199ull, final_hash 0xdaa4c08a67da464aull -> 0x02491a682b38ef59ull
//   Random07/0/0: final_hash 0xdaa4c08a67da464aull -> 0x02491a682b38ef59ull
//   Random07/1/1: rows 0x027af1cb083615a0ull -> 0xcd97b21cdfd102efull, final_hash 0x2c5a0f63cb9844ebull -> 0xdcdf0a34b0e9a35cull
//   Random07/1/0: final_hash 0x2c5a0f63cb9844ebull -> 0xdcdf0a34b0e9a35cull
//   Random08/0/1: rows 0x660a8a35b265e54bull -> 0x8d681de32aecc8adull, final_hash 0x004b116e168cf180ull -> 0x5e781fd30e0fb6ceull
//   Random08/0/0: final_hash 0x004b116e168cf180ull -> 0x5e781fd30e0fb6ceull
//   Random08/1/1: rows 0xc0c8bf5559d58b05ull -> 0xda375994f45584a9ull, final_hash 0x887059b7a817dcbdull -> 0x9ae5022ef7fa5632ull
//   Random08/1/0: final_hash 0x887059b7a817dcbdull -> 0x9ae5022ef7fa5632ull
//   Random09/0/1: rows 0xdbeac74059d55d58ull -> 0x69c959f9480c8a1eull, final_hash 0x0832c6d37013a9a2ull -> 0xaece0e254934fe1dull
//   Random09/0/0: final_hash 0x0832c6d37013a9a2ull -> 0xaece0e254934fe1dull
//   Random09/1/1: rows 0x5c6e1be56201fc8eull -> 0x7791c4f45e0e11ecull, final_hash 0x32c87bb320f498ecull -> 0x9c5d95905697df4bull
//   Random09/1/0: final_hash 0x32c87bb320f498ecull -> 0x9c5d95905697df4bull
//   Random10/0/1: rows 0x878cb49e15a9d871ull -> 0xdec3b813e785644dull, final_hash 0x43f10bdb43639320ull -> 0x770c69baf1edf99dull
//   Random10/0/0: final_hash 0x43f10bdb43639320ull -> 0x770c69baf1edf99dull
//   Random10/1/1: rows 0x6328cbff7713bdffull -> 0x562e19a136a4ae51ull, final_hash 0x29e800e7a564b34cull -> 0xf199c5b63c7d0ca7ull
//   Random10/1/0: final_hash 0x29e800e7a564b34cull -> 0xf199c5b63c7d0ca7ull
//   Random11/0/1: rows 0xa285c8f2cc732a63ull -> 0xb6855c923d53018cull, final_hash 0x9375b39274d6dcb8ull -> 0x71586d5705ef9191ull
//   Random11/0/0: final_hash 0x9375b39274d6dcb8ull -> 0x71586d5705ef9191ull
//   Random11/1/1: rows 0xfa64c524b454cbfeull -> 0xfc8b17e895298685ull, final_hash 0x3c55606af9df7c75ull -> 0xa78c34f201e438e5ull
//   Random11/1/0: final_hash 0x3c55606af9df7c75ull -> 0xa78c34f201e438e5ull
//   Random12/0/1: rows 0xa3008924769dac7full -> 0x64bc55e2ec0d8826ull, final_hash 0x7277f5462eac34f7ull -> 0xbe602d852ba7e436ull
//   Random12/0/0: final_hash 0x7277f5462eac34f7ull -> 0xbe602d852ba7e436ull
//   Random12/1/1: rows 0x8a9d68e4eb6ed938ull -> 0xa4a6551ff356ecf3ull, final_hash 0xa3d7006f17495b3bull -> 0x7f2a5aec0a9ea850ull
//   Random12/1/0: final_hash 0xa3d7006f17495b3bull -> 0x7f2a5aec0a9ea850ull
//   Random13/0/1: rows 0x71c1e7373063be37ull -> 0xd2b7c2c08f19d53full, final_hash 0xd98c45bead018ef4ull -> 0xf4212ccf834ef291ull
//   Random13/0/0: final_hash 0xd98c45bead018ef4ull -> 0xf4212ccf834ef291ull
//   Random13/1/1: rows 0x510cf293aaa026b0ull -> 0x8d492fd03bf54db0ull, final_hash 0x8b5ab1f0f8b54e63ull -> 0x66846aa435c10ff3ull
//   Random13/1/0: final_hash 0x8b5ab1f0f8b54e63ull -> 0x66846aa435c10ff3ull
//   Random14/0/1: rows 0xf981ee2636414e36ull -> 0xe5206f2023c77213ull, final_hash 0x23576098db512aefull -> 0x0af2277ce8d84ddcull
//   Random14/0/0: final_hash 0x23576098db512aefull -> 0x0af2277ce8d84ddcull
//   Random14/1/1: rows 0x2015326aa270647aull -> 0xb971c2930b3a05a1ull, final_hash 0x17f0015e425778cdull -> 0x59605ce3ec100938ull
//   Random14/1/0: final_hash 0x17f0015e425778cdull -> 0x59605ce3ec100938ull
//   Random15/0/1: rows 0x8ae4f630ecbec912ull -> 0x6fc22af9ae8f78b7ull, final_hash 0x8f8a1768584a1453ull -> 0x06a951b8fcd022a8ull
//   Random15/0/0: final_hash 0x8f8a1768584a1453ull -> 0x06a951b8fcd022a8ull
//   Random15/1/1: rows 0xbad5ea5f1a404b82ull -> 0xd1a6bf3cc2028ef0ull, final_hash 0x1c799ed4eb7895f8ull -> 0x07adfb5d03663f41ull
//   Random15/1/0: final_hash 0x1c799ed4eb7895f8ull -> 0x07adfb5d03663f41ull
//   Random16/0/1: rows 0x5567bcf3e8f88195ull -> 0x40812dd415f2169dull, final_hash 0x4018adaecf3028f5ull -> 0x6c21f4265d40e0faull
//   Random16/0/0: final_hash 0x4018adaecf3028f5ull -> 0x6c21f4265d40e0faull
//   Random16/1/1: rows 0x49d045afff3bf2f7ull -> 0xb78d778984de2a69ull, final_hash 0x70240df611bd4f60ull -> 0x1cf3ddb390cb09a8ull
//   Random16/1/0: final_hash 0x70240df611bd4f60ull -> 0x1cf3ddb390cb09a8ull
//   Random17/0/1: rows 0x9c4df741f7c02f5full -> 0x3d6109a76ec02939ull, final_hash 0xea071600f8078599ull -> 0x13fb7b04c7fb87a0ull
//   Random17/0/0: final_hash 0xea071600f8078599ull -> 0x13fb7b04c7fb87a0ull
//   Random17/1/1: rows 0x5a7b33f913d3227dull -> 0x614389249b75c0c4ull, final_hash 0x63cd6e30d2f52fa8ull -> 0xea85e6281f4dc72cull
//   Random17/1/0: final_hash 0x63cd6e30d2f52fa8ull -> 0xea85e6281f4dc72cull
//   Random18/0/1: rows 0x5a2d65a7c72124aaull -> 0xdfd317b932f92722ull, final_hash 0x0dcd836b3d6fbe33ull -> 0x46c43aea526400e2ull
//   Random18/0/0: final_hash 0x0dcd836b3d6fbe33ull -> 0x46c43aea526400e2ull
//   Random18/1/1: rows 0x4258c66d4b4502cfull -> 0x06733f1f7bbfd8aaull, final_hash 0x5ec74e1343309ba1ull -> 0x78865c7fc2f702d4ull
//   Random18/1/0: final_hash 0x5ec74e1343309ba1ull -> 0x78865c7fc2f702d4ull
//   Random19/0/1: rows 0xf10d22d15428ed8eull -> 0x125b2bc9db4016d6ull, final_hash 0x9cbd3ed2f164ed3eull -> 0x2311f974778a99b4ull
//   Random19/0/0: final_hash 0x9cbd3ed2f164ed3eull -> 0x2311f974778a99b4ull
//   Random19/1/1: rows 0x281cc9ecdd01f8c0ull -> 0x50dd65cb17d68460ull, final_hash 0xa082a78a0d36b86eull -> 0xddbe936d4e0be1d9ull
//   Random19/1/0: final_hash 0xa082a78a0d36b86eull -> 0xddbe936d4e0be1d9ull
//   Random20/0/1: rows 0x1f40df70ead1de45ull -> 0x28457d28e1347e95ull, final_hash 0x0763696b062b26f3ull -> 0x76f014d4090c5e50ull
//   Random20/0/0: final_hash 0x0763696b062b26f3ull -> 0x76f014d4090c5e50ull
//   Random20/1/1: rows 0x23d2beb6a698f6eeull -> 0xda9b3c31cf1aff65ull, final_hash 0xa69f9c6ccbd9d7b8ull -> 0x2546d46ddd4c3ec8ull
//   Random20/1/0: final_hash 0xa69f9c6ccbd9d7b8ull -> 0x2546d46ddd4c3ec8ull
//   Random21/0/1: rows 0x55c3e4e9f1980f7full -> 0xc31e3b3ef5f04061ull, final_hash 0xb4c0bd1efc7e3db0ull -> 0xdf157f1ca57094d5ull
//   Random21/0/0: final_hash 0xb4c0bd1efc7e3db0ull -> 0xdf157f1ca57094d5ull
//   Random21/1/1: rows 0x2c64416f4a102677ull -> 0xc595ba0beb082ff5ull, final_hash 0xab5d3c7b89060398ull -> 0xa9a07c092e5cbe4aull
//   Random21/1/0: final_hash 0xab5d3c7b89060398ull -> 0xa9a07c092e5cbe4aull
//   Random22/0/1: rows 0x0bbfad00b4df3a7full -> 0x39b1a1e214af1b71ull, final_hash 0x2bf9364b0ac4eb32ull -> 0x7eb096ac19c51be8ull
//   Random22/0/0: final_hash 0x2bf9364b0ac4eb32ull -> 0x7eb096ac19c51be8ull
//   Random22/1/1: rows 0xdc11eaaf39e88831ull -> 0xc4431133ab9f5316ull, final_hash 0xe35c4a15410ed42bull -> 0x94000240eb396914ull
//   Random22/1/0: final_hash 0xe35c4a15410ed42bull -> 0x94000240eb396914ull
//   Random23/0/1: rows 0x3bfbe74cca9733e5ull -> 0x88e9b334653c5d1bull, final_hash 0x487df27842af8759ull -> 0x692181ece740f520ull
//   Random23/0/0: final_hash 0x487df27842af8759ull -> 0x692181ece740f520ull
//   Random23/1/1: rows 0x8fee76ab99cfde45ull -> 0xdbc166eaee2702e7ull, final_hash 0xf63f5be919e87909ull -> 0x3be8e1140524bea0ull
//   Random23/1/0: final_hash 0xf63f5be919e87909ull -> 0x3be8e1140524bea0ull
//   Random24/0/1: rows 0xb21e7d012a7035a3ull -> 0x47bc4956ff1981bcull, final_hash 0x98fb8f7efe6f0a89ull -> 0xbc4022cad5d2678dull
//   Random24/0/0: final_hash 0x98fb8f7efe6f0a89ull -> 0xbc4022cad5d2678dull
//   Random24/1/1: rows 0x087459b6228d05b1ull -> 0x11b779b250bb8825ull, final_hash 0x401cc55e480d6705ull -> 0x16d6323c253a047aull
//   Random24/1/0: final_hash 0x401cc55e480d6705ull -> 0x16d6323c253a047aull
//   Random25/0/1: rows 0x05982cf88e3f467bull -> 0x630739fcfe5ae730ull, final_hash 0xe5be9623856c0d92ull -> 0x4e6c74a35bffd099ull
//   Random25/0/0: final_hash 0xe5be9623856c0d92ull -> 0x4e6c74a35bffd099ull
//   Random25/1/1: rows 0x96d96e663efa1237ull -> 0x6fc3d6a61b2b441full, final_hash 0x4422f4d8a37b2b56ull -> 0x24bf05514a48be71ull
//   Random25/1/0: final_hash 0x4422f4d8a37b2b56ull -> 0x24bf05514a48be71ull
//   Random26/0/1: rows 0x4fd2bb5dda406e86ull -> 0x19b737e3e153d627ull, final_hash 0x3eb30132f7a5a542ull -> 0x384bd64ea72fb4ecull
//   Random26/0/0: final_hash 0x3eb30132f7a5a542ull -> 0x384bd64ea72fb4ecull
//   Random26/1/1: rows 0xbd836e3ed26fb6abull -> 0x7ff976c7adad30c9ull, final_hash 0xc1d1437f6298b06eull -> 0x9fbbb22f2258f394ull
//   Random26/1/0: final_hash 0xc1d1437f6298b06eull -> 0x9fbbb22f2258f394ull
//   Random27/0/1: rows 0xf7ebc6bdd0c42c09ull -> 0xc4985ecbd440e079ull, final_hash 0x5dcf9c0f0ad797e0ull -> 0x78cb4a04dd3e3bbeull
//   Random27/0/0: final_hash 0x5dcf9c0f0ad797e0ull -> 0x78cb4a04dd3e3bbeull
//   Random27/1/1: rows 0xaff84e49bcbb7979ull -> 0xaf82312558447c3cull, final_hash 0xdee9a4ab6f07df36ull -> 0x13318af2b10abdc8ull
//   Random27/1/0: final_hash 0xdee9a4ab6f07df36ull -> 0x13318af2b10abdc8ull
//   Random28/0/1: rows 0x3e6d11eed068fc77ull -> 0x837ae299c489902bull, final_hash 0x947b69adb9c7a0ecull -> 0xe7d78d05779c99b9ull
//   Random28/0/0: final_hash 0x947b69adb9c7a0ecull -> 0xe7d78d05779c99b9ull
//   Random28/1/1: rows 0xfeae586bc47eaab3ull -> 0xc7422ca2f5c933a8ull, final_hash 0x6d7d832af0a5ca97ull -> 0xd1d952890fe01f6bull
//   Random28/1/0: final_hash 0x6d7d832af0a5ca97ull -> 0xd1d952890fe01f6bull
//   Random29/0/1: rows 0xc1ee491cccec694dull -> 0xe51c3f3e9eb33821ull, final_hash 0x231ea02044b650e3ull -> 0x4451f6b2205d7e11ull
//   Random29/0/0: final_hash 0x231ea02044b650e3ull -> 0x4451f6b2205d7e11ull
//   Random29/1/1: rows 0x72ed867a6ec57f36ull -> 0xa54b6f75f20f6c3bull, final_hash 0x2129aeae95e343b2ull -> 0xf117fc9f052fe924ull
//   Random29/1/0: final_hash 0x2129aeae95e343b2ull -> 0xf117fc9f052fe924ull
//   Random30/0/1: rows 0xddd5b5d0f3a7fd78ull -> 0x0ef1048e70bdbcaaull, final_hash 0x0b47b5dadeb26ed5ull -> 0x01825618a70d793full
//   Random30/0/0: final_hash 0x0b47b5dadeb26ed5ull -> 0x01825618a70d793full
//   Random30/1/1: rows 0x41ea60dc5e94e9beull -> 0x062767ebde3e6d67ull, final_hash 0x60169a8e9762ce80ull -> 0xe3fdfa3855cffa26ull
//   Random30/1/0: final_hash 0x60169a8e9762ce80ull -> 0xe3fdfa3855cffa26ull
//   Random31/0/1: rows 0x29e6d4090ef0f941ull -> 0x00d5ef982a177c39ull, final_hash 0xa0fc8947571d34caull -> 0x328698fea3a1c566ull
//   Random31/0/0: final_hash 0xa0fc8947571d34caull -> 0x328698fea3a1c566ull
//   Random31/1/1: rows 0x8ad112c1c7ca8231ull -> 0x3d3cf5c3e2bcf77cull, final_hash 0xb71dabeeba16154aull -> 0x7af158bf0533e727ull
//   Random31/1/0: final_hash 0xb71dabeeba16154aull -> 0x7af158bf0533e727ull
//   Random32/0/1: rows 0x5e685245df1a92b1ull -> 0x890bc160f9c55797ull, final_hash 0x235013f8265a98e1ull -> 0x43ddb6e16eeeb6ebull
//   Random32/0/0: final_hash 0x235013f8265a98e1ull -> 0x43ddb6e16eeeb6ebull
//   Random32/1/1: rows 0x164c26dcb267606cull -> 0xdd44e53ccf222547ull, final_hash 0x2fb370bcb0f0cf8dull -> 0x36cc0ba014ceb644ull
//   Random32/1/0: final_hash 0x2fb370bcb0f0cf8dull -> 0x36cc0ba014ceb644ull
//   Random33/0/1: rows 0x6bb54a8880813736ull -> 0xf44ae2f04c90d17bull, final_hash 0xc9b992a14193989dull -> 0xb84381e723b2760bull
//   Random33/0/0: final_hash 0xc9b992a14193989dull -> 0xb84381e723b2760bull
//   Random33/1/1: rows 0xbc7645c8660cae98ull -> 0x1a65c333da616a80ull, final_hash 0x7e42b4f1bd7a1507ull -> 0x98ed1c6560974e5dull
//   Random33/1/0: final_hash 0x7e42b4f1bd7a1507ull -> 0x98ed1c6560974e5dull
//   Random34/0/1: rows 0x260ada9c10443749ull -> 0x38b4c0761be2b9d5ull, final_hash 0x472ecf48ecca1173ull -> 0x13df4d82a8525485ull
//   Random34/0/0: final_hash 0x472ecf48ecca1173ull -> 0x13df4d82a8525485ull
//   Random34/1/1: rows 0xea3704b201eaecc5ull -> 0xff0836eedd61382bull, final_hash 0xd3a95210bad07d43ull -> 0x707b652568db11adull
//   Random34/1/0: final_hash 0xd3a95210bad07d43ull -> 0x707b652568db11adull
//   Random35/0/1: rows 0x5b4d137664c620e8ull -> 0x69d6a6449d989585ull, final_hash 0x511e0a4f20d51972ull -> 0x82f5d1edf3cab1a1ull
//   Random35/0/0: final_hash 0x511e0a4f20d51972ull -> 0x82f5d1edf3cab1a1ull
//   Random35/1/1: rows 0xe8e60ea0d9ee74a2ull -> 0xf7de574e8b68d626ull, final_hash 0x4323ea38535008e3ull -> 0x19afe24ced373259ull
//   Random35/1/0: final_hash 0x4323ea38535008e3ull -> 0x19afe24ced373259ull
//   Random36/0/1: rows 0xfee6d826f1d5a761ull -> 0xc99b7d8795acef57ull, final_hash 0xed180c1a0620dadeull -> 0xd352b754ee31c9daull
//   Random36/0/0: final_hash 0xed180c1a0620dadeull -> 0xd352b754ee31c9daull
//   Random36/1/1: rows 0x7c6cd77c69284902ull -> 0x9d51842db8c4a8efull, final_hash 0x4a04f1adc149777cull -> 0xdbf1ceb9708d5329ull
//   Random36/1/0: final_hash 0x4a04f1adc149777cull -> 0xdbf1ceb9708d5329ull
//   Random37/0/1: rows 0x5d4ac2b156d2f675ull -> 0xa996cdbcd7bab806ull, final_hash 0x682879f13c971e6dull -> 0x864c9319b1581ddeull
//   Random37/0/0: final_hash 0x682879f13c971e6dull -> 0x864c9319b1581ddeull
//   Random37/1/1: rows 0x4f863a72de6b3fe5ull -> 0xaceace7b1ef6a485ull, final_hash 0x192c83b1ace107ebull -> 0x124296cec661b644ull
//   Random37/1/0: final_hash 0x192c83b1ace107ebull -> 0x124296cec661b644ull
//   Random38/0/1: rows 0x85c46ec4136e744cull -> 0x3897c857ec18e67cull, final_hash 0x7eec0a12b20daae8ull -> 0xf97149ee834c0701ull
//   Random38/0/0: final_hash 0x7eec0a12b20daae8ull -> 0xf97149ee834c0701ull
//   Random38/1/1: rows 0x8f5b85237b508b59ull -> 0xd41a32f73f3f4d83ull, final_hash 0x552061144182d8bcull -> 0x6ffae2ca77f650e8ull
//   Random38/1/0: final_hash 0x552061144182d8bcull -> 0x6ffae2ca77f650e8ull
//   Random39/0/1: rows 0x692159fbbc9916ffull -> 0xcbb41e60faa7c837ull, final_hash 0xe18a13dd99a26db3ull -> 0xaf332a035d930716ull
//   Random39/0/0: final_hash 0xe18a13dd99a26db3ull -> 0xaf332a035d930716ull
//   Random39/1/1: rows 0x1df2019f1dbe8837ull -> 0xdc0684cb8ccc6236ull, final_hash 0xcea765ab6814b293ull -> 0x218d121b31d456e8ull
//   Random39/1/0: final_hash 0xcea765ab6814b293ull -> 0x218d121b31d456e8ull
//   Random40/0/1: rows 0x789937c730b9e9bbull -> 0x3d54cb6ef8fb231dull, final_hash 0xfb419d40757a63e2ull -> 0xae6a8c99f328998aull
//   Random40/0/0: final_hash 0xfb419d40757a63e2ull -> 0xae6a8c99f328998aull
//   Random40/1/1: rows 0xdebb12eda8056392ull -> 0x226a5179617d4724ull, final_hash 0x1fa80f6bd4ae77c6ull -> 0x528178f872eac4feull
//   Random40/1/0: final_hash 0x1fa80f6bd4ae77c6ull -> 0x528178f872eac4feull
//   Random41/0/1: rows 0x430d462f2d76b646ull -> 0x5f571ccd29c974a5ull, final_hash 0x6b989956c3f5eeebull -> 0xe8e8bbff74475dacull
//   Random41/0/0: final_hash 0x6b989956c3f5eeebull -> 0xe8e8bbff74475dacull
//   Random41/1/1: rows 0x66f328fad8341649ull -> 0x1ba5dea95b1bfd37ull, final_hash 0x7b12ead27bbfa2ecull -> 0xdcb155b4d707118bull
//   Random41/1/0: final_hash 0x7b12ead27bbfa2ecull -> 0xdcb155b4d707118bull
//   Random42/0/1: rows 0xbe3d6eb2e6a4c7c3ull -> 0x9c6622dda2639df5ull, final_hash 0x5b27e06e813fe7beull -> 0x694b7970a92d8de0ull
//   Random42/0/0: final_hash 0x5b27e06e813fe7beull -> 0x694b7970a92d8de0ull
//   Random42/1/1: rows 0x6584d28317150079ull -> 0x35e13c7e71b937f9ull, final_hash 0x0a068cf6dd8eddc9ull -> 0x04fd77b73748edc9ull
//   Random42/1/0: final_hash 0x0a068cf6dd8eddc9ull -> 0x04fd77b73748edc9ull
//   Random43/0/1: rows 0xcf402b333890b62eull -> 0xf25e2b35f033fa04ull, final_hash 0x536c599cd9c8a739ull -> 0x4b34fc0debfcdc0full
//   Random43/0/0: final_hash 0x536c599cd9c8a739ull -> 0x4b34fc0debfcdc0full
//   Random43/1/1: rows 0xdc63fbc117f5537aull -> 0xef94d02971fd8df7ull, final_hash 0x164c3c6f3502a304ull -> 0x5cc74085ab2e9cfcull
//   Random43/1/0: final_hash 0x164c3c6f3502a304ull -> 0x5cc74085ab2e9cfcull
//   Random44/0/1: rows 0xaf7e39b73b0bfa0dull -> 0xdb391f42efbf1389ull, final_hash 0x729c87f0529404c5ull -> 0xf0f20af42305c1edull
//   Random44/0/0: final_hash 0x729c87f0529404c5ull -> 0xf0f20af42305c1edull
//   Random44/1/1: rows 0xe0bc84e58d798d8cull -> 0x7628621da4f01c15ull, final_hash 0xd3d6450c34dc244dull -> 0x95de750495e4397eull
//   Random44/1/0: final_hash 0xd3d6450c34dc244dull -> 0x95de750495e4397eull
//   Random45/0/1: rows 0x7eff39c31367dc4eull -> 0x545d5101b7112b07ull, final_hash 0xf6aa0812820c9ca0ull -> 0xb19cfff99195e50aull
//   Random45/0/0: final_hash 0xf6aa0812820c9ca0ull -> 0xb19cfff99195e50aull
//   Random45/1/1: rows 0x348867a5feaedd62ull -> 0xe16493ad341fad02ull, final_hash 0x5dd63a78707932aaull -> 0x222bca1653cca5cdull
//   Random45/1/0: final_hash 0x5dd63a78707932aaull -> 0x222bca1653cca5cdull
//   Random46/0/1: rows 0xd03ecbed02fa5745ull -> 0xab1881781ed2dcddull, final_hash 0xd151bbd2debc06aaull -> 0xbf69c8b4a2e01a44ull
//   Random46/0/0: final_hash 0xd151bbd2debc06aaull -> 0xbf69c8b4a2e01a44ull
//   Random46/1/1: rows 0x00ce46d552347059ull -> 0x5a7e1996ab8d2f67ull, final_hash 0x5d2ba403a8ee44d0ull -> 0xfbf0e8931704bcdaull
//   Random46/1/0: final_hash 0x5d2ba403a8ee44d0ull -> 0xfbf0e8931704bcdaull
//   Random47/0/1: rows 0x30785ff127db15f0ull -> 0xe015a111bdafe05full, final_hash 0x918d89593d06130bull -> 0x52d52f80fc7ac8d6ull
//   Random47/0/0: final_hash 0x918d89593d06130bull -> 0x52d52f80fc7ac8d6ull
//   Random47/1/1: rows 0xd0a23bf29bc43b42ull -> 0x2f8f2e1a415c18ccull, final_hash 0xa94d3223369c69b4ull -> 0xbeba43572abe107cull
//   Random47/1/0: final_hash 0xa94d3223369c69b4ull -> 0xbeba43572abe107cull
//   Random48/0/1: rows 0xcc12d1e5e174d31aull -> 0xb7b9ec26cf4794bbull, final_hash 0xe6825902019c5ac6ull -> 0xb4798af50cf3f648ull
//   Random48/0/0: final_hash 0xe6825902019c5ac6ull -> 0xb4798af50cf3f648ull
//   Random48/1/1: rows 0x5fd282fd7b3fc869ull -> 0x57fee585d5ff2aa6ull, final_hash 0x9ad7a9208eecdda1ull -> 0xb95a18c6b589114dull
//   Random48/1/0: final_hash 0x9ad7a9208eecdda1ull -> 0xb95a18c6b589114dull
// expectation corrected (v19-E, 680 values), because v19-E folds live adapter state (pineforge-source-adapter/v4: retired placement rows are erased, the append-only logs fold as running digests; pineforge-pine-scheduler/v3); this host projects one fixed execution hash, so the continuation does not enter; the trades, the trade digest and the three counts did not move; harvested with its switch against main 10f20197 (every old pin reproduced), this tree and V19-E's tip 6e97f272 (identical rows):
//   Quiet/0/1: boundary 0x3a796809e596974dull -> 0xc1c15d580d2b91ddull, rows 0xc8c5fef2737653d1ull -> 0x6f2ef9fb0f46bdb6ull, final_hash 0x7fcbc2aeb4710017ull -> 0xfd89dcb68ffd9f6full
//   Quiet/0/0: boundary 0x3a796809e596974dull -> 0xc1c15d580d2b91ddull, final_hash 0x7fcbc2aeb4710017ull -> 0xfd89dcb68ffd9f6full
//   Quiet/1/1: boundary 0x3e5e8dc513df1e35ull -> 0x25925b8843f8c940ull, rows 0x0eb8899e312a2d42ull -> 0x3f01d574c39a983full, final_hash 0x9f59de4478395359ull -> 0x08472c64f2a3b409ull
//   Quiet/1/0: boundary 0x3e5e8dc513df1e35ull -> 0x25925b8843f8c940ull, final_hash 0x9f59de4478395359ull -> 0x08472c64f2a3b409ull
//   HoldLongFull/0/1: boundary 0x77c1e3a5e1143ba2ull -> 0xf7ee6fe81728a11eull, rows 0x89fbfe880001bea0ull -> 0x245fab3eff4d7559ull, final_hash 0x314185ce5f198ed2ull -> 0x68a09e458dc41ed9ull
//   HoldLongFull/0/0: boundary 0x77c1e3a5e1143ba2ull -> 0xf7ee6fe81728a11eull, final_hash 0x314185ce5f198ed2ull -> 0x68a09e458dc41ed9ull
//   HoldLongFull/1/1: boundary 0xbe0c95bd4cd89cadull -> 0x74ba83122973ae7bull, rows 0x66f6cddf7a5a74e4ull -> 0xa773cd9e004bc143ull, final_hash 0x2dc5bb26f3b04821ull -> 0xf7d8d3d0b26bcfb1ull
//   HoldLongFull/1/0: boundary 0xbe0c95bd4cd89cadull -> 0x74ba83122973ae7bull, final_hash 0x2dc5bb26f3b04821ull -> 0xf7d8d3d0b26bcfb1ull
//   MarginCalls/0/1: boundary 0xf9e74f3a91b15d94ull -> 0xf01f77fadacf9b1bull, rows 0xb1909807bff1764eull -> 0x720c4e8e6c1151d7ull, final_hash 0x2b840c18dd39f1c6ull -> 0x1423cb11eff138efull
//   MarginCalls/0/0: boundary 0xf9e74f3a91b15d94ull -> 0xf01f77fadacf9b1bull, final_hash 0x2b840c18dd39f1c6ull -> 0x1423cb11eff138efull
//   MarginCalls/1/1: boundary 0xaf3e6f518f7a0e2full -> 0x04552ef5dd6b0248ull, rows 0x513f1874b2ab78d7ull -> 0x34a90870ea8ff4dfull, final_hash 0x6c821aa201775ed6ull -> 0x39b0b7d450ea1d23ull
//   MarginCalls/1/0: boundary 0xaf3e6f518f7a0e2full -> 0x04552ef5dd6b0248ull, final_hash 0x6c821aa201775ed6ull -> 0x39b0b7d450ea1d23ull
//   MarginCallCycles/0/1: boundary 0x0b3084cf77ce43f2ull -> 0xe517ae23a9d9cd84ull, rows 0x7bba702b498455c7ull -> 0x09d31e44e56049a0ull, final_hash 0xcd53400b82e5c5efull -> 0xd6e5d9e3761d70f7ull
//   MarginCallCycles/0/0: boundary 0x0b3084cf77ce43f2ull -> 0xe517ae23a9d9cd84ull, final_hash 0xcd53400b82e5c5efull -> 0xd6e5d9e3761d70f7ull
//   MarginCallCycles/1/1: boundary 0x0eb82b1316349883ull -> 0x74fb06c3e47e091eull, rows 0xc177930f1e113f4dull -> 0x3015bbe850ee059dull, final_hash 0x0f82109b17bf1707ull -> 0x1a3d548af01065b8ull
//   MarginCallCycles/1/0: boundary 0x0eb82b1316349883ull -> 0x74fb06c3e47e091eull, final_hash 0x0f82109b17bf1707ull -> 0x1a3d548af01065b8ull
//   Bracket/0/1: boundary 0x06432083fa66b951ull -> 0xad121243d42812f0ull, rows 0x9c39620356824df1ull -> 0xc34f99dbe945355dull, final_hash 0x1bc18c13b13001d5ull -> 0xe393d0e31b6c86dbull
//   Bracket/0/0: boundary 0x06432083fa66b951ull -> 0xad121243d42812f0ull, final_hash 0x1bc18c13b13001d5ull -> 0xe393d0e31b6c86dbull
//   Bracket/1/1: boundary 0xc19a44d3151ae7cdull -> 0x42b52abab2bf3209ull, rows 0x90e62865c5f88613ull -> 0xee291f58ffb7864dull, final_hash 0xdf15239ff6e52b85ull -> 0xa953041c662027b9ull
//   Bracket/1/0: boundary 0xc19a44d3151ae7cdull -> 0x42b52abab2bf3209ull, final_hash 0xdf15239ff6e52b85ull -> 0xa953041c662027b9ull
//   Trail/0/1: boundary 0x6bed421c961133c2ull -> 0x38951256c0c8ae91ull, rows 0x3890234a486c20fbull -> 0x630e44444886533full, final_hash 0x1fb783b27fc69cfbull -> 0x0a3d59197f3fbeeeull
//   Trail/0/0: boundary 0x6bed421c961133c2ull -> 0x38951256c0c8ae91ull, final_hash 0x1fb783b27fc69cfbull -> 0x0a3d59197f3fbeeeull
//   Trail/1/1: boundary 0xeb781aa91194bd1cull -> 0x85f94fedc6115fe3ull, rows 0xc8b7a3b36dbb4168ull -> 0x100ea0b5c2385430ull, final_hash 0xca5f22b6b913959full -> 0x913d53654651ef64ull
//   Trail/1/0: boundary 0xeb781aa91194bd1cull -> 0x85f94fedc6115fe3ull, final_hash 0xca5f22b6b913959full -> 0x913d53654651ef64ull
//   DualStops/0/1: boundary 0xf6aaeda950ca9e9full -> 0xbe6893046a46cb96ull, rows 0x9bf10a8ebe9e9b42ull -> 0x95b5ee2aec79ec7cull, final_hash 0x73d3bc6580643304ull -> 0x3ebb8ec1e50f9ed1ull
//   DualStops/0/0: boundary 0xf6aaeda950ca9e9full -> 0xbe6893046a46cb96ull, final_hash 0x73d3bc6580643304ull -> 0x3ebb8ec1e50f9ed1ull
//   DualStops/1/1: boundary 0xdb47cf4ac5f5a7e3ull -> 0xebbf122a6c461306ull, rows 0x67aeaafbaca21d10ull -> 0xb4bc6d57975026c4ull, final_hash 0xa7092be33e24f8f9ull -> 0x7ee6f826d7537cc1ull
//   DualStops/1/0: boundary 0xdb47cf4ac5f5a7e3ull -> 0xebbf122a6c461306ull, final_hash 0xa7092be33e24f8f9ull -> 0x7ee6f826d7537cc1ull
//   InvertedStops/0/1: boundary 0x63d7083bd60d7b76ull -> 0x049f870d696c86d6ull, rows 0x3c74297c14f57223ull -> 0x6c97ddc3cc3bc830ull, final_hash 0x861579348f792fb6ull -> 0x92d980a9427423eeull
//   InvertedStops/0/0: boundary 0x63d7083bd60d7b76ull -> 0x049f870d696c86d6ull, final_hash 0x861579348f792fb6ull -> 0x92d980a9427423eeull
//   InvertedStops/1/1: boundary 0x122cf60fedfe6893ull -> 0x77643e8c893982c3ull, rows 0xd1e2c12219d76eb9ull -> 0xfaede1fa4ce13f90ull, final_hash 0xc98f7ac165556e38ull -> 0x18c13857d9dfa263ull
//   InvertedStops/1/0: boundary 0x122cf60fedfe6893ull -> 0x77643e8c893982c3ull, final_hash 0xc98f7ac165556e38ull -> 0x18c13857d9dfa263ull
//   PendingParent/0/1: boundary 0x2f5add8e063b291eull -> 0x6721cf5983214d9eull, rows 0x6830a53c32326397ull -> 0xc139da1140ad7563ull, final_hash 0xd90bc7468c38a9ebull -> 0xab2eaed98b0f009aull
//   PendingParent/0/0: boundary 0x2f5add8e063b291eull -> 0x6721cf5983214d9eull, final_hash 0xd90bc7468c38a9ebull -> 0xab2eaed98b0f009aull
//   PendingParent/1/1: boundary 0x3b740612b8fc9b1cull -> 0x298f49712d4af19aull, rows 0x8178bdae0e1a3f9eull -> 0xa624ebd2d2ec64a5ull, final_hash 0x885afddb8d023efeull -> 0x660fa440f23c5696ull
//   PendingParent/1/0: boundary 0x3b740612b8fc9b1cull -> 0x298f49712d4af19aull, final_hash 0x885afddb8d023efeull -> 0x660fa440f23c5696ull
//   GappedAdd/0/1: boundary 0xfccd2a52f56e4919ull -> 0x801838fc90033739ull, rows 0xe5c0cb20f459db08ull -> 0x8259b732e34ac913ull, final_hash 0x9ebb2f3cb5791396ull -> 0x58b9e653d25a4647ull
//   GappedAdd/0/0: boundary 0xfccd2a52f56e4919ull -> 0x801838fc90033739ull, final_hash 0x9ebb2f3cb5791396ull -> 0x58b9e653d25a4647ull
//   GappedAdd/1/1: boundary 0x104cb81d0f561ec3ull -> 0x8429bf8f9c179793ull, rows 0xf6ee5af1d423e76dull -> 0xa8491ce8abf7d3ffull, final_hash 0xf41267d4b469b3e6ull -> 0x803cb88784ce9026ull
//   GappedAdd/1/0: boundary 0x104cb81d0f561ec3ull -> 0x8429bf8f9c179793ull, final_hash 0xf41267d4b469b3e6ull -> 0x803cb88784ce9026ull
//   DeclinedReversal/0/1: boundary 0x3cd2b3ecbe08576full -> 0x4261900d11067be0ull, rows 0xbcc27c75c81f9cfcull -> 0x0e153d0a5a977c02ull, final_hash 0x0bb474c848b2df55ull -> 0xc97b9757bb1149e5ull
//   DeclinedReversal/0/0: boundary 0x3cd2b3ecbe08576full -> 0x4261900d11067be0ull, final_hash 0x0bb474c848b2df55ull -> 0xc97b9757bb1149e5ull
//   DeclinedReversal/1/1: boundary 0x1bbc907ccf07cbb4ull -> 0xae41af895aa1b10full, rows 0xacfeb029ac5b58a2ull -> 0x79084294eee2429aull, final_hash 0x1bd3ca07887fcbe8ull -> 0xb0669ffe8f0e2a01ull
//   DeclinedReversal/1/0: boundary 0x1bbc907ccf07cbb4ull -> 0xae41af895aa1b10full, final_hash 0x1bd3ca07887fcbe8ull -> 0xb0669ffe8f0e2a01ull
//   CalcOnFills/0/1: boundary 0xba4f5c603172fe8eull -> 0xfcdb5c78d844c1fdull, rows 0x1ee0e8388da66ecdull -> 0x8fce931c6789b396ull, final_hash 0xe2dca494134c93f1ull -> 0xfe5dc3ec475477d9ull
//   CalcOnFills/0/0: boundary 0xba4f5c603172fe8eull -> 0xfcdb5c78d844c1fdull, final_hash 0xe2dca494134c93f1ull -> 0xfe5dc3ec475477d9ull
//   CalcOnFills/1/1: boundary 0x640a77240fe579baull -> 0x8ae0abb767b7b0bbull, rows 0xbc525727c762e4f8ull -> 0x0b03280df257cee7ull, final_hash 0x7089025bc0e91262ull -> 0xcd17129763baf911ull
//   CalcOnFills/1/0: boundary 0x640a77240fe579baull -> 0x8ae0abb767b7b0bbull, final_hash 0x7089025bc0e91262ull -> 0xcd17129763baf911ull
//   CloseOrders/0/1: boundary 0xf7b97b5d5fe9e8a2ull -> 0x11242155751e54f3ull, rows 0xeec40b1fff0b8920ull -> 0x49267a5dd6a221c1ull, final_hash 0x51f605644deea034ull -> 0x2d39ec28cc86f100ull
//   CloseOrders/0/0: boundary 0xf7b97b5d5fe9e8a2ull -> 0x11242155751e54f3ull, final_hash 0x51f605644deea034ull -> 0x2d39ec28cc86f100ull
//   CloseOrders/1/1: boundary 0x6c3cb6fd4bdd2259ull -> 0xb4ba8101e18b4baeull, rows 0x9a52f0861ef94044ull -> 0x25941e4f7062f84bull, final_hash 0x10c3b0649de953bdull -> 0x6281f01aff005efeull
//   CloseOrders/1/0: boundary 0x6c3cb6fd4bdd2259ull -> 0xb4ba8101e18b4baeull, final_hash 0x10c3b0649de953bdull -> 0x6281f01aff005efeull
//   CloseOrdersSlipped/0/1: boundary 0xc31775f51ab71d4aull -> 0xf41f34b44f14ed1bull, rows 0xa2a9e3378f4b155aull -> 0x88cca9165f3f9ca5ull, final_hash 0x2bd4d450a501719bull -> 0x184081a51dee14b6ull
//   CloseOrdersSlipped/0/0: boundary 0xc31775f51ab71d4aull -> 0xf41f34b44f14ed1bull, final_hash 0x2bd4d450a501719bull -> 0x184081a51dee14b6ull
//   CloseOrdersSlipped/1/1: boundary 0xd5bda1340aa7860cull -> 0xefc7d84261df23b7ull, rows 0x21b19021bfd0becdull -> 0x0952c5f9a055c769ull, final_hash 0x4f7dc4d8f1ff659aull -> 0xdbb9a359df5b7ba7ull
//   CloseOrdersSlipped/1/0: boundary 0xd5bda1340aa7860cull -> 0xefc7d84261df23b7ull, final_hash 0x4f7dc4d8f1ff659aull -> 0xdbb9a359df5b7ba7ull
//   IntradayLoss/0/1: boundary 0xa2d0100260e3325cull -> 0x45d88fb7f59423e4ull, rows 0x1b8d617a36ed7938ull -> 0xf597c17beff44c38ull, final_hash 0x29b318c0c276f639ull -> 0xb18331055f670401ull
//   IntradayLoss/0/0: boundary 0xa2d0100260e3325cull -> 0x45d88fb7f59423e4ull, final_hash 0x29b318c0c276f639ull -> 0xb18331055f670401ull
//   IntradayLoss/1/1: boundary 0x4218985c7d966e5aull -> 0xdccf9450e8e7c00cull, rows 0x3feb98e03c9470ccull -> 0x488f8a32366e0cafull, final_hash 0x4c28e687b7134e5aull -> 0x08be7263bb9faf0eull
//   IntradayLoss/1/0: boundary 0x4218985c7d966e5aull -> 0xdccf9450e8e7c00cull, final_hash 0x4c28e687b7134e5aull -> 0x08be7263bb9faf0eull
//   IntradayCapDeferred/0/1: boundary 0x15da745af830ffcdull -> 0x477d0d155ce99044ull, rows 0x2866b495085da45bull -> 0xd36b92908117e322ull, final_hash 0xbc0f1bf5934650b3ull -> 0xd7b1c15ee8d83724ull
//   IntradayCapDeferred/0/0: boundary 0x15da745af830ffcdull -> 0x477d0d155ce99044ull, final_hash 0xbc0f1bf5934650b3ull -> 0xd7b1c15ee8d83724ull
//   IntradayCapDeferred/1/1: boundary 0x3cc7435072549abbull -> 0x1b27e4ab14777901ull, rows 0xcfeaab75eb875edbull -> 0xbc24b0cb7453804bull, final_hash 0xe597a163bba24ba0ull -> 0x58271303a14cab9bull
//   IntradayCapDeferred/1/0: boundary 0x3cc7435072549abbull -> 0x1b27e4ab14777901ull, final_hash 0xe597a163bba24ba0ull -> 0x58271303a14cab9bull
//   PreopenSlice/0/1: boundary 0xa25d1b40867ae09full -> 0x85bac68b85fa1b10ull, rows 0x8c551e84339a2da2ull -> 0xba4b398db082d988ull, final_hash 0x66bf7f75f3719669ull -> 0xc4645f9196ec1c52ull
//   PreopenSlice/0/0: boundary 0xa25d1b40867ae09full -> 0x85bac68b85fa1b10ull, final_hash 0x66bf7f75f3719669ull -> 0xc4645f9196ec1c52ull
//   PreopenSlice/1/1: boundary 0xa7f38895d5c93ad1ull -> 0x3c681ec4f8ea88e9ull, rows 0xe5284fbfbf5fd9baull -> 0xcfdbbdabfcceb515ull, final_hash 0xc6aff12be76ec96cull -> 0x43fed91080dfbea0ull
//   PreopenSlice/1/0: boundary 0xa7f38895d5c93ad1ull -> 0x3c681ec4f8ea88e9ull, final_hash 0xc6aff12be76ec96cull -> 0x43fed91080dfbea0ull
//   SameBarBatch/0/1: boundary 0xc0a908d47cd6851dull -> 0xd8c6014359e3436dull, rows 0xe0e239162893b61eull -> 0x56ce4bb7913dd63dull, final_hash 0x6e0321e00827705bull -> 0x1e3f3c2b9879352full
//   SameBarBatch/0/0: boundary 0xc0a908d47cd6851dull -> 0xd8c6014359e3436dull, final_hash 0x6e0321e00827705bull -> 0x1e3f3c2b9879352full
//   SameBarBatch/1/1: boundary 0xecdaef1c87a79565ull -> 0xb74c80da185d6f7cull, rows 0xbc52a698e423cf9full -> 0xd3659fc4f7228afcull, final_hash 0x0cf044723f142dabull -> 0xcc0463b3d75fe13cull
//   SameBarBatch/1/0: boundary 0xecdaef1c87a79565ull -> 0xb74c80da185d6f7cull, final_hash 0x0cf044723f142dabull -> 0xcc0463b3d75fe13cull
//   CommissionedShort/0/1: boundary 0x4c7e8fa831c332aeull -> 0xb3b9a9d400a706b7ull, rows 0x314377120173c3b2ull -> 0xe36126265b8bf576ull, final_hash 0xdd3b3e8c0e2c6b4full -> 0x23caa9e57f9f6fb8ull
//   CommissionedShort/0/0: boundary 0x4c7e8fa831c332aeull -> 0xb3b9a9d400a706b7ull, final_hash 0xdd3b3e8c0e2c6b4full -> 0x23caa9e57f9f6fb8ull
//   CommissionedShort/1/1: boundary 0xd54616be8ae3b83bull -> 0x1fba48a99176b809ull, rows 0xfa59d1a1daea5fa1ull -> 0x8599d32cc4e3f365ull, final_hash 0x21960f8f6bb14475ull -> 0xc1bb33f8955d526dull
//   CommissionedShort/1/0: boundary 0xd54616be8ae3b83bull -> 0x1fba48a99176b809ull, final_hash 0x21960f8f6bb14475ull -> 0xc1bb33f8955d526dull
//   AccountFx/0/1: boundary 0xb0c3cf3dca4c267cull -> 0x7ea7cc0696e4e706ull, rows 0x2fc1bbdf854e7bc2ull -> 0x5d06a315cd828e3aull, final_hash 0x0f440a3fe5b1668cull -> 0xf3a8b7577af41cbcull
//   AccountFx/0/0: boundary 0xb0c3cf3dca4c267cull -> 0x7ea7cc0696e4e706ull, final_hash 0x0f440a3fe5b1668cull -> 0xf3a8b7577af41cbcull
//   Stream/0/1: boundary 0xe42d13e76b592aaeull -> 0xc15d422469fc217full, rows 0x5d07d71d7af45568ull -> 0x424b81514797c8b8ull, final_hash 0x66dd516da3eb328aull -> 0xc9c5e2681a211fdaull
//   Stream/0/0: boundary 0xe42d13e76b592aaeull -> 0xc15d422469fc217full, final_hash 0x66dd516da3eb328aull -> 0xc9c5e2681a211fdaull
//   Random01/0/1: boundary 0x4d2031191e1093b0ull -> 0x7b26a7a4fd8ca837ull, rows 0xefa88fe4ae7387c2ull -> 0x8b38d2c0ee3ae09bull, final_hash 0x4e616563c94d80a9ull -> 0x266b3c97e2229932ull
//   Random01/0/0: boundary 0x4d2031191e1093b0ull -> 0x7b26a7a4fd8ca837ull, final_hash 0x4e616563c94d80a9ull -> 0x266b3c97e2229932ull
//   Random01/1/1: boundary 0x81cecf50938a562eull -> 0x240a7d06f5858192ull, rows 0xba484319dc1a9917ull -> 0x5a4bde6c69234036ull, final_hash 0x226dac1e01b42a77ull -> 0xc2da049b01eede0eull
//   Random01/1/0: boundary 0x81cecf50938a562eull -> 0x240a7d06f5858192ull, final_hash 0x226dac1e01b42a77ull -> 0xc2da049b01eede0eull
//   Random02/0/1: boundary 0x21c13d9ee67d7756ull -> 0xf50e670c28558c70ull, rows 0xc71e38b3565b0432ull -> 0xc237af8c6ea5d8a0ull, final_hash 0xd7cdc71d531a0847ull -> 0x785608d44d074ae6ull
//   Random02/0/0: boundary 0x21c13d9ee67d7756ull -> 0xf50e670c28558c70ull, final_hash 0xd7cdc71d531a0847ull -> 0x785608d44d074ae6ull
//   Random02/1/1: boundary 0x547842ec040b5cf3ull -> 0x32b5473f018894caull, rows 0x1175a94a033f9de7ull -> 0xbbb6be89045a263dull, final_hash 0xde0935dcf7e6e909ull -> 0x537ce6356efa7dcdull
//   Random02/1/0: boundary 0x547842ec040b5cf3ull -> 0x32b5473f018894caull, final_hash 0xde0935dcf7e6e909ull -> 0x537ce6356efa7dcdull
//   Random03/0/1: boundary 0xa646a7a6775183bbull -> 0x1058a57a23480c81ull, rows 0xb0f60628c32f69f7ull -> 0xc3e663e104e7a7ccull, final_hash 0xa72ac6eceb11da99ull -> 0x84c305db789fc12bull
//   Random03/0/0: boundary 0xa646a7a6775183bbull -> 0x1058a57a23480c81ull, final_hash 0xa72ac6eceb11da99ull -> 0x84c305db789fc12bull
//   Random03/1/1: boundary 0xdacf1b653735b785ull -> 0x7342e439906b6b8eull, rows 0x27230b8676426ae3ull -> 0x94231d2d60e49098ull, final_hash 0xb154de44035844a6ull -> 0xfe7cab3757c85231ull
//   Random03/1/0: boundary 0xdacf1b653735b785ull -> 0x7342e439906b6b8eull, final_hash 0xb154de44035844a6ull -> 0xfe7cab3757c85231ull
//   Random04/0/1: boundary 0x827062174c292e33ull -> 0xc803c2750b61157dull, rows 0xaf237f78a5d328e7ull -> 0x32e6028cb8a54dc8ull, final_hash 0x758619585ce0f06aull -> 0x84461297f142b9efull
//   Random04/0/0: boundary 0x827062174c292e33ull -> 0xc803c2750b61157dull, final_hash 0x758619585ce0f06aull -> 0x84461297f142b9efull
//   Random04/1/1: boundary 0x8e1f66ee2672c150ull -> 0xaf56d9261eb50536ull, rows 0x214b4a4921156211ull -> 0xfece30308981fb23ull, final_hash 0x5b7f8ccdd3a6841bull -> 0xcd21899a90f1d564ull
//   Random04/1/0: boundary 0x8e1f66ee2672c150ull -> 0xaf56d9261eb50536ull, final_hash 0x5b7f8ccdd3a6841bull -> 0xcd21899a90f1d564ull
//   Random05/0/1: boundary 0x4f6a298d6110d99eull -> 0xc8efa2a84661d593ull, rows 0x91e0916588b1704cull -> 0x90b1a6e9fbe4e13full, final_hash 0x2dfa41d9aa6a5fe6ull -> 0x2dd0cf2ce3ea761eull
//   Random05/0/0: boundary 0x4f6a298d6110d99eull -> 0xc8efa2a84661d593ull, final_hash 0x2dfa41d9aa6a5fe6ull -> 0x2dd0cf2ce3ea761eull
//   Random05/1/1: boundary 0x21419c17e261a575ull -> 0x18519fd8ab2ecf78ull, rows 0xe3e3928c248a40c7ull -> 0x967a69d448f6b43aull, final_hash 0x877dd69817370d17ull -> 0x1733e9f06b55202cull
//   Random05/1/0: boundary 0x21419c17e261a575ull -> 0x18519fd8ab2ecf78ull, final_hash 0x877dd69817370d17ull -> 0x1733e9f06b55202cull
//   Random06/0/1: boundary 0x207d998dcee18ab4ull -> 0x2a682a4ea3dcae53ull, rows 0xf87e3026be4daf31ull -> 0x1859b011dfeed652ull, final_hash 0xd6641c5032ea5abbull -> 0xbe2d610924b8c00dull
//   Random06/0/0: boundary 0x207d998dcee18ab4ull -> 0x2a682a4ea3dcae53ull, final_hash 0xd6641c5032ea5abbull -> 0xbe2d610924b8c00dull
//   Random06/1/1: boundary 0x1086fde5c53f188aull -> 0xcfa38c06a6347474ull, rows 0x5bd3fc90dad226dbull -> 0x14e300d8b24c6c40ull, final_hash 0xa1e760452fa23398ull -> 0x814550b3d8ab202aull
//   Random06/1/0: boundary 0x1086fde5c53f188aull -> 0xcfa38c06a6347474ull, final_hash 0xa1e760452fa23398ull -> 0x814550b3d8ab202aull
//   Random07/0/1: boundary 0x165d4fa1be350b54ull -> 0xa97e5a85a99d870full, rows 0x8260a8ca13421199ull -> 0x232efbe10e5b744dull, final_hash 0x02491a682b38ef59ull -> 0xc1850e436ab19a88ull
//   Random07/0/0: boundary 0x165d4fa1be350b54ull -> 0xa97e5a85a99d870full, final_hash 0x02491a682b38ef59ull -> 0xc1850e436ab19a88ull
//   Random07/1/1: boundary 0x3e85a873a858b8cdull -> 0xc6df3016ec6c9491ull, rows 0xcd97b21cdfd102efull -> 0x2f8088fa08ed52bdull, final_hash 0xdcdf0a34b0e9a35cull -> 0x24806c18caab7670ull
//   Random07/1/0: boundary 0x3e85a873a858b8cdull -> 0xc6df3016ec6c9491ull, final_hash 0xdcdf0a34b0e9a35cull -> 0x24806c18caab7670ull
//   Random08/0/1: boundary 0xa666797d7249ecd2ull -> 0xdf32ef61cd4e19eeull, rows 0x8d681de32aecc8adull -> 0xfd32cff83453f293ull, final_hash 0x5e781fd30e0fb6ceull -> 0x23acd097ec38f43aull
//   Random08/0/0: boundary 0xa666797d7249ecd2ull -> 0xdf32ef61cd4e19eeull, final_hash 0x5e781fd30e0fb6ceull -> 0x23acd097ec38f43aull
//   Random08/1/1: boundary 0x1fb681d8cfb64e4full -> 0x869ee3e727ef0b05ull, rows 0xda375994f45584a9ull -> 0x64f491f787b05affull, final_hash 0x9ae5022ef7fa5632ull -> 0x11144c9c51cb89f3ull
//   Random08/1/0: boundary 0x1fb681d8cfb64e4full -> 0x869ee3e727ef0b05ull, final_hash 0x9ae5022ef7fa5632ull -> 0x11144c9c51cb89f3ull
//   Random09/0/1: boundary 0x3ab5563f57f935ecull -> 0xf123ed994915e172ull, rows 0x69c959f9480c8a1eull -> 0xe262ad1d42b22aa2ull, final_hash 0xaece0e254934fe1dull -> 0x24d07dfd4c311e09ull
//   Random09/0/0: boundary 0x3ab5563f57f935ecull -> 0xf123ed994915e172ull, final_hash 0xaece0e254934fe1dull -> 0x24d07dfd4c311e09ull
//   Random09/1/1: boundary 0xf67d60931403ce44ull -> 0x28f03f19f3977a71ull, rows 0x7791c4f45e0e11ecull -> 0x29fa1939a71c5214ull, final_hash 0x9c5d95905697df4bull -> 0xf22f979ee11a143full
//   Random09/1/0: boundary 0xf67d60931403ce44ull -> 0x28f03f19f3977a71ull, final_hash 0x9c5d95905697df4bull -> 0xf22f979ee11a143full
//   Random10/0/1: boundary 0xf05ce13437fabeaeull -> 0xb3861d80e682c693ull, rows 0xdec3b813e785644dull -> 0xe5b5b2919d650a05ull, final_hash 0x770c69baf1edf99dull -> 0x92408fc414e06780ull
//   Random10/0/0: boundary 0xf05ce13437fabeaeull -> 0xb3861d80e682c693ull, final_hash 0x770c69baf1edf99dull -> 0x92408fc414e06780ull
//   Random10/1/1: boundary 0x20095f145e036998ull -> 0x69fe10c28684def0ull, rows 0x562e19a136a4ae51ull -> 0x10dfb593a8ec827cull, final_hash 0xf199c5b63c7d0ca7ull -> 0x6a8371ba10724f52ull
//   Random10/1/0: boundary 0x20095f145e036998ull -> 0x69fe10c28684def0ull, final_hash 0xf199c5b63c7d0ca7ull -> 0x6a8371ba10724f52ull
//   Random11/0/1: boundary 0xf1ee73f462e322ffull -> 0x90100a03a86d0cdbull, rows 0xb6855c923d53018cull -> 0x016b0db6bef2264cull, final_hash 0x71586d5705ef9191ull -> 0x185f0857180c5a28ull
//   Random11/0/0: boundary 0xf1ee73f462e322ffull -> 0x90100a03a86d0cdbull, final_hash 0x71586d5705ef9191ull -> 0x185f0857180c5a28ull
//   Random11/1/1: boundary 0x4480ce0e9b981361ull -> 0x84170dd33baa79d9ull, rows 0xfc8b17e895298685ull -> 0x1a59bcbe0a910135ull, final_hash 0xa78c34f201e438e5ull -> 0x41861834717a9917ull
//   Random11/1/0: boundary 0x4480ce0e9b981361ull -> 0x84170dd33baa79d9ull, final_hash 0xa78c34f201e438e5ull -> 0x41861834717a9917ull
//   Random12/0/1: boundary 0xcb7a9a8ed7fb6443ull -> 0xc3249ab4cf05233bull, rows 0x64bc55e2ec0d8826ull -> 0xf9f7de7a15130b6cull, final_hash 0xbe602d852ba7e436ull -> 0x39560e1c6a315bbdull
//   Random12/0/0: boundary 0xcb7a9a8ed7fb6443ull -> 0xc3249ab4cf05233bull, final_hash 0xbe602d852ba7e436ull -> 0x39560e1c6a315bbdull
//   Random12/1/1: boundary 0x0ea84080b0b164c1ull -> 0x4ac8b5d9d42b86f5ull, rows 0xa4a6551ff356ecf3ull -> 0xb525118494281087ull, final_hash 0x7f2a5aec0a9ea850ull -> 0x322ae8f4196b3e75ull
//   Random12/1/0: boundary 0x0ea84080b0b164c1ull -> 0x4ac8b5d9d42b86f5ull, final_hash 0x7f2a5aec0a9ea850ull -> 0x322ae8f4196b3e75ull
//   Random13/0/1: boundary 0x80722153bbe06a5full -> 0xc330ac11cbbddd41ull, rows 0xd2b7c2c08f19d53full -> 0xe0a3d24e78674f95ull, final_hash 0xf4212ccf834ef291ull -> 0xefab5484e3574594ull
//   Random13/0/0: boundary 0x80722153bbe06a5full -> 0xc330ac11cbbddd41ull, final_hash 0xf4212ccf834ef291ull -> 0xefab5484e3574594ull
//   Random13/1/1: boundary 0xcd91de6cb5d23ce8ull -> 0xcdb3c6546824667dull, rows 0x8d492fd03bf54db0ull -> 0x653d5c4469730325ull, final_hash 0x66846aa435c10ff3ull -> 0x027984836057c1fbull
//   Random13/1/0: boundary 0xcd91de6cb5d23ce8ull -> 0xcdb3c6546824667dull, final_hash 0x66846aa435c10ff3ull -> 0x027984836057c1fbull
//   Random14/0/1: boundary 0x02f8f843d94bb9d0ull -> 0x3bc54d3dda74eed1ull, rows 0xe5206f2023c77213ull -> 0x39e24e8c4c0cf5e8ull, final_hash 0x0af2277ce8d84ddcull -> 0x4da290343bef0430ull
//   Random14/0/0: boundary 0x02f8f843d94bb9d0ull -> 0x3bc54d3dda74eed1ull, final_hash 0x0af2277ce8d84ddcull -> 0x4da290343bef0430ull
//   Random14/1/1: boundary 0x9655806fda540677ull -> 0x082506120a682f91ull, rows 0xb971c2930b3a05a1ull -> 0xa87115943d8a4576ull, final_hash 0x59605ce3ec100938ull -> 0xaf54857aebe2069eull
//   Random14/1/0: boundary 0x9655806fda540677ull -> 0x082506120a682f91ull, final_hash 0x59605ce3ec100938ull -> 0xaf54857aebe2069eull
//   Random15/0/1: boundary 0x9f843a2c67213404ull -> 0x8d19883bfd7b9827ull, rows 0x6fc22af9ae8f78b7ull -> 0x617a8b521ffd19a8ull, final_hash 0x06a951b8fcd022a8ull -> 0xc5c912afdfb5429eull
//   Random15/0/0: boundary 0x9f843a2c67213404ull -> 0x8d19883bfd7b9827ull, final_hash 0x06a951b8fcd022a8ull -> 0xc5c912afdfb5429eull
//   Random15/1/1: boundary 0xf4d6b20326bdd170ull -> 0xce37c225571916fbull, rows 0xd1a6bf3cc2028ef0ull -> 0x5ea9fcc2c1385c3full, final_hash 0x07adfb5d03663f41ull -> 0xf1a244775b55839dull
//   Random15/1/0: boundary 0xf4d6b20326bdd170ull -> 0xce37c225571916fbull, final_hash 0x07adfb5d03663f41ull -> 0xf1a244775b55839dull
//   Random16/0/1: boundary 0x41fc01b452c91af6ull -> 0xc6936cda82ca4015ull, rows 0x40812dd415f2169dull -> 0x92ab4831c042bfa2ull, final_hash 0x6c21f4265d40e0faull -> 0x3ee1c25f2c9ce286ull
//   Random16/0/0: boundary 0x41fc01b452c91af6ull -> 0xc6936cda82ca4015ull, final_hash 0x6c21f4265d40e0faull -> 0x3ee1c25f2c9ce286ull
//   Random16/1/1: boundary 0xc1bdd941a22efda4ull -> 0xd54109c1c01d45ceull, rows 0xb78d778984de2a69ull -> 0x682f3f78dc824bc8ull, final_hash 0x1cf3ddb390cb09a8ull -> 0xdde93ec35437e7afull
//   Random16/1/0: boundary 0xc1bdd941a22efda4ull -> 0xd54109c1c01d45ceull, final_hash 0x1cf3ddb390cb09a8ull -> 0xdde93ec35437e7afull
//   Random17/0/1: boundary 0xfac00cfdfac618c2ull -> 0x6304fa3c28e07e49ull, rows 0x3d6109a76ec02939ull -> 0xf1ed3675ad25ec11ull, final_hash 0x13fb7b04c7fb87a0ull -> 0xfda9c1caff36448bull
//   Random17/0/0: boundary 0xfac00cfdfac618c2ull -> 0x6304fa3c28e07e49ull, final_hash 0x13fb7b04c7fb87a0ull -> 0xfda9c1caff36448bull
//   Random17/1/1: boundary 0x79760a6773857205ull -> 0x91c41f4b53c60db8ull, rows 0x614389249b75c0c4ull -> 0x4927f647a64a6aa1ull, final_hash 0xea85e6281f4dc72cull -> 0xb386dd8b4f22a827ull
//   Random17/1/0: boundary 0x79760a6773857205ull -> 0x91c41f4b53c60db8ull, final_hash 0xea85e6281f4dc72cull -> 0xb386dd8b4f22a827ull
//   Random18/0/1: boundary 0xe249d7d0fb43cc30ull -> 0x2f90853903ca8075ull, rows 0xdfd317b932f92722ull -> 0x9614b30d8372e1b6ull, final_hash 0x46c43aea526400e2ull -> 0x827427f9640b4e97ull
//   Random18/0/0: boundary 0xe249d7d0fb43cc30ull -> 0x2f90853903ca8075ull, final_hash 0x46c43aea526400e2ull -> 0x827427f9640b4e97ull
//   Random18/1/1: boundary 0x55d6fccae9a2f966ull -> 0xd759ce46e3583dd6ull, rows 0x06733f1f7bbfd8aaull -> 0x4d62656618897636ull, final_hash 0x78865c7fc2f702d4ull -> 0x35a46c04db974779ull
//   Random18/1/0: boundary 0x55d6fccae9a2f966ull -> 0xd759ce46e3583dd6ull, final_hash 0x78865c7fc2f702d4ull -> 0x35a46c04db974779ull
//   Random19/0/1: boundary 0x5a75ef92c1272683ull -> 0x429d9a4cff7068fcull, rows 0x125b2bc9db4016d6ull -> 0xe717295c664e09e9ull, final_hash 0x2311f974778a99b4ull -> 0x04e27e2d65aa1452ull
//   Random19/0/0: boundary 0x5a75ef92c1272683ull -> 0x429d9a4cff7068fcull, final_hash 0x2311f974778a99b4ull -> 0x04e27e2d65aa1452ull
//   Random19/1/1: boundary 0xf3613b0d3b151a50ull -> 0x1e2eaa0ecfeb3392ull, rows 0x50dd65cb17d68460ull -> 0xc850e8283034d833ull, final_hash 0xddbe936d4e0be1d9ull -> 0x88071f404445eb2bull
//   Random19/1/0: boundary 0xf3613b0d3b151a50ull -> 0x1e2eaa0ecfeb3392ull, final_hash 0xddbe936d4e0be1d9ull -> 0x88071f404445eb2bull
//   Random20/0/1: boundary 0xd87ae7800328fc80ull -> 0x8caa054b399b232aull, rows 0x28457d28e1347e95ull -> 0x4fc0f37c9645d067ull, final_hash 0x76f014d4090c5e50ull -> 0x9facee5fae21aa7cull
//   Random20/0/0: boundary 0xd87ae7800328fc80ull -> 0x8caa054b399b232aull, final_hash 0x76f014d4090c5e50ull -> 0x9facee5fae21aa7cull
//   Random20/1/1: boundary 0x9c68d69917793d70ull -> 0x0bed3f4e6ae2ef8full, rows 0xda9b3c31cf1aff65ull -> 0xbc77d10bf614561dull, final_hash 0x2546d46ddd4c3ec8ull -> 0x0d9fbc941287b1cfull
//   Random20/1/0: boundary 0x9c68d69917793d70ull -> 0x0bed3f4e6ae2ef8full, final_hash 0x2546d46ddd4c3ec8ull -> 0x0d9fbc941287b1cfull
//   Random21/0/1: boundary 0x65e7f42742aa9796ull -> 0x546f79d5d3874035ull, rows 0xc31e3b3ef5f04061ull -> 0x57db503858f2c920ull, final_hash 0xdf157f1ca57094d5ull -> 0x6a7c93c51185957aull
//   Random21/0/0: boundary 0x65e7f42742aa9796ull -> 0x546f79d5d3874035ull, final_hash 0xdf157f1ca57094d5ull -> 0x6a7c93c51185957aull
//   Random21/1/1: boundary 0x1f425961da5eb5bdull -> 0x61ab9c4cf365da62ull, rows 0xc595ba0beb082ff5ull -> 0x6034eb0b07bc00b6ull, final_hash 0xa9a07c092e5cbe4aull -> 0x8a67a9fe7abd9c00ull
//   Random21/1/0: boundary 0x1f425961da5eb5bdull -> 0x61ab9c4cf365da62ull, final_hash 0xa9a07c092e5cbe4aull -> 0x8a67a9fe7abd9c00ull
//   Random22/0/1: boundary 0xf55529ea4200864eull -> 0x7264962e97a25a80ull, rows 0x39b1a1e214af1b71ull -> 0x744915b658d1f5d1ull, final_hash 0x7eb096ac19c51be8ull -> 0x1a6bcb392d46b371ull
//   Random22/0/0: boundary 0xf55529ea4200864eull -> 0x7264962e97a25a80ull, final_hash 0x7eb096ac19c51be8ull -> 0x1a6bcb392d46b371ull
//   Random22/1/1: boundary 0x0d2e1d8f45ef15c3ull -> 0x47d74c02fc2519e4ull, rows 0xc4431133ab9f5316ull -> 0x7443f9b7143dacd2ull, final_hash 0x94000240eb396914ull -> 0xa04d40cbbd623427ull
//   Random22/1/0: boundary 0x0d2e1d8f45ef15c3ull -> 0x47d74c02fc2519e4ull, final_hash 0x94000240eb396914ull -> 0xa04d40cbbd623427ull
//   Random23/0/1: boundary 0xdf20c50abfd250a5ull -> 0xfa4d25cb6d9764cbull, rows 0x88e9b334653c5d1bull -> 0xdea9813bde333195ull, final_hash 0x692181ece740f520ull -> 0x5058d81da9e2cf89ull
//   Random23/0/0: boundary 0xdf20c50abfd250a5ull -> 0xfa4d25cb6d9764cbull, final_hash 0x692181ece740f520ull -> 0x5058d81da9e2cf89ull
//   Random23/1/1: boundary 0x5200e99ec0a2cc86ull -> 0xd2626d74d1ddbe27ull, rows 0xdbc166eaee2702e7ull -> 0x55431bc925b28038ull, final_hash 0x3be8e1140524bea0ull -> 0x510aa1745bdba1c7ull
//   Random23/1/0: boundary 0x5200e99ec0a2cc86ull -> 0xd2626d74d1ddbe27ull, final_hash 0x3be8e1140524bea0ull -> 0x510aa1745bdba1c7ull
//   Random24/0/1: boundary 0x0e2723a2532c49bdull -> 0x1972ae0bc903694dull, rows 0x47bc4956ff1981bcull -> 0x2b240d2a365b8703ull, final_hash 0xbc4022cad5d2678dull -> 0x82e72364e836634full
//   Random24/0/0: boundary 0x0e2723a2532c49bdull -> 0x1972ae0bc903694dull, final_hash 0xbc4022cad5d2678dull -> 0x82e72364e836634full
//   Random24/1/1: boundary 0x10b0c163c46253a3ull -> 0x3df4ec3fe0efc36full, rows 0x11b779b250bb8825ull -> 0x8ee35827bfb734d3ull, final_hash 0x16d6323c253a047aull -> 0x521e8d8456c37b6dull
//   Random24/1/0: boundary 0x10b0c163c46253a3ull -> 0x3df4ec3fe0efc36full, final_hash 0x16d6323c253a047aull -> 0x521e8d8456c37b6dull
//   Random25/0/1: boundary 0x85e966b1efffc2d9ull -> 0x34fc7d2dd7c74f50ull, rows 0x630739fcfe5ae730ull -> 0xd5bc5831c840df16ull, final_hash 0x4e6c74a35bffd099ull -> 0x74d22f0254d1d14aull
//   Random25/0/0: boundary 0x85e966b1efffc2d9ull -> 0x34fc7d2dd7c74f50ull, final_hash 0x4e6c74a35bffd099ull -> 0x74d22f0254d1d14aull
//   Random25/1/1: boundary 0x2b3457067a8659c4ull -> 0x2da85270ed8b8e32ull, rows 0x6fc3d6a61b2b441full -> 0xd9976f48a3e5d4d6ull, final_hash 0x24bf05514a48be71ull -> 0xe27506e279646458ull
//   Random25/1/0: boundary 0x2b3457067a8659c4ull -> 0x2da85270ed8b8e32ull, final_hash 0x24bf05514a48be71ull -> 0xe27506e279646458ull
//   Random26/0/1: boundary 0x466cabe128357bc7ull -> 0xf3bbd59278bbf967ull, rows 0x19b737e3e153d627ull -> 0x1bc27907f389539aull, final_hash 0x384bd64ea72fb4ecull -> 0x86792f22105fe886ull
//   Random26/0/0: boundary 0x466cabe128357bc7ull -> 0xf3bbd59278bbf967ull, final_hash 0x384bd64ea72fb4ecull -> 0x86792f22105fe886ull
//   Random26/1/1: boundary 0x48a41c8b150e4acaull -> 0x14e61f95689ec8dfull, rows 0x7ff976c7adad30c9ull -> 0x2117af0b816b84ebull, final_hash 0x9fbbb22f2258f394ull -> 0x32b7fe08ce5005d2ull
//   Random26/1/0: boundary 0x48a41c8b150e4acaull -> 0x14e61f95689ec8dfull, final_hash 0x9fbbb22f2258f394ull -> 0x32b7fe08ce5005d2ull
//   Random27/0/1: boundary 0xe13c1b51c263969eull -> 0x75311b332bc3b546ull, rows 0xc4985ecbd440e079ull -> 0xa06c0a5c0993ce52ull, final_hash 0x78cb4a04dd3e3bbeull -> 0x9806c99ed2cb07fcull
//   Random27/0/0: boundary 0xe13c1b51c263969eull -> 0x75311b332bc3b546ull, final_hash 0x78cb4a04dd3e3bbeull -> 0x9806c99ed2cb07fcull
//   Random27/1/1: boundary 0x059a0e952db0e735ull -> 0x221564757dacff73ull, rows 0xaf82312558447c3cull -> 0xf5686759b991c5f1ull, final_hash 0x13318af2b10abdc8ull -> 0x91aeda18b48beb19ull
//   Random27/1/0: boundary 0x059a0e952db0e735ull -> 0x221564757dacff73ull, final_hash 0x13318af2b10abdc8ull -> 0x91aeda18b48beb19ull
//   Random28/0/1: boundary 0xc47e3d61f7b3dc86ull -> 0x5afaa18295f32ff1ull, rows 0x837ae299c489902bull -> 0xca9c90143f0b6a95ull, final_hash 0xe7d78d05779c99b9ull -> 0xe019b821239c3290ull
//   Random28/0/0: boundary 0xc47e3d61f7b3dc86ull -> 0x5afaa18295f32ff1ull, final_hash 0xe7d78d05779c99b9ull -> 0xe019b821239c3290ull
//   Random28/1/1: boundary 0x0e25d83eb45f6748ull -> 0x6d2667eba32721eeull, rows 0xc7422ca2f5c933a8ull -> 0x3c23578f7764250aull, final_hash 0xd1d952890fe01f6bull -> 0xdb07854a60f415d7ull
//   Random28/1/0: boundary 0x0e25d83eb45f6748ull -> 0x6d2667eba32721eeull, final_hash 0xd1d952890fe01f6bull -> 0xdb07854a60f415d7ull
//   Random29/0/1: boundary 0xd45d1f15ed599c98ull -> 0xd1301252d9b58f96ull, rows 0xe51c3f3e9eb33821ull -> 0xa789410c6a57a22aull, final_hash 0x4451f6b2205d7e11ull -> 0x595deac764871057ull
//   Random29/0/0: boundary 0xd45d1f15ed599c98ull -> 0xd1301252d9b58f96ull, final_hash 0x4451f6b2205d7e11ull -> 0x595deac764871057ull
//   Random29/1/1: boundary 0x106585599aaf1127ull -> 0x1a25b83f5e6fed46ull, rows 0xa54b6f75f20f6c3bull -> 0x85c4d848e014bb3cull, final_hash 0xf117fc9f052fe924ull -> 0x13fb019ad44bec5aull
//   Random29/1/0: boundary 0x106585599aaf1127ull -> 0x1a25b83f5e6fed46ull, final_hash 0xf117fc9f052fe924ull -> 0x13fb019ad44bec5aull
//   Random30/0/1: boundary 0xbd6fdf2ee11f522cull -> 0xa9365ffd65e05a09ull, rows 0x0ef1048e70bdbcaaull -> 0xc88de1ce1dcfafe1ull, final_hash 0x01825618a70d793full -> 0x134c921985714562ull
//   Random30/0/0: boundary 0xbd6fdf2ee11f522cull -> 0xa9365ffd65e05a09ull, final_hash 0x01825618a70d793full -> 0x134c921985714562ull
//   Random30/1/1: boundary 0xae962d3f93cfbdb4ull -> 0x1facc2c1cfd4c971ull, rows 0x062767ebde3e6d67ull -> 0x941084ae8c1b103bull, final_hash 0xe3fdfa3855cffa26ull -> 0xf2973241ae3d2097ull
//   Random30/1/0: boundary 0xae962d3f93cfbdb4ull -> 0x1facc2c1cfd4c971ull, final_hash 0xe3fdfa3855cffa26ull -> 0xf2973241ae3d2097ull
//   Random31/0/1: boundary 0x351f4cff87f23dffull -> 0x8eaa06b36232a908ull, rows 0x00d5ef982a177c39ull -> 0xc98390a9e23599d3ull, final_hash 0x328698fea3a1c566ull -> 0xb86249f3491f97b0ull
//   Random31/0/0: boundary 0x351f4cff87f23dffull -> 0x8eaa06b36232a908ull, final_hash 0x328698fea3a1c566ull -> 0xb86249f3491f97b0ull
//   Random31/1/1: boundary 0x44f284ea7d48f89dull -> 0xf90b0737ee4a025aull, rows 0x3d3cf5c3e2bcf77cull -> 0x09ec11f94ae3f675ull, final_hash 0x7af158bf0533e727ull -> 0xc7f3afe378a05865ull
//   Random31/1/0: boundary 0x44f284ea7d48f89dull -> 0xf90b0737ee4a025aull, final_hash 0x7af158bf0533e727ull -> 0xc7f3afe378a05865ull
//   Random32/0/1: boundary 0x5cda5928257d8c29ull -> 0x32da5eed595ee5b4ull, rows 0x890bc160f9c55797ull -> 0xec4a0ece9475e1d3ull, final_hash 0x43ddb6e16eeeb6ebull -> 0x4b6c0af81cc429f5ull
//   Random32/0/0: boundary 0x5cda5928257d8c29ull -> 0x32da5eed595ee5b4ull, final_hash 0x43ddb6e16eeeb6ebull -> 0x4b6c0af81cc429f5ull
//   Random32/1/1: boundary 0xc0a991ac990da922ull -> 0xe733831f206a336aull, rows 0xdd44e53ccf222547ull -> 0xb4483230298afef5ull, final_hash 0x36cc0ba014ceb644ull -> 0xe6f2b4aa4d8611a1ull
//   Random32/1/0: boundary 0xc0a991ac990da922ull -> 0xe733831f206a336aull, final_hash 0x36cc0ba014ceb644ull -> 0xe6f2b4aa4d8611a1ull
//   Random33/0/1: boundary 0x5e85ba42d9c1842bull -> 0xc70a47f8a53011e5ull, rows 0xf44ae2f04c90d17bull -> 0xd0fd26991c9583c5ull, final_hash 0xb84381e723b2760bull -> 0xbe29a314289e811cull
//   Random33/0/0: boundary 0x5e85ba42d9c1842bull -> 0xc70a47f8a53011e5ull, final_hash 0xb84381e723b2760bull -> 0xbe29a314289e811cull
//   Random33/1/1: boundary 0xf96d89747745cbeeull -> 0x26028452eb411830ull, rows 0x1a65c333da616a80ull -> 0x5c2f2e8de242605bull, final_hash 0x98ed1c6560974e5dull -> 0x1122e3cca5beb09dull
//   Random33/1/0: boundary 0xf96d89747745cbeeull -> 0x26028452eb411830ull, final_hash 0x98ed1c6560974e5dull -> 0x1122e3cca5beb09dull
//   Random34/0/1: boundary 0x530514ecf057f1a3ull -> 0x9191daa7f287cff2ull, rows 0x38b4c0761be2b9d5ull -> 0x1351d75d55b69237ull, final_hash 0x13df4d82a8525485ull -> 0x53efe9facdfed383ull
//   Random34/0/0: boundary 0x530514ecf057f1a3ull -> 0x9191daa7f287cff2ull, final_hash 0x13df4d82a8525485ull -> 0x53efe9facdfed383ull
//   Random34/1/1: boundary 0x8e924b043e50c9faull -> 0xe3bd733dbb7dd142ull, rows 0xff0836eedd61382bull -> 0xcc37232ed176c590ull, final_hash 0x707b652568db11adull -> 0x3c1c21ded8a43a85ull
//   Random34/1/0: boundary 0x8e924b043e50c9faull -> 0xe3bd733dbb7dd142ull, final_hash 0x707b652568db11adull -> 0x3c1c21ded8a43a85ull
//   Random35/0/1: boundary 0xde7f1dbd07f699ebull -> 0xa1dad91ce6005cbbull, rows 0x69d6a6449d989585ull -> 0xac8c4802e8a8b254ull, final_hash 0x82f5d1edf3cab1a1ull -> 0xc8f7b4decfbdeed1ull
//   Random35/0/0: boundary 0xde7f1dbd07f699ebull -> 0xa1dad91ce6005cbbull, final_hash 0x82f5d1edf3cab1a1ull -> 0xc8f7b4decfbdeed1ull
//   Random35/1/1: boundary 0xbe5633d8c32d145cull -> 0x6ce4272e31e5439eull, rows 0xf7de574e8b68d626ull -> 0xf840ac1e5c21e71full, final_hash 0x19afe24ced373259ull -> 0x0020809913835e3dull
//   Random35/1/0: boundary 0xbe5633d8c32d145cull -> 0x6ce4272e31e5439eull, final_hash 0x19afe24ced373259ull -> 0x0020809913835e3dull
//   Random36/0/1: boundary 0xeac1383f4fb063c9ull -> 0xb6ed02402286f1daull, rows 0xc99b7d8795acef57ull -> 0xb93cb982740efaf2ull, final_hash 0xd352b754ee31c9daull -> 0x99b1254e54e63283ull
//   Random36/0/0: boundary 0xeac1383f4fb063c9ull -> 0xb6ed02402286f1daull, final_hash 0xd352b754ee31c9daull -> 0x99b1254e54e63283ull
//   Random36/1/1: boundary 0xe6852926eb00cc00ull -> 0xe4e7726a4db33f47ull, rows 0x9d51842db8c4a8efull -> 0xbbc67cbeef5f3d57ull, final_hash 0xdbf1ceb9708d5329ull -> 0xaf91a8392845aa5aull
//   Random36/1/0: boundary 0xe6852926eb00cc00ull -> 0xe4e7726a4db33f47ull, final_hash 0xdbf1ceb9708d5329ull -> 0xaf91a8392845aa5aull
//   Random37/0/1: boundary 0x1003607a5f8353cbull -> 0xf50f40fe8bb210c2ull, rows 0xa996cdbcd7bab806ull -> 0x6830953a611d9b95ull, final_hash 0x864c9319b1581ddeull -> 0x218cf254fd19cf8aull
//   Random37/0/0: boundary 0x1003607a5f8353cbull -> 0xf50f40fe8bb210c2ull, final_hash 0x864c9319b1581ddeull -> 0x218cf254fd19cf8aull
//   Random37/1/1: boundary 0x15ef436a220846f6ull -> 0x44783e84e44eaf3bull, rows 0xaceace7b1ef6a485ull -> 0x5024d58d139a41c8ull, final_hash 0x124296cec661b644ull -> 0x7d6333767228433aull
//   Random37/1/0: boundary 0x15ef436a220846f6ull -> 0x44783e84e44eaf3bull, final_hash 0x124296cec661b644ull -> 0x7d6333767228433aull
//   Random38/0/1: boundary 0x9e847bc8579e51c1ull -> 0x9b34eab31c08b2e9ull, rows 0x3897c857ec18e67cull -> 0x611d2e69cd985562ull, final_hash 0xf97149ee834c0701ull -> 0xf756c93d8d25c11cull
//   Random38/0/0: boundary 0x9e847bc8579e51c1ull -> 0x9b34eab31c08b2e9ull, final_hash 0xf97149ee834c0701ull -> 0xf756c93d8d25c11cull
//   Random38/1/1: boundary 0xba7d3d0648c0d6bcull -> 0x6b22f5a3ffc931ceull, rows 0xd41a32f73f3f4d83ull -> 0xa60b5a9b3c472885ull, final_hash 0x6ffae2ca77f650e8ull -> 0xf2c798a5bba88d39ull
//   Random38/1/0: boundary 0xba7d3d0648c0d6bcull -> 0x6b22f5a3ffc931ceull, final_hash 0x6ffae2ca77f650e8ull -> 0xf2c798a5bba88d39ull
//   Random39/0/1: boundary 0x9d548b7fb1fdf4adull -> 0x271e3d6676599351ull, rows 0xcbb41e60faa7c837ull -> 0xd43319fbe54e5588ull, final_hash 0xaf332a035d930716ull -> 0xbbf808ba5ac7b67eull
//   Random39/0/0: boundary 0x9d548b7fb1fdf4adull -> 0x271e3d6676599351ull, final_hash 0xaf332a035d930716ull -> 0xbbf808ba5ac7b67eull
//   Random39/1/1: boundary 0xa259281d87257741ull -> 0x4b1fcedefee60801ull, rows 0xdc0684cb8ccc6236ull -> 0xd3eb60fe967c52eeull, final_hash 0x218d121b31d456e8ull -> 0x6842fdc2e3fbde5dull
//   Random39/1/0: boundary 0xa259281d87257741ull -> 0x4b1fcedefee60801ull, final_hash 0x218d121b31d456e8ull -> 0x6842fdc2e3fbde5dull
//   Random40/0/1: boundary 0xad248e406624945full -> 0x246be525b446fce2ull, rows 0x3d54cb6ef8fb231dull -> 0x9d244e811d5ca386ull, final_hash 0xae6a8c99f328998aull -> 0x933f71890476b2bbull
//   Random40/0/0: boundary 0xad248e406624945full -> 0x246be525b446fce2ull, final_hash 0xae6a8c99f328998aull -> 0x933f71890476b2bbull
//   Random40/1/1: boundary 0xebfdf5c6313eb35cull -> 0x04841e1d0f981de5ull, rows 0x226a5179617d4724ull -> 0xc3ce4c5ae0eef6cdull, final_hash 0x528178f872eac4feull -> 0x41541580ac45b572ull
//   Random40/1/0: boundary 0xebfdf5c6313eb35cull -> 0x04841e1d0f981de5ull, final_hash 0x528178f872eac4feull -> 0x41541580ac45b572ull
//   Random41/0/1: boundary 0x3788b7f257b2fb92ull -> 0x01aece7db4fb0866ull, rows 0x5f571ccd29c974a5ull -> 0x1b30dc30f66b89bcull, final_hash 0xe8e8bbff74475dacull -> 0x691121b701b14a94ull
//   Random41/0/0: boundary 0x3788b7f257b2fb92ull -> 0x01aece7db4fb0866ull, final_hash 0xe8e8bbff74475dacull -> 0x691121b701b14a94ull
//   Random41/1/1: boundary 0x2b549e2cb695b00cull -> 0x6a009217c0648447ull, rows 0x1ba5dea95b1bfd37ull -> 0x94eeabc325d68785ull, final_hash 0xdcb155b4d707118bull -> 0x29d310f0bf1d7a89ull
//   Random41/1/0: boundary 0x2b549e2cb695b00cull -> 0x6a009217c0648447ull, final_hash 0xdcb155b4d707118bull -> 0x29d310f0bf1d7a89ull
//   Random42/0/1: boundary 0xed3806bedaac6377ull -> 0xa3de44c07792cd0dull, rows 0x9c6622dda2639df5ull -> 0x312360cab7efe78cull, final_hash 0x694b7970a92d8de0ull -> 0xd5c7b24ea3676694ull
//   Random42/0/0: boundary 0xed3806bedaac6377ull -> 0xa3de44c07792cd0dull, final_hash 0x694b7970a92d8de0ull -> 0xd5c7b24ea3676694ull
//   Random42/1/1: boundary 0x1fd240ea218f7a52ull -> 0x37deeaf18300c441ull, rows 0x35e13c7e71b937f9ull -> 0xe63e3be578eeae99ull, final_hash 0x04fd77b73748edc9ull -> 0xa3fed64ce5a4b1f9ull
//   Random42/1/0: boundary 0x1fd240ea218f7a52ull -> 0x37deeaf18300c441ull, final_hash 0x04fd77b73748edc9ull -> 0xa3fed64ce5a4b1f9ull
//   Random43/0/1: boundary 0xb7b4ed0a2fcbd8e6ull -> 0x5747aa51601a3eb4ull, rows 0xf25e2b35f033fa04ull -> 0x0393291d68d0b886ull, final_hash 0x4b34fc0debfcdc0full -> 0xeb9aef460c2a6b69ull
//   Random43/0/0: boundary 0xb7b4ed0a2fcbd8e6ull -> 0x5747aa51601a3eb4ull, final_hash 0x4b34fc0debfcdc0full -> 0xeb9aef460c2a6b69ull
//   Random43/1/1: boundary 0x8b7cbb2d53d4f3d1ull -> 0xbd7f8047e9badaf6ull, rows 0xef94d02971fd8df7ull -> 0x0564d66280f05c3eull, final_hash 0x5cc74085ab2e9cfcull -> 0x0584f3141a2ac711ull
//   Random43/1/0: boundary 0x8b7cbb2d53d4f3d1ull -> 0xbd7f8047e9badaf6ull, final_hash 0x5cc74085ab2e9cfcull -> 0x0584f3141a2ac711ull
//   Random44/0/1: boundary 0xd78e476991194014ull -> 0x40302399ca38ec05ull, rows 0xdb391f42efbf1389ull -> 0x4bdc4b7a1c4ca040ull, final_hash 0xf0f20af42305c1edull -> 0x55eec4db28687678ull
//   Random44/0/0: boundary 0xd78e476991194014ull -> 0x40302399ca38ec05ull, final_hash 0xf0f20af42305c1edull -> 0x55eec4db28687678ull
//   Random44/1/1: boundary 0x1200c3c4015ac19dull -> 0xb8a256af96ab69efull, rows 0x7628621da4f01c15ull -> 0xf56d5278e6ece7caull, final_hash 0x95de750495e4397eull -> 0x49bc82761efd9785ull
//   Random44/1/0: boundary 0x1200c3c4015ac19dull -> 0xb8a256af96ab69efull, final_hash 0x95de750495e4397eull -> 0x49bc82761efd9785ull
//   Random45/0/1: boundary 0xcc1cc6b165de2447ull -> 0x5003dc20b29834deull, rows 0x545d5101b7112b07ull -> 0x4a0e4313ca23913aull, final_hash 0xb19cfff99195e50aull -> 0x04738375938345ecull
//   Random45/0/0: boundary 0xcc1cc6b165de2447ull -> 0x5003dc20b29834deull, final_hash 0xb19cfff99195e50aull -> 0x04738375938345ecull
//   Random45/1/1: boundary 0x4c54227867ae9bbbull -> 0xe533a1889b12d640ull, rows 0xe16493ad341fad02ull -> 0xcbd7ba3dcaa84472ull, final_hash 0x222bca1653cca5cdull -> 0x25f9884b53794cfbull
//   Random45/1/0: boundary 0x4c54227867ae9bbbull -> 0xe533a1889b12d640ull, final_hash 0x222bca1653cca5cdull -> 0x25f9884b53794cfbull
//   Random46/0/1: boundary 0xc9393c45f658137dull -> 0xf293162843324a42ull, rows 0xab1881781ed2dcddull -> 0x2fe9f58c05bd5391ull, final_hash 0xbf69c8b4a2e01a44ull -> 0x138273f0186f99a8ull
//   Random46/0/0: boundary 0xc9393c45f658137dull -> 0xf293162843324a42ull, final_hash 0xbf69c8b4a2e01a44ull -> 0x138273f0186f99a8ull
//   Random46/1/1: boundary 0xc1386f7bc3d58c46ull -> 0x5c518c7c16ad6eb9ull, rows 0x5a7e1996ab8d2f67ull -> 0x7114f97576e05de1ull, final_hash 0xfbf0e8931704bcdaull -> 0x02c866a863d14e29ull
//   Random46/1/0: boundary 0xc1386f7bc3d58c46ull -> 0x5c518c7c16ad6eb9ull, final_hash 0xfbf0e8931704bcdaull -> 0x02c866a863d14e29ull
//   Random47/0/1: boundary 0x9c597748efab97fbull -> 0xeb703ef1728b8316ull, rows 0xe015a111bdafe05full -> 0xcf65482f5b03def1ull, final_hash 0x52d52f80fc7ac8d6ull -> 0x6d733b355b0b4883ull
//   Random47/0/0: boundary 0x9c597748efab97fbull -> 0xeb703ef1728b8316ull, final_hash 0x52d52f80fc7ac8d6ull -> 0x6d733b355b0b4883ull
//   Random47/1/1: boundary 0xf7197b03df006669ull -> 0x25be4848720c17deull, rows 0x2f8f2e1a415c18ccull -> 0xb3c87f095d3d086dull, final_hash 0xbeba43572abe107cull -> 0xc9b10c1db4c2a51aull
//   Random47/1/0: boundary 0xf7197b03df006669ull -> 0x25be4848720c17deull, final_hash 0xbeba43572abe107cull -> 0xc9b10c1db4c2a51aull
//   Random48/0/1: boundary 0x1f1f97e40e739c61ull -> 0xd305a0f9d0984075ull, rows 0xb7b9ec26cf4794bbull -> 0x8ffcf510dfc7a24bull, final_hash 0xb4798af50cf3f648ull -> 0x840184ac798a5de2ull
//   Random48/0/0: boundary 0x1f1f97e40e739c61ull -> 0xd305a0f9d0984075ull, final_hash 0xb4798af50cf3f648ull -> 0x840184ac798a5de2ull
//   Random48/1/1: boundary 0x890a1e21d9208244ull -> 0x0630c1ad99e83057ull, rows 0x57fee585d5ff2aa6ull -> 0x37e26f78b1f4e325ull, final_hash 0xb95a18c6b589114dull -> 0x1969d9a52bdd51c2ull
//   Random48/1/0: boundary 0x890a1e21d9208244ull -> 0x0630c1ad99e83057ull, final_hash 0xb95a18c6b589114dull -> 0x1969d9a52bdd51c2ull
// expectation corrected (INT21 K3, 10 values), because K3 now keeps the first re-issue of each revival candidate instead of every one (INT21): more retired exit legs are erased, and the v4 fold reads the retained rows beside the erased-row count and digest; this host projects one fixed execution hash, so the continuation does not enter; the trades, the trade digest and the three counts did not move; harvested with its switch on this tree; the tree before the correction reproduces every old pin:
//   CalcOnFills/0/1: boundary 0xfcdb5c78d844c1fdull -> 0xf4ba5f0c197f016eull, rows 0x8fce931c6789b396ull -> 0x06eeb2b1f4958e7aull, final_hash 0xfe5dc3ec475477d9ull -> 0xd27b26b32d137ec9ull
//   CalcOnFills/0/0: boundary 0xfcdb5c78d844c1fdull -> 0xf4ba5f0c197f016eull, final_hash 0xfe5dc3ec475477d9ull -> 0xd27b26b32d137ec9ull
//   CalcOnFills/1/1: boundary 0x8ae0abb767b7b0bbull -> 0xd69e36869ed8b309ull, rows 0x0b03280df257cee7ull -> 0xa48ec14c088a0fd9ull, final_hash 0xcd17129763baf911ull -> 0x18c7c13cfd36a5e9ull
//   CalcOnFills/1/0: boundary 0x8ae0abb767b7b0bbull -> 0xd69e36869ed8b309ull, final_hash 0xcd17129763baf911ull -> 0x18c7c13cfd36a5e9ull
// expectation corrected (v19-D, 145 values), because v19-D keeps fewer retired rows and folds a bracket family's erased members behind a retained one as runs (pineforge-source-adapter/v4: K1 releases a current-cycle leg the revival's superseded test answers for; BracketRoster parks); this host projects one fixed execution hash, so the continuation does not enter; the trades, the trade digest and the three counts did not move; harvested with the TU's own switch on this tree (r5/v19-d):
//   DeclinedReversal/0/1: boundary 0x4261900d11067be0ull -> 0xd0083ac2e7a9723eull, rows 0x0e153d0a5a977c02ull -> 0x8567c0614fd76d26ull, final_hash 0xc97b9757bb1149e5ull -> 0x8f1c4a1a41f5a3bcull
//   DeclinedReversal/0/0: boundary 0x4261900d11067be0ull -> 0xd0083ac2e7a9723eull, final_hash 0xc97b9757bb1149e5ull -> 0x8f1c4a1a41f5a3bcull
//   DeclinedReversal/1/1: boundary 0xae41af895aa1b10full -> 0x0fd76da84e65f1c0ull, rows 0x79084294eee2429aull -> 0x1eb7bc54208e0534ull, final_hash 0xb0669ffe8f0e2a01ull -> 0x22d70277dc1fc0a4ull
//   DeclinedReversal/1/0: boundary 0xae41af895aa1b10full -> 0x0fd76da84e65f1c0ull, final_hash 0xb0669ffe8f0e2a01ull -> 0x22d70277dc1fc0a4ull
//   CalcOnFills/0/1: boundary 0xf4ba5f0c197f016eull -> 0xdd17a201bb4d06b2ull, rows 0x06eeb2b1f4958e7aull -> 0x52de59217319fd8aull, final_hash 0xd27b26b32d137ec9ull -> 0xfe5dc3ec475477d9ull
//   CalcOnFills/0/0: boundary 0xf4ba5f0c197f016eull -> 0xdd17a201bb4d06b2ull, final_hash 0xd27b26b32d137ec9ull -> 0xfe5dc3ec475477d9ull
//   CalcOnFills/1/1: boundary 0xd69e36869ed8b309ull -> 0xaa733db98dd5e688ull, rows 0xa48ec14c088a0fd9ull -> 0x07c506ff68fc1a7bull, final_hash 0x18c7c13cfd36a5e9ull -> 0xcd17129763baf911ull
//   CalcOnFills/1/0: boundary 0xd69e36869ed8b309ull -> 0xaa733db98dd5e688ull, final_hash 0x18c7c13cfd36a5e9ull -> 0xcd17129763baf911ull
//   CloseOrders/0/1: boundary 0x11242155751e54f3ull -> 0x31b0c8d9e01f772full, rows 0x49267a5dd6a221c1ull -> 0x09c999edd76d4fdaull
//   CloseOrders/0/0: boundary 0x11242155751e54f3ull -> 0x31b0c8d9e01f772full
//   CloseOrders/1/1: boundary 0xb4ba8101e18b4baeull -> 0xf65b916be995d95full, rows 0x25941e4f7062f84bull -> 0x755bccd5cafc099eull
//   CloseOrders/1/0: boundary 0xb4ba8101e18b4baeull -> 0xf65b916be995d95full
//   Stream/0/1: boundary 0xc15d422469fc217full -> 0xb558b6c7840b2228ull, rows 0x424b81514797c8b8ull -> 0x34ad20d2694b5c4eull
//   Stream/0/0: boundary 0xc15d422469fc217full -> 0xb558b6c7840b2228ull
//   Random03/0/1: boundary 0x1058a57a23480c81ull -> 0x4de9d3289f070b10ull, rows 0xc3e663e104e7a7ccull -> 0xc623f8fada361795ull, final_hash 0x84c305db789fc12bull -> 0xbc66b161650e945aull
//   Random03/0/0: boundary 0x1058a57a23480c81ull -> 0x4de9d3289f070b10ull, final_hash 0x84c305db789fc12bull -> 0xbc66b161650e945aull
//   Random03/1/1: boundary 0x7342e439906b6b8eull -> 0x74c92954e88611faull, rows 0x94231d2d60e49098ull -> 0xff0a6422eb94899eull, final_hash 0xfe7cab3757c85231ull -> 0x66e224895c00a770ull
//   Random03/1/0: boundary 0x7342e439906b6b8eull -> 0x74c92954e88611faull, final_hash 0xfe7cab3757c85231ull -> 0x66e224895c00a770ull
//   Random06/0/1: boundary 0x2a682a4ea3dcae53ull -> 0x2bfc102809cb61b6ull, rows 0x1859b011dfeed652ull -> 0xe71623e0f3912192ull
//   Random06/0/0: boundary 0x2a682a4ea3dcae53ull -> 0x2bfc102809cb61b6ull
//   Random06/1/1: boundary 0xcfa38c06a6347474ull -> 0xe00ef146710c5f72ull, rows 0x14e300d8b24c6c40ull -> 0xf468c8529e501c5full
//   Random06/1/0: boundary 0xcfa38c06a6347474ull -> 0xe00ef146710c5f72ull
//   Random07/0/1: boundary 0xa97e5a85a99d870full -> 0x4a60ce0b1c4fb021ull, rows 0x232efbe10e5b744dull -> 0xd3043f597a7a6f7eull, final_hash 0xc1850e436ab19a88ull -> 0xbfd9ebdfdd9a9abbull
//   Random07/0/0: boundary 0xa97e5a85a99d870full -> 0x4a60ce0b1c4fb021ull, final_hash 0xc1850e436ab19a88ull -> 0xbfd9ebdfdd9a9abbull
//   Random07/1/1: boundary 0xc6df3016ec6c9491ull -> 0x8f487efcaf6960daull, rows 0x2f8088fa08ed52bdull -> 0x9ddf86f7c192467bull, final_hash 0x24806c18caab7670ull -> 0xb858f3c326ae5bebull
//   Random07/1/0: boundary 0xc6df3016ec6c9491ull -> 0x8f487efcaf6960daull, final_hash 0x24806c18caab7670ull -> 0xb858f3c326ae5bebull
//   Random12/0/1: boundary 0xc3249ab4cf05233bull -> 0x3d418ef44da4cd2eull, rows 0xf9f7de7a15130b6cull -> 0xe892e674e3e6e0caull, final_hash 0x39560e1c6a315bbdull -> 0x4e9bd726c9f377b7ull
//   Random12/0/0: boundary 0xc3249ab4cf05233bull -> 0x3d418ef44da4cd2eull, final_hash 0x39560e1c6a315bbdull -> 0x4e9bd726c9f377b7ull
//   Random12/1/1: boundary 0x4ac8b5d9d42b86f5ull -> 0x1fc620580d6c6a8cull, rows 0xb525118494281087ull -> 0xba2f51ae1c4ffcaeull, final_hash 0x322ae8f4196b3e75ull -> 0x5209c01b2d9075bbull
//   Random12/1/0: boundary 0x4ac8b5d9d42b86f5ull -> 0x1fc620580d6c6a8cull, final_hash 0x322ae8f4196b3e75ull -> 0x5209c01b2d9075bbull
//   Random14/0/1: boundary 0x3bc54d3dda74eed1ull -> 0x1bc30bb176abfe75ull, rows 0x39e24e8c4c0cf5e8ull -> 0x38ac385d83173e6dull, final_hash 0x4da290343bef0430ull -> 0x240563decf410a07ull
//   Random14/0/0: boundary 0x3bc54d3dda74eed1ull -> 0x1bc30bb176abfe75ull, final_hash 0x4da290343bef0430ull -> 0x240563decf410a07ull
//   Random14/1/1: boundary 0x082506120a682f91ull -> 0xf70698eaf69fe0f7ull, rows 0xa87115943d8a4576ull -> 0x32373b160f2cb0a4ull, final_hash 0xaf54857aebe2069eull -> 0x2a207608d53d6e31ull
//   Random14/1/0: boundary 0x082506120a682f91ull -> 0xf70698eaf69fe0f7ull, final_hash 0xaf54857aebe2069eull -> 0x2a207608d53d6e31ull
//   Random16/0/1: boundary 0xc6936cda82ca4015ull -> 0x41c794238409bacbull, rows 0x92ab4831c042bfa2ull -> 0xe3000785740c45c4ull, final_hash 0x3ee1c25f2c9ce286ull -> 0xdbb1c1b8835a2f52ull
//   Random16/0/0: boundary 0xc6936cda82ca4015ull -> 0x41c794238409bacbull, final_hash 0x3ee1c25f2c9ce286ull -> 0xdbb1c1b8835a2f52ull
//   Random16/1/1: boundary 0xd54109c1c01d45ceull -> 0xb469144e85e0a627ull, rows 0x682f3f78dc824bc8ull -> 0x5adace02ce0745bcull, final_hash 0xdde93ec35437e7afull -> 0xd6a03051ba6c30efull
//   Random16/1/0: boundary 0xd54109c1c01d45ceull -> 0xb469144e85e0a627ull, final_hash 0xdde93ec35437e7afull -> 0xd6a03051ba6c30efull
//   Random19/0/1: boundary 0x429d9a4cff7068fcull -> 0x0829e86674bb90a3ull, rows 0xe717295c664e09e9ull -> 0x0c808692940a05a0ull, final_hash 0x04e27e2d65aa1452ull -> 0x26e403c4671fad9full
//   Random19/0/0: boundary 0x429d9a4cff7068fcull -> 0x0829e86674bb90a3ull, final_hash 0x04e27e2d65aa1452ull -> 0x26e403c4671fad9full
//   Random19/1/1: boundary 0x1e2eaa0ecfeb3392ull -> 0x62aea0c1de6a6c13ull, rows 0xc850e8283034d833ull -> 0xf4f4a620ee4a7ab5ull, final_hash 0x88071f404445eb2bull -> 0xb4aa06caa7722a48ull
//   Random19/1/0: boundary 0x1e2eaa0ecfeb3392ull -> 0x62aea0c1de6a6c13ull, final_hash 0x88071f404445eb2bull -> 0xb4aa06caa7722a48ull
//   Random25/0/1: boundary 0x34fc7d2dd7c74f50ull -> 0x5fde9097294eff95ull, rows 0xd5bc5831c840df16ull -> 0xeb739137b8b09c27ull
//   Random25/0/0: boundary 0x34fc7d2dd7c74f50ull -> 0x5fde9097294eff95ull
//   Random25/1/1: boundary 0x2da85270ed8b8e32ull -> 0x61de85290b80e276ull, rows 0xd9976f48a3e5d4d6ull -> 0x3e2dace4bb30d118ull
//   Random25/1/0: boundary 0x2da85270ed8b8e32ull -> 0x61de85290b80e276ull
//   Random27/0/1: boundary 0x75311b332bc3b546ull -> 0x053cfa54c61f3bd4ull, rows 0xa06c0a5c0993ce52ull -> 0x49d4da1649eefffbull, final_hash 0x9806c99ed2cb07fcull -> 0x2b62c8cc0129f74cull
//   Random27/0/0: boundary 0x75311b332bc3b546ull -> 0x053cfa54c61f3bd4ull, final_hash 0x9806c99ed2cb07fcull -> 0x2b62c8cc0129f74cull
//   Random27/1/1: boundary 0x221564757dacff73ull -> 0x26f99a57bee85343ull, rows 0xf5686759b991c5f1ull -> 0x4ff364a8de81f569ull, final_hash 0x91aeda18b48beb19ull -> 0xd48238aab27e8f44ull
//   Random27/1/0: boundary 0x221564757dacff73ull -> 0x26f99a57bee85343ull, final_hash 0x91aeda18b48beb19ull -> 0xd48238aab27e8f44ull
//   Random33/0/1: boundary 0xc70a47f8a53011e5ull -> 0x33bf503af7204823ull, rows 0xd0fd26991c9583c5ull -> 0x39a8de40423427d1ull
//   Random33/0/0: boundary 0xc70a47f8a53011e5ull -> 0x33bf503af7204823ull
//   Random33/1/1: boundary 0x26028452eb411830ull -> 0x7f150e22e1c3c861ull, rows 0x5c2f2e8de242605bull -> 0xda9af32172dac1a4ull
//   Random33/1/0: boundary 0x26028452eb411830ull -> 0x7f150e22e1c3c861ull
//   Random42/0/1: boundary 0xa3de44c07792cd0dull -> 0x983f3224390b3aa8ull, rows 0x312360cab7efe78cull -> 0xdd47cc8bbcfac978ull
//   Random42/0/0: boundary 0xa3de44c07792cd0dull -> 0x983f3224390b3aa8ull
//   Random42/1/1: boundary 0x37deeaf18300c441ull -> 0x5391d71c90732f54ull, rows 0xe63e3be578eeae99ull -> 0x95d22736dbced96dull
//   Random42/1/0: boundary 0x37deeaf18300c441ull -> 0x5391d71c90732f54ull
//   Random43/0/1: boundary 0x5747aa51601a3eb4ull -> 0x3721d2c0c27485e6ull, rows 0x0393291d68d0b886ull -> 0x83e3892349cf9593ull
//   Random43/0/0: boundary 0x5747aa51601a3eb4ull -> 0x3721d2c0c27485e6ull
//   Random43/1/1: boundary 0xbd7f8047e9badaf6ull -> 0x38e80c0ff09bd6efull, rows 0x0564d66280f05c3eull -> 0xb48d33783f27e610ull
//   Random43/1/0: boundary 0xbd7f8047e9badaf6ull -> 0x38e80c0ff09bd6efull
//   Random45/0/1: boundary 0x5003dc20b29834deull -> 0x2c707948f442e0a5ull, rows 0x4a0e4313ca23913aull -> 0xe0be91403cc03881ull, final_hash 0x04738375938345ecull -> 0x49b18e779e62a8fbull
//   Random45/0/0: boundary 0x5003dc20b29834deull -> 0x2c707948f442e0a5ull, final_hash 0x04738375938345ecull -> 0x49b18e779e62a8fbull
//   Random45/1/1: boundary 0xe533a1889b12d640ull -> 0x8c2ae0a418e236daull, rows 0xcbd7ba3dcaa84472ull -> 0xe32f963f031175d8ull, final_hash 0x25f9884b53794cfbull -> 0x6fe2fe4de0b41257ull
//   Random45/1/0: boundary 0xe533a1889b12d640ull -> 0x8c2ae0a418e236daull, final_hash 0x25f9884b53794cfbull -> 0x6fe2fe4de0b41257ull
//   Random48/0/1: boundary 0xd305a0f9d0984075ull -> 0xd0c92fe6fc290e0bull, rows 0x8ffcf510dfc7a24bull -> 0x0af2126a53f8f7b9ull
//   Random48/0/0: boundary 0xd305a0f9d0984075ull -> 0xd0c92fe6fc290e0bull
//   Random48/1/1: boundary 0x0630c1ad99e83057ull -> 0xd39f4122a0d0a91dull, rows 0x37e26f78b1f4e325ull -> 0xd439da8c48c337ddull
//   Random48/1/0: boundary 0x0630c1ad99e83057ull -> 0xd39f4122a0d0a91dull
// expectation corrected (V19-FIX, 680 values), because R5 lane V19-FIX moves the Pine hash values once, inside v19: the dead PineExecutionAdapter::path_order_ no longer folds into pineforge-source-adapter/v4, a script cancel retires every leg of each exit it names and K1 lets a withdrawn leg go, and an origin that can no longer be bound leaves its kernel cohort roster; this host projects one fixed execution hash, so the continuation does not enter; the trades, the trade digest and the three counts did not move; harvested with this TU's harvest switch against main 91d65ad6 (every old pin reproduced) and the lane's tree:
//   Quiet/0/1: boundary 0xc1c15d580d2b91ddull -> 0x1c2b2e37a44f6877ull, rows 0x6f2ef9fb0f46bdb6ull -> 0x81799b58f0e052b1ull, final_hash 0xfd89dcb68ffd9f6full -> 0xe0aa4c0240939bcfull
//   Quiet/0/0: boundary 0xc1c15d580d2b91ddull -> 0x1c2b2e37a44f6877ull, final_hash 0xfd89dcb68ffd9f6full -> 0xe0aa4c0240939bcfull
//   Quiet/1/1: boundary 0x25925b8843f8c940ull -> 0x0a542471817c0616ull, rows 0x3f01d574c39a983full -> 0x4210b9ca5a7d77e6ull, final_hash 0x08472c64f2a3b409ull -> 0x2e91db500a83cbe9ull
//   Quiet/1/0: boundary 0x25925b8843f8c940ull -> 0x0a542471817c0616ull, final_hash 0x08472c64f2a3b409ull -> 0x2e91db500a83cbe9ull
//   HoldLongFull/0/1: boundary 0xf7ee6fe81728a11eull -> 0xbb82b8cd85ffaae1ull, rows 0x245fab3eff4d7559ull -> 0xae96abef4356f11full, final_hash 0x68a09e458dc41ed9ull -> 0xbe87239166611559ull
//   HoldLongFull/0/0: boundary 0xf7ee6fe81728a11eull -> 0xbb82b8cd85ffaae1ull, final_hash 0x68a09e458dc41ed9ull -> 0xbe87239166611559ull
//   HoldLongFull/1/1: boundary 0x74ba83122973ae7bull -> 0x83f82e6a0b0a5677ull, rows 0xa773cd9e004bc143ull -> 0x7700c6fdca38234full, final_hash 0xf7d8d3d0b26bcfb1ull -> 0xb6f097b6a6318731ull
//   HoldLongFull/1/0: boundary 0x74ba83122973ae7bull -> 0x83f82e6a0b0a5677ull, final_hash 0xf7d8d3d0b26bcfb1ull -> 0xb6f097b6a6318731ull
//   MarginCalls/0/1: boundary 0xf01f77fadacf9b1bull -> 0x3633b5fd46e3ee1eull, rows 0x720c4e8e6c1151d7ull -> 0x35e47773037c0f4dull, final_hash 0x1423cb11eff138efull -> 0x64d4054219e25f2full
//   MarginCalls/0/0: boundary 0xf01f77fadacf9b1bull -> 0x3633b5fd46e3ee1eull, final_hash 0x1423cb11eff138efull -> 0x64d4054219e25f2full
//   MarginCalls/1/1: boundary 0x04552ef5dd6b0248ull -> 0x96ca6ef83164c685ull, rows 0x34a90870ea8ff4dfull -> 0x4b1bc134021b04cbull, final_hash 0x39b0b7d450ea1d23ull -> 0xc10c1fc9e88e7c23ull
//   MarginCalls/1/0: boundary 0x04552ef5dd6b0248ull -> 0x96ca6ef83164c685ull, final_hash 0x39b0b7d450ea1d23ull -> 0xc10c1fc9e88e7c23ull
//   MarginCallCycles/0/1: boundary 0xe517ae23a9d9cd84ull -> 0x38656d5e12929625ull, rows 0x09d31e44e56049a0ull -> 0x5c4aa443e8e76ef9ull, final_hash 0xd6e5d9e3761d70f7ull -> 0x55f80f5340e670b7ull
//   MarginCallCycles/0/0: boundary 0xe517ae23a9d9cd84ull -> 0x38656d5e12929625ull, final_hash 0xd6e5d9e3761d70f7ull -> 0x55f80f5340e670b7ull
//   MarginCallCycles/1/1: boundary 0x74fb06c3e47e091eull -> 0xc969906a8f7b57bbull, rows 0x3015bbe850ee059dull -> 0x67e6e63eb71a661cull, final_hash 0x1a3d548af01065b8ull -> 0x8e9e930063b52698ull
//   MarginCallCycles/1/0: boundary 0x74fb06c3e47e091eull -> 0xc969906a8f7b57bbull, final_hash 0x1a3d548af01065b8ull -> 0x8e9e930063b52698ull
//   Bracket/0/1: boundary 0xad121243d42812f0ull -> 0x5ceffdf162a625baull, rows 0xc34f99dbe945355dull -> 0x2c0b9b25e51c3426ull, final_hash 0xe393d0e31b6c86dbull -> 0xb62be9f335a690fbull
//   Bracket/0/0: boundary 0xad121243d42812f0ull -> 0x5ceffdf162a625baull, final_hash 0xe393d0e31b6c86dbull -> 0xb62be9f335a690fbull
//   Bracket/1/1: boundary 0x42b52abab2bf3209ull -> 0x53093fd52be58e6aull, rows 0xee291f58ffb7864dull -> 0x26c6fc455e7a9e3full, final_hash 0xa953041c662027b9ull -> 0x2fbb7d2d1228c959ull
//   Bracket/1/0: boundary 0x42b52abab2bf3209ull -> 0x53093fd52be58e6aull, final_hash 0xa953041c662027b9ull -> 0x2fbb7d2d1228c959ull
//   Trail/0/1: boundary 0x38951256c0c8ae91ull -> 0x5211ba6f22808f15ull, rows 0x630e44444886533full -> 0x5fd4f2baecf12c67ull, final_hash 0x0a3d59197f3fbeeeull -> 0x1ca55654b8ff3a6eull
//   Trail/0/0: boundary 0x38951256c0c8ae91ull -> 0x5211ba6f22808f15ull, final_hash 0x0a3d59197f3fbeeeull -> 0x1ca55654b8ff3a6eull
//   Trail/1/1: boundary 0x85f94fedc6115fe3ull -> 0x596611dc371ff159ull, rows 0x100ea0b5c2385430ull -> 0x5ebc78f3f36b3da8ull, final_hash 0x913d53654651ef64ull -> 0x2d3ea22c1cd861e4ull
//   Trail/1/0: boundary 0x85f94fedc6115fe3ull -> 0x596611dc371ff159ull, final_hash 0x913d53654651ef64ull -> 0x2d3ea22c1cd861e4ull
//   DualStops/0/1: boundary 0xbe6893046a46cb96ull -> 0xf53e1aa110098577ull, rows 0x95b5ee2aec79ec7cull -> 0xa45979a387e09947ull, final_hash 0x3ebb8ec1e50f9ed1ull -> 0x9d20a3a0e3c4b991ull
//   DualStops/0/0: boundary 0xbe6893046a46cb96ull -> 0xf53e1aa110098577ull, final_hash 0x3ebb8ec1e50f9ed1ull -> 0x9d20a3a0e3c4b991ull
//   DualStops/1/1: boundary 0xebbf122a6c461306ull -> 0xd56451041c2c2c4full, rows 0xb4bc6d57975026c4ull -> 0x3525fdbfa98d36bcull, final_hash 0x7ee6f826d7537cc1ull -> 0x0e2c1cdfb96b6301ull
//   DualStops/1/0: boundary 0xebbf122a6c461306ull -> 0xd56451041c2c2c4full, final_hash 0x7ee6f826d7537cc1ull -> 0x0e2c1cdfb96b6301ull
//   InvertedStops/0/1: boundary 0x049f870d696c86d6ull -> 0xa3daa491b43f22e9ull, rows 0x6c97ddc3cc3bc830ull -> 0x054c4b8ef1709dc3ull, final_hash 0x92d980a9427423eeull -> 0x056329caffb5a22eull
//   InvertedStops/0/0: boundary 0x049f870d696c86d6ull -> 0xa3daa491b43f22e9ull, final_hash 0x92d980a9427423eeull -> 0x056329caffb5a22eull
//   InvertedStops/1/1: boundary 0x77643e8c893982c3ull -> 0x9b50fac734e90657ull, rows 0xfaede1fa4ce13f90ull -> 0x2d65179a434eebf0ull, final_hash 0x18c13857d9dfa263ull -> 0xab655e08a941e123ull
//   InvertedStops/1/0: boundary 0x77643e8c893982c3ull -> 0x9b50fac734e90657ull, final_hash 0x18c13857d9dfa263ull -> 0xab655e08a941e123ull
//   PendingParent/0/1: boundary 0x6721cf5983214d9eull -> 0x47212775031c61c2ull, rows 0xc139da1140ad7563ull -> 0xfd5dbde93e8d520aull, final_hash 0xab2eaed98b0f009aull -> 0xe74974df53ef097aull
//   PendingParent/0/0: boundary 0x6721cf5983214d9eull -> 0x47212775031c61c2ull, final_hash 0xab2eaed98b0f009aull -> 0xe74974df53ef097aull
//   PendingParent/1/1: boundary 0x298f49712d4af19aull -> 0x08f4fa130f86a0daull, rows 0xa624ebd2d2ec64a5ull -> 0xbd725b4c1d8be787ull, final_hash 0x660fa440f23c5696ull -> 0x27cdc3dedee10a56ull
//   PendingParent/1/0: boundary 0x298f49712d4af19aull -> 0x08f4fa130f86a0daull, final_hash 0x660fa440f23c5696ull -> 0x27cdc3dedee10a56ull
//   GappedAdd/0/1: boundary 0x801838fc90033739ull -> 0x8bd71987fffc51e9ull, rows 0x8259b732e34ac913ull -> 0x42f9dd5f352155d8ull, final_hash 0x58b9e653d25a4647ull -> 0xaa98c069e5a019a7ull
//   GappedAdd/0/0: boundary 0x801838fc90033739ull -> 0x8bd71987fffc51e9ull, final_hash 0x58b9e653d25a4647ull -> 0xaa98c069e5a019a7ull
//   GappedAdd/1/1: boundary 0x8429bf8f9c179793ull -> 0x7b93202bd79c1393ull, rows 0xa8491ce8abf7d3ffull -> 0xa4421baf09234dd7ull, final_hash 0x803cb88784ce9026ull -> 0xe086e35f4f128b46ull
//   GappedAdd/1/0: boundary 0x8429bf8f9c179793ull -> 0x7b93202bd79c1393ull, final_hash 0x803cb88784ce9026ull -> 0xe086e35f4f128b46ull
//   DeclinedReversal/0/1: boundary 0xd0083ac2e7a9723eull -> 0xfcb9145b39ec7b5aull, rows 0x8567c0614fd76d26ull -> 0xbbddf0514f17aef7ull, final_hash 0x8f1c4a1a41f5a3bcull -> 0x764450acb0596bfcull
//   DeclinedReversal/0/0: boundary 0xd0083ac2e7a9723eull -> 0xfcb9145b39ec7b5aull, final_hash 0x8f1c4a1a41f5a3bcull -> 0x764450acb0596bfcull
//   DeclinedReversal/1/1: boundary 0x0fd76da84e65f1c0ull -> 0x4a1378eba012ef40ull, rows 0x1eb7bc54208e0534ull -> 0x16ea86de895d5b40ull, final_hash 0x22d70277dc1fc0a4ull -> 0x14734f0e113371e4ull
//   DeclinedReversal/1/0: boundary 0x0fd76da84e65f1c0ull -> 0x4a1378eba012ef40ull, final_hash 0x22d70277dc1fc0a4ull -> 0x14734f0e113371e4ull
//   CalcOnFills/0/1: boundary 0xdd17a201bb4d06b2ull -> 0x7641907936993779ull, rows 0x52de59217319fd8aull -> 0x3777fe6ece242880ull, final_hash 0xfe5dc3ec475477d9ull -> 0x2583ce2bc6a3c1d9ull
//   CalcOnFills/0/0: boundary 0xdd17a201bb4d06b2ull -> 0x7641907936993779ull, final_hash 0xfe5dc3ec475477d9ull -> 0x2583ce2bc6a3c1d9ull
//   CalcOnFills/1/1: boundary 0xaa733db98dd5e688ull -> 0xd120e8fefb0dc236ull, rows 0x07c506ff68fc1a7bull -> 0xdbb28de374d30bc3ull, final_hash 0xcd17129763baf911ull -> 0xad6e26cf69199431ull
//   CalcOnFills/1/0: boundary 0xaa733db98dd5e688ull -> 0xd120e8fefb0dc236ull, final_hash 0xcd17129763baf911ull -> 0xad6e26cf69199431ull
//   CloseOrders/0/1: boundary 0x31b0c8d9e01f772full -> 0x78ff3352f7085436ull, rows 0x09c999edd76d4fdaull -> 0x896bcdcf15c8a4c6ull, final_hash 0x2d39ec28cc86f100ull -> 0x8e67de695bfb2c60ull
//   CloseOrders/0/0: boundary 0x31b0c8d9e01f772full -> 0x78ff3352f7085436ull, final_hash 0x2d39ec28cc86f100ull -> 0x8e67de695bfb2c60ull
//   CloseOrders/1/1: boundary 0xf65b916be995d95full -> 0xe00ae8be3caf3315ull, rows 0x755bccd5cafc099eull -> 0x2584350c36f68309ull, final_hash 0x6281f01aff005efeull -> 0xa998448999ff9b1eull
//   CloseOrders/1/0: boundary 0xf65b916be995d95full -> 0xe00ae8be3caf3315ull, final_hash 0x6281f01aff005efeull -> 0xa998448999ff9b1eull
//   CloseOrdersSlipped/0/1: boundary 0xf41f34b44f14ed1bull -> 0x58e9160429c03bcaull, rows 0x88cca9165f3f9ca5ull -> 0xd3632e32e35ef626ull, final_hash 0x184081a51dee14b6ull -> 0x13f82da0279b5836ull
//   CloseOrdersSlipped/0/0: boundary 0xf41f34b44f14ed1bull -> 0x58e9160429c03bcaull, final_hash 0x184081a51dee14b6ull -> 0x13f82da0279b5836ull
//   CloseOrdersSlipped/1/1: boundary 0xefc7d84261df23b7ull -> 0x3507072a3e435b83ull, rows 0x0952c5f9a055c769ull -> 0xc5d77a9b36fdec85ull, final_hash 0xdbb9a359df5b7ba7ull -> 0xc7b132391c547427ull
//   CloseOrdersSlipped/1/0: boundary 0xefc7d84261df23b7ull -> 0x3507072a3e435b83ull, final_hash 0xdbb9a359df5b7ba7ull -> 0xc7b132391c547427ull
//   IntradayLoss/0/1: boundary 0x45d88fb7f59423e4ull -> 0x644e48072a025ba8ull, rows 0xf597c17beff44c38ull -> 0x0421711a90fdf429ull, final_hash 0xb18331055f670401ull -> 0xecca5625486ab2e1ull
//   IntradayLoss/0/0: boundary 0x45d88fb7f59423e4ull -> 0x644e48072a025ba8ull, final_hash 0xb18331055f670401ull -> 0xecca5625486ab2e1ull
//   IntradayLoss/1/1: boundary 0xdccf9450e8e7c00cull -> 0xce3032bd544d7840ull, rows 0x488f8a32366e0cafull -> 0x4e9d2d333bd5f470ull, final_hash 0x08be7263bb9faf0eull -> 0x4e071c66b57dfeaeull
//   IntradayLoss/1/0: boundary 0xdccf9450e8e7c00cull -> 0xce3032bd544d7840ull, final_hash 0x08be7263bb9faf0eull -> 0x4e071c66b57dfeaeull
//   IntradayCapDeferred/0/1: boundary 0x477d0d155ce99044ull -> 0x93a814e5f6e8feb7ull, rows 0xd36b92908117e322ull -> 0xbbbb2712dbbd9ac8ull, final_hash 0xd7b1c15ee8d83724ull -> 0x77c142bb11eab624ull
//   IntradayCapDeferred/0/0: boundary 0x477d0d155ce99044ull -> 0x93a814e5f6e8feb7ull, final_hash 0xd7b1c15ee8d83724ull -> 0x77c142bb11eab624ull
//   IntradayCapDeferred/1/1: boundary 0x1b27e4ab14777901ull -> 0xe23e69831f88d404ull, rows 0xbc24b0cb7453804bull -> 0x63469b00b6747db8ull, final_hash 0x58271303a14cab9bull -> 0x72de8fef40c4c89bull
//   IntradayCapDeferred/1/0: boundary 0x1b27e4ab14777901ull -> 0xe23e69831f88d404ull, final_hash 0x58271303a14cab9bull -> 0x72de8fef40c4c89bull
//   PreopenSlice/0/1: boundary 0x85bac68b85fa1b10ull -> 0x9f401876dd941c1full, rows 0xba4b398db082d988ull -> 0xbbce87ebf0d3f638ull, final_hash 0xc4645f9196ec1c52ull -> 0xf73c6812e8dd0f92ull
//   PreopenSlice/0/0: boundary 0x85bac68b85fa1b10ull -> 0x9f401876dd941c1full, final_hash 0xc4645f9196ec1c52ull -> 0xf73c6812e8dd0f92ull
//   PreopenSlice/1/1: boundary 0x3c681ec4f8ea88e9ull -> 0x98345cf423b84f80ull, rows 0xcfdbbdabfcceb515ull -> 0x36db489e43fd8218ull, final_hash 0x43fed91080dfbea0ull -> 0xa5916c1cffd7bae0ull
//   PreopenSlice/1/0: boundary 0x3c681ec4f8ea88e9ull -> 0x98345cf423b84f80ull, final_hash 0x43fed91080dfbea0ull -> 0xa5916c1cffd7bae0ull
//   SameBarBatch/0/1: boundary 0xd8c6014359e3436dull -> 0xcfa0ce2dfed4b2e4ull, rows 0x56ce4bb7913dd63dull -> 0x7de7b618b505f51eull, final_hash 0x1e3f3c2b9879352full -> 0x0bb34499121582afull
//   SameBarBatch/0/0: boundary 0xd8c6014359e3436dull -> 0xcfa0ce2dfed4b2e4ull, final_hash 0x1e3f3c2b9879352full -> 0x0bb34499121582afull
//   SameBarBatch/1/1: boundary 0xb74c80da185d6f7cull -> 0x4557f9683348bf23ull, rows 0xd3659fc4f7228afcull -> 0x79d43d1295ddc801ull, final_hash 0xcc0463b3d75fe13cull -> 0x26a5e7a6d5104dbcull
//   SameBarBatch/1/0: boundary 0xb74c80da185d6f7cull -> 0x4557f9683348bf23ull, final_hash 0xcc0463b3d75fe13cull -> 0x26a5e7a6d5104dbcull
//   CommissionedShort/0/1: boundary 0xb3b9a9d400a706b7ull -> 0x18b476bd2a3db96dull, rows 0xe36126265b8bf576ull -> 0x54b6ba2e7a2e11c2ull, final_hash 0x23caa9e57f9f6fb8ull -> 0x48a9193b2c1c1118ull
//   CommissionedShort/0/0: boundary 0xb3b9a9d400a706b7ull -> 0x18b476bd2a3db96dull, final_hash 0x23caa9e57f9f6fb8ull -> 0x48a9193b2c1c1118ull
//   CommissionedShort/1/1: boundary 0x1fba48a99176b809ull -> 0xb24ff7e17f80c347ull, rows 0x8599d32cc4e3f365ull -> 0x20608b72a057ada4ull, final_hash 0xc1bb33f8955d526dull -> 0x3ef1eaec89e6430dull
//   CommissionedShort/1/0: boundary 0x1fba48a99176b809ull -> 0xb24ff7e17f80c347ull, final_hash 0xc1bb33f8955d526dull -> 0x3ef1eaec89e6430dull
//   AccountFx/0/1: boundary 0x7ea7cc0696e4e706ull -> 0xaeec7af5859fe6b3ull, rows 0x5d06a315cd828e3aull -> 0x37039a16e8c77a7bull, final_hash 0xf3a8b7577af41cbcull -> 0x2a28b4e61cd5cbbcull
//   AccountFx/0/0: boundary 0x7ea7cc0696e4e706ull -> 0xaeec7af5859fe6b3ull, final_hash 0xf3a8b7577af41cbcull -> 0x2a28b4e61cd5cbbcull
//   Stream/0/1: boundary 0xb558b6c7840b2228ull -> 0x49574a22705f52c0ull, rows 0x34ad20d2694b5c4eull -> 0x3cade1c0a6b65b75ull, final_hash 0xc9c5e2681a211fdaull -> 0x8c9bb935bf0c32daull
//   Stream/0/0: boundary 0xb558b6c7840b2228ull -> 0x49574a22705f52c0ull, final_hash 0xc9c5e2681a211fdaull -> 0x8c9bb935bf0c32daull
//   Random01/0/1: boundary 0x7b26a7a4fd8ca837ull -> 0xc538d8fca421c703ull, rows 0x8b38d2c0ee3ae09bull -> 0xc035295ef347b140ull, final_hash 0x266b3c97e2229932ull -> 0x166e28e2277135d2ull
//   Random01/0/0: boundary 0x7b26a7a4fd8ca837ull -> 0xc538d8fca421c703ull, final_hash 0x266b3c97e2229932ull -> 0x166e28e2277135d2ull
//   Random01/1/1: boundary 0x240a7d06f5858192ull -> 0x5b8b5e29d258d71cull, rows 0x5a4bde6c69234036ull -> 0x16a72508f5fdd125ull, final_hash 0xc2da049b01eede0eull -> 0xa0becd4ec596146eull
//   Random01/1/0: boundary 0x240a7d06f5858192ull -> 0x5b8b5e29d258d71cull, final_hash 0xc2da049b01eede0eull -> 0xa0becd4ec596146eull
//   Random02/0/1: boundary 0xf50e670c28558c70ull -> 0xf933c58464603966ull, rows 0xc237af8c6ea5d8a0ull -> 0x8549ba7d7e735482ull, final_hash 0x785608d44d074ae6ull -> 0xd9870babe5d18626ull
//   Random02/0/0: boundary 0xf50e670c28558c70ull -> 0xf933c58464603966ull, final_hash 0x785608d44d074ae6ull -> 0xd9870babe5d18626ull
//   Random02/1/1: boundary 0x32b5473f018894caull -> 0x25ba5f1040df3b56ull, rows 0xbbb6be89045a263dull -> 0x1593a8e96a0c90a9ull, final_hash 0x537ce6356efa7dcdull -> 0x0fb50af38c25c54dull
//   Random02/1/0: boundary 0x32b5473f018894caull -> 0x25ba5f1040df3b56ull, final_hash 0x537ce6356efa7dcdull -> 0x0fb50af38c25c54dull
//   Random03/0/1: boundary 0x4de9d3289f070b10ull -> 0xb5a3f885ba0143fdull, rows 0xc623f8fada361795ull -> 0xd91b7712cc62209bull, final_hash 0xbc66b161650e945aull -> 0xd93ef819aff693f3ull
//   Random03/0/0: boundary 0x4de9d3289f070b10ull -> 0xb5a3f885ba0143fdull, final_hash 0xbc66b161650e945aull -> 0xd93ef819aff693f3ull
//   Random03/1/1: boundary 0x74c92954e88611faull -> 0x36230659e7bf570eull, rows 0xff0a6422eb94899eull -> 0xdb83075a5690e996ull, final_hash 0x66e224895c00a770ull -> 0x23acbc0041308125ull
//   Random03/1/0: boundary 0x74c92954e88611faull -> 0x36230659e7bf570eull, final_hash 0x66e224895c00a770ull -> 0x23acbc0041308125ull
//   Random04/0/1: boundary 0xc803c2750b61157dull -> 0x0c41be29e4497a7cull, rows 0x32e6028cb8a54dc8ull -> 0x1fdaabb2a1c312cdull, final_hash 0x84461297f142b9efull -> 0x542f1b01ef32d535ull
//   Random04/0/0: boundary 0xc803c2750b61157dull -> 0x0c41be29e4497a7cull, final_hash 0x84461297f142b9efull -> 0x542f1b01ef32d535ull
//   Random04/1/1: boundary 0xaf56d9261eb50536ull -> 0x93ade030662e0b77ull, rows 0xfece30308981fb23ull -> 0x5beecc5dc910feb8ull, final_hash 0xcd21899a90f1d564ull -> 0x254f4f4933be69e0ull
//   Random04/1/0: boundary 0xaf56d9261eb50536ull -> 0x93ade030662e0b77ull, final_hash 0xcd21899a90f1d564ull -> 0x254f4f4933be69e0ull
//   Random05/0/1: boundary 0xc8efa2a84661d593ull -> 0x38db302327d01e80ull, rows 0x90b1a6e9fbe4e13full -> 0x2461425b8e878f0dull, final_hash 0x2dd0cf2ce3ea761eull -> 0xe32002fbd2d6ea3eull
//   Random05/0/0: boundary 0xc8efa2a84661d593ull -> 0x38db302327d01e80ull, final_hash 0x2dd0cf2ce3ea761eull -> 0xe32002fbd2d6ea3eull
//   Random05/1/1: boundary 0x18519fd8ab2ecf78ull -> 0xe3ae54dee0ba6db5ull, rows 0x967a69d448f6b43aull -> 0xc1c6474702bc75e0ull, final_hash 0x1733e9f06b55202cull -> 0x70343cf2d03a75ebull
//   Random05/1/0: boundary 0x18519fd8ab2ecf78ull -> 0xe3ae54dee0ba6db5ull, final_hash 0x1733e9f06b55202cull -> 0x70343cf2d03a75ebull
//   Random06/0/1: boundary 0x2bfc102809cb61b6ull -> 0xecb44da2cbdb5c42ull, rows 0xe71623e0f3912192ull -> 0x5485171eedfc86cfull, final_hash 0xbe2d610924b8c00dull -> 0xff2efe1925e45a9dull
//   Random06/0/0: boundary 0x2bfc102809cb61b6ull -> 0xecb44da2cbdb5c42ull, final_hash 0xbe2d610924b8c00dull -> 0xff2efe1925e45a9dull
//   Random06/1/1: boundary 0xe00ef146710c5f72ull -> 0x65cc03efd59ec6b5ull, rows 0xf468c8529e501c5full -> 0x2dce2153546130dbull, final_hash 0x814550b3d8ab202aull -> 0x1130940f86f108d2ull
//   Random06/1/0: boundary 0xe00ef146710c5f72ull -> 0x65cc03efd59ec6b5ull, final_hash 0x814550b3d8ab202aull -> 0x1130940f86f108d2ull
//   Random07/0/1: boundary 0x4a60ce0b1c4fb021ull -> 0x7bf47df91573ab9cull, rows 0xd3043f597a7a6f7eull -> 0x3c1feb47a4d6d81eull, final_hash 0xbfd9ebdfdd9a9abbull -> 0x438a828933e58c5bull
//   Random07/0/0: boundary 0x4a60ce0b1c4fb021ull -> 0x7bf47df91573ab9cull, final_hash 0xbfd9ebdfdd9a9abbull -> 0x438a828933e58c5bull
//   Random07/1/1: boundary 0x8f487efcaf6960daull -> 0x29fcc776be13bdf5ull, rows 0x9ddf86f7c192467bull -> 0x9a5ab1b2b00efff1ull, final_hash 0xb858f3c326ae5bebull -> 0x353c5f0e5bc1c22bull
//   Random07/1/0: boundary 0x8f487efcaf6960daull -> 0x29fcc776be13bdf5ull, final_hash 0xb858f3c326ae5bebull -> 0x353c5f0e5bc1c22bull
//   Random08/0/1: boundary 0xdf32ef61cd4e19eeull -> 0xc2f66d9e5f4943c7ull, rows 0xfd32cff83453f293ull -> 0xf4a38750150093e6ull, final_hash 0x23acd097ec38f43aull -> 0x9ce7eb6ee2b122baull
//   Random08/0/0: boundary 0xdf32ef61cd4e19eeull -> 0xc2f66d9e5f4943c7ull, final_hash 0x23acd097ec38f43aull -> 0x9ce7eb6ee2b122baull
//   Random08/1/1: boundary 0x869ee3e727ef0b05ull -> 0xbc7e1d017519ee12ull, rows 0x64f491f787b05affull -> 0xc18ef29ec0f4fa8eull, final_hash 0x11144c9c51cb89f3ull -> 0xbd83191789f76873ull
//   Random08/1/0: boundary 0x869ee3e727ef0b05ull -> 0xbc7e1d017519ee12ull, final_hash 0x11144c9c51cb89f3ull -> 0xbd83191789f76873ull
//   Random09/0/1: boundary 0xf123ed994915e172ull -> 0x400c46d70e6e84bcull, rows 0xe262ad1d42b22aa2ull -> 0xc58cc1d4b5bd3580ull, final_hash 0x24d07dfd4c311e09ull -> 0x1f6cdd2e111e016aull
//   Random09/0/0: boundary 0xf123ed994915e172ull -> 0x400c46d70e6e84bcull, final_hash 0x24d07dfd4c311e09ull -> 0x1f6cdd2e111e016aull
//   Random09/1/1: boundary 0x28f03f19f3977a71ull -> 0x32d4b35f029bad56ull, rows 0x29fa1939a71c5214ull -> 0x447742874dde46e0ull, final_hash 0xf22f979ee11a143full -> 0x3d99e7abdd92a773ull
//   Random09/1/0: boundary 0x28f03f19f3977a71ull -> 0x32d4b35f029bad56ull, final_hash 0xf22f979ee11a143full -> 0x3d99e7abdd92a773ull
//   Random10/0/1: boundary 0xb3861d80e682c693ull -> 0xe529db3277bf6dd9ull, rows 0xe5b5b2919d650a05ull -> 0x1a655455a6bd3b6full, final_hash 0x92408fc414e06780ull -> 0xffdc77023cb52b44ull
//   Random10/0/0: boundary 0xb3861d80e682c693ull -> 0xe529db3277bf6dd9ull, final_hash 0x92408fc414e06780ull -> 0xffdc77023cb52b44ull
//   Random10/1/1: boundary 0x69fe10c28684def0ull -> 0xb3a8db733c3f30bdull, rows 0x10dfb593a8ec827cull -> 0xa2424fa6550c3ef1ull, final_hash 0x6a8371ba10724f52ull -> 0x61b614a68f564bf0ull
//   Random10/1/0: boundary 0x69fe10c28684def0ull -> 0xb3a8db733c3f30bdull, final_hash 0x6a8371ba10724f52ull -> 0x61b614a68f564bf0ull
//   Random11/0/1: boundary 0x90100a03a86d0cdbull -> 0x805038d1cb0a3848ull, rows 0x016b0db6bef2264cull -> 0xebc3fde6db4a37f3ull, final_hash 0x185f0857180c5a28ull -> 0x27bc15c5c142c728ull
//   Random11/0/0: boundary 0x90100a03a86d0cdbull -> 0x805038d1cb0a3848ull, final_hash 0x185f0857180c5a28ull -> 0x27bc15c5c142c728ull
//   Random11/1/1: boundary 0x84170dd33baa79d9ull -> 0x4a739854a58a3db9ull, rows 0x1a59bcbe0a910135ull -> 0xc91f1e4519586a06ull, final_hash 0x41861834717a9917ull -> 0x8af717fd3fa04097ull
//   Random11/1/0: boundary 0x84170dd33baa79d9ull -> 0x4a739854a58a3db9ull, final_hash 0x41861834717a9917ull -> 0x8af717fd3fa04097ull
//   Random12/0/1: boundary 0x3d418ef44da4cd2eull -> 0xb9a9bb3fafab7349ull, rows 0xe892e674e3e6e0caull -> 0xe93fb7b821f6a7feull, final_hash 0x4e9bd726c9f377b7ull -> 0x29c94b5df165789aull
//   Random12/0/0: boundary 0x3d418ef44da4cd2eull -> 0xb9a9bb3fafab7349ull, final_hash 0x4e9bd726c9f377b7ull -> 0x29c94b5df165789aull
//   Random12/1/1: boundary 0x1fc620580d6c6a8cull -> 0x298ab491e3caa61full, rows 0xba2f51ae1c4ffcaeull -> 0xf8e78400f1c148acull, final_hash 0x5209c01b2d9075bbull -> 0x927c4394206b42b2ull
//   Random12/1/0: boundary 0x1fc620580d6c6a8cull -> 0x298ab491e3caa61full, final_hash 0x5209c01b2d9075bbull -> 0x927c4394206b42b2ull
//   Random13/0/1: boundary 0xc330ac11cbbddd41ull -> 0xf255fd63895e7badull, rows 0xe0a3d24e78674f95ull -> 0x41470781ab6bef64ull, final_hash 0xefab5484e3574594ull -> 0x57de0e46784b0f54ull
//   Random13/0/0: boundary 0xc330ac11cbbddd41ull -> 0xf255fd63895e7badull, final_hash 0xefab5484e3574594ull -> 0x57de0e46784b0f54ull
//   Random13/1/1: boundary 0xcdb3c6546824667dull -> 0x7abd074c56aca8c6ull, rows 0x653d5c4469730325ull -> 0x41de5734424c47edull, final_hash 0x027984836057c1fbull -> 0xa6590b3d3950e7bbull
//   Random13/1/0: boundary 0xcdb3c6546824667dull -> 0x7abd074c56aca8c6ull, final_hash 0x027984836057c1fbull -> 0xa6590b3d3950e7bbull
//   Random14/0/1: boundary 0x1bc30bb176abfe75ull -> 0x6b59b79be4590a72ull, rows 0x38ac385d83173e6dull -> 0x6d4cbed6a7d3b423ull, final_hash 0x240563decf410a07ull -> 0x918405ca8ede2f52ull
//   Random14/0/0: boundary 0x1bc30bb176abfe75ull -> 0x6b59b79be4590a72ull, final_hash 0x240563decf410a07ull -> 0x918405ca8ede2f52ull
//   Random14/1/1: boundary 0xf70698eaf69fe0f7ull -> 0x8dfaddf560dc80caull, rows 0x32373b160f2cb0a4ull -> 0x13e689009402d1e0ull, final_hash 0x2a207608d53d6e31ull -> 0x70415e0fc326b13cull
//   Random14/1/0: boundary 0xf70698eaf69fe0f7ull -> 0x8dfaddf560dc80caull, final_hash 0x2a207608d53d6e31ull -> 0x70415e0fc326b13cull
//   Random15/0/1: boundary 0x8d19883bfd7b9827ull -> 0x1fe125a3fb2912dfull, rows 0x617a8b521ffd19a8ull -> 0x9ef69d4200bfb809ull, final_hash 0xc5c912afdfb5429eull -> 0xf8da94854e79d89eull
//   Random15/0/0: boundary 0x8d19883bfd7b9827ull -> 0x1fe125a3fb2912dfull, final_hash 0xc5c912afdfb5429eull -> 0xf8da94854e79d89eull
//   Random15/1/1: boundary 0xce37c225571916fbull -> 0x7b17bf5a43044023ull, rows 0x5ea9fcc2c1385c3full -> 0xdeec1e0290d1370dull, final_hash 0xf1a244775b55839dull -> 0x01758143c50ac69dull
//   Random15/1/0: boundary 0xce37c225571916fbull -> 0x7b17bf5a43044023ull, final_hash 0xf1a244775b55839dull -> 0x01758143c50ac69dull
//   Random16/0/1: boundary 0x41c794238409bacbull -> 0xde7f3e61caa50135ull, rows 0xe3000785740c45c4ull -> 0x9593cb63a9a06f14ull, final_hash 0xdbb1c1b8835a2f52ull -> 0xc99a0f083d0b2507ull
//   Random16/0/0: boundary 0x41c794238409bacbull -> 0xde7f3e61caa50135ull, final_hash 0xdbb1c1b8835a2f52ull -> 0xc99a0f083d0b2507ull
//   Random16/1/1: boundary 0xb469144e85e0a627ull -> 0xffbc34c7947c611bull, rows 0x5adace02ce0745bcull -> 0xa2b9ee52a1338391ull, final_hash 0xd6a03051ba6c30efull -> 0x1f80513beefa0b1aull
//   Random16/1/0: boundary 0xb469144e85e0a627ull -> 0xffbc34c7947c611bull, final_hash 0xd6a03051ba6c30efull -> 0x1f80513beefa0b1aull
//   Random17/0/1: boundary 0x6304fa3c28e07e49ull -> 0x15bab2d913fb1d81ull, rows 0xf1ed3675ad25ec11ull -> 0x0a3d587bbe912dcfull, final_hash 0xfda9c1caff36448bull -> 0x7185bfaebb406df5ull
//   Random17/0/0: boundary 0x6304fa3c28e07e49ull -> 0x15bab2d913fb1d81ull, final_hash 0xfda9c1caff36448bull -> 0x7185bfaebb406df5ull
//   Random17/1/1: boundary 0x91c41f4b53c60db8ull -> 0xa978788efbcc1631ull, rows 0x4927f647a64a6aa1ull -> 0xbcbe24594a8acfcaull, final_hash 0xb386dd8b4f22a827ull -> 0x285eaf39ec559587ull
//   Random17/1/0: boundary 0x91c41f4b53c60db8ull -> 0xa978788efbcc1631ull, final_hash 0xb386dd8b4f22a827ull -> 0x285eaf39ec559587ull
//   Random18/0/1: boundary 0x2f90853903ca8075ull -> 0x90ed08a2949b1fb6ull, rows 0x9614b30d8372e1b6ull -> 0x665cda1c725bc1b2ull, final_hash 0x827427f9640b4e97ull -> 0xe16bbfbfa58dbeb1ull
//   Random18/0/0: boundary 0x2f90853903ca8075ull -> 0x90ed08a2949b1fb6ull, final_hash 0x827427f9640b4e97ull -> 0xe16bbfbfa58dbeb1ull
//   Random18/1/1: boundary 0xd759ce46e3583dd6ull -> 0xbf864c04890cf915ull, rows 0x4d62656618897636ull -> 0x807a82ee057fbb5dull, final_hash 0x35a46c04db974779ull -> 0x5c359872c6618dd3ull
//   Random18/1/0: boundary 0xd759ce46e3583dd6ull -> 0xbf864c04890cf915ull, final_hash 0x35a46c04db974779ull -> 0x5c359872c6618dd3ull
//   Random19/0/1: boundary 0x0829e86674bb90a3ull -> 0x3c5cc975f928b775ull, rows 0x0c808692940a05a0ull -> 0xea691a951590ece8ull, final_hash 0x26e403c4671fad9full -> 0x17d53da80c3a895full
//   Random19/0/0: boundary 0x0829e86674bb90a3ull -> 0x3c5cc975f928b775ull, final_hash 0x26e403c4671fad9full -> 0x17d53da80c3a895full
//   Random19/1/1: boundary 0x62aea0c1de6a6c13ull -> 0x92536ad17b9fbf26ull, rows 0xf4f4a620ee4a7ab5ull -> 0xabc10759f5e1ffa1ull, final_hash 0xb4aa06caa7722a48ull -> 0x168791086f51c368ull
//   Random19/1/0: boundary 0x62aea0c1de6a6c13ull -> 0x92536ad17b9fbf26ull, final_hash 0xb4aa06caa7722a48ull -> 0x168791086f51c368ull
//   Random20/0/1: boundary 0x8caa054b399b232aull -> 0x90b20d166e1357e0ull, rows 0x4fc0f37c9645d067ull -> 0x808745030887aa13ull, final_hash 0x9facee5fae21aa7cull -> 0x3968c24c22199036ull
//   Random20/0/0: boundary 0x8caa054b399b232aull -> 0x90b20d166e1357e0ull, final_hash 0x9facee5fae21aa7cull -> 0x3968c24c22199036ull
//   Random20/1/1: boundary 0x0bed3f4e6ae2ef8full -> 0xa82d4e53d2192a5full, rows 0xbc77d10bf614561dull -> 0x8e1411c055e37af3ull, final_hash 0x0d9fbc941287b1cfull -> 0x5c29f15f02417fcaull
//   Random20/1/0: boundary 0x0bed3f4e6ae2ef8full -> 0xa82d4e53d2192a5full, final_hash 0x0d9fbc941287b1cfull -> 0x5c29f15f02417fcaull
//   Random21/0/1: boundary 0x546f79d5d3874035ull -> 0xe1da26acd373548aull, rows 0x57db503858f2c920ull -> 0xad828c8396454565ull, final_hash 0x6a7c93c51185957aull -> 0xd6a6f90f609ca64aull
//   Random21/0/0: boundary 0x546f79d5d3874035ull -> 0xe1da26acd373548aull, final_hash 0x6a7c93c51185957aull -> 0xd6a6f90f609ca64aull
//   Random21/1/1: boundary 0x61ab9c4cf365da62ull -> 0x1b2388f0f789a7d1ull, rows 0x6034eb0b07bc00b6ull -> 0xc1ca64a0715ca8b4ull, final_hash 0x8a67a9fe7abd9c00ull -> 0x08da0b92c166b77cull
//   Random21/1/0: boundary 0x61ab9c4cf365da62ull -> 0x1b2388f0f789a7d1ull, final_hash 0x8a67a9fe7abd9c00ull -> 0x08da0b92c166b77cull
//   Random22/0/1: boundary 0x7264962e97a25a80ull -> 0xef05a2f01ca0c05cull, rows 0x744915b658d1f5d1ull -> 0xb151ebfe9a44ebebull, final_hash 0x1a6bcb392d46b371ull -> 0x1b1b4c42bccc2191ull
//   Random22/0/0: boundary 0x7264962e97a25a80ull -> 0xef05a2f01ca0c05cull, final_hash 0x1a6bcb392d46b371ull -> 0x1b1b4c42bccc2191ull
//   Random22/1/1: boundary 0x47d74c02fc2519e4ull -> 0x1624d3651edc9089ull, rows 0x7443f9b7143dacd2ull -> 0x2a2f5457686fdb7full, final_hash 0xa04d40cbbd623427ull -> 0xb361721c9b7f57e7ull
//   Random22/1/0: boundary 0x47d74c02fc2519e4ull -> 0x1624d3651edc9089ull, final_hash 0xa04d40cbbd623427ull -> 0xb361721c9b7f57e7ull
//   Random23/0/1: boundary 0xfa4d25cb6d9764cbull -> 0xc50cf90af093d6e9ull, rows 0xdea9813bde333195ull -> 0x629f1e11fd65d1cdull, final_hash 0x5058d81da9e2cf89ull -> 0x9f14134203a5c6e9ull
//   Random23/0/0: boundary 0xfa4d25cb6d9764cbull -> 0xc50cf90af093d6e9ull, final_hash 0x5058d81da9e2cf89ull -> 0x9f14134203a5c6e9ull
//   Random23/1/1: boundary 0xd2626d74d1ddbe27ull -> 0x61be7e60a493c3efull, rows 0x55431bc925b28038ull -> 0x7182d347041d20e2ull, final_hash 0x510aa1745bdba1c7ull -> 0xcac3f08a3f6d7067ull
//   Random23/1/0: boundary 0xd2626d74d1ddbe27ull -> 0x61be7e60a493c3efull, final_hash 0x510aa1745bdba1c7ull -> 0xcac3f08a3f6d7067ull
//   Random24/0/1: boundary 0x1972ae0bc903694dull -> 0xadbbd26ef9c19430ull, rows 0x2b240d2a365b8703ull -> 0x7cea484a04d1bc6dull, final_hash 0x82e72364e836634full -> 0x880c60f0ef5d5a7dull
//   Random24/0/0: boundary 0x1972ae0bc903694dull -> 0xadbbd26ef9c19430ull, final_hash 0x82e72364e836634full -> 0x880c60f0ef5d5a7dull
//   Random24/1/1: boundary 0x3df4ec3fe0efc36full -> 0x91d34f247834f4e9ull, rows 0x8ee35827bfb734d3ull -> 0xf9fa951efcc4b1caull, final_hash 0x521e8d8456c37b6dull -> 0xc06e2e1df8db5231ull
//   Random24/1/0: boundary 0x3df4ec3fe0efc36full -> 0x91d34f247834f4e9ull, final_hash 0x521e8d8456c37b6dull -> 0xc06e2e1df8db5231ull
//   Random25/0/1: boundary 0x5fde9097294eff95ull -> 0x873e291d0017c5dcull, rows 0xeb739137b8b09c27ull -> 0xd20a941e2361c0dfull, final_hash 0x74d22f0254d1d14aull -> 0x7573fe39c01fb659ull
//   Random25/0/0: boundary 0x5fde9097294eff95ull -> 0x873e291d0017c5dcull, final_hash 0x74d22f0254d1d14aull -> 0x7573fe39c01fb659ull
//   Random25/1/1: boundary 0x61de85290b80e276ull -> 0x9006cf963c9d2db4ull, rows 0x3e2dace4bb30d118ull -> 0x39a1e5e11f193964ull, final_hash 0xe27506e279646458ull -> 0xb372cc6219683abdull
//   Random25/1/0: boundary 0x61de85290b80e276ull -> 0x9006cf963c9d2db4ull, final_hash 0xe27506e279646458ull -> 0xb372cc6219683abdull
//   Random26/0/1: boundary 0xf3bbd59278bbf967ull -> 0x406a1ebe98ed8ee5ull, rows 0x1bc27907f389539aull -> 0xd559a888703734a3ull, final_hash 0x86792f22105fe886ull -> 0x758182130fa5a986ull
//   Random26/0/0: boundary 0xf3bbd59278bbf967ull -> 0x406a1ebe98ed8ee5ull, final_hash 0x86792f22105fe886ull -> 0x758182130fa5a986ull
//   Random26/1/1: boundary 0x14e61f95689ec8dfull -> 0x28ce140c5a29687dull, rows 0x2117af0b816b84ebull -> 0x3d8702945df8ba26ull, final_hash 0x32b7fe08ce5005d2ull -> 0xeeb06d8c2a0c4772ull
//   Random26/1/0: boundary 0x14e61f95689ec8dfull -> 0x28ce140c5a29687dull, final_hash 0x32b7fe08ce5005d2ull -> 0xeeb06d8c2a0c4772ull
//   Random27/0/1: boundary 0x053cfa54c61f3bd4ull -> 0x51e3ce5734d5737cull, rows 0x49d4da1649eefffbull -> 0x35105942d4fa267full, final_hash 0x2b62c8cc0129f74cull -> 0x030065ad81ba4087ull
//   Random27/0/0: boundary 0x053cfa54c61f3bd4ull -> 0x51e3ce5734d5737cull, final_hash 0x2b62c8cc0129f74cull -> 0x030065ad81ba4087ull
//   Random27/1/1: boundary 0x26f99a57bee85343ull -> 0x81fa7403c9f754c1ull, rows 0x4ff364a8de81f569ull -> 0x4f43080f0a6c1974ull, final_hash 0xd48238aab27e8f44ull -> 0x900de7e225926b9bull
//   Random27/1/0: boundary 0x26f99a57bee85343ull -> 0x81fa7403c9f754c1ull, final_hash 0xd48238aab27e8f44ull -> 0x900de7e225926b9bull
//   Random28/0/1: boundary 0x5afaa18295f32ff1ull -> 0xa0bb8c1925814fe3ull, rows 0xca9c90143f0b6a95ull -> 0x8e8763454e7d2dcaull, final_hash 0xe019b821239c3290ull -> 0xe51c2406011cd87dull
//   Random28/0/0: boundary 0x5afaa18295f32ff1ull -> 0xa0bb8c1925814fe3ull, final_hash 0xe019b821239c3290ull -> 0xe51c2406011cd87dull
//   Random28/1/1: boundary 0x6d2667eba32721eeull -> 0xdb2802eb6d8819c7ull, rows 0x3c23578f7764250aull -> 0x910e834a102ac932ull, final_hash 0xdb07854a60f415d7ull -> 0x7406a539e21e57ceull
//   Random28/1/0: boundary 0x6d2667eba32721eeull -> 0xdb2802eb6d8819c7ull, final_hash 0xdb07854a60f415d7ull -> 0x7406a539e21e57ceull
//   Random29/0/1: boundary 0xd1301252d9b58f96ull -> 0x2bbdb428b4a78935ull, rows 0xa789410c6a57a22aull -> 0x6c7b12c7d5aa45e4ull, final_hash 0x595deac764871057ull -> 0xfd3d2fb6be6132d7ull
//   Random29/0/0: boundary 0xd1301252d9b58f96ull -> 0x2bbdb428b4a78935ull, final_hash 0x595deac764871057ull -> 0xfd3d2fb6be6132d7ull
//   Random29/1/1: boundary 0x1a25b83f5e6fed46ull -> 0x2bc2551fd2128e20ull, rows 0x85c4d848e014bb3cull -> 0xb9728dfab890bfd1ull, final_hash 0x13fb019ad44bec5aull -> 0x082924a3760602daull
//   Random29/1/0: boundary 0x1a25b83f5e6fed46ull -> 0x2bc2551fd2128e20ull, final_hash 0x13fb019ad44bec5aull -> 0x082924a3760602daull
//   Random30/0/1: boundary 0xa9365ffd65e05a09ull -> 0x585a1a9f34476a29ull, rows 0xc88de1ce1dcfafe1ull -> 0x5d09652b7dc7603full, final_hash 0x134c921985714562ull -> 0x8e3b3ec00a1454a2ull
//   Random30/0/0: boundary 0xa9365ffd65e05a09ull -> 0x585a1a9f34476a29ull, final_hash 0x134c921985714562ull -> 0x8e3b3ec00a1454a2ull
//   Random30/1/1: boundary 0x1facc2c1cfd4c971ull -> 0x8de0587cbe97fc0aull, rows 0x941084ae8c1b103bull -> 0x2fc1ebe82b1783eeull, final_hash 0xf2973241ae3d2097ull -> 0x7ffde30dc93fc9d7ull
//   Random30/1/0: boundary 0x1facc2c1cfd4c971ull -> 0x8de0587cbe97fc0aull, final_hash 0xf2973241ae3d2097ull -> 0x7ffde30dc93fc9d7ull
//   Random31/0/1: boundary 0x8eaa06b36232a908ull -> 0x3b5f096d5bca7d49ull, rows 0xc98390a9e23599d3ull -> 0xd72b574c253a553dull, final_hash 0xb86249f3491f97b0ull -> 0x466ccec496cced70ull
//   Random31/0/0: boundary 0x8eaa06b36232a908ull -> 0x3b5f096d5bca7d49ull, final_hash 0xb86249f3491f97b0ull -> 0x466ccec496cced70ull
//   Random31/1/1: boundary 0xf90b0737ee4a025aull -> 0xc14041a700ffb6c1ull, rows 0x09ec11f94ae3f675ull -> 0xe3f3a394089513b1ull, final_hash 0xc7f3afe378a05865ull -> 0xc8e0343f71af4c85ull
//   Random31/1/0: boundary 0xf90b0737ee4a025aull -> 0xc14041a700ffb6c1ull, final_hash 0xc7f3afe378a05865ull -> 0xc8e0343f71af4c85ull
//   Random32/0/1: boundary 0x32da5eed595ee5b4ull -> 0x7bb420637675ac73ull, rows 0xec4a0ece9475e1d3ull -> 0x7a70d1e9f78179c9ull, final_hash 0x4b6c0af81cc429f5ull -> 0x0e03ccad16dc63fdull
//   Random32/0/0: boundary 0x32da5eed595ee5b4ull -> 0x7bb420637675ac73ull, final_hash 0x4b6c0af81cc429f5ull -> 0x0e03ccad16dc63fdull
//   Random32/1/1: boundary 0xe733831f206a336aull -> 0x8bd26473f6dc3b55ull, rows 0xb4483230298afef5ull -> 0xeb260eee22e520f0ull, final_hash 0xe6f2b4aa4d8611a1ull -> 0x597150e94c1a4012ull
//   Random32/1/0: boundary 0xe733831f206a336aull -> 0x8bd26473f6dc3b55ull, final_hash 0xe6f2b4aa4d8611a1ull -> 0x597150e94c1a4012ull
//   Random33/0/1: boundary 0x33bf503af7204823ull -> 0xbfcfebe96632bd39ull, rows 0x39a8de40423427d1ull -> 0x116bc2f57887ba4bull, final_hash 0xbe29a314289e811cull -> 0xf8a0e855a842d39cull
//   Random33/0/0: boundary 0x33bf503af7204823ull -> 0xbfcfebe96632bd39ull, final_hash 0xbe29a314289e811cull -> 0xf8a0e855a842d39cull
//   Random33/1/1: boundary 0x7f150e22e1c3c861ull -> 0xe17f0353f37c41f1ull, rows 0xda9af32172dac1a4ull -> 0x0ba98d74e8b769b7ull, final_hash 0x1122e3cca5beb09dull -> 0xb76d3ca699ea4ebdull
//   Random33/1/0: boundary 0x7f150e22e1c3c861ull -> 0xe17f0353f37c41f1ull, final_hash 0x1122e3cca5beb09dull -> 0xb76d3ca699ea4ebdull
//   Random34/0/1: boundary 0x9191daa7f287cff2ull -> 0xea1b68237d95805full, rows 0x1351d75d55b69237ull -> 0xb29d9ea005d9c282ull, final_hash 0x53efe9facdfed383ull -> 0xb03d41aea9c00c63ull
//   Random34/0/0: boundary 0x9191daa7f287cff2ull -> 0xea1b68237d95805full, final_hash 0x53efe9facdfed383ull -> 0xb03d41aea9c00c63ull
//   Random34/1/1: boundary 0xe3bd733dbb7dd142ull -> 0x4ecacf8ab2d85c47ull, rows 0xcc37232ed176c590ull -> 0x294fe821fef3729dull, final_hash 0x3c1c21ded8a43a85ull -> 0xe4e38b6ada294505ull
//   Random34/1/0: boundary 0xe3bd733dbb7dd142ull -> 0x4ecacf8ab2d85c47ull, final_hash 0x3c1c21ded8a43a85ull -> 0xe4e38b6ada294505ull
//   Random35/0/1: boundary 0xa1dad91ce6005cbbull -> 0xeb16fb1a6bc80c17ull, rows 0xac8c4802e8a8b254ull -> 0x7265444996669f20ull, final_hash 0xc8f7b4decfbdeed1ull -> 0xb971c6bc6d7ac182ull
//   Random35/0/0: boundary 0xa1dad91ce6005cbbull -> 0xeb16fb1a6bc80c17ull, final_hash 0xc8f7b4decfbdeed1ull -> 0xb971c6bc6d7ac182ull
//   Random35/1/1: boundary 0x6ce4272e31e5439eull -> 0x193ec2cb9a163900ull, rows 0xf840ac1e5c21e71full -> 0x9233c78214742aa3ull, final_hash 0x0020809913835e3dull -> 0xa9bdcf22e28567b2ull
//   Random35/1/0: boundary 0x6ce4272e31e5439eull -> 0x193ec2cb9a163900ull, final_hash 0x0020809913835e3dull -> 0xa9bdcf22e28567b2ull
//   Random36/0/1: boundary 0xb6ed02402286f1daull -> 0x3e3529f5adea5e8aull, rows 0xb93cb982740efaf2ull -> 0x16d9ee97193355f5ull, final_hash 0x99b1254e54e63283ull -> 0x32a2c44c6f794a8cull
//   Random36/0/0: boundary 0xb6ed02402286f1daull -> 0x3e3529f5adea5e8aull, final_hash 0x99b1254e54e63283ull -> 0x32a2c44c6f794a8cull
//   Random36/1/1: boundary 0xe4e7726a4db33f47ull -> 0x99c3ad89ead2d6e0ull, rows 0xbbc67cbeef5f3d57ull -> 0x144bd2f3c5b9c0f8ull, final_hash 0xaf91a8392845aa5aull -> 0xe6c283fd13b0660cull
//   Random36/1/0: boundary 0xe4e7726a4db33f47ull -> 0x99c3ad89ead2d6e0ull, final_hash 0xaf91a8392845aa5aull -> 0xe6c283fd13b0660cull
//   Random37/0/1: boundary 0xf50f40fe8bb210c2ull -> 0x3a89ff92c403a3e7ull, rows 0x6830953a611d9b95ull -> 0xddda8d77310d918full, final_hash 0x218cf254fd19cf8aull -> 0xff9c74898bf3a72aull
//   Random37/0/0: boundary 0xf50f40fe8bb210c2ull -> 0x3a89ff92c403a3e7ull, final_hash 0x218cf254fd19cf8aull -> 0xff9c74898bf3a72aull
//   Random37/1/1: boundary 0x44783e84e44eaf3bull -> 0x8a36a09ee42999ecull, rows 0x5024d58d139a41c8ull -> 0xf12dd4800e96f492ull, final_hash 0x7d6333767228433aull -> 0x9f14bee9808ee8baull
//   Random37/1/0: boundary 0x44783e84e44eaf3bull -> 0x8a36a09ee42999ecull, final_hash 0x7d6333767228433aull -> 0x9f14bee9808ee8baull
//   Random38/0/1: boundary 0x9b34eab31c08b2e9ull -> 0x36ed3eee0da888c2ull, rows 0x611d2e69cd985562ull -> 0x2bf58dbe1c5bd9acull, final_hash 0xf756c93d8d25c11cull -> 0x5b82e26291364fc0ull
//   Random38/0/0: boundary 0x9b34eab31c08b2e9ull -> 0x36ed3eee0da888c2ull, final_hash 0xf756c93d8d25c11cull -> 0x5b82e26291364fc0ull
//   Random38/1/1: boundary 0x6b22f5a3ffc931ceull -> 0xa3225feaf19b98feull, rows 0xa60b5a9b3c472885ull -> 0x59c34402fcf68e2aull, final_hash 0xf2c798a5bba88d39ull -> 0x135197c1ef34cbbdull
//   Random38/1/0: boundary 0x6b22f5a3ffc931ceull -> 0xa3225feaf19b98feull, final_hash 0xf2c798a5bba88d39ull -> 0x135197c1ef34cbbdull
//   Random39/0/1: boundary 0x271e3d6676599351ull -> 0x9bcdaebb98702055ull, rows 0xd43319fbe54e5588ull -> 0xf4194fbb3f8e63f4ull, final_hash 0xbbf808ba5ac7b67eull -> 0x3026f01eb73dfcdaull
//   Random39/0/0: boundary 0x271e3d6676599351ull -> 0x9bcdaebb98702055ull, final_hash 0xbbf808ba5ac7b67eull -> 0x3026f01eb73dfcdaull
//   Random39/1/1: boundary 0x4b1fcedefee60801ull -> 0x5fa98522adc27aafull, rows 0xd3eb60fe967c52eeull -> 0x50b22f98ba965993ull, final_hash 0x6842fdc2e3fbde5dull -> 0x123f0c5b09aeae91ull
//   Random39/1/0: boundary 0x4b1fcedefee60801ull -> 0x5fa98522adc27aafull, final_hash 0x6842fdc2e3fbde5dull -> 0x123f0c5b09aeae91ull
//   Random40/0/1: boundary 0x246be525b446fce2ull -> 0x9f1a94ed263d51dfull, rows 0x9d244e811d5ca386ull -> 0x6f59acfa20c1f416ull, final_hash 0x933f71890476b2bbull -> 0x95af814f9835cb65ull
//   Random40/0/0: boundary 0x246be525b446fce2ull -> 0x9f1a94ed263d51dfull, final_hash 0x933f71890476b2bbull -> 0x95af814f9835cb65ull
//   Random40/1/1: boundary 0x04841e1d0f981de5ull -> 0xf0451d9a693e54e3ull, rows 0xc3ce4c5ae0eef6cdull -> 0x01721733b1316376ull, final_hash 0x41541580ac45b572ull -> 0x8fdb15e706d2b994ull
//   Random40/1/0: boundary 0x04841e1d0f981de5ull -> 0xf0451d9a693e54e3ull, final_hash 0x41541580ac45b572ull -> 0x8fdb15e706d2b994ull
//   Random41/0/1: boundary 0x01aece7db4fb0866ull -> 0x6509ace183e7d2cdull, rows 0x1b30dc30f66b89bcull -> 0x957c5244d07600b9ull, final_hash 0x691121b701b14a94ull -> 0xcc1203a69591b134ull
//   Random41/0/0: boundary 0x01aece7db4fb0866ull -> 0x6509ace183e7d2cdull, final_hash 0x691121b701b14a94ull -> 0xcc1203a69591b134ull
//   Random41/1/1: boundary 0x6a009217c0648447ull -> 0xd2d43ab5fdd2ef0bull, rows 0x94eeabc325d68785ull -> 0xb907bbe1bb2da083ull, final_hash 0x29d310f0bf1d7a89ull -> 0xc55d2d003a5bec69ull
//   Random41/1/0: boundary 0x6a009217c0648447ull -> 0xd2d43ab5fdd2ef0bull, final_hash 0x29d310f0bf1d7a89ull -> 0xc55d2d003a5bec69ull
//   Random42/0/1: boundary 0x983f3224390b3aa8ull -> 0xa0ee848263988a3dull, rows 0xdd47cc8bbcfac978ull -> 0x1f2ad0a937e24f39ull, final_hash 0xd5c7b24ea3676694ull -> 0xb124b9f52e8cf9f2ull
//   Random42/0/0: boundary 0x983f3224390b3aa8ull -> 0xa0ee848263988a3dull, final_hash 0xd5c7b24ea3676694ull -> 0xb124b9f52e8cf9f2ull
//   Random42/1/1: boundary 0x5391d71c90732f54ull -> 0x7f6282e75cc72bb0ull, rows 0x95d22736dbced96dull -> 0x23bb6075acaedb2eull, final_hash 0xa3fed64ce5a4b1f9ull -> 0xdb2120d5edab4d9full
//   Random42/1/0: boundary 0x5391d71c90732f54ull -> 0x7f6282e75cc72bb0ull, final_hash 0xa3fed64ce5a4b1f9ull -> 0xdb2120d5edab4d9full
//   Random43/0/1: boundary 0x3721d2c0c27485e6ull -> 0x83e2926a6d6dec9cull, rows 0x83e3892349cf9593ull -> 0x784d62a813c85f48ull, final_hash 0xeb9aef460c2a6b69ull -> 0x5d4d3ed83aa4608cull
//   Random43/0/0: boundary 0x3721d2c0c27485e6ull -> 0x83e2926a6d6dec9cull, final_hash 0xeb9aef460c2a6b69ull -> 0x5d4d3ed83aa4608cull
//   Random43/1/1: boundary 0x38e80c0ff09bd6efull -> 0xb9a953e47994593full, rows 0xb48d33783f27e610ull -> 0x879899b5598309eeull, final_hash 0x0584f3141a2ac711ull -> 0xb6e20a498ce1b25aull
//   Random43/1/0: boundary 0x38e80c0ff09bd6efull -> 0xb9a953e47994593full, final_hash 0x0584f3141a2ac711ull -> 0xb6e20a498ce1b25aull
//   Random44/0/1: boundary 0x40302399ca38ec05ull -> 0xdae57d80c4d2a634ull, rows 0x4bdc4b7a1c4ca040ull -> 0xf7669a1535370da9ull, final_hash 0x55eec4db28687678ull -> 0xc555f0e88b5eb378ull
//   Random44/0/0: boundary 0x40302399ca38ec05ull -> 0xdae57d80c4d2a634ull, final_hash 0x55eec4db28687678ull -> 0xc555f0e88b5eb378ull
//   Random44/1/1: boundary 0xb8a256af96ab69efull -> 0x360b704190f157adull, rows 0xf56d5278e6ece7caull -> 0x5b2a75cdabd7235cull, final_hash 0x49bc82761efd9785ull -> 0xde36f7f290af13a5ull
//   Random44/1/0: boundary 0xb8a256af96ab69efull -> 0x360b704190f157adull, final_hash 0x49bc82761efd9785ull -> 0xde36f7f290af13a5ull
//   Random45/0/1: boundary 0x2c707948f442e0a5ull -> 0xbf351c50f4a44f8full, rows 0xe0be91403cc03881ull -> 0xfee900af4573bbc1ull, final_hash 0x49b18e779e62a8fbull -> 0x7f2867c87c78723eull
//   Random45/0/0: boundary 0x2c707948f442e0a5ull -> 0xbf351c50f4a44f8full, final_hash 0x49b18e779e62a8fbull -> 0x7f2867c87c78723eull
//   Random45/1/1: boundary 0x8c2ae0a418e236daull -> 0x81ba9360ff25c670ull, rows 0xe32f963f031175d8ull -> 0xe345a7e76585b1deull, final_hash 0x6fe2fe4de0b41257ull -> 0x6c13a376f0a1a840ull
//   Random45/1/0: boundary 0x8c2ae0a418e236daull -> 0x81ba9360ff25c670ull, final_hash 0x6fe2fe4de0b41257ull -> 0x6c13a376f0a1a840ull
//   Random46/0/1: boundary 0xf293162843324a42ull -> 0x3ad713968f2546d9ull, rows 0x2fe9f58c05bd5391ull -> 0x84dd0930114cd829ull, final_hash 0x138273f0186f99a8ull -> 0x6bb382cfb976e148ull
//   Random46/0/0: boundary 0xf293162843324a42ull -> 0x3ad713968f2546d9ull, final_hash 0x138273f0186f99a8ull -> 0x6bb382cfb976e148ull
//   Random46/1/1: boundary 0x5c518c7c16ad6eb9ull -> 0x997f2e0ee9786d91ull, rows 0x7114f97576e05de1ull -> 0x1ce86cd2c755bea3ull, final_hash 0x02c866a863d14e29ull -> 0x556134f8585397c9ull
//   Random46/1/0: boundary 0x5c518c7c16ad6eb9ull -> 0x997f2e0ee9786d91ull, final_hash 0x02c866a863d14e29ull -> 0x556134f8585397c9ull
//   Random47/0/1: boundary 0xeb703ef1728b8316ull -> 0x8ce025f147bbdb2dull, rows 0xcf65482f5b03def1ull -> 0x74895c0d69d6b4a8ull, final_hash 0x6d733b355b0b4883ull -> 0x31ec37588708d5f1ull
//   Random47/0/0: boundary 0xeb703ef1728b8316ull -> 0x8ce025f147bbdb2dull, final_hash 0x6d733b355b0b4883ull -> 0x31ec37588708d5f1ull
//   Random47/1/1: boundary 0x25be4848720c17deull -> 0xebf0544eb982483aull, rows 0xb3c87f095d3d086dull -> 0xca410d7365dca790ull, final_hash 0xc9b10c1db4c2a51aull -> 0xa8aff582382caf48ull
//   Random47/1/0: boundary 0x25be4848720c17deull -> 0xebf0544eb982483aull, final_hash 0xc9b10c1db4c2a51aull -> 0xa8aff582382caf48ull
//   Random48/0/1: boundary 0xd0c92fe6fc290e0bull -> 0xaa6b18e4254e2f68ull, rows 0x0af2126a53f8f7b9ull -> 0x2f80186c69547a10ull, final_hash 0x840184ac798a5de2ull -> 0xaff43b38e6bc02acull
//   Random48/0/0: boundary 0xd0c92fe6fc290e0bull -> 0xaa6b18e4254e2f68ull, final_hash 0x840184ac798a5de2ull -> 0xaff43b38e6bc02acull
//   Random48/1/1: boundary 0xd39f4122a0d0a91dull -> 0x655a5637ae415d60ull, rows 0xd439da8c48c337ddull -> 0x13866e45be14eaeeull, final_hash 0x1969d9a52bdd51c2ull -> 0xa0e205aa768a0bc9ull
//   Random48/1/0: boundary 0xd39f4122a0d0a91dull -> 0x655a5637ae415d60ull, final_hash 0x1969d9a52bdd51c2ull -> 0xa0e205aa768a0bc9ull
// R5 lane PAR-ORDERS re-harvested it the same way on its tree (62 of 272 runs move;
// see the lane's hash commit for why each moves):
//   Trail/0/1: boundary 0x5211ba6f22808f15 -> 0xa09d959161ab83b5, rows 0x5fd4f2baecf12c67 -> 0x6db293f4d9ba407e, final_hash 0x1ca55654b8ff3a6e -> 0xf9e526fb6a079950
//   Trail/0/0: boundary 0x5211ba6f22808f15 -> 0xa09d959161ab83b5, final_hash 0x1ca55654b8ff3a6e -> 0xf9e526fb6a079950
//   Trail/1/1: boundary 0x596611dc371ff159 -> 0x2a39cc8c834a5287, rows 0x5ebc78f3f36b3da8 -> 0xe23d175bdbe6d254, final_hash 0x2d3ea22c1cd861e4 -> 0xffac0df8a2dbc19e
//   Trail/1/0: boundary 0x596611dc371ff159 -> 0x2a39cc8c834a5287, final_hash 0x2d3ea22c1cd861e4 -> 0xffac0df8a2dbc19e
//   Random01/0/1: boundary 0xc538d8fca421c703 -> 0xfa3864e9f5f3a619, rows 0xc035295ef347b140 -> 0x22da84303f34b121, trades 0x5f25a36c2031c262 -> 0x71cab0c731b4168d, final_hash 0x166e28e2277135d2 -> 0xa142013345fb25dd
//   Random01/0/0: boundary 0xc538d8fca421c703 -> 0xfa3864e9f5f3a619, trades 0x5f25a36c2031c262 -> 0x71cab0c731b4168d, final_hash 0x166e28e2277135d2 -> 0xa142013345fb25dd
//   Random01/1/1: boundary 0x5b8b5e29d258d71c -> 0xfdafd368c22a3bb4, rows 0x16a72508f5fdd125 -> 0x30ace005c5ea2e1b, trades 0x81d6c3334c3a4ed0 -> 0xee55d992ca5fbe97, final_hash 0xa0becd4ec596146e -> 0xb44a05ed46f5109d
//   Random01/1/0: boundary 0x5b8b5e29d258d71c -> 0xfdafd368c22a3bb4, trades 0x81d6c3334c3a4ed0 -> 0xee55d992ca5fbe97, final_hash 0xa0becd4ec596146e -> 0xb44a05ed46f5109d
//   Random05/0/1: boundary 0x38db302327d01e80 -> 0x3e8e6495fe2e6dc1, rows 0x2461425b8e878f0d -> 0x2703dbc59d3516a6, trades 0xae3b2415c28fc5cb -> 0x5da77f8971db3d7a, final_hash 0xe32002fbd2d6ea3e -> 0xb72557b7f6894eaf
//   Random05/0/0: boundary 0x38db302327d01e80 -> 0x3e8e6495fe2e6dc1, trades 0xae3b2415c28fc5cb -> 0x5da77f8971db3d7a, final_hash 0xe32002fbd2d6ea3e -> 0xb72557b7f6894eaf
//   Random05/1/1: boundary 0xe3ae54dee0ba6db5 -> 0x4be132773d14c0e0, rows 0xc1c6474702bc75e0 -> 0xa6803dead66d3343, trades 0x6b60d1d77a27186f -> 0xdf770d465571b020, final_hash 0x70343cf2d03a75eb -> 0xb3561169b361d8bf
//   Random05/1/0: boundary 0xe3ae54dee0ba6db5 -> 0x4be132773d14c0e0, trades 0x6b60d1d77a27186f -> 0xdf770d465571b020, final_hash 0x70343cf2d03a75eb -> 0xb3561169b361d8bf
//   Random10/0/1: boundary 0xe529db3277bf6dd9 -> 0x998ee74ea73334c6, rows 0x1a655455a6bd3b6f -> 0x5a3104007433abc0, trades 0x70ed423d22403976 -> 0x274b98362186a62f, final_hash 0xffdc77023cb52b44 -> 0x0a7888fb9b890fb4, boundary_count 71 -> 70, trade_count 24 -> 23
//   Random10/0/0: boundary 0xe529db3277bf6dd9 -> 0x998ee74ea73334c6, trades 0x70ed423d22403976 -> 0x274b98362186a62f, final_hash 0xffdc77023cb52b44 -> 0x0a7888fb9b890fb4, boundary_count 71 -> 70, trade_count 24 -> 23
//   Random10/1/1: boundary 0xb3a8db733c3f30bd -> 0xde7dbd52ac33a7ff, rows 0xa2424fa6550c3ef1 -> 0xaf8f0ca763b06949, trades 0x2d930b1932bc89bd -> 0x5dcb9b87576b70ed, final_hash 0x61b614a68f564bf0 -> 0xfbe70c357ac296cf, boundary_count 68 -> 67, trade_count 23 -> 22
//   Random10/1/0: boundary 0xb3a8db733c3f30bd -> 0xde7dbd52ac33a7ff, trades 0x2d930b1932bc89bd -> 0x5dcb9b87576b70ed, final_hash 0x61b614a68f564bf0 -> 0xfbe70c357ac296cf, boundary_count 68 -> 67, trade_count 23 -> 22
//   Random13/0/1: boundary 0xf255fd63895e7bad -> 0x39caae56088c1db8, rows 0x41470781ab6bef64 -> 0x4028b545fb2c3ad9, trades 0xb5e34ee7b3268a86 -> 0x7f720ad1034d2991, final_hash 0x57de0e46784b0f54 -> 0xc19e5f5bbefe0e7f
//   Random13/0/0: boundary 0xf255fd63895e7bad -> 0x39caae56088c1db8, trades 0xb5e34ee7b3268a86 -> 0x7f720ad1034d2991, final_hash 0x57de0e46784b0f54 -> 0xc19e5f5bbefe0e7f
//   Random13/1/1: boundary 0x7abd074c56aca8c6 -> 0x07b714c492b5ae85, rows 0x41de5734424c47ed -> 0xa03b3f8bed95186d, trades 0xab774513033e4b33 -> 0xfe702e1eb2f469c0, final_hash 0xa6590b3d3950e7bb -> 0xa9f1d1a463925b52
//   Random13/1/0: boundary 0x7abd074c56aca8c6 -> 0x07b714c492b5ae85, trades 0xab774513033e4b33 -> 0xfe702e1eb2f469c0, final_hash 0xa6590b3d3950e7bb -> 0xa9f1d1a463925b52
//   Random14/0/1: boundary 0x6b59b79be4590a72 -> 0x21085d70db4c336b, rows 0x6d4cbed6a7d3b423 -> 0x394ebfbc2c15522a, trades 0xbcda0382be163781 -> 0x4e8d14373274d558, final_hash 0x918405ca8ede2f52 -> 0xad1f71b1afdd37f8, trade_count 17 -> 16
//   Random14/0/0: boundary 0x6b59b79be4590a72 -> 0x21085d70db4c336b, trades 0xbcda0382be163781 -> 0x4e8d14373274d558, final_hash 0x918405ca8ede2f52 -> 0xad1f71b1afdd37f8, trade_count 17 -> 16
//   Random14/1/1: boundary 0x8dfaddf560dc80ca -> 0x5a8dbff598244e8c, rows 0x13e689009402d1e0 -> 0x4c48857b5cd25816, trades 0x8bca41c28a85c67a -> 0xbec287230f684e1d, final_hash 0x70415e0fc326b13c -> 0x7f45221fb1111aa3, trade_count 17 -> 16
//   Random14/1/0: boundary 0x8dfaddf560dc80ca -> 0x5a8dbff598244e8c, trades 0x8bca41c28a85c67a -> 0xbec287230f684e1d, final_hash 0x70415e0fc326b13c -> 0x7f45221fb1111aa3, trade_count 17 -> 16
//   Random21/0/1: boundary 0xe1da26acd373548a -> 0x18e96beb584671fc, rows 0xad828c8396454565 -> 0x6c5b4e5a67209f94, trades 0x7d29f9137d1bc337 -> 0x0571605cf9e50eb1, final_hash 0xd6a6f90f609ca64a -> 0xca719be0f46f42ac
//   Random21/0/0: boundary 0xe1da26acd373548a -> 0x18e96beb584671fc, trades 0x7d29f9137d1bc337 -> 0x0571605cf9e50eb1, final_hash 0xd6a6f90f609ca64a -> 0xca719be0f46f42ac
//   Random21/1/1: boundary 0x1b2388f0f789a7d1 -> 0xd32a038cc5b6ea6e, rows 0xc1ca64a0715ca8b4 -> 0x9074957bf890f8e6, trades 0x32f61d041209dc84 -> 0x20e23c0060589e5e, final_hash 0x08da0b92c166b77c -> 0xb6387745e72b0075
//   Random21/1/0: boundary 0x1b2388f0f789a7d1 -> 0xd32a038cc5b6ea6e, trades 0x32f61d041209dc84 -> 0x20e23c0060589e5e, final_hash 0x08da0b92c166b77c -> 0xb6387745e72b0075
//   Random25/0/1: boundary 0x873e291d0017c5dc -> 0x1091baf336b7e96a, rows 0xd20a941e2361c0df -> 0x5836120ce0f3d783, trades 0x5ff4203ec9ac379e -> 0xf92cddf4fc024c49, final_hash 0x7573fe39c01fb659 -> 0x3f0d68bfc74cf35f
//   Random25/0/0: boundary 0x873e291d0017c5dc -> 0x1091baf336b7e96a, trades 0x5ff4203ec9ac379e -> 0xf92cddf4fc024c49, final_hash 0x7573fe39c01fb659 -> 0x3f0d68bfc74cf35f
//   Random25/1/1: boundary 0x9006cf963c9d2db4 -> 0x4c59eab8929878ab, rows 0x39a1e5e11f193964 -> 0x90bf03c0ed26cd99, trades 0x5ff4203ec9ac379e -> 0xf92cddf4fc024c49, final_hash 0xb372cc6219683abd -> 0xeb9d9c36364972df
//   Random25/1/0: boundary 0x9006cf963c9d2db4 -> 0x4c59eab8929878ab, trades 0x5ff4203ec9ac379e -> 0xf92cddf4fc024c49, final_hash 0xb372cc6219683abd -> 0xeb9d9c36364972df
//   Random26/0/1: rows 0xd559a888703734a3 -> 0xc4ceaa2f46b3b1c7, final_hash 0x758182130fa5a986 -> 0x6d9f1512cb3c63ef
//   Random26/0/0: final_hash 0x758182130fa5a986 -> 0x6d9f1512cb3c63ef
//   Random26/1/1: rows 0x3d8702945df8ba26 -> 0x383f34833e879b1b, final_hash 0xeeb06d8c2a0c4772 -> 0x500ef2279c277ba0
//   Random26/1/0: final_hash 0xeeb06d8c2a0c4772 -> 0x500ef2279c277ba0
//   Random27/0/1: boundary 0x51e3ce5734d5737c -> 0xeafcaf72c5794d88, rows 0x35105942d4fa267f -> 0xfbb77c6f11f0944f, final_hash 0x030065ad81ba4087 -> 0xf2d844781c8e9819
//   Random27/0/0: boundary 0x51e3ce5734d5737c -> 0xeafcaf72c5794d88, final_hash 0x030065ad81ba4087 -> 0xf2d844781c8e9819
//   Random27/1/1: boundary 0x81fa7403c9f754c1 -> 0x433fe5d0340138d8, rows 0x4f43080f0a6c1974 -> 0x80e409be19d6131d, final_hash 0x900de7e225926b9b -> 0x2a459b69d95400b1
//   Random27/1/0: boundary 0x81fa7403c9f754c1 -> 0x433fe5d0340138d8, final_hash 0x900de7e225926b9b -> 0x2a459b69d95400b1
//   Random31/0/1: boundary 0x3b5f096d5bca7d49 -> 0x09b545a0067e8c26, rows 0xd72b574c253a553d -> 0x3c6ae0150bb63629, final_hash 0x466ccec496cced70 -> 0xfaf6a143c8b0d038
//   Random31/0/0: boundary 0x3b5f096d5bca7d49 -> 0x09b545a0067e8c26, final_hash 0x466ccec496cced70 -> 0xfaf6a143c8b0d038
//   Random31/1/1: boundary 0xc14041a700ffb6c1 -> 0xb42bd9cfbe060a73, rows 0xe3f3a394089513b1 -> 0x928f7cfde5bdb105, final_hash 0xc8e0343f71af4c85 -> 0xf602f077ecb2b7cf
//   Random31/1/0: boundary 0xc14041a700ffb6c1 -> 0xb42bd9cfbe060a73, final_hash 0xc8e0343f71af4c85 -> 0xf602f077ecb2b7cf
//   Random32/0/1: boundary 0x7bb420637675ac73 -> 0x220cd02053dbc40a, rows 0x7a70d1e9f78179c9 -> 0x9afe2ada13350695, final_hash 0x0e03ccad16dc63fd -> 0xe577534dd3033b52
//   Random32/0/0: boundary 0x7bb420637675ac73 -> 0x220cd02053dbc40a, final_hash 0x0e03ccad16dc63fd -> 0xe577534dd3033b52
//   Random32/1/1: boundary 0x8bd26473f6dc3b55 -> 0xc0ca43094334f008, rows 0xeb260eee22e520f0 -> 0xe85eb4136b8c435d, final_hash 0x597150e94c1a4012 -> 0xba233c9fc46444a4
//   Random32/1/0: boundary 0x8bd26473f6dc3b55 -> 0xc0ca43094334f008, final_hash 0x597150e94c1a4012 -> 0xba233c9fc46444a4
//   Random36/0/1: boundary 0x3e3529f5adea5e8a -> 0xd4b5588948d4b755, rows 0x16d9ee97193355f5 -> 0x6af2dc7dd208ef5d, trades 0x8ee686191c3d0539 -> 0x24bf064fe8416ffd, final_hash 0x32a2c44c6f794a8c -> 0x1980e25b047dea78
//   Random36/0/0: boundary 0x3e3529f5adea5e8a -> 0xd4b5588948d4b755, trades 0x8ee686191c3d0539 -> 0x24bf064fe8416ffd, final_hash 0x32a2c44c6f794a8c -> 0x1980e25b047dea78
//   Random36/1/1: boundary 0x99c3ad89ead2d6e0 -> 0xd1532dd9d52ae5f4, rows 0x144bd2f3c5b9c0f8 -> 0x0e1d68d8d6c4c5e2, trades 0x10b01a5c8873f5d5 -> 0x86d1b68c9a0453a2, final_hash 0xe6c283fd13b0660c -> 0xe93d9e6c724efef6
//   Random36/1/0: boundary 0x99c3ad89ead2d6e0 -> 0xd1532dd9d52ae5f4, trades 0x10b01a5c8873f5d5 -> 0x86d1b68c9a0453a2, final_hash 0xe6c283fd13b0660c -> 0xe93d9e6c724efef6
//   Random40/0/1: boundary 0x9f1a94ed263d51df -> 0x869959795f89f7af, rows 0x6f59acfa20c1f416 -> 0xe9919376f3542ea7, final_hash 0x95af814f9835cb65 -> 0xd91533e6639e22f9
//   Random40/0/0: boundary 0x9f1a94ed263d51df -> 0x869959795f89f7af, final_hash 0x95af814f9835cb65 -> 0xd91533e6639e22f9
//   Random40/1/1: boundary 0xf0451d9a693e54e3 -> 0xf47407130500a9bd, rows 0x01721733b1316376 -> 0xda9b4928c023ca07, final_hash 0x8fdb15e706d2b994 -> 0x5a8f1ec5a18f3731
//   Random40/1/0: boundary 0xf0451d9a693e54e3 -> 0xf47407130500a9bd, final_hash 0x8fdb15e706d2b994 -> 0x5a8f1ec5a18f3731
//   Random43/0/1: boundary 0x83e2926a6d6dec9c -> 0x7fb8967d6d6d24d2, rows 0x784d62a813c85f48 -> 0x98cae92178fe91eb, final_hash 0x5d4d3ed83aa4608c -> 0xdc29a310e9c7d06b
//   Random43/0/0: boundary 0x83e2926a6d6dec9c -> 0x7fb8967d6d6d24d2, final_hash 0x5d4d3ed83aa4608c -> 0xdc29a310e9c7d06b
//   Random43/1/1: boundary 0xb9a953e47994593f -> 0xffd1c55ce888ef72, rows 0x879899b5598309ee -> 0x8ac3c2c1053a4819, final_hash 0xb6e20a498ce1b25a -> 0x45393018e93b32be
//   Random43/1/0: boundary 0xb9a953e47994593f -> 0xffd1c55ce888ef72, final_hash 0xb6e20a498ce1b25a -> 0x45393018e93b32be
//   Random45/1/1: boundary 0x81ba9360ff25c670 -> 0x32a1d85e85b93f0a, rows 0xe345a7e76585b1de -> 0xebcfcdf78cd848a5, final_hash 0x6c13a376f0a1a840 -> 0x56b0e295081820c0
//   Random45/1/0: boundary 0x81ba9360ff25c670 -> 0x32a1d85e85b93f0a, final_hash 0x6c13a376f0a1a840 -> 0x56b0e295081820c0
// R5 lane PAR-ORDERS-2 moves these runs' trades (INT26: the behaviour half of
// the lane's hash commit 9f7a025c, which says why each moves; harvested the
// same way on the integrated tree, every value equal to the lane's). Their
// hash values, and those of the other runs the lane's new fold moves, move
// once, in the INT26 v19 hash re-pin:
//   CalcOnFills/0/1: trades 0x4603f8c446ca5c1e -> 0x0b9be2eee5282e84
//   CalcOnFills/0/0: trades 0x4603f8c446ca5c1e -> 0x0b9be2eee5282e84
//   Random10/0/1: trades 0x274b98362186a62f -> 0xe335d30ff30abed4, boundary_count 70 -> 69, trade_count 23 -> 22
//   Random10/0/0: trades 0x274b98362186a62f -> 0xe335d30ff30abed4, boundary_count 70 -> 69, trade_count 23 -> 22
//   Random22/0/1: trades 0x603c10590ec68778 -> 0x87be32daa84c6426
//   Random22/0/0: trades 0x603c10590ec68778 -> 0x87be32daa84c6426
// R5 lane H-THIN moves these runs' trade digests and counts (INT26: the
// behaviour half of its hash step ab297364, harvested on the integrated tree
// with this TU's own switch): E19 books every lot's excursions from the
// kernel's sampler, which the trade digest folds, and P10 reserves a default
// FIFO strategy.exit(from_entry)'s own entry quantity. Their hash values move
// once, in the INT26 v19 hash re-pin (140 runs):
//   CalcOnFills/0/1: trades 0x0b9be2eee5282e84 -> 0x87698c8f4050ce14, boundary_count 57 -> 55, trade_count 10 -> 8 (a run an earlier pick had moved too)
//   CalcOnFills/0/0: trades 0x0b9be2eee5282e84 -> 0x87698c8f4050ce14, boundary_count 57 -> 55, trade_count 10 -> 8 (a run an earlier pick had moved too)
//   CalcOnFills/1/1: trades 0x57aa00467e66c075 -> 0xa16d17de4933efda, boundary_count 54 -> 52, trade_count 8 -> 6
//   CalcOnFills/1/0: trades 0x57aa00467e66c075 -> 0xa16d17de4933efda, boundary_count 54 -> 52, trade_count 8 -> 6
//   CloseOrders/0/1: trades 0x36ebbdb119ff2815 -> 0xbc944a52841481b1
//   CloseOrders/0/0: trades 0x36ebbdb119ff2815 -> 0xbc944a52841481b1
//   CloseOrders/1/1: trades 0x8261dcdcaf8f5751 -> 0x4cdcb678c06d1a59
//   CloseOrders/1/0: trades 0x8261dcdcaf8f5751 -> 0x4cdcb678c06d1a59
//   Random01/0/1: trades 0x71cab0c731b4168d -> 0xf2ae7ef9674d4425 (a run an earlier pick had moved too)
//   Random01/0/0: trades 0x71cab0c731b4168d -> 0xf2ae7ef9674d4425 (a run an earlier pick had moved too)
//   Random01/1/1: trades 0xee55d992ca5fbe97 -> 0x64f480bfff90f76a (a run an earlier pick had moved too)
//   Random01/1/0: trades 0xee55d992ca5fbe97 -> 0x64f480bfff90f76a (a run an earlier pick had moved too)
//   Random02/0/1: trades 0xec986a4dcdf52208 -> 0x41793244d920be3d
//   Random02/0/0: trades 0xec986a4dcdf52208 -> 0x41793244d920be3d
//   Random02/1/1: trades 0x95d130f42b9a5776 -> 0x31c2efb8bb736f56
//   Random02/1/0: trades 0x95d130f42b9a5776 -> 0x31c2efb8bb736f56
//   Random03/0/1: trades 0x922d2ca744f5f225 -> 0x8bd01a3594893c29, trade_count 12 -> 11
//   Random03/0/0: trades 0x922d2ca744f5f225 -> 0x8bd01a3594893c29, trade_count 12 -> 11
//   Random03/1/1: trades 0xe213d492a23dfa58 -> 0xde85b8fe81a28e59, trade_count 12 -> 11
//   Random03/1/0: trades 0xe213d492a23dfa58 -> 0xde85b8fe81a28e59, trade_count 12 -> 11
//   Random04/0/1: trades 0xc687874028817a5b -> 0x38df0af477021418
//   Random04/0/0: trades 0xc687874028817a5b -> 0x38df0af477021418
//   Random04/1/1: trades 0x37389353e6ac8bae -> 0x28a43578727c43b5
//   Random04/1/0: trades 0x37389353e6ac8bae -> 0x28a43578727c43b5
//   Random05/0/1: trades 0x5da77f8971db3d7a -> 0x3d4c3aae7bbc3b85 (a run an earlier pick had moved too)
//   Random05/0/0: trades 0x5da77f8971db3d7a -> 0x3d4c3aae7bbc3b85 (a run an earlier pick had moved too)
//   Random05/1/1: trades 0xdf770d465571b020 -> 0xc83a1288cfd7ee4d (a run an earlier pick had moved too)
//   Random05/1/0: trades 0xdf770d465571b020 -> 0xc83a1288cfd7ee4d (a run an earlier pick had moved too)
//   Random06/0/1: trades 0xb724c9aadc73f827 -> 0x19f4b2ac9f3f76f7
//   Random06/0/0: trades 0xb724c9aadc73f827 -> 0x19f4b2ac9f3f76f7
//   Random06/1/1: trades 0xb1c8ba35c0dd393d -> 0x3561041c80b6994f
//   Random06/1/0: trades 0xb1c8ba35c0dd393d -> 0x3561041c80b6994f
//   Random07/0/1: trades 0x93cafb8ccdf82d3f -> 0x67ce7c56c16f12a7
//   Random07/0/0: trades 0x93cafb8ccdf82d3f -> 0x67ce7c56c16f12a7
//   Random07/1/1: trades 0xf516d07702627571 -> 0x9cbc1f07c0648d08
//   Random07/1/0: trades 0xf516d07702627571 -> 0x9cbc1f07c0648d08
//   Random08/0/1: trades 0x1d5e1165b05daf8f -> 0x68ebab74d498abed
//   Random08/0/0: trades 0x1d5e1165b05daf8f -> 0x68ebab74d498abed
//   Random08/1/1: trades 0x7f7cc33b43e81cae -> 0x84ec2499efb30a9e
//   Random08/1/0: trades 0x7f7cc33b43e81cae -> 0x84ec2499efb30a9e
//   Random09/0/1: trades 0x8abae7302d7911be -> 0xcda6a598aa82b5f0
//   Random09/0/0: trades 0x8abae7302d7911be -> 0xcda6a598aa82b5f0
//   Random09/1/1: trades 0x99aae055f2d8091d -> 0x86b2bbc1465372d7
//   Random09/1/0: trades 0x99aae055f2d8091d -> 0x86b2bbc1465372d7
//   Random10/0/1: trades 0xe335d30ff30abed4 -> 0xc9ae66b6356d2649, trade_count 22 -> 23 (P10 on the book PAR-ORDERS' and PAR-ORDERS-2's rules leave: the FIFO exit Y reserves its entry's own unit and the reversal closes the rest, one row more)
//   Random10/0/0: trades 0xe335d30ff30abed4 -> 0xc9ae66b6356d2649, trade_count 22 -> 23 (P10 on the book PAR-ORDERS' and PAR-ORDERS-2's rules leave: the FIFO exit Y reserves its entry's own unit and the reversal closes the rest, one row more)
//   Random10/1/1: trades 0x5dcb9b87576b70ed -> 0x724c585ad13c935d, trade_count 22 -> 23 (a run an earlier pick had moved too)
//   Random10/1/0: trades 0x5dcb9b87576b70ed -> 0x724c585ad13c935d, trade_count 22 -> 23 (a run an earlier pick had moved too)
//   Random12/0/1: trades 0x9ec5e7b69521e874 -> 0xf60b6b66546787a7
//   Random12/0/0: trades 0x9ec5e7b69521e874 -> 0xf60b6b66546787a7
//   Random12/1/1: trades 0xf2c6cada55accbe3 -> 0xfc32c809c37c8290
//   Random12/1/0: trades 0xf2c6cada55accbe3 -> 0xfc32c809c37c8290
//   Random13/0/1: trades 0x7f720ad1034d2991 -> 0x9b711a1ad7f0af2b (a run an earlier pick had moved too)
//   Random13/0/0: trades 0x7f720ad1034d2991 -> 0x9b711a1ad7f0af2b (a run an earlier pick had moved too)
//   Random13/1/1: trades 0xfe702e1eb2f469c0 -> 0x78cbd8f6435e4796 (a run an earlier pick had moved too)
//   Random13/1/0: trades 0xfe702e1eb2f469c0 -> 0x78cbd8f6435e4796 (a run an earlier pick had moved too)
//   Random15/0/1: trades 0xa729d04861d8f487 -> 0xc7b14196e7b743c6, trade_count 11 -> 12
//   Random15/0/0: trades 0xa729d04861d8f487 -> 0xc7b14196e7b743c6, trade_count 11 -> 12
//   Random15/1/1: trades 0xeabf56d064294b39 -> 0x9e6d92b9d40ff818, trade_count 11 -> 12
//   Random15/1/0: trades 0xeabf56d064294b39 -> 0x9e6d92b9d40ff818, trade_count 11 -> 12
//   Random16/0/1: trades 0xf751fbce2a822df8 -> 0x6889115fd9a0fe41, trade_count 17 -> 15
//   Random16/0/0: trades 0xf751fbce2a822df8 -> 0x6889115fd9a0fe41, trade_count 17 -> 15
//   Random16/1/1: trades 0xeb8b68e57975e307 -> 0x552b70400304fd25, trade_count 17 -> 15
//   Random16/1/0: trades 0xeb8b68e57975e307 -> 0x552b70400304fd25, trade_count 17 -> 15
//   Random17/0/1: trades 0xf5006d7b3dcd5c43 -> 0xf3e62779123d7565
//   Random17/0/0: trades 0xf5006d7b3dcd5c43 -> 0xf3e62779123d7565
//   Random17/1/1: trades 0x364d848056b18418 -> 0x5814b347681cd974
//   Random17/1/0: trades 0x364d848056b18418 -> 0x5814b347681cd974
//   Random18/0/1: trades 0x946751d8a6dd7292 -> 0x5f03c99e6854a46a
//   Random18/0/0: trades 0x946751d8a6dd7292 -> 0x5f03c99e6854a46a
//   Random18/1/1: trades 0x946751d8a6dd7292 -> 0x5f03c99e6854a46a
//   Random18/1/0: trades 0x946751d8a6dd7292 -> 0x5f03c99e6854a46a
//   Random19/0/1: trades 0xcfd0b979ead5a43c -> 0x95d7b888983ad8e6
//   Random19/0/0: trades 0xcfd0b979ead5a43c -> 0x95d7b888983ad8e6
//   Random20/0/1: trades 0x35a6da11cd35b29c -> 0x3e0974d0ca2bfa0b
//   Random20/0/0: trades 0x35a6da11cd35b29c -> 0x3e0974d0ca2bfa0b
//   Random20/1/1: trades 0xafad93e39c8617d9 -> 0x1cd39188275caa5d
//   Random20/1/0: trades 0xafad93e39c8617d9 -> 0x1cd39188275caa5d
//   Random21/0/1: trades 0x0571605cf9e50eb1 -> 0xbc89178a735678f6 (a run an earlier pick had moved too)
//   Random21/0/0: trades 0x0571605cf9e50eb1 -> 0xbc89178a735678f6 (a run an earlier pick had moved too)
//   Random21/1/1: trades 0x20e23c0060589e5e -> 0x0cb28a314aa970ae (a run an earlier pick had moved too)
//   Random21/1/0: trades 0x20e23c0060589e5e -> 0x0cb28a314aa970ae (a run an earlier pick had moved too)
//   Random22/0/1: trades 0x87be32daa84c6426 -> 0xedb01aeb6057c25b (a run an earlier pick had moved too)
//   Random22/0/0: trades 0x87be32daa84c6426 -> 0xedb01aeb6057c25b (a run an earlier pick had moved too)
//   Random22/1/1: trades 0x29259a278cc3600b -> 0x70ac53cf16690a86
//   Random22/1/0: trades 0x29259a278cc3600b -> 0x70ac53cf16690a86
//   Random27/0/1: trades 0x3c0165d3836f406a -> 0xbfe2f70bda5f466d
//   Random27/0/0: trades 0x3c0165d3836f406a -> 0xbfe2f70bda5f466d
//   Random27/1/1: trades 0x97addcfee41a4dd8 -> 0x0bb88d26b3dc8cc1
//   Random27/1/0: trades 0x97addcfee41a4dd8 -> 0x0bb88d26b3dc8cc1
//   Random28/0/1: trades 0xc447f3f5058cd81b -> 0x7ee366b7feceed4e
//   Random28/0/0: trades 0xc447f3f5058cd81b -> 0x7ee366b7feceed4e
//   Random31/1/1: trades 0x2569d07bfd939198 -> 0xc42eba994cb3bc7f
//   Random31/1/0: trades 0x2569d07bfd939198 -> 0xc42eba994cb3bc7f
//   Random32/0/1: trades 0x4994badb687c8750 -> 0xb5c0211fafc19569, trade_count 19 -> 18
//   Random32/0/0: trades 0x4994badb687c8750 -> 0xb5c0211fafc19569, trade_count 19 -> 18
//   Random33/0/1: trades 0xf8d13762bc3ab9c0 -> 0xb69ec34fc873583a
//   Random33/0/0: trades 0xf8d13762bc3ab9c0 -> 0xb69ec34fc873583a
//   Random33/1/1: trades 0xca5862380ffcef2f -> 0xa51cad1078866f00
//   Random33/1/0: trades 0xca5862380ffcef2f -> 0xa51cad1078866f00
//   Random35/0/1: trades 0xa8b1e0f14c6cc49a -> 0x2e59ea5f57457b34
//   Random35/0/0: trades 0xa8b1e0f14c6cc49a -> 0x2e59ea5f57457b34
//   Random35/1/1: trades 0xb2b344c283516d30 -> 0xf9dd1b1a0c3d9f6a
//   Random35/1/0: trades 0xb2b344c283516d30 -> 0xf9dd1b1a0c3d9f6a
//   Random36/0/1: trades 0x24bf064fe8416ffd -> 0x71b837070cbf3386 (a run an earlier pick had moved too)
//   Random36/0/0: trades 0x24bf064fe8416ffd -> 0x71b837070cbf3386 (a run an earlier pick had moved too)
//   Random36/1/1: trades 0x86d1b68c9a0453a2 -> 0xc91ddda9c3e46569 (a run an earlier pick had moved too)
//   Random36/1/0: trades 0x86d1b68c9a0453a2 -> 0xc91ddda9c3e46569 (a run an earlier pick had moved too)
//   Random37/0/1: trades 0x6b11a4e13499e50d -> 0x52bf39ca2f2765c6
//   Random37/0/0: trades 0x6b11a4e13499e50d -> 0x52bf39ca2f2765c6
//   Random37/1/1: trades 0x15dfd0d0fb5f649a -> 0x41de938f8a060981
//   Random37/1/0: trades 0x15dfd0d0fb5f649a -> 0x41de938f8a060981
//   Random38/0/1: trades 0x3a8c8a474889316e -> 0xe38062999cae5744
//   Random38/0/0: trades 0x3a8c8a474889316e -> 0xe38062999cae5744
//   Random38/1/1: trades 0x084c597474eaa400 -> 0x13e4cce8783da942
//   Random38/1/0: trades 0x084c597474eaa400 -> 0x13e4cce8783da942
//   Random40/0/1: trades 0x76e2d6a7f41b161c -> 0x78c15a2f88a24807
//   Random40/0/0: trades 0x76e2d6a7f41b161c -> 0x78c15a2f88a24807
//   Random40/1/1: trades 0x9a84d463fe03ec91 -> 0x41975079a7cc855b
//   Random40/1/0: trades 0x9a84d463fe03ec91 -> 0x41975079a7cc855b
//   Random41/0/1: trades 0x4b1ec3cc1bd0909c -> 0xf6e95f1e1a0264bc
//   Random41/0/0: trades 0x4b1ec3cc1bd0909c -> 0xf6e95f1e1a0264bc
//   Random41/1/1: trades 0x4b1ec3cc1bd0909c -> 0xf6e95f1e1a0264bc
//   Random41/1/0: trades 0x4b1ec3cc1bd0909c -> 0xf6e95f1e1a0264bc
//   Random42/0/1: trades 0x75b6953551fe9575 -> 0xd4d5dd88f1bc7129
//   Random42/0/0: trades 0x75b6953551fe9575 -> 0xd4d5dd88f1bc7129
//   Random42/1/1: trades 0xf736fbadc411c223 -> 0x292b9553961fa163
//   Random42/1/0: trades 0xf736fbadc411c223 -> 0x292b9553961fa163
//   Random44/0/1: trades 0x513c7ae44c865ec8 -> 0x316199af1a335dbf
//   Random44/0/0: trades 0x513c7ae44c865ec8 -> 0x316199af1a335dbf
//   Random44/1/1: trades 0x1a79f44215a3981e -> 0x9f0e22f8fb2632f6
//   Random44/1/0: trades 0x1a79f44215a3981e -> 0x9f0e22f8fb2632f6
//   Random45/0/1: trades 0xf543670d7cc61817 -> 0xf7b0f07cda17c110
//   Random45/0/0: trades 0xf543670d7cc61817 -> 0xf7b0f07cda17c110
//   Random45/1/1: trades 0xa2aef1f38f464b4d -> 0x797aff1ec9239dff
//   Random45/1/0: trades 0xa2aef1f38f464b4d -> 0x797aff1ec9239dff
//   Random48/0/1: trades 0x62a7be3a970dee6a -> 0xda74ceb1a9e7d904, trade_count 9 -> 8
//   Random48/0/0: trades 0x62a7be3a970dee6a -> 0xda74ceb1a9e7d904, trade_count 9 -> 8
//   Random48/1/1: trades 0xee7c0d61bfb7dd62 -> 0x0876fbb01d747e2d, trade_count 9 -> 8
//   Random48/1/0: trades 0xee7c0d61bfb7dd62 -> 0x0876fbb01d747e2d, trade_count 9 -> 8
// R5 lane PAR-CASHFEE moves these runs' trade digests (INT26: the behaviour half
// of its hash step 214c082c, harvested on the integrated tree with this TU's own
// switch): a percent-of-equity default quantity under a cash commission sizes
// from strategy.equity net of the open entries' fees, less its own order's fee.
// Random05/0/* had been moved by an earlier pick too. Their hash values move once,
// in the INT26 v19 hash re-pin:
//   Random05/0/1: trades 0x3d4c3aae7bbc3b85 -> 0xf91857e72781f5ae
//   Random05/0/0: trades 0x3d4c3aae7bbc3b85 -> 0xf91857e72781f5ae
//   Random06/0/1: trades 0x19f4b2ac9f3f76f7 -> 0x75cd705050ee6ca5
//   Random06/0/0: trades 0x19f4b2ac9f3f76f7 -> 0x75cd705050ee6ca5
//   Random06/1/1: trades 0x3561041c80b6994f -> 0x5f84022dee0bd36f
//   Random06/1/0: trades 0x3561041c80b6994f -> 0x5f84022dee0bd36f
//   Random20/0/1: trades 0x3e0974d0ca2bfa0b -> 0x936a146fbbfb7d4b
//   Random20/0/0: trades 0x3e0974d0ca2bfa0b -> 0x936a146fbbfb7d4b
//   Random20/1/1: trades 0x1cd39188275caa5d -> 0x57a697ba56dd611e
//   Random20/1/0: trades 0x1cd39188275caa5d -> 0x57a697ba56dd611e
//   Random35/0/1: trades 0x2e59ea5f57457b34 -> 0x834764512474d77c
//   Random35/0/0: trades 0x2e59ea5f57457b34 -> 0x834764512474d77c
//   Random35/1/1: trades 0xf9dd1b1a0c3d9f6a -> 0xce2ab725ccda6ebd
//   Random35/1/0: trades 0xf9dd1b1a0c3d9f6a -> 0xce2ab725ccda6ebd
//   Random38/0/1: trades 0xe38062999cae5744 -> 0xac12ee6aa53f424c
//   Random38/0/0: trades 0xe38062999cae5744 -> 0xac12ee6aa53f424c
//   Random38/1/1: trades 0x13e4cce8783da942 -> 0xc6a0970cf0716d7c
//   Random38/1/0: trades 0x13e4cce8783da942 -> 0xc6a0970cf0716d7c
//   Random46/0/1: trades 0x8fe7667934a81f87 -> 0xfaba8060a07d20fd
//   Random46/0/0: trades 0x8fe7667934a81f87 -> 0xfaba8060a07d20fd
//   Random46/1/1: trades 0x8fe7667934a81f87 -> 0xfaba8060a07d20fd
//   Random46/1/0: trades 0x8fe7667934a81f87 -> 0xfaba8060a07d20fd
// expectation corrected (INT26 v19 hash re-pin, 680 values over 272 rows), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane H-THIN's hash step (the Pine host's excursion model leaves the source-layer fold, E19; a FIFO exit reserves its own entry's quantity, P10), R5 lane PAR-ORDERS-2's new transient coof_fill_forced_ fold and R5 lane PAR-MARGIN-2's post-fill margin waypoint and R5 lane PAR-CASHFEE's hash step (the fold folds the sizing snapshot's strategy.equity where a percent-of-equity quantity under a cash fee is recorded); old values are the pins every pick since PAR-ORDERS' re-pin (1518fa8f) kept, harvested with this TU's own switch on the integrated tree and at every pick boundary; this host projects one fixed execution hash, so the continuation does not enter; the trades, the trade digest and the three counts did not move:
//   Quiet/0/1: boundary 0x1c2b2e37a44f6877ull -> 0xcd8d3ecb4d2c740aull, rows 0x81799b58f0e052b1ull -> 0xe6da4342a82c460cull, final_hash 0xe0aa4c0240939bcfull -> 0x458f1ecf00ef1d5cull [H-THIN]
//   Quiet/0/0: boundary 0x1c2b2e37a44f6877ull -> 0xcd8d3ecb4d2c740aull, final_hash 0xe0aa4c0240939bcfull -> 0x458f1ecf00ef1d5cull [H-THIN]
//   Quiet/1/1: boundary 0x0a542471817c0616ull -> 0x2a102759d268d84dull, rows 0x4210b9ca5a7d77e6ull -> 0x923a578d640d7fdbull, final_hash 0x2e91db500a83cbe9ull -> 0x6ca064a309be6bfaull [H-THIN]
//   Quiet/1/0: boundary 0x0a542471817c0616ull -> 0x2a102759d268d84dull, final_hash 0x2e91db500a83cbe9ull -> 0x6ca064a309be6bfaull [H-THIN]
//   HoldLongFull/0/1: boundary 0xbb82b8cd85ffaae1ull -> 0x2be6111eb08aff44ull, rows 0xae96abef4356f11full -> 0x37135fdf24860586ull, final_hash 0xbe87239166611559ull -> 0x6caa6506b43c5228ull [H-THIN]
//   HoldLongFull/0/0: boundary 0xbb82b8cd85ffaae1ull -> 0x2be6111eb08aff44ull, final_hash 0xbe87239166611559ull -> 0x6caa6506b43c5228ull [H-THIN]
//   HoldLongFull/1/1: boundary 0x83f82e6a0b0a5677ull -> 0x6da42e25a0445e57ull, rows 0x7700c6fdca38234full -> 0xa90a9669249de5f2ull, final_hash 0xb6f097b6a6318731ull -> 0x1fdb374b13f0dd9cull [H-THIN]
//   HoldLongFull/1/0: boundary 0x83f82e6a0b0a5677ull -> 0x6da42e25a0445e57ull, final_hash 0xb6f097b6a6318731ull -> 0x1fdb374b13f0dd9cull [H-THIN]
//   MarginCalls/0/1: boundary 0x3633b5fd46e3ee1eull -> 0x9ffada7afff423a5ull, rows 0x35e47773037c0f4dull -> 0xf1451bf046aea2aeull, final_hash 0x64d4054219e25f2full -> 0xd58658d95c54acbeull [H-THIN]
//   MarginCalls/0/0: boundary 0x3633b5fd46e3ee1eull -> 0x9ffada7afff423a5ull, final_hash 0x64d4054219e25f2full -> 0xd58658d95c54acbeull [H-THIN]
//   MarginCalls/1/1: boundary 0x96ca6ef83164c685ull -> 0x548dd443b38f74b0ull, rows 0x4b1bc134021b04cbull -> 0xfdd852b37b07d408ull, final_hash 0xc10c1fc9e88e7c23ull -> 0x76f99b20ecd7e68cull [H-THIN]
//   MarginCalls/1/0: boundary 0x96ca6ef83164c685ull -> 0x548dd443b38f74b0ull, final_hash 0xc10c1fc9e88e7c23ull -> 0x76f99b20ecd7e68cull [H-THIN]
//   MarginCallCycles/0/1: boundary 0x38656d5e12929625ull -> 0x53630c222ae9b1a8ull, rows 0x5c4aa443e8e76ef9ull -> 0x26e80eef46f3b822ull, final_hash 0x55f80f5340e670b7ull -> 0x9dea3ea1d3cbc012ull [H-THIN]
//   MarginCallCycles/0/0: boundary 0x38656d5e12929625ull -> 0x53630c222ae9b1a8ull, final_hash 0x55f80f5340e670b7ull -> 0x9dea3ea1d3cbc012ull [H-THIN]
//   MarginCallCycles/1/1: boundary 0xc969906a8f7b57bbull -> 0xd66af1875ed2e747ull, rows 0x67e6e63eb71a661cull -> 0xffa2470471097d1dull, final_hash 0x8e9e930063b52698ull -> 0x22ec1fab2742fa75ull [H-THIN]
//   MarginCallCycles/1/0: boundary 0xc969906a8f7b57bbull -> 0xd66af1875ed2e747ull, final_hash 0x8e9e930063b52698ull -> 0x22ec1fab2742fa75ull [H-THIN]
//   Bracket/0/1: boundary 0x5ceffdf162a625baull -> 0xaeddc0d29a30f3f7ull, rows 0x2c0b9b25e51c3426ull -> 0xd324958e0a8a53deull, final_hash 0xb62be9f335a690fbull -> 0xef163ce5ad81ccceull [H-THIN]
//   Bracket/0/0: boundary 0x5ceffdf162a625baull -> 0xaeddc0d29a30f3f7ull, final_hash 0xb62be9f335a690fbull -> 0xef163ce5ad81ccceull [H-THIN]
//   Bracket/1/1: boundary 0x53093fd52be58e6aull -> 0xdb519033b2a6887bull, rows 0x26c6fc455e7a9e3full -> 0xb2b3be51c614016dull, final_hash 0x2fbb7d2d1228c959ull -> 0x488b102fb534087cull [H-THIN]
//   Bracket/1/0: boundary 0x53093fd52be58e6aull -> 0xdb519033b2a6887bull, final_hash 0x2fbb7d2d1228c959ull -> 0x488b102fb534087cull [H-THIN]
//   Trail/0/1: boundary 0xa09d959161ab83b5ull -> 0xa901a158196e8e90ull, rows 0x6db293f4d9ba407eull -> 0x3bf9ee1972d44776ull, final_hash 0xf9e526fb6a079950ull -> 0xf1d8827b0b6f8fcfull [H-THIN]
//   Trail/0/0: boundary 0xa09d959161ab83b5ull -> 0xa901a158196e8e90ull, final_hash 0xf9e526fb6a079950ull -> 0xf1d8827b0b6f8fcfull [H-THIN]
//   Trail/1/1: boundary 0x2a39cc8c834a5287ull -> 0xb23147b5951428e2ull, rows 0xe23d175bdbe6d254ull -> 0x4f409e92b7a0d98aull, final_hash 0xffac0df8a2dbc19eull -> 0x2716f35c358b7145ull [H-THIN]
//   Trail/1/0: boundary 0x2a39cc8c834a5287ull -> 0xb23147b5951428e2ull, final_hash 0xffac0df8a2dbc19eull -> 0x2716f35c358b7145ull [H-THIN]
//   DualStops/0/1: boundary 0xf53e1aa110098577ull -> 0x29701bba6d7182b5ull, rows 0xa45979a387e09947ull -> 0xf109235082c1860eull, final_hash 0x9d20a3a0e3c4b991ull -> 0x34452d46a85f96c6ull [H-THIN]
//   DualStops/0/0: boundary 0xf53e1aa110098577ull -> 0x29701bba6d7182b5ull, final_hash 0x9d20a3a0e3c4b991ull -> 0x34452d46a85f96c6ull [H-THIN]
//   DualStops/1/1: boundary 0xd56451041c2c2c4full -> 0x86f9654dc6f8ed22ull, rows 0x3525fdbfa98d36bcull -> 0x4393ede3dede0436ull, final_hash 0x0e2c1cdfb96b6301ull -> 0x1bfce4a56d0314f2ull [H-THIN]
//   DualStops/1/0: boundary 0xd56451041c2c2c4full -> 0x86f9654dc6f8ed22ull, final_hash 0x0e2c1cdfb96b6301ull -> 0x1bfce4a56d0314f2ull [H-THIN]
//   InvertedStops/0/1: boundary 0xa3daa491b43f22e9ull -> 0xeb8c3dfbd9d273a2ull, rows 0x054c4b8ef1709dc3ull -> 0xb7ecf902b2ff0befull, final_hash 0x056329caffb5a22eull -> 0x1993f824c35e154dull [H-THIN]
//   InvertedStops/0/0: boundary 0xa3daa491b43f22e9ull -> 0xeb8c3dfbd9d273a2ull, final_hash 0x056329caffb5a22eull -> 0x1993f824c35e154dull [H-THIN]
//   InvertedStops/1/1: boundary 0x9b50fac734e90657ull -> 0x976cbd72b16fcb6full, rows 0x2d65179a434eebf0ull -> 0xe49f3d6c9fe39f5bull, final_hash 0xab655e08a941e123ull -> 0x69e468caa11329ccull [H-THIN]
//   InvertedStops/1/0: boundary 0x9b50fac734e90657ull -> 0x976cbd72b16fcb6full, final_hash 0xab655e08a941e123ull -> 0x69e468caa11329ccull [H-THIN]
//   PendingParent/0/1: boundary 0x47212775031c61c2ull -> 0xc2b9247123313aceull, rows 0xfd5dbde93e8d520aull -> 0xa8f42cb39e1aeff3ull, final_hash 0xe74974df53ef097aull -> 0xfbf54246d7baccdaull [H-THIN]
//   PendingParent/0/0: boundary 0x47212775031c61c2ull -> 0xc2b9247123313aceull, final_hash 0xe74974df53ef097aull -> 0xfbf54246d7baccdaull [H-THIN]
//   PendingParent/1/1: boundary 0x08f4fa130f86a0daull -> 0xcb795828e625815full, rows 0xbd725b4c1d8be787ull -> 0x808c94697e2231bdull, final_hash 0x27cdc3dedee10a56ull -> 0x0f9d2bb1d6492d5aull [H-THIN]
//   PendingParent/1/0: boundary 0x08f4fa130f86a0daull -> 0xcb795828e625815full, final_hash 0x27cdc3dedee10a56ull -> 0x0f9d2bb1d6492d5aull [H-THIN]
//   GappedAdd/0/1: boundary 0x8bd71987fffc51e9ull -> 0x2aa05060cf893d51ull, rows 0x42f9dd5f352155d8ull -> 0xca15382fd8d71504ull, final_hash 0xaa98c069e5a019a7ull -> 0x53a5ed59712f1502ull [H-THIN]
//   GappedAdd/0/0: boundary 0x8bd71987fffc51e9ull -> 0x2aa05060cf893d51ull, final_hash 0xaa98c069e5a019a7ull -> 0x53a5ed59712f1502ull [H-THIN]
//   GappedAdd/1/1: boundary 0x7b93202bd79c1393ull -> 0x1de2b53c2e69ae04ull, rows 0xa4421baf09234dd7ull -> 0xb77b870b1ef756a3ull, final_hash 0xe086e35f4f128b46ull -> 0xc471e2ce0ce5b5b7ull [H-THIN]
//   GappedAdd/1/0: boundary 0x7b93202bd79c1393ull -> 0x1de2b53c2e69ae04ull, final_hash 0xe086e35f4f128b46ull -> 0xc471e2ce0ce5b5b7ull [H-THIN]
//   DeclinedReversal/0/1: boundary 0xfcb9145b39ec7b5aull -> 0x5dc519d25cd8e456ull, rows 0xbbddf0514f17aef7ull -> 0xcd770d65bdeb5e28ull, final_hash 0x764450acb0596bfcull -> 0xacfb010f8ef2c1dbull [H-THIN]
//   DeclinedReversal/0/0: boundary 0xfcb9145b39ec7b5aull -> 0x5dc519d25cd8e456ull, final_hash 0x764450acb0596bfcull -> 0xacfb010f8ef2c1dbull [H-THIN]
//   DeclinedReversal/1/1: boundary 0x4a1378eba012ef40ull -> 0x506c2023d62a4e6full, rows 0x16ea86de895d5b40ull -> 0xe0eb2dae405533f4ull, final_hash 0x14734f0e113371e4ull -> 0x980a839868ba5b97ull [H-THIN]
//   DeclinedReversal/1/0: boundary 0x4a1378eba012ef40ull -> 0x506c2023d62a4e6full, final_hash 0x14734f0e113371e4ull -> 0x980a839868ba5b97ull [H-THIN]
//   CalcOnFills/0/1: boundary 0x7641907936993779ull -> 0xbd9feba11a355c84ull, rows 0x3777fe6ece242880ull -> 0xbd62a54cd2c27075ull, final_hash 0x2583ce2bc6a3c1d9ull -> 0x8e716ca55979f5e4ull [PAR-ORDERS-2, H-THIN]
//   CalcOnFills/0/0: boundary 0x7641907936993779ull -> 0xbd9feba11a355c84ull, final_hash 0x2583ce2bc6a3c1d9ull -> 0x8e716ca55979f5e4ull [PAR-ORDERS-2, H-THIN]
//   CalcOnFills/1/1: boundary 0xd120e8fefb0dc236ull -> 0x0e2860d938b1d36dull, rows 0xdbb28de374d30bc3ull -> 0xfe826221acbd42c3ull, final_hash 0xad6e26cf69199431ull -> 0x1ae91d50d6762fd0ull [PAR-ORDERS-2, H-THIN]
//   CalcOnFills/1/0: boundary 0xd120e8fefb0dc236ull -> 0x0e2860d938b1d36dull, final_hash 0xad6e26cf69199431ull -> 0x1ae91d50d6762fd0ull [PAR-ORDERS-2, H-THIN]
//   CloseOrders/0/1: boundary 0x78ff3352f7085436ull -> 0xb27e9cfcbfee41d3ull, rows 0x896bcdcf15c8a4c6ull -> 0x0033c4a6a644f0ddull, final_hash 0x8e67de695bfb2c60ull -> 0xbedbc1183b7526a1ull [H-THIN]
//   CloseOrders/0/0: boundary 0x78ff3352f7085436ull -> 0xb27e9cfcbfee41d3ull, final_hash 0x8e67de695bfb2c60ull -> 0xbedbc1183b7526a1ull [H-THIN]
//   CloseOrders/1/1: boundary 0xe00ae8be3caf3315ull -> 0xff96b9591f72f309ull, rows 0x2584350c36f68309ull -> 0x5aaa3b2f562a501dull, final_hash 0xa998448999ff9b1eull -> 0xc542c7fb8bb5559dull [H-THIN]
//   CloseOrders/1/0: boundary 0xe00ae8be3caf3315ull -> 0xff96b9591f72f309ull, final_hash 0xa998448999ff9b1eull -> 0xc542c7fb8bb5559dull [H-THIN]
//   CloseOrdersSlipped/0/1: boundary 0x58e9160429c03bcaull -> 0xc2834ef9bd74b55full, rows 0xd3632e32e35ef626ull -> 0xb1042b50a92de9a0ull, final_hash 0x13f82da0279b5836ull -> 0x6c12422a13b3f077ull [H-THIN]
//   CloseOrdersSlipped/0/0: boundary 0x58e9160429c03bcaull -> 0xc2834ef9bd74b55full, final_hash 0x13f82da0279b5836ull -> 0x6c12422a13b3f077ull [H-THIN]
//   CloseOrdersSlipped/1/1: boundary 0x3507072a3e435b83ull -> 0x71906fcd647a169full, rows 0xc5d77a9b36fdec85ull -> 0xa2710dbb1c8a57edull, final_hash 0xc7b132391c547427ull -> 0x0330185649c756c2ull [H-THIN]
//   CloseOrdersSlipped/1/0: boundary 0x3507072a3e435b83ull -> 0x71906fcd647a169full, final_hash 0xc7b132391c547427ull -> 0x0330185649c756c2ull [H-THIN]
//   IntradayLoss/0/1: boundary 0x644e48072a025ba8ull -> 0x8a9bd5dd1429352eull, rows 0x0421711a90fdf429ull -> 0x616b96966292e8adull, final_hash 0xecca5625486ab2e1ull -> 0xc14ffe9c379fbee2ull [H-THIN]
//   IntradayLoss/0/0: boundary 0x644e48072a025ba8ull -> 0x8a9bd5dd1429352eull, final_hash 0xecca5625486ab2e1ull -> 0xc14ffe9c379fbee2ull [H-THIN]
//   IntradayLoss/1/1: boundary 0xce3032bd544d7840ull -> 0x1a92d1a03b233c6cull, rows 0x4e9d2d333bd5f470ull -> 0x3382648892a694c4ull, final_hash 0x4e071c66b57dfeaeull -> 0x66f4f2fe8b455019ull [H-THIN]
//   IntradayLoss/1/0: boundary 0xce3032bd544d7840ull -> 0x1a92d1a03b233c6cull, final_hash 0x4e071c66b57dfeaeull -> 0x66f4f2fe8b455019ull [H-THIN]
//   IntradayCapDeferred/0/1: boundary 0x93a814e5f6e8feb7ull -> 0xde19edf4087b930dull, rows 0xbbbb2712dbbd9ac8ull -> 0x560de539b138ea41ull, final_hash 0x77c142bb11eab624ull -> 0x7730e7fe703663edull [H-THIN]
//   IntradayCapDeferred/0/0: boundary 0x93a814e5f6e8feb7ull -> 0xde19edf4087b930dull, final_hash 0x77c142bb11eab624ull -> 0x7730e7fe703663edull [H-THIN]
//   IntradayCapDeferred/1/1: boundary 0xe23e69831f88d404ull -> 0x293bdcc491ae22d7ull, rows 0x63469b00b6747db8ull -> 0xd165d92830916a1cull, final_hash 0x72de8fef40c4c89bull -> 0x7ba4fe8defa15d4aull [H-THIN]
//   IntradayCapDeferred/1/0: boundary 0xe23e69831f88d404ull -> 0x293bdcc491ae22d7ull, final_hash 0x72de8fef40c4c89bull -> 0x7ba4fe8defa15d4aull [H-THIN]
//   PreopenSlice/0/1: boundary 0x9f401876dd941c1full -> 0xcbc8fb1ba9d83481ull, rows 0xbbce87ebf0d3f638ull -> 0xbe57d9275ae9e2bbull, final_hash 0xf73c6812e8dd0f92ull -> 0x62f97adf60eb373full [H-THIN]
//   PreopenSlice/0/0: boundary 0x9f401876dd941c1full -> 0xcbc8fb1ba9d83481ull, final_hash 0xf73c6812e8dd0f92ull -> 0x62f97adf60eb373full [H-THIN]
//   PreopenSlice/1/1: boundary 0x98345cf423b84f80ull -> 0xadcfe58d853cedfcull, rows 0x36db489e43fd8218ull -> 0xecc971145edced41ull, final_hash 0xa5916c1cffd7bae0ull -> 0x94fff9b38c7f1bd5ull [H-THIN]
//   PreopenSlice/1/0: boundary 0x98345cf423b84f80ull -> 0xadcfe58d853cedfcull, final_hash 0xa5916c1cffd7bae0ull -> 0x94fff9b38c7f1bd5ull [H-THIN]
//   SameBarBatch/0/1: boundary 0xcfa0ce2dfed4b2e4ull -> 0x85b57cf2c67fbdd6ull, rows 0x7de7b618b505f51eull -> 0xdf711eb77d489011ull, final_hash 0x0bb34499121582afull -> 0x77819f662ca36048ull [H-THIN]
//   SameBarBatch/0/0: boundary 0xcfa0ce2dfed4b2e4ull -> 0x85b57cf2c67fbdd6ull, final_hash 0x0bb34499121582afull -> 0x77819f662ca36048ull [H-THIN]
//   SameBarBatch/1/1: boundary 0x4557f9683348bf23ull -> 0x5d280dcf47a7af5bull, rows 0x79d43d1295ddc801ull -> 0x987f9a54f93d934eull, final_hash 0x26a5e7a6d5104dbcull -> 0x6a0ac704a36bef57ull [H-THIN]
//   SameBarBatch/1/0: boundary 0x4557f9683348bf23ull -> 0x5d280dcf47a7af5bull, final_hash 0x26a5e7a6d5104dbcull -> 0x6a0ac704a36bef57ull [H-THIN]
//   CommissionedShort/0/1: boundary 0x18b476bd2a3db96dull -> 0xc73e33b30bf09c29ull, rows 0x54b6ba2e7a2e11c2ull -> 0x26a3e6d04263ad27ull, final_hash 0x48a9193b2c1c1118ull -> 0xeb0d9ea9f0bdfd4bull [H-THIN]
//   CommissionedShort/0/0: boundary 0x18b476bd2a3db96dull -> 0xc73e33b30bf09c29ull, final_hash 0x48a9193b2c1c1118ull -> 0xeb0d9ea9f0bdfd4bull [H-THIN]
//   CommissionedShort/1/1: boundary 0xb24ff7e17f80c347ull -> 0x350b092698bdf8fdull, rows 0x20608b72a057ada4ull -> 0x3fe25a6001c6c4feull, final_hash 0x3ef1eaec89e6430dull -> 0xd73d8e053dbf5cd2ull [H-THIN]
//   CommissionedShort/1/0: boundary 0xb24ff7e17f80c347ull -> 0x350b092698bdf8fdull, final_hash 0x3ef1eaec89e6430dull -> 0xd73d8e053dbf5cd2ull [H-THIN]
//   AccountFx/0/1: boundary 0xaeec7af5859fe6b3ull -> 0xd27340484425ff52ull, rows 0x37039a16e8c77a7bull -> 0x4e596d61c552597eull, final_hash 0x2a28b4e61cd5cbbcull -> 0xea884b09a27ea73bull [H-THIN]
//   AccountFx/0/0: boundary 0xaeec7af5859fe6b3ull -> 0xd27340484425ff52ull, final_hash 0x2a28b4e61cd5cbbcull -> 0xea884b09a27ea73bull [H-THIN]
//   Stream/0/1: boundary 0x49574a22705f52c0ull -> 0x4f42bff80af50865ull, rows 0x3cade1c0a6b65b75ull -> 0xca31c77c5a63ae01ull, final_hash 0x8c9bb935bf0c32daull -> 0xeb75594695696b2full [H-THIN]
//   Stream/0/0: boundary 0x49574a22705f52c0ull -> 0x4f42bff80af50865ull, final_hash 0x8c9bb935bf0c32daull -> 0xeb75594695696b2full [H-THIN]
//   Random01/0/1: boundary 0xfa3864e9f5f3a619ull -> 0xe7fdfddc42a2ae75ull, rows 0x22da84303f34b121ull -> 0x0b38785d82bc8fe6ull, final_hash 0xa142013345fb25ddull -> 0x16fe00b5a88e8ff2ull [H-THIN]
//   Random01/0/0: boundary 0xfa3864e9f5f3a619ull -> 0xe7fdfddc42a2ae75ull, final_hash 0xa142013345fb25ddull -> 0x16fe00b5a88e8ff2ull [H-THIN]
//   Random01/1/1: boundary 0xfdafd368c22a3bb4ull -> 0x64e7087d99c04c46ull, rows 0x30ace005c5ea2e1bull -> 0x1c0e119f02f64ddeull, final_hash 0xb44a05ed46f5109dull -> 0xb0114372fd93a01cull [H-THIN]
//   Random01/1/0: boundary 0xfdafd368c22a3bb4ull -> 0x64e7087d99c04c46ull, final_hash 0xb44a05ed46f5109dull -> 0xb0114372fd93a01cull [H-THIN]
//   Random02/0/1: boundary 0xf933c58464603966ull -> 0x8a2b805556d22659ull, rows 0x8549ba7d7e735482ull -> 0xce086fbf3b8296e5ull, final_hash 0xd9870babe5d18626ull -> 0x5221fce916020c33ull [PAR-ORDERS-2, H-THIN]
//   Random02/0/0: boundary 0xf933c58464603966ull -> 0x8a2b805556d22659ull, final_hash 0xd9870babe5d18626ull -> 0x5221fce916020c33ull [PAR-ORDERS-2, H-THIN]
//   Random02/1/1: boundary 0x25ba5f1040df3b56ull -> 0x844819f3aaccd232ull, rows 0x1593a8e96a0c90a9ull -> 0x74fc5a6e6a256e36ull, final_hash 0x0fb50af38c25c54dull -> 0x089086fe02b53049ull [PAR-ORDERS-2, H-THIN]
//   Random02/1/0: boundary 0x25ba5f1040df3b56ull -> 0x844819f3aaccd232ull, final_hash 0x0fb50af38c25c54dull -> 0x089086fe02b53049ull [PAR-ORDERS-2, H-THIN]
//   Random03/0/1: boundary 0xb5a3f885ba0143fdull -> 0x2dad9d24e763c932ull, rows 0xd91b7712cc62209bull -> 0xa3f6fa59cf96ba52ull, final_hash 0xd93ef819aff693f3ull -> 0xeb202b23d902f8b8ull [H-THIN]
//   Random03/0/0: boundary 0xb5a3f885ba0143fdull -> 0x2dad9d24e763c932ull, final_hash 0xd93ef819aff693f3ull -> 0xeb202b23d902f8b8ull [H-THIN]
//   Random03/1/1: boundary 0x36230659e7bf570eull -> 0x7196371dde7bb130ull, rows 0xdb83075a5690e996ull -> 0xbdf7be966e72b156ull, final_hash 0x23acbc0041308125ull -> 0x92f95865ffb213eeull [H-THIN]
//   Random03/1/0: boundary 0x36230659e7bf570eull -> 0x7196371dde7bb130ull, final_hash 0x23acbc0041308125ull -> 0x92f95865ffb213eeull [H-THIN]
//   Random04/0/1: boundary 0x0c41be29e4497a7cull -> 0x9d74676f534d8b76ull, rows 0x1fdaabb2a1c312cdull -> 0xdb1c5eb0cb9f6c32ull, final_hash 0x542f1b01ef32d535ull -> 0x82a419ac37909af3ull [H-THIN]
//   Random04/0/0: boundary 0x0c41be29e4497a7cull -> 0x9d74676f534d8b76ull, final_hash 0x542f1b01ef32d535ull -> 0x82a419ac37909af3ull [H-THIN]
//   Random04/1/1: boundary 0x93ade030662e0b77ull -> 0x19cdc09e2267df5aull, rows 0x5beecc5dc910feb8ull -> 0x7eca887896ce161bull, final_hash 0x254f4f4933be69e0ull -> 0x350e10b2e19f0b44ull [H-THIN]
//   Random04/1/0: boundary 0x93ade030662e0b77ull -> 0x19cdc09e2267df5aull, final_hash 0x254f4f4933be69e0ull -> 0x350e10b2e19f0b44ull [H-THIN]
//   Random05/0/1: boundary 0x3e8e6495fe2e6dc1ull -> 0xac397a37be0d4789ull, rows 0x2703dbc59d3516a6ull -> 0x64b7bb57706476b6ull, final_hash 0xb72557b7f6894eafull -> 0x85274de8bea42d05ull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random05/0/0: boundary 0x3e8e6495fe2e6dc1ull -> 0xac397a37be0d4789ull, final_hash 0xb72557b7f6894eafull -> 0x85274de8bea42d05ull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random05/1/1: boundary 0x4be132773d14c0e0ull -> 0xbd96a4fdd0383edbull, rows 0xa6803dead66d3343ull -> 0xdcb784e78fba749eull, final_hash 0xb3561169b361d8bfull -> 0x919116731d265d3dull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random05/1/0: boundary 0x4be132773d14c0e0ull -> 0xbd96a4fdd0383edbull, final_hash 0xb3561169b361d8bfull -> 0x919116731d265d3dull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random06/0/1: boundary 0xecb44da2cbdb5c42ull -> 0x7deef79bd0217f64ull, rows 0x5485171eedfc86cfull -> 0xd166805cc595f38eull, final_hash 0xff2efe1925e45a9dull -> 0x4c7c821f90e38cccull [H-THIN, PAR-CASHFEE]
//   Random06/0/0: boundary 0xecb44da2cbdb5c42ull -> 0x7deef79bd0217f64ull, final_hash 0xff2efe1925e45a9dull -> 0x4c7c821f90e38cccull [H-THIN, PAR-CASHFEE]
//   Random06/1/1: boundary 0x65cc03efd59ec6b5ull -> 0x8a1bb21a5e64fed9ull, rows 0x2dce2153546130dbull -> 0x7d881d1a93512848ull, final_hash 0x1130940f86f108d2ull -> 0xaa8716ffbdfd11b2ull [H-THIN, PAR-CASHFEE]
//   Random06/1/0: boundary 0x65cc03efd59ec6b5ull -> 0x8a1bb21a5e64fed9ull, final_hash 0x1130940f86f108d2ull -> 0xaa8716ffbdfd11b2ull [H-THIN, PAR-CASHFEE]
//   Random07/0/1: boundary 0x7bf47df91573ab9cull -> 0xe6120913fe5da50dull, rows 0x3c1feb47a4d6d81eull -> 0xe90b62d77e4625b7ull, final_hash 0x438a828933e58c5bull -> 0xbc6f67df5b0f0b2full [H-THIN]
//   Random07/0/0: boundary 0x7bf47df91573ab9cull -> 0xe6120913fe5da50dull, final_hash 0x438a828933e58c5bull -> 0xbc6f67df5b0f0b2full [H-THIN]
//   Random07/1/1: boundary 0x29fcc776be13bdf5ull -> 0xe2c3492a3155bf7dull, rows 0x9a5ab1b2b00efff1ull -> 0x0e5f1aa6cabc3a32ull, final_hash 0x353c5f0e5bc1c22bull -> 0xbad1ae57e202b2d0ull [H-THIN]
//   Random07/1/0: boundary 0x29fcc776be13bdf5ull -> 0xe2c3492a3155bf7dull, final_hash 0x353c5f0e5bc1c22bull -> 0xbad1ae57e202b2d0ull [H-THIN]
//   Random08/0/1: boundary 0xc2f66d9e5f4943c7ull -> 0x0a3af182d33c479aull, rows 0xf4a38750150093e6ull -> 0xc91b237d10b90ecbull, final_hash 0x9ce7eb6ee2b122baull -> 0x1930dc38f4f605fbull [PAR-ORDERS-2, H-THIN]
//   Random08/0/0: boundary 0xc2f66d9e5f4943c7ull -> 0x0a3af182d33c479aull, final_hash 0x9ce7eb6ee2b122baull -> 0x1930dc38f4f605fbull [PAR-ORDERS-2, H-THIN]
//   Random08/1/1: boundary 0xbc7e1d017519ee12ull -> 0xc981400f75b65614ull, rows 0xc18ef29ec0f4fa8eull -> 0xb37d8f41ed349758ull, final_hash 0xbd83191789f76873ull -> 0xe48afc5e8a67e872ull [PAR-ORDERS-2, H-THIN]
//   Random08/1/0: boundary 0xbc7e1d017519ee12ull -> 0xc981400f75b65614ull, final_hash 0xbd83191789f76873ull -> 0xe48afc5e8a67e872ull [PAR-ORDERS-2, H-THIN]
//   Random09/0/1: boundary 0x400c46d70e6e84bcull -> 0xaaff8dcd840f16ceull, rows 0xc58cc1d4b5bd3580ull -> 0x9021538022cf9b9bull, final_hash 0x1f6cdd2e111e016aull -> 0xbbe3c07366b0cbeaull [H-THIN]
//   Random09/0/0: boundary 0x400c46d70e6e84bcull -> 0xaaff8dcd840f16ceull, final_hash 0x1f6cdd2e111e016aull -> 0xbbe3c07366b0cbeaull [H-THIN]
//   Random09/1/1: boundary 0x32d4b35f029bad56ull -> 0x026e829145cad6f5ull, rows 0x447742874dde46e0ull -> 0x32ca6bae685cea43ull, final_hash 0x3d99e7abdd92a773ull -> 0xc55c597c6508d557ull [H-THIN]
//   Random09/1/0: boundary 0x32d4b35f029bad56ull -> 0x026e829145cad6f5ull, final_hash 0x3d99e7abdd92a773ull -> 0xc55c597c6508d557ull [H-THIN]
//   Random10/0/1: boundary 0x998ee74ea73334c6ull -> 0x82933908492fe6aeull, rows 0x5a3104007433abc0ull -> 0x839a10fa6478c692ull, final_hash 0x0a7888fb9b890fb4ull -> 0x6907e89e2f502bd7ull [PAR-ORDERS-2, H-THIN]
//   Random10/0/0: boundary 0x998ee74ea73334c6ull -> 0x82933908492fe6aeull, final_hash 0x0a7888fb9b890fb4ull -> 0x6907e89e2f502bd7ull [PAR-ORDERS-2, H-THIN]
//   Random10/1/1: boundary 0xde7dbd52ac33a7ffull -> 0x3af283057bf553afull, rows 0xaf8f0ca763b06949ull -> 0xd948d5022e691361ull, final_hash 0xfbe70c357ac296cfull -> 0x794b361143554e09ull [PAR-ORDERS-2, H-THIN]
//   Random10/1/0: boundary 0xde7dbd52ac33a7ffull -> 0x3af283057bf553afull, final_hash 0xfbe70c357ac296cfull -> 0x794b361143554e09ull [PAR-ORDERS-2, H-THIN]
//   Random11/0/1: boundary 0x805038d1cb0a3848ull -> 0x9f99f3ad7d35ba23ull, rows 0xebc3fde6db4a37f3ull -> 0x29a35fd6705cc48aull, final_hash 0x27bc15c5c142c728ull -> 0x8235cad2a6dfb3f3ull [H-THIN]
//   Random11/0/0: boundary 0x805038d1cb0a3848ull -> 0x9f99f3ad7d35ba23ull, final_hash 0x27bc15c5c142c728ull -> 0x8235cad2a6dfb3f3ull [H-THIN]
//   Random11/1/1: boundary 0x4a739854a58a3db9ull -> 0xf6fb4588ed987e13ull, rows 0xc91f1e4519586a06ull -> 0x7dcd8b5314fb7562ull, final_hash 0x8af717fd3fa04097ull -> 0xb44ac10707d01de6ull [H-THIN]
//   Random11/1/0: boundary 0x4a739854a58a3db9ull -> 0xf6fb4588ed987e13ull, final_hash 0x8af717fd3fa04097ull -> 0xb44ac10707d01de6ull [H-THIN]
//   Random12/0/1: boundary 0xb9a9bb3fafab7349ull -> 0xc02247ff66b0dc87ull, rows 0xe93fb7b821f6a7feull -> 0xf39477bfa32bb689ull, final_hash 0x29c94b5df165789aull -> 0x5f0b7200f4ecdfe1ull [PAR-ORDERS-2, H-THIN]
//   Random12/0/0: boundary 0xb9a9bb3fafab7349ull -> 0xc02247ff66b0dc87ull, final_hash 0x29c94b5df165789aull -> 0x5f0b7200f4ecdfe1ull [PAR-ORDERS-2, H-THIN]
//   Random12/1/1: boundary 0x298ab491e3caa61full -> 0xfcd62d0327e0553aull, rows 0xf8e78400f1c148acull -> 0xf36949e9fad408efull, final_hash 0x927c4394206b42b2ull -> 0x84a69af72aae96d9ull [PAR-ORDERS-2, H-THIN]
//   Random12/1/0: boundary 0x298ab491e3caa61full -> 0xfcd62d0327e0553aull, final_hash 0x927c4394206b42b2ull -> 0x84a69af72aae96d9ull [PAR-ORDERS-2, H-THIN]
//   Random13/0/1: boundary 0x39caae56088c1db8ull -> 0x53be6a77e760fe0bull, rows 0x4028b545fb2c3ad9ull -> 0xe1b3dac90adb9f4bull, final_hash 0xc19e5f5bbefe0e7full -> 0xafd98319ef581f61ull [H-THIN]
//   Random13/0/0: boundary 0x39caae56088c1db8ull -> 0x53be6a77e760fe0bull, final_hash 0xc19e5f5bbefe0e7full -> 0xafd98319ef581f61ull [H-THIN]
//   Random13/1/1: boundary 0x07b714c492b5ae85ull -> 0xd238668589210027ull, rows 0xa03b3f8bed95186dull -> 0xc4b7dac2b95e01fbull, final_hash 0xa9f1d1a463925b52ull -> 0xb4b9d6f51a69342full [H-THIN]
//   Random13/1/0: boundary 0x07b714c492b5ae85ull -> 0xd238668589210027ull, final_hash 0xa9f1d1a463925b52ull -> 0xb4b9d6f51a69342full [H-THIN]
//   Random14/0/1: boundary 0x21085d70db4c336bull -> 0xd37abe66ab158e44ull, rows 0x394ebfbc2c15522aull -> 0x38cb950f5ceb9d9cull, final_hash 0xad1f71b1afdd37f8ull -> 0x9d488869e9756d89ull [H-THIN]
//   Random14/0/0: boundary 0x21085d70db4c336bull -> 0xd37abe66ab158e44ull, final_hash 0xad1f71b1afdd37f8ull -> 0x9d488869e9756d89ull [H-THIN]
//   Random14/1/1: boundary 0x5a8dbff598244e8cull -> 0xb8392469a2a0abfeull, rows 0x4c48857b5cd25816ull -> 0xfed8161637d6c938ull, final_hash 0x7f45221fb1111aa3ull -> 0x04b4096b966c585cull [H-THIN]
//   Random14/1/0: boundary 0x5a8dbff598244e8cull -> 0xb8392469a2a0abfeull, final_hash 0x7f45221fb1111aa3ull -> 0x04b4096b966c585cull [H-THIN]
//   Random15/0/1: boundary 0x1fe125a3fb2912dfull -> 0x1fc67d9462b82276ull, rows 0x9ef69d4200bfb809ull -> 0x167381a97aa1c870ull, final_hash 0xf8da94854e79d89eull -> 0x69f92f0d300a126eull [H-THIN]
//   Random15/0/0: boundary 0x1fe125a3fb2912dfull -> 0x1fc67d9462b82276ull, final_hash 0xf8da94854e79d89eull -> 0x69f92f0d300a126eull [H-THIN]
//   Random15/1/1: boundary 0x7b17bf5a43044023ull -> 0xbbbd08da5449bc19ull, rows 0xdeec1e0290d1370dull -> 0x2a9a6d807f0676baull, final_hash 0x01758143c50ac69dull -> 0xe8dd6d8df584c6d6ull [H-THIN]
//   Random15/1/0: boundary 0x7b17bf5a43044023ull -> 0xbbbd08da5449bc19ull, final_hash 0x01758143c50ac69dull -> 0xe8dd6d8df584c6d6ull [H-THIN]
//   Random16/0/1: boundary 0xde7f3e61caa50135ull -> 0xe8851a2e0e645003ull, rows 0x9593cb63a9a06f14ull -> 0xcfab7fb0e8c24b17ull, final_hash 0xc99a0f083d0b2507ull -> 0x5639dcc11e103f25ull [H-THIN]
//   Random16/0/0: boundary 0xde7f3e61caa50135ull -> 0xe8851a2e0e645003ull, final_hash 0xc99a0f083d0b2507ull -> 0x5639dcc11e103f25ull [H-THIN]
//   Random16/1/1: boundary 0xffbc34c7947c611bull -> 0x431440275998e000ull, rows 0xa2b9ee52a1338391ull -> 0x90d12ea4ed48dd05ull, final_hash 0x1f80513beefa0b1aull -> 0xf7e82ef73f56a2eeull [H-THIN]
//   Random16/1/0: boundary 0xffbc34c7947c611bull -> 0x431440275998e000ull, final_hash 0x1f80513beefa0b1aull -> 0xf7e82ef73f56a2eeull [H-THIN]
//   Random17/0/1: boundary 0x15bab2d913fb1d81ull -> 0xf4b303a1ef4696f7ull, rows 0x0a3d587bbe912dcfull -> 0x6417861b10d32340ull, final_hash 0x7185bfaebb406df5ull -> 0xa61332be53034674ull [PAR-ORDERS-2, H-THIN]
//   Random17/0/0: boundary 0x15bab2d913fb1d81ull -> 0xf4b303a1ef4696f7ull, final_hash 0x7185bfaebb406df5ull -> 0xa61332be53034674ull [PAR-ORDERS-2, H-THIN]
//   Random17/1/1: boundary 0xa978788efbcc1631ull -> 0x4fbe2d70efbe24eaull, rows 0xbcbe24594a8acfcaull -> 0xaf770da711891f5aull, final_hash 0x285eaf39ec559587ull -> 0x6665febf6fb7f354ull [PAR-ORDERS-2, H-THIN]
//   Random17/1/0: boundary 0xa978788efbcc1631ull -> 0x4fbe2d70efbe24eaull, final_hash 0x285eaf39ec559587ull -> 0x6665febf6fb7f354ull [PAR-ORDERS-2, H-THIN]
//   Random18/0/1: boundary 0x90ed08a2949b1fb6ull -> 0xf434360cb53f77baull, rows 0x665cda1c725bc1b2ull -> 0xff5d4060a3fafb03ull, final_hash 0xe16bbfbfa58dbeb1ull -> 0xe0f82ba48054cc9cull [PAR-ORDERS-2, H-THIN]
//   Random18/0/0: boundary 0x90ed08a2949b1fb6ull -> 0xf434360cb53f77baull, final_hash 0xe16bbfbfa58dbeb1ull -> 0xe0f82ba48054cc9cull [PAR-ORDERS-2, H-THIN]
//   Random18/1/1: boundary 0xbf864c04890cf915ull -> 0x7b6ad000002b1959ull, rows 0x807a82ee057fbb5dull -> 0x70472dcb4abe021full, final_hash 0x5c359872c6618dd3ull -> 0x73a5afc061a888aaull [PAR-ORDERS-2, H-THIN]
//   Random18/1/0: boundary 0xbf864c04890cf915ull -> 0x7b6ad000002b1959ull, final_hash 0x5c359872c6618dd3ull -> 0x73a5afc061a888aaull [PAR-ORDERS-2, H-THIN]
//   Random19/0/1: boundary 0x3c5cc975f928b775ull -> 0xa21f7b0b62d72170ull, rows 0xea691a951590ece8ull -> 0xda14d42130450ee8ull, final_hash 0x17d53da80c3a895full -> 0x0bcf8b1fde6775f8ull [H-THIN]
//   Random19/0/0: boundary 0x3c5cc975f928b775ull -> 0xa21f7b0b62d72170ull, final_hash 0x17d53da80c3a895full -> 0x0bcf8b1fde6775f8ull [H-THIN]
//   Random19/1/1: boundary 0x92536ad17b9fbf26ull -> 0xceae18544190aae2ull, rows 0xabc10759f5e1ffa1ull -> 0x7d8104e455ec01f8ull, final_hash 0x168791086f51c368ull -> 0x426c72d5bce82609ull [H-THIN]
//   Random19/1/0: boundary 0x92536ad17b9fbf26ull -> 0xceae18544190aae2ull, final_hash 0x168791086f51c368ull -> 0x426c72d5bce82609ull [H-THIN]
//   Random20/0/1: boundary 0x90b20d166e1357e0ull -> 0xad83f25bfdab1b50ull, rows 0x808745030887aa13ull -> 0x103b25042e1313deull, final_hash 0x3968c24c22199036ull -> 0xb27d00f43fad75a1ull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random20/0/0: boundary 0x90b20d166e1357e0ull -> 0xad83f25bfdab1b50ull, final_hash 0x3968c24c22199036ull -> 0xb27d00f43fad75a1ull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random20/1/1: boundary 0xa82d4e53d2192a5full -> 0x4b8bcf5ef1f015c5ull, rows 0x8e1411c055e37af3ull -> 0x6e2d790aa8a7762bull, final_hash 0x5c29f15f02417fcaull -> 0xd3e60d6fda04adbeull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random20/1/0: boundary 0xa82d4e53d2192a5full -> 0x4b8bcf5ef1f015c5ull, final_hash 0x5c29f15f02417fcaull -> 0xd3e60d6fda04adbeull [PAR-ORDERS-2, H-THIN, PAR-CASHFEE]
//   Random21/0/1: boundary 0x18e96beb584671fcull -> 0xd7b52779988b5cfaull, rows 0x6c5b4e5a67209f94ull -> 0xcfddcd52fb629169ull, final_hash 0xca719be0f46f42acull -> 0xfd9b7e4dad3197b5ull [H-THIN]
//   Random21/0/0: boundary 0x18e96beb584671fcull -> 0xd7b52779988b5cfaull, final_hash 0xca719be0f46f42acull -> 0xfd9b7e4dad3197b5ull [H-THIN]
//   Random21/1/1: boundary 0xd32a038cc5b6ea6eull -> 0x1c515b9e6cd109a3ull, rows 0x9074957bf890f8e6ull -> 0xaa94d0f54845ee02ull, final_hash 0xb6387745e72b0075ull -> 0x29fa4e20de356d85ull [H-THIN]
//   Random21/1/0: boundary 0xd32a038cc5b6ea6eull -> 0x1c515b9e6cd109a3ull, final_hash 0xb6387745e72b0075ull -> 0x29fa4e20de356d85ull [H-THIN]
//   Random22/0/1: boundary 0xef05a2f01ca0c05cull -> 0x79adc16250cf8292ull, rows 0xb151ebfe9a44ebebull -> 0x2512a0afd913906aull, final_hash 0x1b1b4c42bccc2191ull -> 0x1d49ae94182c6dc3ull [PAR-ORDERS-2, H-THIN]
//   Random22/0/0: boundary 0xef05a2f01ca0c05cull -> 0x79adc16250cf8292ull, final_hash 0x1b1b4c42bccc2191ull -> 0x1d49ae94182c6dc3ull [PAR-ORDERS-2, H-THIN]
//   Random22/1/1: boundary 0x1624d3651edc9089ull -> 0x301434ca9762fba0ull, rows 0x2a2f5457686fdb7full -> 0x9371e473c135020full, final_hash 0xb361721c9b7f57e7ull -> 0xaadd4e3cb0a37251ull [PAR-ORDERS-2, H-THIN]
//   Random22/1/0: boundary 0x1624d3651edc9089ull -> 0x301434ca9762fba0ull, final_hash 0xb361721c9b7f57e7ull -> 0xaadd4e3cb0a37251ull [PAR-ORDERS-2, H-THIN]
//   Random23/0/1: boundary 0xc50cf90af093d6e9ull -> 0xbf93319ca8e8ed61ull, rows 0x629f1e11fd65d1cdull -> 0x178099cac4dd9abfull, final_hash 0x9f14134203a5c6e9ull -> 0x160121fb8a6ad628ull [PAR-ORDERS-2, H-THIN]
//   Random23/0/0: boundary 0xc50cf90af093d6e9ull -> 0xbf93319ca8e8ed61ull, final_hash 0x9f14134203a5c6e9ull -> 0x160121fb8a6ad628ull [PAR-ORDERS-2, H-THIN]
//   Random23/1/1: boundary 0x61be7e60a493c3efull -> 0x03336650fccfc186ull, rows 0x7182d347041d20e2ull -> 0x84fb2988132b6bdbull, final_hash 0xcac3f08a3f6d7067ull -> 0x54a3c7db6bc08316ull [PAR-ORDERS-2, H-THIN]
//   Random23/1/0: boundary 0x61be7e60a493c3efull -> 0x03336650fccfc186ull, final_hash 0xcac3f08a3f6d7067ull -> 0x54a3c7db6bc08316ull [PAR-ORDERS-2, H-THIN]
//   Random24/0/1: boundary 0xadbbd26ef9c19430ull -> 0x21714f0414285779ull, rows 0x7cea484a04d1bc6dull -> 0x8e07ee97e3c18592ull, final_hash 0x880c60f0ef5d5a7dull -> 0x9b4982cc0c9d5836ull [H-THIN]
//   Random24/0/0: boundary 0xadbbd26ef9c19430ull -> 0x21714f0414285779ull, final_hash 0x880c60f0ef5d5a7dull -> 0x9b4982cc0c9d5836ull [H-THIN]
//   Random24/1/1: boundary 0x91d34f247834f4e9ull -> 0x104927385caf1bafull, rows 0xf9fa951efcc4b1caull -> 0xb58d2d29b9934e0cull, final_hash 0xc06e2e1df8db5231ull -> 0x892eef5960b6186eull [H-THIN]
//   Random24/1/0: boundary 0x91d34f247834f4e9ull -> 0x104927385caf1bafull, final_hash 0xc06e2e1df8db5231ull -> 0x892eef5960b6186eull [H-THIN]
//   Random25/0/1: boundary 0x1091baf336b7e96aull -> 0x7fe75851d29b5f6aull, rows 0x5836120ce0f3d783ull -> 0xbd64625a7bcfb73cull, final_hash 0x3f0d68bfc74cf35full -> 0xdbdc12e31962344aull [H-THIN]
//   Random25/0/0: boundary 0x1091baf336b7e96aull -> 0x7fe75851d29b5f6aull, final_hash 0x3f0d68bfc74cf35full -> 0xdbdc12e31962344aull [H-THIN]
//   Random25/1/1: boundary 0x4c59eab8929878abull -> 0x49f7049727341b3cull, rows 0x90bf03c0ed26cd99ull -> 0x4023f4c7efa8cdacull, final_hash 0xeb9d9c36364972dfull -> 0x1e3a08e1516ae4beull [H-THIN]
//   Random25/1/0: boundary 0x4c59eab8929878abull -> 0x49f7049727341b3cull, final_hash 0xeb9d9c36364972dfull -> 0x1e3a08e1516ae4beull [H-THIN]
//   Random26/0/1: boundary 0x406a1ebe98ed8ee5ull -> 0xca8049a8b96e9724ull, rows 0xc4ceaa2f46b3b1c7ull -> 0xc7e39193f4b07aa7ull, final_hash 0x6d9f1512cb3c63efull -> 0xf4e31e8175400963ull [H-THIN]
//   Random26/0/0: boundary 0x406a1ebe98ed8ee5ull -> 0xca8049a8b96e9724ull, final_hash 0x6d9f1512cb3c63efull -> 0xf4e31e8175400963ull [H-THIN]
//   Random26/1/1: boundary 0x28ce140c5a29687dull -> 0x9c9ac48945130531ull, rows 0x383f34833e879b1bull -> 0xeb4d985eb31f93e0ull, final_hash 0x500ef2279c277ba0ull -> 0xecd913dc1c2bb6edull [H-THIN]
//   Random26/1/0: boundary 0x28ce140c5a29687dull -> 0x9c9ac48945130531ull, final_hash 0x500ef2279c277ba0ull -> 0xecd913dc1c2bb6edull [H-THIN]
//   Random27/0/1: boundary 0xeafcaf72c5794d88ull -> 0xa5ad84fe83acddfcull, rows 0xfbb77c6f11f0944full -> 0x4da695a0a63ae358ull, final_hash 0xf2d844781c8e9819ull -> 0x70f7b127c522e392ull [H-THIN]
//   Random27/0/0: boundary 0xeafcaf72c5794d88ull -> 0xa5ad84fe83acddfcull, final_hash 0xf2d844781c8e9819ull -> 0x70f7b127c522e392ull [H-THIN]
//   Random27/1/1: boundary 0x433fe5d0340138d8ull -> 0x67f5b41cf936747aull, rows 0x80e409be19d6131dull -> 0x06665f5393126836ull, final_hash 0x2a459b69d95400b1ull -> 0x94de1904cefde26dull [H-THIN]
//   Random27/1/0: boundary 0x433fe5d0340138d8ull -> 0x67f5b41cf936747aull, final_hash 0x2a459b69d95400b1ull -> 0x94de1904cefde26dull [H-THIN]
//   Random28/0/1: boundary 0xa0bb8c1925814fe3ull -> 0x8b9cc3d4febe4c62ull, rows 0x8e8763454e7d2dcaull -> 0x69471bc0f4097ac1ull, final_hash 0xe51c2406011cd87dull -> 0x373d1983261f87caull [H-THIN]
//   Random28/0/0: boundary 0xa0bb8c1925814fe3ull -> 0x8b9cc3d4febe4c62ull, final_hash 0xe51c2406011cd87dull -> 0x373d1983261f87caull [H-THIN]
//   Random28/1/1: boundary 0xdb2802eb6d8819c7ull -> 0x57f3120e39e241ddull, rows 0x910e834a102ac932ull -> 0x0a99bf96869493a4ull, final_hash 0x7406a539e21e57ceull -> 0xe2efc2ce4ee88ff9ull [H-THIN]
//   Random28/1/0: boundary 0xdb2802eb6d8819c7ull -> 0x57f3120e39e241ddull, final_hash 0x7406a539e21e57ceull -> 0xe2efc2ce4ee88ff9ull [H-THIN]
//   Random29/0/1: boundary 0x2bbdb428b4a78935ull -> 0x89b631d5f5f01691ull, rows 0x6c7b12c7d5aa45e4ull -> 0x167525b5e459f6a9ull, final_hash 0xfd3d2fb6be6132d7ull -> 0xb42cb126538c6f0aull [H-THIN]
//   Random29/0/0: boundary 0x2bbdb428b4a78935ull -> 0x89b631d5f5f01691ull, final_hash 0xfd3d2fb6be6132d7ull -> 0xb42cb126538c6f0aull [H-THIN]
//   Random29/1/1: boundary 0x2bc2551fd2128e20ull -> 0x41cca15aeaf4b30dull, rows 0xb9728dfab890bfd1ull -> 0x5c0fe05b50ac7989ull, final_hash 0x082924a3760602daull -> 0xdaa6c53d3410e42full [H-THIN]
//   Random29/1/0: boundary 0x2bc2551fd2128e20ull -> 0x41cca15aeaf4b30dull, final_hash 0x082924a3760602daull -> 0xdaa6c53d3410e42full [H-THIN]
//   Random30/0/1: boundary 0x585a1a9f34476a29ull -> 0xe74142e41965c7f8ull, rows 0x5d09652b7dc7603full -> 0x7d6bfad3ec602d1eull, final_hash 0x8e3b3ec00a1454a2ull -> 0x2ff813254130dde5ull [H-THIN]
//   Random30/0/0: boundary 0x585a1a9f34476a29ull -> 0xe74142e41965c7f8ull, final_hash 0x8e3b3ec00a1454a2ull -> 0x2ff813254130dde5ull [H-THIN]
//   Random30/1/1: boundary 0x8de0587cbe97fc0aull -> 0xffbc3c35359e842dull, rows 0x2fc1ebe82b1783eeull -> 0xa978037a0a737ed4ull, final_hash 0x7ffde30dc93fc9d7ull -> 0xb713984b2b953de4ull [H-THIN]
//   Random30/1/0: boundary 0x8de0587cbe97fc0aull -> 0xffbc3c35359e842dull, final_hash 0x7ffde30dc93fc9d7ull -> 0xb713984b2b953de4ull [H-THIN]
//   Random31/0/1: boundary 0x09b545a0067e8c26ull -> 0xc1d0985632aa85e5ull, rows 0x3c6ae0150bb63629ull -> 0xb1ff4f1ff8ce7ff6ull, final_hash 0xfaf6a143c8b0d038ull -> 0x0138efa3affc4452ull [H-THIN]
//   Random31/0/0: boundary 0x09b545a0067e8c26ull -> 0xc1d0985632aa85e5ull, final_hash 0xfaf6a143c8b0d038ull -> 0x0138efa3affc4452ull [H-THIN]
//   Random31/1/1: boundary 0xb42bd9cfbe060a73ull -> 0x29c04e4a3f62fc66ull, rows 0x928f7cfde5bdb105ull -> 0xd37bee2bcbd5b472ull, final_hash 0xf602f077ecb2b7cfull -> 0x3e283f49a532b649ull [H-THIN]
//   Random31/1/0: boundary 0xb42bd9cfbe060a73ull -> 0x29c04e4a3f62fc66ull, final_hash 0xf602f077ecb2b7cfull -> 0x3e283f49a532b649ull [H-THIN]
//   Random32/0/1: boundary 0x220cd02053dbc40aull -> 0x69fb37996a1a6d8aull, rows 0x9afe2ada13350695ull -> 0x4eda1ad97257444eull, final_hash 0xe577534dd3033b52ull -> 0xf8f4a46581375bc8ull [H-THIN]
//   Random32/0/0: boundary 0x220cd02053dbc40aull -> 0x69fb37996a1a6d8aull, final_hash 0xe577534dd3033b52ull -> 0xf8f4a46581375bc8ull [H-THIN]
//   Random32/1/1: boundary 0xc0ca43094334f008ull -> 0x6aa8d3b227f15c4cull, rows 0xe85eb4136b8c435dull -> 0xefb2e786712329f2ull, final_hash 0xba233c9fc46444a4ull -> 0xe3b7a40e1641948dull [H-THIN]
//   Random32/1/0: boundary 0xc0ca43094334f008ull -> 0x6aa8d3b227f15c4cull, final_hash 0xba233c9fc46444a4ull -> 0xe3b7a40e1641948dull [H-THIN]
//   Random33/0/1: boundary 0xbfcfebe96632bd39ull -> 0x9651f8f876e48213ull, rows 0x116bc2f57887ba4bull -> 0x70cd8d3b24d08640ull, final_hash 0xf8a0e855a842d39cull -> 0x495394a49a7d3245ull [H-THIN]
//   Random33/0/0: boundary 0xbfcfebe96632bd39ull -> 0x9651f8f876e48213ull, final_hash 0xf8a0e855a842d39cull -> 0x495394a49a7d3245ull [H-THIN]
//   Random33/1/1: boundary 0xe17f0353f37c41f1ull -> 0x23686cd9a3fe0637ull, rows 0x0ba98d74e8b769b7ull -> 0x17ecf363cd313d8aull, final_hash 0xb76d3ca699ea4ebdull -> 0xcefd187486fda5beull [H-THIN]
//   Random33/1/0: boundary 0xe17f0353f37c41f1ull -> 0x23686cd9a3fe0637ull, final_hash 0xb76d3ca699ea4ebdull -> 0xcefd187486fda5beull [H-THIN]
//   Random34/0/1: boundary 0xea1b68237d95805full -> 0xb0d26d6ec62ee0b2ull, rows 0xb29d9ea005d9c282ull -> 0xe24b790bed6d8209ull, final_hash 0xb03d41aea9c00c63ull -> 0xa8799fc8644a8be8ull [H-THIN]
//   Random34/0/0: boundary 0xea1b68237d95805full -> 0xb0d26d6ec62ee0b2ull, final_hash 0xb03d41aea9c00c63ull -> 0xa8799fc8644a8be8ull [H-THIN]
//   Random34/1/1: boundary 0x4ecacf8ab2d85c47ull -> 0x40ad548ed9e2d639ull, rows 0x294fe821fef3729dull -> 0x2d022bb216743a64ull, final_hash 0xe4e38b6ada294505ull -> 0x0a4527b569ac8a10ull [H-THIN]
//   Random34/1/0: boundary 0x4ecacf8ab2d85c47ull -> 0x40ad548ed9e2d639ull, final_hash 0xe4e38b6ada294505ull -> 0x0a4527b569ac8a10ull [H-THIN]
//   Random35/0/1: boundary 0xeb16fb1a6bc80c17ull -> 0xbdcab3fd94434657ull, rows 0x7265444996669f20ull -> 0x8f07aebde3f01772ull, final_hash 0xb971c6bc6d7ac182ull -> 0x4d118037ee1626adull [H-THIN, PAR-CASHFEE]
//   Random35/0/0: boundary 0xeb16fb1a6bc80c17ull -> 0xbdcab3fd94434657ull, final_hash 0xb971c6bc6d7ac182ull -> 0x4d118037ee1626adull [H-THIN, PAR-CASHFEE]
//   Random35/1/1: boundary 0x193ec2cb9a163900ull -> 0x9f158f1f03765f97ull, rows 0x9233c78214742aa3ull -> 0x7ea6860a76f2a755ull, final_hash 0xa9bdcf22e28567b2ull -> 0x07820fe3390f4450ull [H-THIN, PAR-CASHFEE]
//   Random35/1/0: boundary 0x193ec2cb9a163900ull -> 0x9f158f1f03765f97ull, final_hash 0xa9bdcf22e28567b2ull -> 0x07820fe3390f4450ull [H-THIN, PAR-CASHFEE]
//   Random36/0/1: boundary 0xd4b5588948d4b755ull -> 0xc06367cbde7ecc9aull, rows 0x6af2dc7dd208ef5dull -> 0x9c13280fd6c4a76full, final_hash 0x1980e25b047dea78ull -> 0x4a73a58e1bf4c419ull [PAR-ORDERS-2, H-THIN]
//   Random36/0/0: boundary 0xd4b5588948d4b755ull -> 0xc06367cbde7ecc9aull, final_hash 0x1980e25b047dea78ull -> 0x4a73a58e1bf4c419ull [PAR-ORDERS-2, H-THIN]
//   Random36/1/1: boundary 0xd1532dd9d52ae5f4ull -> 0xf78394d86184426aull, rows 0x0e1d68d8d6c4c5e2ull -> 0xd52d837b120d0443ull, final_hash 0xe93d9e6c724efef6ull -> 0x1e90f705be3da39full [PAR-ORDERS-2, H-THIN]
//   Random36/1/0: boundary 0xd1532dd9d52ae5f4ull -> 0xf78394d86184426aull, final_hash 0xe93d9e6c724efef6ull -> 0x1e90f705be3da39full [PAR-ORDERS-2, H-THIN]
//   Random37/0/1: boundary 0x3a89ff92c403a3e7ull -> 0x8af4f686e8afd249ull, rows 0xddda8d77310d918full -> 0xb43a05dbbc636b7aull, final_hash 0xff9c74898bf3a72aull -> 0x8692adfc1cddb1c4ull [PAR-ORDERS-2, H-THIN]
//   Random37/0/0: boundary 0x3a89ff92c403a3e7ull -> 0x8af4f686e8afd249ull, final_hash 0xff9c74898bf3a72aull -> 0x8692adfc1cddb1c4ull [PAR-ORDERS-2, H-THIN]
//   Random37/1/1: boundary 0x8a36a09ee42999ecull -> 0xe82a3b0bd1861474ull, rows 0xf12dd4800e96f492ull -> 0x5ea014024a78fb97ull, final_hash 0x9f14bee9808ee8baull -> 0x1ace37e580b56141ull [PAR-ORDERS-2, H-THIN]
//   Random37/1/0: boundary 0x8a36a09ee42999ecull -> 0xe82a3b0bd1861474ull, final_hash 0x9f14bee9808ee8baull -> 0x1ace37e580b56141ull [PAR-ORDERS-2, H-THIN]
//   Random38/0/1: boundary 0x36ed3eee0da888c2ull -> 0xeaf05ca1aedc03d5ull, rows 0x2bf58dbe1c5bd9acull -> 0x80971876a1156ee8ull, final_hash 0x5b82e26291364fc0ull -> 0x2d7719753c10f4bbull [H-THIN, PAR-CASHFEE]
//   Random38/0/0: boundary 0x36ed3eee0da888c2ull -> 0xeaf05ca1aedc03d5ull, final_hash 0x5b82e26291364fc0ull -> 0x2d7719753c10f4bbull [H-THIN, PAR-CASHFEE]
//   Random38/1/1: boundary 0xa3225feaf19b98feull -> 0x8285a035265d771cull, rows 0x59c34402fcf68e2aull -> 0x0d3b927724077789ull, final_hash 0x135197c1ef34cbbdull -> 0x09336a0556a2fbb7ull [H-THIN, PAR-CASHFEE]
//   Random38/1/0: boundary 0xa3225feaf19b98feull -> 0x8285a035265d771cull, final_hash 0x135197c1ef34cbbdull -> 0x09336a0556a2fbb7ull [H-THIN, PAR-CASHFEE]
//   Random39/0/1: boundary 0x9bcdaebb98702055ull -> 0xc989defa2d8cff00ull, rows 0xf4194fbb3f8e63f4ull -> 0x31db41d49a36aaa6ull, final_hash 0x3026f01eb73dfcdaull -> 0xb8c88da9531702d9ull [H-THIN]
//   Random39/0/0: boundary 0x9bcdaebb98702055ull -> 0xc989defa2d8cff00ull, final_hash 0x3026f01eb73dfcdaull -> 0xb8c88da9531702d9ull [H-THIN]
//   Random39/1/1: boundary 0x5fa98522adc27aafull -> 0x588cc94ab848f7e6ull, rows 0x50b22f98ba965993ull -> 0x3b1478373127afc0ull, final_hash 0x123f0c5b09aeae91ull -> 0xbdab759ad2c5865cull [H-THIN]
//   Random39/1/0: boundary 0x5fa98522adc27aafull -> 0x588cc94ab848f7e6ull, final_hash 0x123f0c5b09aeae91ull -> 0xbdab759ad2c5865cull [H-THIN]
//   Random40/0/1: boundary 0x869959795f89f7afull -> 0xc4f976129a95a285ull, rows 0xe9919376f3542ea7ull -> 0x389162a99c89a49cull, final_hash 0xd91533e6639e22f9ull -> 0xcafbb488b92f9edaull [PAR-ORDERS-2, H-THIN]
//   Random40/0/0: boundary 0x869959795f89f7afull -> 0xc4f976129a95a285ull, final_hash 0xd91533e6639e22f9ull -> 0xcafbb488b92f9edaull [PAR-ORDERS-2, H-THIN]
//   Random40/1/1: boundary 0xf47407130500a9bdull -> 0x1cc6041a6a6fcdcdull, rows 0xda9b4928c023ca07ull -> 0x33a043a5f0e45941ull, final_hash 0x5a8f1ec5a18f3731ull -> 0x798fe8d7596aaa9cull [PAR-ORDERS-2, H-THIN]
//   Random40/1/0: boundary 0xf47407130500a9bdull -> 0x1cc6041a6a6fcdcdull, final_hash 0x5a8f1ec5a18f3731ull -> 0x798fe8d7596aaa9cull [PAR-ORDERS-2, H-THIN]
//   Random41/0/1: boundary 0x6509ace183e7d2cdull -> 0xb4b89e111611f4ceull, rows 0x957c5244d07600b9ull -> 0xea3d6d0c77bc7e76ull, final_hash 0xcc1203a69591b134ull -> 0x980f8a18b3d80a23ull [PAR-ORDERS-2, H-THIN]
//   Random41/0/0: boundary 0x6509ace183e7d2cdull -> 0xb4b89e111611f4ceull, final_hash 0xcc1203a69591b134ull -> 0x980f8a18b3d80a23ull [PAR-ORDERS-2, H-THIN]
//   Random41/1/1: boundary 0xd2d43ab5fdd2ef0bull -> 0xf15d27dad1575f3bull, rows 0xb907bbe1bb2da083ull -> 0x9245b7b247af4cc1ull, final_hash 0xc55d2d003a5bec69ull -> 0xc730d99532f512d8ull [PAR-ORDERS-2, H-THIN]
//   Random41/1/0: boundary 0xd2d43ab5fdd2ef0bull -> 0xf15d27dad1575f3bull, final_hash 0xc55d2d003a5bec69ull -> 0xc730d99532f512d8ull [PAR-ORDERS-2, H-THIN]
//   Random42/0/1: boundary 0xa0ee848263988a3dull -> 0x0da9b26d3847ab1full, rows 0x1f2ad0a937e24f39ull -> 0x9c9329bd9adc3676ull, final_hash 0xb124b9f52e8cf9f2ull -> 0x49a0634035589083ull [H-THIN]
//   Random42/0/0: boundary 0xa0ee848263988a3dull -> 0x0da9b26d3847ab1full, final_hash 0xb124b9f52e8cf9f2ull -> 0x49a0634035589083ull [H-THIN]
//   Random42/1/1: boundary 0x7f6282e75cc72bb0ull -> 0xbd61639c42be9cb3ull, rows 0x23bb6075acaedb2eull -> 0x5d2fe8dd7d35b222ull, final_hash 0xdb2120d5edab4d9full -> 0xa80c78dbeddc4468ull [H-THIN]
//   Random42/1/0: boundary 0x7f6282e75cc72bb0ull -> 0xbd61639c42be9cb3ull, final_hash 0xdb2120d5edab4d9full -> 0xa80c78dbeddc4468ull [H-THIN]
//   Random43/0/1: boundary 0x7fb8967d6d6d24d2ull -> 0xc521512ba92a54c9ull, rows 0x98cae92178fe91ebull -> 0x5993bd8ceee91044ull, final_hash 0xdc29a310e9c7d06bull -> 0x5fbe995d73a8bff0ull [H-THIN]
//   Random43/0/0: boundary 0x7fb8967d6d6d24d2ull -> 0xc521512ba92a54c9ull, final_hash 0xdc29a310e9c7d06bull -> 0x5fbe995d73a8bff0ull [H-THIN]
//   Random43/1/1: boundary 0xffd1c55ce888ef72ull -> 0xa9607e56c7e4b193ull, rows 0x8ac3c2c1053a4819ull -> 0x941becccb8fe75ceull, final_hash 0x45393018e93b32beull -> 0xfdc00d0d1ff0cbfbull [H-THIN]
//   Random43/1/0: boundary 0xffd1c55ce888ef72ull -> 0xa9607e56c7e4b193ull, final_hash 0x45393018e93b32beull -> 0xfdc00d0d1ff0cbfbull [H-THIN]
//   Random44/0/1: boundary 0xdae57d80c4d2a634ull -> 0x971e7c4a38e301ddull, rows 0xf7669a1535370da9ull -> 0x55249f0ae51722feull, final_hash 0xc555f0e88b5eb378ull -> 0x5a13570337a638b5ull [PAR-ORDERS-2, H-THIN]
//   Random44/0/0: boundary 0xdae57d80c4d2a634ull -> 0x971e7c4a38e301ddull, final_hash 0xc555f0e88b5eb378ull -> 0x5a13570337a638b5ull [PAR-ORDERS-2, H-THIN]
//   Random44/1/1: boundary 0x360b704190f157adull -> 0x3ee064c4f943e93aull, rows 0x5b2a75cdabd7235cull -> 0xbc62822019ded427ull, final_hash 0xde36f7f290af13a5ull -> 0xc18096028ba7be64ull [PAR-ORDERS-2, H-THIN]
//   Random44/1/0: boundary 0x360b704190f157adull -> 0x3ee064c4f943e93aull, final_hash 0xde36f7f290af13a5ull -> 0xc18096028ba7be64ull [PAR-ORDERS-2, H-THIN]
//   Random45/0/1: boundary 0xbf351c50f4a44f8full -> 0xa155b80e37d41cf2ull, rows 0xfee900af4573bbc1ull -> 0xbacd32f5d8b94030ull, final_hash 0x7f2867c87c78723eull -> 0x598c179a2d5c735cull [PAR-ORDERS-2, H-THIN]
//   Random45/0/0: boundary 0xbf351c50f4a44f8full -> 0xa155b80e37d41cf2ull, final_hash 0x7f2867c87c78723eull -> 0x598c179a2d5c735cull [PAR-ORDERS-2, H-THIN]
//   Random45/1/1: boundary 0x32a1d85e85b93f0aull -> 0x9644e4dd1f925cdfull, rows 0xebcfcdf78cd848a5ull -> 0x2cc38b54d184cf0aull, final_hash 0x56b0e295081820c0ull -> 0x290739687f68f174ull [PAR-ORDERS-2, H-THIN]
//   Random45/1/0: boundary 0x32a1d85e85b93f0aull -> 0x9644e4dd1f925cdfull, final_hash 0x56b0e295081820c0ull -> 0x290739687f68f174ull [PAR-ORDERS-2, H-THIN]
//   Random46/0/1: boundary 0x3ad713968f2546d9ull -> 0xa74f65268d41a1bbull, rows 0x84dd0930114cd829ull -> 0xfc4e6864d98d62c0ull, final_hash 0x6bb382cfb976e148ull -> 0x7097cd184da8a4dcull [H-THIN, PAR-CASHFEE]
//   Random46/0/0: boundary 0x3ad713968f2546d9ull -> 0xa74f65268d41a1bbull, final_hash 0x6bb382cfb976e148ull -> 0x7097cd184da8a4dcull [H-THIN, PAR-CASHFEE]
//   Random46/1/1: boundary 0x997f2e0ee9786d91ull -> 0xb1406e1f21710782ull, rows 0x1ce86cd2c755bea3ull -> 0xe0b910f752cbc3adull, final_hash 0x556134f8585397c9ull -> 0x2539296694529005ull [H-THIN, PAR-CASHFEE]
//   Random46/1/0: boundary 0x997f2e0ee9786d91ull -> 0xb1406e1f21710782ull, final_hash 0x556134f8585397c9ull -> 0x2539296694529005ull [H-THIN, PAR-CASHFEE]
//   Random47/0/1: boundary 0x8ce025f147bbdb2dull -> 0x214dc1308457a01aull, rows 0x74895c0d69d6b4a8ull -> 0xe293c7196f5c9f95ull, final_hash 0x31ec37588708d5f1ull -> 0x8176d644a621a19eull [H-THIN]
//   Random47/0/0: boundary 0x8ce025f147bbdb2dull -> 0x214dc1308457a01aull, final_hash 0x31ec37588708d5f1ull -> 0x8176d644a621a19eull [H-THIN]
//   Random47/1/1: boundary 0xebf0544eb982483aull -> 0xfe949f5ba003397dull, rows 0xca410d7365dca790ull -> 0xe112a9940c547399ull, final_hash 0xa8aff582382caf48ull -> 0x9ee620e1bf252639ull [H-THIN]
//   Random47/1/0: boundary 0xebf0544eb982483aull -> 0xfe949f5ba003397dull, final_hash 0xa8aff582382caf48ull -> 0x9ee620e1bf252639ull [H-THIN]
//   Random48/0/1: boundary 0xaa6b18e4254e2f68ull -> 0x21c0bf9ae910d3acull, rows 0x2f80186c69547a10ull -> 0xe2d98f0ffb5c1b96ull, final_hash 0xaff43b38e6bc02acull -> 0x1e560afd06471537ull [H-THIN]
//   Random48/0/0: boundary 0xaa6b18e4254e2f68ull -> 0x21c0bf9ae910d3acull, final_hash 0xaff43b38e6bc02acull -> 0x1e560afd06471537ull [H-THIN]
//   Random48/1/1: boundary 0x655a5637ae415d60ull -> 0x4fd90c8ca1025428ull, rows 0x13866e45be14eaeeull -> 0x7706d5e9ebfd9226ull, final_hash 0xa0e205aa768a0bc9ull -> 0xee01efa6d7b40515ull [H-THIN]
//   Random48/1/0: boundary 0x655a5637ae415d60ull -> 0x4fd90c8ca1025428ull, final_hash 0xa0e205aa768a0bc9ull -> 0xee01efa6d7b40515ull [H-THIN]
constexpr Pinned kPinned[] = {
    {"Quiet", 0, 1, 0xcd8d3ecb4d2c740aull, 0xe6da4342a82c460cull, 0x47fe0d7eaf8e51e3ull, 0x458f1ecf00ef1d5cull, 40, 40, 0},
    {"Quiet", 0, 0, 0xcd8d3ecb4d2c740aull, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0x458f1ecf00ef1d5cull, 40, 0, 0},
    {"Quiet", 1, 1, 0x2a102759d268d84dull, 0x923a578d640d7fdbull, 0x47fe0d7eaf8e51e3ull, 0x6ca064a309be6bfaull, 40, 40, 0},
    {"Quiet", 1, 0, 0x2a102759d268d84dull, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0x6ca064a309be6bfaull, 40, 0, 0},
    {"HoldLongFull", 0, 1, 0x2be6111eb08aff44ull, 0x37135fdf24860586ull, 0xa32454de68248651ull, 0x6caa6506b43c5228ull, 40, 40, 1},
    {"HoldLongFull", 0, 0, 0x2be6111eb08aff44ull, 0x14650fb0739d0383ull, 0xa32454de68248651ull, 0x6caa6506b43c5228ull, 40, 0, 1},
    {"HoldLongFull", 1, 1, 0x6da42e25a0445e57ull, 0xa90a9669249de5f2ull, 0xa32454de68248651ull, 0x1fdb374b13f0dd9cull, 40, 40, 1},
    {"HoldLongFull", 1, 0, 0x6da42e25a0445e57ull, 0x14650fb0739d0383ull, 0xa32454de68248651ull, 0x1fdb374b13f0dd9cull, 40, 0, 1},
    {"MarginCalls", 0, 1, 0x9ffada7afff423a5ull, 0xf1451bf046aea2aeull, 0x259f52c0c76aafe9ull, 0xd58658d95c54acbeull, 36, 36, 3},
    {"MarginCalls", 0, 0, 0x9ffada7afff423a5ull, 0x14650fb0739d0383ull, 0x259f52c0c76aafe9ull, 0xd58658d95c54acbeull, 36, 0, 3},
    {"MarginCalls", 1, 1, 0x548dd443b38f74b0ull, 0xfdd852b37b07d408ull, 0x92cc9a49e414ed25ull, 0x76f99b20ecd7e68cull, 36, 36, 2},
    {"MarginCalls", 1, 0, 0x548dd443b38f74b0ull, 0x14650fb0739d0383ull, 0x92cc9a49e414ed25ull, 0x76f99b20ecd7e68cull, 36, 0, 2},
    {"MarginCallCycles", 0, 1, 0x53630c222ae9b1a8ull, 0x26e80eef46f3b822ull, 0x657a702082e43f34ull, 0x9dea3ea1d3cbc012ull, 64, 64, 1},
    {"MarginCallCycles", 0, 0, 0x53630c222ae9b1a8ull, 0x14650fb0739d0383ull, 0x657a702082e43f34ull, 0x9dea3ea1d3cbc012ull, 64, 0, 1},
    {"MarginCallCycles", 1, 1, 0xd66af1875ed2e747ull, 0xffa2470471097d1dull, 0x9b23f7fef31dec0cull, 0x22ec1fab2742fa75ull, 64, 64, 3},
    {"MarginCallCycles", 1, 0, 0xd66af1875ed2e747ull, 0x14650fb0739d0383ull, 0x9b23f7fef31dec0cull, 0x22ec1fab2742fa75ull, 64, 0, 3},
    {"Bracket", 0, 1, 0xaeddc0d29a30f3f7ull, 0xd324958e0a8a53deull, 0xc17c8a18143849dcull, 0xef163ce5ad81ccceull, 48, 48, 1},
    {"Bracket", 0, 0, 0xaeddc0d29a30f3f7ull, 0x14650fb0739d0383ull, 0xc17c8a18143849dcull, 0xef163ce5ad81ccceull, 48, 0, 1},
    {"Bracket", 1, 1, 0xdb519033b2a6887bull, 0xb2b3be51c614016dull, 0xd474aeacc0bce022ull, 0x488b102fb534087cull, 48, 48, 1},
    {"Bracket", 1, 0, 0xdb519033b2a6887bull, 0x14650fb0739d0383ull, 0xd474aeacc0bce022ull, 0x488b102fb534087cull, 48, 0, 1},
    {"Trail", 0, 1, 0xa901a158196e8e90ull, 0x3bf9ee1972d44776ull, 0xd244c7dc0fdc94a1ull, 0xf1d8827b0b6f8fcfull, 48, 48, 1},
    {"Trail", 0, 0, 0xa901a158196e8e90ull, 0x14650fb0739d0383ull, 0xd244c7dc0fdc94a1ull, 0xf1d8827b0b6f8fcfull, 48, 0, 1},
    {"Trail", 1, 1, 0xb23147b5951428e2ull, 0x4f409e92b7a0d98aull, 0xd244c7dc0fdc94a1ull, 0x2716f35c358b7145ull, 48, 48, 1},
    {"Trail", 1, 0, 0xb23147b5951428e2ull, 0x14650fb0739d0383ull, 0xd244c7dc0fdc94a1ull, 0x2716f35c358b7145ull, 48, 0, 1},
    {"DualStops", 0, 1, 0x29701bba6d7182b5ull, 0xf109235082c1860eull, 0xb19c9aea35851016ull, 0x34452d46a85f96c6ull, 40, 40, 1},
    {"DualStops", 0, 0, 0x29701bba6d7182b5ull, 0x14650fb0739d0383ull, 0xb19c9aea35851016ull, 0x34452d46a85f96c6ull, 40, 0, 1},
    {"DualStops", 1, 1, 0x86f9654dc6f8ed22ull, 0x4393ede3dede0436ull, 0xb19c9aea35851016ull, 0x1bfce4a56d0314f2ull, 40, 40, 1},
    {"DualStops", 1, 0, 0x86f9654dc6f8ed22ull, 0x14650fb0739d0383ull, 0xb19c9aea35851016ull, 0x1bfce4a56d0314f2ull, 40, 0, 1},
    {"InvertedStops", 0, 1, 0xeb8c3dfbd9d273a2ull, 0xb7ecf902b2ff0befull, 0x089245abb04b9cb7ull, 0x1993f824c35e154dull, 40, 40, 4},
    {"InvertedStops", 0, 0, 0xeb8c3dfbd9d273a2ull, 0x14650fb0739d0383ull, 0x089245abb04b9cb7ull, 0x1993f824c35e154dull, 40, 0, 4},
    {"InvertedStops", 1, 1, 0x976cbd72b16fcb6full, 0xe49f3d6c9fe39f5bull, 0x089245abb04b9cb7ull, 0x69e468caa11329ccull, 40, 40, 4},
    {"InvertedStops", 1, 0, 0x976cbd72b16fcb6full, 0x14650fb0739d0383ull, 0x089245abb04b9cb7ull, 0x69e468caa11329ccull, 40, 0, 4},
    {"PendingParent", 0, 1, 0xc2b9247123313aceull, 0xa8f42cb39e1aeff3ull, 0x47fe0d7eaf8e51e3ull, 0xfbf54246d7baccdaull, 40, 40, 0},
    {"PendingParent", 0, 0, 0xc2b9247123313aceull, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0xfbf54246d7baccdaull, 40, 0, 0},
    {"PendingParent", 1, 1, 0xcb795828e625815full, 0x808c94697e2231bdull, 0x47fe0d7eaf8e51e3ull, 0x0f9d2bb1d6492d5aull, 40, 40, 0},
    {"PendingParent", 1, 0, 0xcb795828e625815full, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0x0f9d2bb1d6492d5aull, 40, 0, 0},
    {"GappedAdd", 0, 1, 0x2aa05060cf893d51ull, 0xca15382fd8d71504ull, 0x6528238d0771c54eull, 0x53a5ed59712f1502ull, 40, 40, 2},
    {"GappedAdd", 0, 0, 0x2aa05060cf893d51ull, 0x14650fb0739d0383ull, 0x6528238d0771c54eull, 0x53a5ed59712f1502ull, 40, 0, 2},
    {"GappedAdd", 1, 1, 0x1de2b53c2e69ae04ull, 0xb77b870b1ef756a3ull, 0x6528238d0771c54eull, 0xc471e2ce0ce5b5b7ull, 40, 40, 2},
    {"GappedAdd", 1, 0, 0x1de2b53c2e69ae04ull, 0x14650fb0739d0383ull, 0x6528238d0771c54eull, 0xc471e2ce0ce5b5b7ull, 40, 0, 2},
    {"DeclinedReversal", 0, 1, 0x5dc519d25cd8e456ull, 0xcd770d65bdeb5e28ull, 0xc3448bef59024d51ull, 0xacfb010f8ef2c1dbull, 24, 24, 1},
    {"DeclinedReversal", 0, 0, 0x5dc519d25cd8e456ull, 0x14650fb0739d0383ull, 0xc3448bef59024d51ull, 0xacfb010f8ef2c1dbull, 24, 0, 1},
    {"DeclinedReversal", 1, 1, 0x506c2023d62a4e6full, 0xe0eb2dae405533f4ull, 0xc3448bef59024d51ull, 0x980a839868ba5b97ull, 24, 24, 1},
    {"DeclinedReversal", 1, 0, 0x506c2023d62a4e6full, 0x14650fb0739d0383ull, 0xc3448bef59024d51ull, 0x980a839868ba5b97ull, 24, 0, 1},
    {"CalcOnFills", 0, 1, 0xbd9feba11a355c84ull, 0xbd62a54cd2c27075ull, 0x87698c8f4050ce14ull, 0x8e716ca55979f5e4ull, 55, 40, 8},
    {"CalcOnFills", 0, 0, 0xbd9feba11a355c84ull, 0x14650fb0739d0383ull, 0x87698c8f4050ce14ull, 0x8e716ca55979f5e4ull, 55, 0, 8},
    {"CalcOnFills", 1, 1, 0x0e2860d938b1d36dull, 0xfe826221acbd42c3ull, 0xa16d17de4933efdaull, 0x1ae91d50d6762fd0ull, 52, 40, 6},
    {"CalcOnFills", 1, 0, 0x0e2860d938b1d36dull, 0x14650fb0739d0383ull, 0xa16d17de4933efdaull, 0x1ae91d50d6762fd0ull, 52, 0, 6},
    {"CloseOrders", 0, 1, 0xb27e9cfcbfee41d3ull, 0x0033c4a6a644f0ddull, 0xbc944a52841481b1ull, 0xbedbc1183b7526a1ull, 48, 48, 16},
    {"CloseOrders", 0, 0, 0xb27e9cfcbfee41d3ull, 0x14650fb0739d0383ull, 0xbc944a52841481b1ull, 0xbedbc1183b7526a1ull, 48, 0, 16},
    {"CloseOrders", 1, 1, 0xff96b9591f72f309ull, 0x5aaa3b2f562a501dull, 0x4cdcb678c06d1a59ull, 0xc542c7fb8bb5559dull, 48, 48, 16},
    {"CloseOrders", 1, 0, 0xff96b9591f72f309ull, 0x14650fb0739d0383ull, 0x4cdcb678c06d1a59ull, 0xc542c7fb8bb5559dull, 48, 0, 16},
    {"CloseOrdersSlipped", 0, 1, 0xc2834ef9bd74b55full, 0xb1042b50a92de9a0ull, 0x0f201395d3fe211full, 0x6c12422a13b3f077ull, 36, 36, 2},
    {"CloseOrdersSlipped", 0, 0, 0xc2834ef9bd74b55full, 0x14650fb0739d0383ull, 0x0f201395d3fe211full, 0x6c12422a13b3f077ull, 36, 0, 2},
    {"CloseOrdersSlipped", 1, 1, 0x71906fcd647a169full, 0xa2710dbb1c8a57edull, 0x0f201395d3fe211full, 0x0330185649c756c2ull, 36, 36, 2},
    {"CloseOrdersSlipped", 1, 0, 0x71906fcd647a169full, 0x14650fb0739d0383ull, 0x0f201395d3fe211full, 0x0330185649c756c2ull, 36, 0, 2},
    {"IntradayLoss", 0, 1, 0x8a9bd5dd1429352eull, 0x616b96966292e8adull, 0x60c9f061d60a69edull, 0xc14ffe9c379fbee2ull, 40, 40, 1},
    {"IntradayLoss", 0, 0, 0x8a9bd5dd1429352eull, 0x14650fb0739d0383ull, 0x60c9f061d60a69edull, 0xc14ffe9c379fbee2ull, 40, 0, 1},
    {"IntradayLoss", 1, 1, 0x1a92d1a03b233c6cull, 0x3382648892a694c4ull, 0x60c9f061d60a69edull, 0x66f4f2fe8b455019ull, 40, 40, 1},
    {"IntradayLoss", 1, 0, 0x1a92d1a03b233c6cull, 0x14650fb0739d0383ull, 0x60c9f061d60a69edull, 0x66f4f2fe8b455019ull, 40, 0, 1},
    {"IntradayCapDeferred", 0, 1, 0xde19edf4087b930dull, 0x560de539b138ea41ull, 0x2fa61401cccb3845ull, 0x7730e7fe703663edull, 24, 24, 1},
    {"IntradayCapDeferred", 0, 0, 0xde19edf4087b930dull, 0x14650fb0739d0383ull, 0x2fa61401cccb3845ull, 0x7730e7fe703663edull, 24, 0, 1},
    {"IntradayCapDeferred", 1, 1, 0x293bdcc491ae22d7ull, 0xd165d92830916a1cull, 0x6517fe8ea7437b74ull, 0x7ba4fe8defa15d4aull, 24, 24, 1},
    {"IntradayCapDeferred", 1, 0, 0x293bdcc491ae22d7ull, 0x14650fb0739d0383ull, 0x6517fe8ea7437b74ull, 0x7ba4fe8defa15d4aull, 24, 0, 1},
    {"PreopenSlice", 0, 1, 0xcbc8fb1ba9d83481ull, 0xbe57d9275ae9e2bbull, 0x96bb4615ab0ad33bull, 0x62f97adf60eb373full, 36, 36, 1},
    {"PreopenSlice", 0, 0, 0xcbc8fb1ba9d83481ull, 0x14650fb0739d0383ull, 0x96bb4615ab0ad33bull, 0x62f97adf60eb373full, 36, 0, 1},
    {"PreopenSlice", 1, 1, 0xadcfe58d853cedfcull, 0xecc971145edced41ull, 0x96bb4615ab0ad33bull, 0x94fff9b38c7f1bd5ull, 36, 36, 1},
    {"PreopenSlice", 1, 0, 0xadcfe58d853cedfcull, 0x14650fb0739d0383ull, 0x96bb4615ab0ad33bull, 0x94fff9b38c7f1bd5ull, 36, 0, 1},
    {"SameBarBatch", 0, 1, 0x85b57cf2c67fbdd6ull, 0xdf711eb77d489011ull, 0xa1c83f47f2e97c0dull, 0x77819f662ca36048ull, 36, 36, 8},
    {"SameBarBatch", 0, 0, 0x85b57cf2c67fbdd6ull, 0x14650fb0739d0383ull, 0xa1c83f47f2e97c0dull, 0x77819f662ca36048ull, 36, 0, 8},
    {"SameBarBatch", 1, 1, 0x5d280dcf47a7af5bull, 0x987f9a54f93d934eull, 0xa1c83f47f2e97c0dull, 0x6a0ac704a36bef57ull, 36, 36, 8},
    {"SameBarBatch", 1, 0, 0x5d280dcf47a7af5bull, 0x14650fb0739d0383ull, 0xa1c83f47f2e97c0dull, 0x6a0ac704a36bef57ull, 36, 0, 8},
    {"CommissionedShort", 0, 1, 0xc73e33b30bf09c29ull, 0x26a3e6d04263ad27ull, 0x35b634c9ced2d3caull, 0xeb0d9ea9f0bdfd4bull, 32, 32, 1},
    {"CommissionedShort", 0, 0, 0xc73e33b30bf09c29ull, 0x14650fb0739d0383ull, 0x35b634c9ced2d3caull, 0xeb0d9ea9f0bdfd4bull, 32, 0, 1},
    {"CommissionedShort", 1, 1, 0x350b092698bdf8fdull, 0x3fe25a6001c6c4feull, 0x35b634c9ced2d3caull, 0xd73d8e053dbf5cd2ull, 32, 32, 1},
    {"CommissionedShort", 1, 0, 0x350b092698bdf8fdull, 0x14650fb0739d0383ull, 0x35b634c9ced2d3caull, 0xd73d8e053dbf5cd2ull, 32, 0, 1},
    {"AccountFx", 0, 1, 0xd27340484425ff52ull, 0x4e596d61c552597eull, 0x3b853d0c008c3078ull, 0xea884b09a27ea73bull, 48, 48, 1},
    {"AccountFx", 0, 0, 0xd27340484425ff52ull, 0x14650fb0739d0383ull, 0x3b853d0c008c3078ull, 0xea884b09a27ea73bull, 48, 0, 1},
    {"Stream", 0, 1, 0x4f42bff80af50865ull, 0xca31c77c5a63ae01ull, 0xce38e7485b22ec04ull, 0xeb75594695696b2full, 40, 40, 5},
    {"Stream", 0, 0, 0x4f42bff80af50865ull, 0x14650fb0739d0383ull, 0xce38e7485b22ec04ull, 0xeb75594695696b2full, 40, 0, 5},
    {"Random01", 0, 1, 0xe7fdfddc42a2ae75ull, 0x0b38785d82bc8fe6ull, 0xf2ae7ef9674d4425ull, 0x16fe00b5a88e8ff2ull, 55, 55, 15},
    {"Random01", 0, 0, 0xe7fdfddc42a2ae75ull, 0x14650fb0739d0383ull, 0xf2ae7ef9674d4425ull, 0x16fe00b5a88e8ff2ull, 55, 0, 15},
    {"Random01", 1, 1, 0x64e7087d99c04c46ull, 0x1c0e119f02f64ddeull, 0x64f480bfff90f76aull, 0xb0114372fd93a01cull, 55, 55, 15},
    {"Random01", 1, 0, 0x64e7087d99c04c46ull, 0x14650fb0739d0383ull, 0x64f480bfff90f76aull, 0xb0114372fd93a01cull, 55, 0, 15},
    {"Random02", 0, 1, 0x8a2b805556d22659ull, 0xce086fbf3b8296e5ull, 0x41793244d920be3dull, 0x5221fce916020c33ull, 58, 40, 12},
    {"Random02", 0, 0, 0x8a2b805556d22659ull, 0x14650fb0739d0383ull, 0x41793244d920be3dull, 0x5221fce916020c33ull, 58, 0, 12},
    {"Random02", 1, 1, 0x844819f3aaccd232ull, 0x74fc5a6e6a256e36ull, 0x31c2efb8bb736f56ull, 0x089086fe02b53049ull, 58, 40, 12},
    {"Random02", 1, 0, 0x844819f3aaccd232ull, 0x14650fb0739d0383ull, 0x31c2efb8bb736f56ull, 0x089086fe02b53049ull, 58, 0, 12},
    {"Random03", 0, 1, 0x2dad9d24e763c932ull, 0xa3f6fa59cf96ba52ull, 0x8bd01a3594893c29ull, 0xeb202b23d902f8b8ull, 44, 44, 11},
    {"Random03", 0, 0, 0x2dad9d24e763c932ull, 0x14650fb0739d0383ull, 0x8bd01a3594893c29ull, 0xeb202b23d902f8b8ull, 44, 0, 11},
    {"Random03", 1, 1, 0x7196371dde7bb130ull, 0xbdf7be966e72b156ull, 0xde85b8fe81a28e59ull, 0x92f95865ffb213eeull, 44, 44, 11},
    {"Random03", 1, 0, 0x7196371dde7bb130ull, 0x14650fb0739d0383ull, 0xde85b8fe81a28e59ull, 0x92f95865ffb213eeull, 44, 0, 11},
    {"Random04", 0, 1, 0x9d74676f534d8b76ull, 0xdb1c5eb0cb9f6c32ull, 0x38df0af477021418ull, 0x82a419ac37909af3ull, 44, 44, 18},
    {"Random04", 0, 0, 0x9d74676f534d8b76ull, 0x14650fb0739d0383ull, 0x38df0af477021418ull, 0x82a419ac37909af3ull, 44, 0, 18},
    {"Random04", 1, 1, 0x19cdc09e2267df5aull, 0x7eca887896ce161bull, 0x28a43578727c43b5ull, 0x350e10b2e19f0b44ull, 44, 44, 18},
    {"Random04", 1, 0, 0x19cdc09e2267df5aull, 0x14650fb0739d0383ull, 0x28a43578727c43b5ull, 0x350e10b2e19f0b44ull, 44, 0, 18},
    {"Random05", 0, 1, 0xac397a37be0d4789ull, 0x64b7bb57706476b6ull, 0xf91857e72781f5aeull, 0x85274de8bea42d05ull, 58, 53, 3},
    {"Random05", 0, 0, 0xac397a37be0d4789ull, 0x14650fb0739d0383ull, 0xf91857e72781f5aeull, 0x85274de8bea42d05ull, 58, 0, 3},
    {"Random05", 1, 1, 0xbd96a4fdd0383edbull, 0xdcb784e78fba749eull, 0xc83a1288cfd7ee4dull, 0x919116731d265d3dull, 66, 53, 10},
    {"Random05", 1, 0, 0xbd96a4fdd0383edbull, 0x14650fb0739d0383ull, 0xc83a1288cfd7ee4dull, 0x919116731d265d3dull, 66, 0, 10},
    {"Random06", 0, 1, 0x7deef79bd0217f64ull, 0xd166805cc595f38eull, 0x75cd705050ee6ca5ull, 0x4c7c821f90e38cccull, 46, 46, 13},
    {"Random06", 0, 0, 0x7deef79bd0217f64ull, 0x14650fb0739d0383ull, 0x75cd705050ee6ca5ull, 0x4c7c821f90e38cccull, 46, 0, 13},
    {"Random06", 1, 1, 0x8a1bb21a5e64fed9ull, 0x7d881d1a93512848ull, 0x5f84022dee0bd36full, 0xaa8716ffbdfd11b2ull, 46, 46, 13},
    {"Random06", 1, 0, 0x8a1bb21a5e64fed9ull, 0x14650fb0739d0383ull, 0x5f84022dee0bd36full, 0xaa8716ffbdfd11b2ull, 46, 0, 13},
    {"Random07", 0, 1, 0xe6120913fe5da50dull, 0xe90b62d77e4625b7ull, 0x67ce7c56c16f12a7ull, 0xbc6f67df5b0f0b2full, 46, 46, 12},
    {"Random07", 0, 0, 0xe6120913fe5da50dull, 0x14650fb0739d0383ull, 0x67ce7c56c16f12a7ull, 0xbc6f67df5b0f0b2full, 46, 0, 12},
    {"Random07", 1, 1, 0xe2c3492a3155bf7dull, 0x0e5f1aa6cabc3a32ull, 0x9cbc1f07c0648d08ull, 0xbad1ae57e202b2d0ull, 46, 46, 12},
    {"Random07", 1, 0, 0xe2c3492a3155bf7dull, 0x14650fb0739d0383ull, 0x9cbc1f07c0648d08ull, 0xbad1ae57e202b2d0ull, 46, 0, 12},
    {"Random08", 0, 1, 0x0a3af182d33c479aull, 0xc91b237d10b90ecbull, 0x68ebab74d498abedull, 0x1930dc38f4f605fbull, 93, 59, 24},
    {"Random08", 0, 0, 0x0a3af182d33c479aull, 0x14650fb0739d0383ull, 0x68ebab74d498abedull, 0x1930dc38f4f605fbull, 93, 0, 24},
    {"Random08", 1, 1, 0xc981400f75b65614ull, 0xb37d8f41ed349758ull, 0x84ec2499efb30a9eull, 0xe48afc5e8a67e872ull, 93, 59, 24},
    {"Random08", 1, 0, 0xc981400f75b65614ull, 0x14650fb0739d0383ull, 0x84ec2499efb30a9eull, 0xe48afc5e8a67e872ull, 93, 0, 24},
    {"Random09", 0, 1, 0xaaff8dcd840f16ceull, 0x9021538022cf9b9bull, 0xcda6a598aa82b5f0ull, 0xbbe3c07366b0cbeaull, 58, 58, 2},
    {"Random09", 0, 0, 0xaaff8dcd840f16ceull, 0x14650fb0739d0383ull, 0xcda6a598aa82b5f0ull, 0xbbe3c07366b0cbeaull, 58, 0, 2},
    {"Random09", 1, 1, 0x026e829145cad6f5ull, 0x32ca6bae685cea43ull, 0x86b2bbc1465372d7ull, 0xc55c597c6508d557ull, 58, 58, 2},
    {"Random09", 1, 0, 0x026e829145cad6f5ull, 0x14650fb0739d0383ull, 0x86b2bbc1465372d7ull, 0xc55c597c6508d557ull, 58, 0, 2},
    {"Random10", 0, 1, 0x82933908492fe6aeull, 0x839a10fa6478c692ull, 0xc9ae66b6356d2649ull, 0x6907e89e2f502bd7ull, 69, 51, 23},
    {"Random10", 0, 0, 0x82933908492fe6aeull, 0x14650fb0739d0383ull, 0xc9ae66b6356d2649ull, 0x6907e89e2f502bd7ull, 69, 0, 23},
    {"Random10", 1, 1, 0x3af283057bf553afull, 0xd948d5022e691361ull, 0x724c585ad13c935dull, 0x794b361143554e09ull, 67, 51, 23},
    {"Random10", 1, 0, 0x3af283057bf553afull, 0x14650fb0739d0383ull, 0x724c585ad13c935dull, 0x794b361143554e09ull, 67, 0, 23},
    {"Random11", 0, 1, 0x9f99f3ad7d35ba23ull, 0x29a35fd6705cc48aull, 0x2f92b1af8d2cfafcull, 0x8235cad2a6dfb3f3ull, 60, 60, 3},
    {"Random11", 0, 0, 0x9f99f3ad7d35ba23ull, 0x14650fb0739d0383ull, 0x2f92b1af8d2cfafcull, 0x8235cad2a6dfb3f3ull, 60, 0, 3},
    {"Random11", 1, 1, 0xf6fb4588ed987e13ull, 0x7dcd8b5314fb7562ull, 0xad6d6d2577cca5c9ull, 0xb44ac10707d01de6ull, 60, 60, 8},
    {"Random11", 1, 0, 0xf6fb4588ed987e13ull, 0x14650fb0739d0383ull, 0xad6d6d2577cca5c9ull, 0xb44ac10707d01de6ull, 60, 0, 8},
    {"Random12", 0, 1, 0xc02247ff66b0dc87ull, 0xf39477bfa32bb689ull, 0xf60b6b66546787a7ull, 0x5f0b7200f4ecdfe1ull, 92, 62, 22},
    {"Random12", 0, 0, 0xc02247ff66b0dc87ull, 0x14650fb0739d0383ull, 0xf60b6b66546787a7ull, 0x5f0b7200f4ecdfe1ull, 92, 0, 22},
    {"Random12", 1, 1, 0xfcd62d0327e0553aull, 0xf36949e9fad408efull, 0xfc32c809c37c8290ull, 0x84a69af72aae96d9ull, 93, 62, 22},
    {"Random12", 1, 0, 0xfcd62d0327e0553aull, 0x14650fb0739d0383ull, 0xfc32c809c37c8290ull, 0x84a69af72aae96d9ull, 93, 0, 22},
    {"Random13", 0, 1, 0x53be6a77e760fe0bull, 0xe1b3dac90adb9f4bull, 0x9b711a1ad7f0af2bull, 0xafd98319ef581f61ull, 60, 60, 16},
    {"Random13", 0, 0, 0x53be6a77e760fe0bull, 0x14650fb0739d0383ull, 0x9b711a1ad7f0af2bull, 0xafd98319ef581f61ull, 60, 0, 16},
    {"Random13", 1, 1, 0xd238668589210027ull, 0xc4b7dac2b95e01fbull, 0x78cbd8f6435e4796ull, 0xb4b9d6f51a69342full, 60, 60, 16},
    {"Random13", 1, 0, 0xd238668589210027ull, 0x14650fb0739d0383ull, 0x78cbd8f6435e4796ull, 0xb4b9d6f51a69342full, 60, 0, 16},
    {"Random14", 0, 1, 0xd37abe66ab158e44ull, 0x38cb950f5ceb9d9cull, 0x4e8d14373274d558ull, 0x9d488869e9756d89ull, 49, 49, 16},
    {"Random14", 0, 0, 0xd37abe66ab158e44ull, 0x14650fb0739d0383ull, 0x4e8d14373274d558ull, 0x9d488869e9756d89ull, 49, 0, 16},
    {"Random14", 1, 1, 0xb8392469a2a0abfeull, 0xfed8161637d6c938ull, 0xbec287230f684e1dull, 0x04b4096b966c585cull, 49, 49, 16},
    {"Random14", 1, 0, 0xb8392469a2a0abfeull, 0x14650fb0739d0383ull, 0xbec287230f684e1dull, 0x04b4096b966c585cull, 49, 0, 16},
    {"Random15", 0, 1, 0x1fc67d9462b82276ull, 0x167381a97aa1c870ull, 0xc7b14196e7b743c6ull, 0x69f92f0d300a126eull, 42, 42, 12},
    {"Random15", 0, 0, 0x1fc67d9462b82276ull, 0x14650fb0739d0383ull, 0xc7b14196e7b743c6ull, 0x69f92f0d300a126eull, 42, 0, 12},
    {"Random15", 1, 1, 0xbbbd08da5449bc19ull, 0x2a9a6d807f0676baull, 0x9e6d92b9d40ff818ull, 0xe8dd6d8df584c6d6ull, 42, 42, 12},
    {"Random15", 1, 0, 0xbbbd08da5449bc19ull, 0x14650fb0739d0383ull, 0x9e6d92b9d40ff818ull, 0xe8dd6d8df584c6d6ull, 42, 0, 12},
    {"Random16", 0, 1, 0xe8851a2e0e645003ull, 0xcfab7fb0e8c24b17ull, 0x6889115fd9a0fe41ull, 0x5639dcc11e103f25ull, 54, 54, 15},
    {"Random16", 0, 0, 0xe8851a2e0e645003ull, 0x14650fb0739d0383ull, 0x6889115fd9a0fe41ull, 0x5639dcc11e103f25ull, 54, 0, 15},
    {"Random16", 1, 1, 0x431440275998e000ull, 0x90d12ea4ed48dd05ull, 0x552b70400304fd25ull, 0xf7e82ef73f56a2eeull, 54, 54, 15},
    {"Random16", 1, 0, 0x431440275998e000ull, 0x14650fb0739d0383ull, 0x552b70400304fd25ull, 0xf7e82ef73f56a2eeull, 54, 0, 15},
    {"Random17", 0, 1, 0xf4b303a1ef4696f7ull, 0x6417861b10d32340ull, 0xf3e62779123d7565ull, 0xa61332be53034674ull, 49, 47, 1},
    {"Random17", 0, 0, 0xf4b303a1ef4696f7ull, 0x14650fb0739d0383ull, 0xf3e62779123d7565ull, 0xa61332be53034674ull, 49, 0, 1},
    {"Random17", 1, 1, 0x4fbe2d70efbe24eaull, 0xaf770da711891f5aull, 0x5814b347681cd974ull, 0x6665febf6fb7f354ull, 50, 47, 2},
    {"Random17", 1, 0, 0x4fbe2d70efbe24eaull, 0x14650fb0739d0383ull, 0x5814b347681cd974ull, 0x6665febf6fb7f354ull, 50, 0, 2},
    {"Random18", 0, 1, 0xf434360cb53f77baull, 0xff5d4060a3fafb03ull, 0x5f03c99e6854a46aull, 0xe0f82ba48054cc9cull, 66, 62, 3},
    {"Random18", 0, 0, 0xf434360cb53f77baull, 0x14650fb0739d0383ull, 0x5f03c99e6854a46aull, 0xe0f82ba48054cc9cull, 66, 0, 3},
    {"Random18", 1, 1, 0x7b6ad000002b1959ull, 0x70472dcb4abe021full, 0x5f03c99e6854a46aull, 0x73a5afc061a888aaull, 66, 62, 3},
    {"Random18", 1, 0, 0x7b6ad000002b1959ull, 0x14650fb0739d0383ull, 0x5f03c99e6854a46aull, 0x73a5afc061a888aaull, 66, 0, 3},
    {"Random19", 0, 1, 0xa21f7b0b62d72170ull, 0xda14d42130450ee8ull, 0x95d7b888983ad8e6ull, 0x0bcf8b1fde6775f8ull, 40, 40, 10},
    {"Random19", 0, 0, 0xa21f7b0b62d72170ull, 0x14650fb0739d0383ull, 0x95d7b888983ad8e6ull, 0x0bcf8b1fde6775f8ull, 40, 0, 10},
    {"Random19", 1, 1, 0xceae18544190aae2ull, 0x7d8104e455ec01f8ull, 0x011bec7774b82ec9ull, 0x426c72d5bce82609ull, 40, 40, 11},
    {"Random19", 1, 0, 0xceae18544190aae2ull, 0x14650fb0739d0383ull, 0x011bec7774b82ec9ull, 0x426c72d5bce82609ull, 40, 0, 11},
    {"Random20", 0, 1, 0xad83f25bfdab1b50ull, 0x103b25042e1313deull, 0x936a146fbbfb7d4bull, 0xb27d00f43fad75a1ull, 70, 57, 21},
    {"Random20", 0, 0, 0xad83f25bfdab1b50ull, 0x14650fb0739d0383ull, 0x936a146fbbfb7d4bull, 0xb27d00f43fad75a1ull, 70, 0, 21},
    {"Random20", 1, 1, 0x4b8bcf5ef1f015c5ull, 0x6e2d790aa8a7762bull, 0x57a697ba56dd611eull, 0xd3e60d6fda04adbeull, 72, 57, 21},
    {"Random20", 1, 0, 0x4b8bcf5ef1f015c5ull, 0x14650fb0739d0383ull, 0x57a697ba56dd611eull, 0xd3e60d6fda04adbeull, 72, 0, 21},
    {"Random21", 0, 1, 0xd7b52779988b5cfaull, 0xcfddcd52fb629169ull, 0xbc89178a735678f6ull, 0xfd9b7e4dad3197b5ull, 54, 54, 18},
    {"Random21", 0, 0, 0xd7b52779988b5cfaull, 0x14650fb0739d0383ull, 0xbc89178a735678f6ull, 0xfd9b7e4dad3197b5ull, 54, 0, 18},
    {"Random21", 1, 1, 0x1c515b9e6cd109a3ull, 0xaa94d0f54845ee02ull, 0x0cb28a314aa970aeull, 0x29fa4e20de356d85ull, 54, 54, 18},
    {"Random21", 1, 0, 0x1c515b9e6cd109a3ull, 0x14650fb0739d0383ull, 0x0cb28a314aa970aeull, 0x29fa4e20de356d85ull, 54, 0, 18},
    {"Random22", 0, 1, 0x79adc16250cf8292ull, 0x2512a0afd913906aull, 0xedb01aeb6057c25bull, 0x1d49ae94182c6dc3ull, 68, 49, 13},
    {"Random22", 0, 0, 0x79adc16250cf8292ull, 0x14650fb0739d0383ull, 0xedb01aeb6057c25bull, 0x1d49ae94182c6dc3ull, 68, 0, 13},
    {"Random22", 1, 1, 0x301434ca9762fba0ull, 0x9371e473c135020full, 0x70ac53cf16690a86ull, 0xaadd4e3cb0a37251ull, 68, 49, 13},
    {"Random22", 1, 0, 0x301434ca9762fba0ull, 0x14650fb0739d0383ull, 0x70ac53cf16690a86ull, 0xaadd4e3cb0a37251ull, 68, 0, 13},
    {"Random23", 0, 1, 0xbf93319ca8e8ed61ull, 0x178099cac4dd9abfull, 0xf0e62f804df9e022ull, 0x160121fb8a6ad628ull, 56, 54, 1},
    {"Random23", 0, 0, 0xbf93319ca8e8ed61ull, 0x14650fb0739d0383ull, 0xf0e62f804df9e022ull, 0x160121fb8a6ad628ull, 56, 0, 1},
    {"Random23", 1, 1, 0x03336650fccfc186ull, 0x84fb2988132b6bdbull, 0xf0e62f804df9e022ull, 0x54a3c7db6bc08316ull, 56, 54, 1},
    {"Random23", 1, 0, 0x03336650fccfc186ull, 0x14650fb0739d0383ull, 0xf0e62f804df9e022ull, 0x54a3c7db6bc08316ull, 56, 0, 1},
    {"Random24", 0, 1, 0x21714f0414285779ull, 0x8e07ee97e3c18592ull, 0xcdafd09a1c076093ull, 0x9b4982cc0c9d5836ull, 60, 60, 16},
    {"Random24", 0, 0, 0x21714f0414285779ull, 0x14650fb0739d0383ull, 0xcdafd09a1c076093ull, 0x9b4982cc0c9d5836ull, 60, 0, 16},
    {"Random24", 1, 1, 0x104927385caf1bafull, 0xb58d2d29b9934e0cull, 0xe744bb83904b658aull, 0x892eef5960b6186eull, 60, 60, 16},
    {"Random24", 1, 0, 0x104927385caf1bafull, 0x14650fb0739d0383ull, 0xe744bb83904b658aull, 0x892eef5960b6186eull, 60, 0, 16},
    {"Random25", 0, 1, 0x7fe75851d29b5f6aull, 0xbd64625a7bcfb73cull, 0xf92cddf4fc024c49ull, 0xdbdc12e31962344aull, 44, 44, 8},
    {"Random25", 0, 0, 0x7fe75851d29b5f6aull, 0x14650fb0739d0383ull, 0xf92cddf4fc024c49ull, 0xdbdc12e31962344aull, 44, 0, 8},
    {"Random25", 1, 1, 0x49f7049727341b3cull, 0x4023f4c7efa8cdacull, 0xf92cddf4fc024c49ull, 0x1e3a08e1516ae4beull, 44, 44, 8},
    {"Random25", 1, 0, 0x49f7049727341b3cull, 0x14650fb0739d0383ull, 0xf92cddf4fc024c49ull, 0x1e3a08e1516ae4beull, 44, 0, 8},
    {"Random26", 0, 1, 0xca8049a8b96e9724ull, 0xc7e39193f4b07aa7ull, 0xd9522717c794d53bull, 0xf4e31e8175400963ull, 42, 42, 12},
    {"Random26", 0, 0, 0xca8049a8b96e9724ull, 0x14650fb0739d0383ull, 0xd9522717c794d53bull, 0xf4e31e8175400963ull, 42, 0, 12},
    {"Random26", 1, 1, 0x9c9ac48945130531ull, 0xeb4d985eb31f93e0ull, 0xf7b8acd382701dd1ull, 0xecd913dc1c2bb6edull, 42, 42, 12},
    {"Random26", 1, 0, 0x9c9ac48945130531ull, 0x14650fb0739d0383ull, 0xf7b8acd382701dd1ull, 0xecd913dc1c2bb6edull, 42, 0, 12},
    {"Random27", 0, 1, 0xa5ad84fe83acddfcull, 0x4da695a0a63ae358ull, 0xbfe2f70bda5f466dull, 0x70f7b127c522e392ull, 50, 50, 22},
    {"Random27", 0, 0, 0xa5ad84fe83acddfcull, 0x14650fb0739d0383ull, 0xbfe2f70bda5f466dull, 0x70f7b127c522e392ull, 50, 0, 22},
    {"Random27", 1, 1, 0x67f5b41cf936747aull, 0x06665f5393126836ull, 0x0bb88d26b3dc8cc1ull, 0x94de1904cefde26dull, 50, 50, 21},
    {"Random27", 1, 0, 0x67f5b41cf936747aull, 0x14650fb0739d0383ull, 0x0bb88d26b3dc8cc1ull, 0x94de1904cefde26dull, 50, 0, 21},
    {"Random28", 0, 1, 0x8b9cc3d4febe4c62ull, 0x69471bc0f4097ac1ull, 0x7ee366b7feceed4eull, 0x373d1983261f87caull, 54, 54, 19},
    {"Random28", 0, 0, 0x8b9cc3d4febe4c62ull, 0x14650fb0739d0383ull, 0x7ee366b7feceed4eull, 0x373d1983261f87caull, 54, 0, 19},
    {"Random28", 1, 1, 0x57f3120e39e241ddull, 0x0a99bf96869493a4ull, 0xee17522a5f3c15dfull, 0xe2efc2ce4ee88ff9ull, 54, 54, 19},
    {"Random28", 1, 0, 0x57f3120e39e241ddull, 0x14650fb0739d0383ull, 0xee17522a5f3c15dfull, 0xe2efc2ce4ee88ff9ull, 54, 0, 19},
    {"Random29", 0, 1, 0x89b631d5f5f01691ull, 0x167525b5e459f6a9ull, 0xe6aec4ce1ae7ccd1ull, 0xb42cb126538c6f0aull, 57, 57, 1},
    {"Random29", 0, 0, 0x89b631d5f5f01691ull, 0x14650fb0739d0383ull, 0xe6aec4ce1ae7ccd1ull, 0xb42cb126538c6f0aull, 57, 0, 1},
    {"Random29", 1, 1, 0x41cca15aeaf4b30dull, 0x5c0fe05b50ac7989ull, 0xe6aec4ce1ae7ccd1ull, 0xdaa6c53d3410e42full, 57, 57, 1},
    {"Random29", 1, 0, 0x41cca15aeaf4b30dull, 0x14650fb0739d0383ull, 0xe6aec4ce1ae7ccd1ull, 0xdaa6c53d3410e42full, 57, 0, 1},
    {"Random30", 0, 1, 0xe74142e41965c7f8ull, 0x7d6bfad3ec602d1eull, 0x260809f2d3657490ull, 0x2ff813254130dde5ull, 61, 61, 1},
    {"Random30", 0, 0, 0xe74142e41965c7f8ull, 0x14650fb0739d0383ull, 0x260809f2d3657490ull, 0x2ff813254130dde5ull, 61, 0, 1},
    {"Random30", 1, 1, 0xffbc3c35359e842dull, 0xa978037a0a737ed4ull, 0x260809f2d3657490ull, 0xb713984b2b953de4ull, 61, 61, 1},
    {"Random30", 1, 0, 0xffbc3c35359e842dull, 0x14650fb0739d0383ull, 0x260809f2d3657490ull, 0xb713984b2b953de4ull, 61, 0, 1},
    {"Random31", 0, 1, 0xc1d0985632aa85e5ull, 0xb1ff4f1ff8ce7ff6ull, 0xcad16841aad879ddull, 0x0138efa3affc4452ull, 49, 49, 9},
    {"Random31", 0, 0, 0xc1d0985632aa85e5ull, 0x14650fb0739d0383ull, 0xcad16841aad879ddull, 0x0138efa3affc4452ull, 49, 0, 9},
    {"Random31", 1, 1, 0x29c04e4a3f62fc66ull, 0xd37bee2bcbd5b472ull, 0xc42eba994cb3bc7full, 0x3e283f49a532b649ull, 49, 49, 9},
    {"Random31", 1, 0, 0x29c04e4a3f62fc66ull, 0x14650fb0739d0383ull, 0xc42eba994cb3bc7full, 0x3e283f49a532b649ull, 49, 0, 9},
    {"Random32", 0, 1, 0x69fb37996a1a6d8aull, 0x4eda1ad97257444eull, 0xb5c0211fafc19569ull, 0xf8f4a46581375bc8ull, 62, 62, 18},
    {"Random32", 0, 0, 0x69fb37996a1a6d8aull, 0x14650fb0739d0383ull, 0xb5c0211fafc19569ull, 0xf8f4a46581375bc8ull, 62, 0, 18},
    {"Random32", 1, 1, 0x6aa8d3b227f15c4cull, 0xefb2e786712329f2ull, 0xb1a7a66b620c4fd5ull, 0xe3b7a40e1641948dull, 62, 62, 19},
    {"Random32", 1, 0, 0x6aa8d3b227f15c4cull, 0x14650fb0739d0383ull, 0xb1a7a66b620c4fd5ull, 0xe3b7a40e1641948dull, 62, 0, 19},
    {"Random33", 0, 1, 0x9651f8f876e48213ull, 0x70cd8d3b24d08640ull, 0xb69ec34fc873583aull, 0x495394a49a7d3245ull, 51, 51, 13},
    {"Random33", 0, 0, 0x9651f8f876e48213ull, 0x14650fb0739d0383ull, 0xb69ec34fc873583aull, 0x495394a49a7d3245ull, 51, 0, 13},
    {"Random33", 1, 1, 0x23686cd9a3fe0637ull, 0x17ecf363cd313d8aull, 0xa51cad1078866f00ull, 0xcefd187486fda5beull, 51, 51, 13},
    {"Random33", 1, 0, 0x23686cd9a3fe0637ull, 0x14650fb0739d0383ull, 0xa51cad1078866f00ull, 0xcefd187486fda5beull, 51, 0, 13},
    {"Random34", 0, 1, 0xb0d26d6ec62ee0b2ull, 0xe24b790bed6d8209ull, 0x81535eb933e496e7ull, 0xa8799fc8644a8be8ull, 51, 51, 1},
    {"Random34", 0, 0, 0xb0d26d6ec62ee0b2ull, 0x14650fb0739d0383ull, 0x81535eb933e496e7ull, 0xa8799fc8644a8be8ull, 51, 0, 1},
    {"Random34", 1, 1, 0x40ad548ed9e2d639ull, 0x2d022bb216743a64ull, 0xa788aa75c6b931e3ull, 0x0a4527b569ac8a10ull, 51, 51, 2},
    {"Random34", 1, 0, 0x40ad548ed9e2d639ull, 0x14650fb0739d0383ull, 0xa788aa75c6b931e3ull, 0x0a4527b569ac8a10ull, 51, 0, 2},
    {"Random35", 0, 1, 0xbdcab3fd94434657ull, 0x8f07aebde3f01772ull, 0x834764512474d77cull, 0x4d118037ee1626adull, 63, 63, 24},
    {"Random35", 0, 0, 0xbdcab3fd94434657ull, 0x14650fb0739d0383ull, 0x834764512474d77cull, 0x4d118037ee1626adull, 63, 0, 24},
    {"Random35", 1, 1, 0x9f158f1f03765f97ull, 0x7ea6860a76f2a755ull, 0xce2ab725ccda6ebdull, 0x07820fe3390f4450ull, 63, 63, 24},
    {"Random35", 1, 0, 0x9f158f1f03765f97ull, 0x14650fb0739d0383ull, 0xce2ab725ccda6ebdull, 0x07820fe3390f4450ull, 63, 0, 24},
    {"Random36", 0, 1, 0xc06367cbde7ecc9aull, 0x9c13280fd6c4a76full, 0x71b837070cbf3386ull, 0x4a73a58e1bf4c419ull, 87, 60, 21},
    {"Random36", 0, 0, 0xc06367cbde7ecc9aull, 0x14650fb0739d0383ull, 0x71b837070cbf3386ull, 0x4a73a58e1bf4c419ull, 87, 0, 21},
    {"Random36", 1, 1, 0xf78394d86184426aull, 0xd52d837b120d0443ull, 0xc91ddda9c3e46569ull, 0x1e90f705be3da39full, 87, 60, 21},
    {"Random36", 1, 0, 0xf78394d86184426aull, 0x14650fb0739d0383ull, 0xc91ddda9c3e46569ull, 0x1e90f705be3da39full, 87, 0, 21},
    {"Random37", 0, 1, 0x8af4f686e8afd249ull, 0xb43a05dbbc636b7aull, 0x52bf39ca2f2765c6ull, 0x8692adfc1cddb1c4ull, 71, 45, 18},
    {"Random37", 0, 0, 0x8af4f686e8afd249ull, 0x14650fb0739d0383ull, 0x52bf39ca2f2765c6ull, 0x8692adfc1cddb1c4ull, 71, 0, 18},
    {"Random37", 1, 1, 0xe82a3b0bd1861474ull, 0x5ea014024a78fb97ull, 0x41de938f8a060981ull, 0x1ace37e580b56141ull, 70, 45, 17},
    {"Random37", 1, 0, 0xe82a3b0bd1861474ull, 0x14650fb0739d0383ull, 0x41de938f8a060981ull, 0x1ace37e580b56141ull, 70, 0, 17},
    {"Random38", 0, 1, 0xeaf05ca1aedc03d5ull, 0x80971876a1156ee8ull, 0xac12ee6aa53f424cull, 0x2d7719753c10f4bbull, 54, 54, 25},
    {"Random38", 0, 0, 0xeaf05ca1aedc03d5ull, 0x14650fb0739d0383ull, 0xac12ee6aa53f424cull, 0x2d7719753c10f4bbull, 54, 0, 25},
    {"Random38", 1, 1, 0x8285a035265d771cull, 0x0d3b927724077789ull, 0xc6a0970cf0716d7cull, 0x09336a0556a2fbb7ull, 54, 54, 25},
    {"Random38", 1, 0, 0x8285a035265d771cull, 0x14650fb0739d0383ull, 0xc6a0970cf0716d7cull, 0x09336a0556a2fbb7ull, 54, 0, 25},
    {"Random39", 0, 1, 0xc989defa2d8cff00ull, 0x31db41d49a36aaa6ull, 0xc193386eb35a8e65ull, 0xb8c88da9531702d9ull, 44, 44, 19},
    {"Random39", 0, 0, 0xc989defa2d8cff00ull, 0x14650fb0739d0383ull, 0xc193386eb35a8e65ull, 0xb8c88da9531702d9ull, 44, 0, 19},
    {"Random39", 1, 1, 0x588cc94ab848f7e6ull, 0x3b1478373127afc0ull, 0xeea051f0749b0428ull, 0xbdab759ad2c5865cull, 44, 44, 19},
    {"Random39", 1, 0, 0x588cc94ab848f7e6ull, 0x14650fb0739d0383ull, 0xeea051f0749b0428ull, 0xbdab759ad2c5865cull, 44, 0, 19},
    {"Random40", 0, 1, 0xc4f976129a95a285ull, 0x389162a99c89a49cull, 0x78c15a2f88a24807ull, 0xcafbb488b92f9edaull, 77, 53, 18},
    {"Random40", 0, 0, 0xc4f976129a95a285ull, 0x14650fb0739d0383ull, 0x78c15a2f88a24807ull, 0xcafbb488b92f9edaull, 77, 0, 18},
    {"Random40", 1, 1, 0x1cc6041a6a6fcdcdull, 0x33a043a5f0e45941ull, 0x41975079a7cc855bull, 0x798fe8d7596aaa9cull, 78, 53, 19},
    {"Random40", 1, 0, 0x1cc6041a6a6fcdcdull, 0x14650fb0739d0383ull, 0x41975079a7cc855bull, 0x798fe8d7596aaa9cull, 78, 0, 19},
    {"Random41", 0, 1, 0xb4b89e111611f4ceull, 0xea3d6d0c77bc7e76ull, 0xf6e95f1e1a0264bcull, 0x980f8a18b3d80a23ull, 51, 40, 6},
    {"Random41", 0, 0, 0xb4b89e111611f4ceull, 0x14650fb0739d0383ull, 0xf6e95f1e1a0264bcull, 0x980f8a18b3d80a23ull, 51, 0, 6},
    {"Random41", 1, 1, 0xf15d27dad1575f3bull, 0x9245b7b247af4cc1ull, 0xf6e95f1e1a0264bcull, 0xc730d99532f512d8ull, 51, 40, 6},
    {"Random41", 1, 0, 0xf15d27dad1575f3bull, 0x14650fb0739d0383ull, 0xf6e95f1e1a0264bcull, 0xc730d99532f512d8ull, 51, 0, 6},
    {"Random42", 0, 1, 0x0da9b26d3847ab1full, 0x9c9329bd9adc3676ull, 0xd4d5dd88f1bc7129ull, 0x49a0634035589083ull, 60, 60, 12},
    {"Random42", 0, 0, 0x0da9b26d3847ab1full, 0x14650fb0739d0383ull, 0xd4d5dd88f1bc7129ull, 0x49a0634035589083ull, 60, 0, 12},
    {"Random42", 1, 1, 0xbd61639c42be9cb3ull, 0x5d2fe8dd7d35b222ull, 0x292b9553961fa163ull, 0xa80c78dbeddc4468ull, 60, 60, 12},
    {"Random42", 1, 0, 0xbd61639c42be9cb3ull, 0x14650fb0739d0383ull, 0x292b9553961fa163ull, 0xa80c78dbeddc4468ull, 60, 0, 12},
    {"Random43", 0, 1, 0xc521512ba92a54c9ull, 0x5993bd8ceee91044ull, 0xbd8ba613082e0da5ull, 0x5fbe995d73a8bff0ull, 63, 63, 14},
    {"Random43", 0, 0, 0xc521512ba92a54c9ull, 0x14650fb0739d0383ull, 0xbd8ba613082e0da5ull, 0x5fbe995d73a8bff0ull, 63, 0, 14},
    {"Random43", 1, 1, 0xa9607e56c7e4b193ull, 0x941becccb8fe75ceull, 0x572d3f9f9521e7caull, 0xfdc00d0d1ff0cbfbull, 63, 63, 14},
    {"Random43", 1, 0, 0xa9607e56c7e4b193ull, 0x14650fb0739d0383ull, 0x572d3f9f9521e7caull, 0xfdc00d0d1ff0cbfbull, 63, 0, 14},
    {"Random44", 0, 1, 0x971e7c4a38e301ddull, 0x55249f0ae51722feull, 0x316199af1a335dbfull, 0x5a13570337a638b5ull, 69, 47, 15},
    {"Random44", 0, 0, 0x971e7c4a38e301ddull, 0x14650fb0739d0383ull, 0x316199af1a335dbfull, 0x5a13570337a638b5ull, 69, 0, 15},
    {"Random44", 1, 1, 0x3ee064c4f943e93aull, 0xbc62822019ded427ull, 0x9f0e22f8fb2632f6ull, 0xc18096028ba7be64ull, 69, 47, 15},
    {"Random44", 1, 0, 0x3ee064c4f943e93aull, 0x14650fb0739d0383ull, 0x9f0e22f8fb2632f6ull, 0xc18096028ba7be64ull, 69, 0, 15},
    {"Random45", 0, 1, 0xa155b80e37d41cf2ull, 0xbacd32f5d8b94030ull, 0xf7b0f07cda17c110ull, 0x598c179a2d5c735cull, 99, 63, 25},
    {"Random45", 0, 0, 0xa155b80e37d41cf2ull, 0x14650fb0739d0383ull, 0xf7b0f07cda17c110ull, 0x598c179a2d5c735cull, 99, 0, 25},
    {"Random45", 1, 1, 0x9644e4dd1f925cdfull, 0x2cc38b54d184cf0aull, 0x797aff1ec9239dffull, 0x290739687f68f174ull, 99, 63, 24},
    {"Random45", 1, 0, 0x9644e4dd1f925cdfull, 0x14650fb0739d0383ull, 0x797aff1ec9239dffull, 0x290739687f68f174ull, 99, 0, 24},
    {"Random46", 0, 1, 0xa74f65268d41a1bbull, 0xfc4e6864d98d62c0ull, 0xfaba8060a07d20fdull, 0x7097cd184da8a4dcull, 52, 52, 1},
    {"Random46", 0, 0, 0xa74f65268d41a1bbull, 0x14650fb0739d0383ull, 0xfaba8060a07d20fdull, 0x7097cd184da8a4dcull, 52, 0, 1},
    {"Random46", 1, 1, 0xb1406e1f21710782ull, 0xe0b910f752cbc3adull, 0xfaba8060a07d20fdull, 0x2539296694529005ull, 52, 52, 1},
    {"Random46", 1, 0, 0xb1406e1f21710782ull, 0x14650fb0739d0383ull, 0xfaba8060a07d20fdull, 0x2539296694529005ull, 52, 0, 1},
    {"Random47", 0, 1, 0x214dc1308457a01aull, 0xe293c7196f5c9f95ull, 0x072cedccf12d6a31ull, 0x8176d644a621a19eull, 57, 57, 13},
    {"Random47", 0, 0, 0x214dc1308457a01aull, 0x14650fb0739d0383ull, 0x072cedccf12d6a31ull, 0x8176d644a621a19eull, 57, 0, 13},
    {"Random47", 1, 1, 0xfe949f5ba003397dull, 0xe112a9940c547399ull, 0x33811b67f07874f3ull, 0x9ee620e1bf252639ull, 57, 57, 13},
    {"Random47", 1, 0, 0xfe949f5ba003397dull, 0x14650fb0739d0383ull, 0x33811b67f07874f3ull, 0x9ee620e1bf252639ull, 57, 0, 13},
    {"Random48", 0, 1, 0x21c0bf9ae910d3acull, 0xe2d98f0ffb5c1b96ull, 0xda74ceb1a9e7d904ull, 0x1e560afd06471537ull, 48, 48, 8},
    {"Random48", 0, 0, 0x21c0bf9ae910d3acull, 0x14650fb0739d0383ull, 0xda74ceb1a9e7d904ull, 0x1e560afd06471537ull, 48, 0, 8},
    {"Random48", 1, 1, 0x4fd90c8ca1025428ull, 0x7706d5e9ebfd9226ull, 0x0876fbb01d747e2dull, 0xee01efa6d7b40515ull, 48, 48, 8},
    {"Random48", 1, 0, 0x4fd90c8ca1025428ull, 0x14650fb0739d0383ull, 0x0876fbb01d747e2dull, 0xee01efa6d7b40515ull, 48, 0, 8},
};
