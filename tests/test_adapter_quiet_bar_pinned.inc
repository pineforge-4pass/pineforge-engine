// expectation corrected (F09, 1 of 276 rows), because the REALTIME range-end
// hook no longer re-marks the stored point or re-folds hashed extremes; harvested
// against 227c2236 and the corrected 8da00ac9 runtime tree. Only the recorded-row
// digest moves; boundary/trade/final hashes and counts stay identical:
//   Stream/0/1: rows 0xca31c77c5a63ae01ull -> 0x8c7324fade164a50ull
// expectation corrected (G + L source money fold, 110 of 276 rows), because the source layer's fold (pineforge-source-adapter/v4) now folds the adapter's G + L source money (gains and losses, two realized-money accumulators) wherever their sum differs from the engine's running net profit; every closed trade is unchanged in every Trade field, as are the counts and net profits; harvested with this TU's own switch on the tree that adds the G + L money:
//   Random02/0/1: boundary 0x23c5ef1dd3dd7befull -> 0x0b6509e69384691aull; rows 0x9d6061d013de4f2dull -> 0x2446e9ce790e8950ull
//   Random02/0/0: boundary 0x23c5ef1dd3dd7befull -> 0x0b6509e69384691aull
//   Random02/1/1: boundary 0xeda8dd9b04dd5171ull -> 0xd9b975ecbba66cc1ull; rows 0x92dae6f76960708cull -> 0x04099bcd77418920ull
//   Random02/1/0: boundary 0xeda8dd9b04dd5171ull -> 0xd9b975ecbba66cc1ull
//   Random03/0/1: boundary 0x8efdccf7ffce778full -> 0x55f5fbb1ad46af78ull; rows 0x4e448bd28567a84eull -> 0x91c200dd67f2c6d3ull; final 0xf89c3da9a246c56aull -> 0xae6a17ed7812bfd1ull
//   Random03/0/0: boundary 0x8efdccf7ffce778full -> 0x55f5fbb1ad46af78ull; final 0xf89c3da9a246c56aull -> 0xae6a17ed7812bfd1ull
//   Random03/1/1: boundary 0xa984da78281a6f67ull -> 0xb587dcd78562fc33ull; rows 0x23192b37dc9451faull -> 0xefbf3dbd57d0d024ull; final 0x58cc7ecc091d34d7ull -> 0x35ee7a28303aa6c5ull
//   Random03/1/0: boundary 0xa984da78281a6f67ull -> 0xb587dcd78562fc33ull; final 0x58cc7ecc091d34d7ull -> 0x35ee7a28303aa6c5ull
//   Random04/0/1: boundary 0xd280c250b0a5c7e1ull -> 0x1ac71820aa7472f6ull; rows 0x3f346416821514d1ull -> 0xc41eab81ee457dd2ull; final 0x4baafe4f5ce072deull -> 0x697574dbf62c8ea2ull
//   Random04/0/0: boundary 0xd280c250b0a5c7e1ull -> 0x1ac71820aa7472f6ull; final 0x4baafe4f5ce072deull -> 0x697574dbf62c8ea2ull
//   Random04/1/1: boundary 0xb7bd621548544961ull -> 0x6cd2901c7ff5ce8eull; rows 0x44175452c39d8931ull -> 0xa5a45a69ac5a66ecull; final 0x8a1e40bc7b77aca8ull -> 0x65a20dcd9f1980a1ull
//   Random04/1/0: boundary 0xb7bd621548544961ull -> 0x6cd2901c7ff5ce8eull; final 0x8a1e40bc7b77aca8ull -> 0x65a20dcd9f1980a1ull
//   Random05/1/1: boundary 0xa01aa2d37370ef27ull -> 0xdf482d00a8b5e374ull
//   Random05/1/0: boundary 0xa01aa2d37370ef27ull -> 0xdf482d00a8b5e374ull
//   Random06/0/1: boundary 0x3f691ea6c5b82a94ull -> 0x93905ddd8a40a8e9ull; rows 0x95903f8eb6032786ull -> 0xab6c94fe61d304bcull; final 0x07dd12270224ae8aull -> 0xdbab6a336c90c65eull
//   Random06/0/0: boundary 0x3f691ea6c5b82a94ull -> 0x93905ddd8a40a8e9ull; final 0x07dd12270224ae8aull -> 0xdbab6a336c90c65eull
//   Random06/1/1: boundary 0xed98de32895886baull -> 0x09d7fe0f1aa61b3full; rows 0xc10b04b6ed43f4eaull -> 0x676cd5f3f8ed4300ull; final 0x9cbe3e2d9e23fb08ull -> 0x1347015357d2d8a6ull
//   Random06/1/0: boundary 0xed98de32895886baull -> 0x09d7fe0f1aa61b3full; final 0x9cbe3e2d9e23fb08ull -> 0x1347015357d2d8a6ull
//   Random07/0/1: boundary 0x38a6916856b137f8ull -> 0xdf808db11dde4c8aull; rows 0x3aa6e1f6474d849bull -> 0xbf2e34a504ca5333ull
//   Random07/0/0: boundary 0x38a6916856b137f8ull -> 0xdf808db11dde4c8aull
//   Random07/1/1: boundary 0x9bff9a3da19955ccull -> 0x08510ec5518f91a6ull; rows 0xf21f6b0c16a83c13ull -> 0x6082d5a6943988d9ull
//   Random07/1/0: boundary 0x9bff9a3da19955ccull -> 0x08510ec5518f91a6ull
//   Random08/0/1: boundary 0xeea57cda5350bb04ull -> 0x940251d9fc1847a8ull; rows 0x2f1e61ea38fb2675ull -> 0x05329eca8b44a2ecull; final 0x88f868b2637805ecull -> 0x58a481d75ef5c17cull
//   Random08/0/0: boundary 0xeea57cda5350bb04ull -> 0x940251d9fc1847a8ull; final 0x88f868b2637805ecull -> 0x58a481d75ef5c17cull
//   Random08/1/1: boundary 0xd797b030f1d2f01eull -> 0x8f792d51d286e4caull; rows 0x41b80b51926bd33eull -> 0xe4faba20ecef9864ull
//   Random08/1/0: boundary 0xd797b030f1d2f01eull -> 0x8f792d51d286e4caull
//   Random10/0/1: boundary 0xb564a90c13cde858ull -> 0x43507c7025af0a6full; rows 0x0b6dc08fd115fe0aull -> 0xe11899f309400695ull; final 0xb08edbbce30e6189ull -> 0x05050a7f6f43f8deull
//   Random10/0/0: boundary 0xb564a90c13cde858ull -> 0x43507c7025af0a6full; final 0xb08edbbce30e6189ull -> 0x05050a7f6f43f8deull
//   Random10/1/1: boundary 0xfe6e546d4d6d57a3ull -> 0x078823e295aa1e5aull; rows 0x3e2f50c0d5e7339full -> 0x074d774449e4001eull; final 0x71c635959f1365eeull -> 0x14c1017cb42c82adull
//   Random10/1/0: boundary 0xfe6e546d4d6d57a3ull -> 0x078823e295aa1e5aull; final 0x71c635959f1365eeull -> 0x14c1017cb42c82adull
//   Random11/1/1: boundary 0x3812719fa8195320ull -> 0x8e46e1ef7751abeeull; rows 0xeac4605f7e20f62aull -> 0x7edce871e6834062ull; final 0x2e6025586772f3c4ull -> 0xfc8d53a8d4020ceeull
//   Random11/1/0: boundary 0x3812719fa8195320ull -> 0x8e46e1ef7751abeeull; final 0x2e6025586772f3c4ull -> 0xfc8d53a8d4020ceeull
//   Random13/0/1: boundary 0x925f08aa970fd4d1ull -> 0xa86dd8023f9dfc7full; rows 0xb5423ffc76e2442aull -> 0x39207687f52009f5ull; final 0x6700d4ad6da7f598ull -> 0xbeb50a875a54f142ull
//   Random13/0/0: boundary 0x925f08aa970fd4d1ull -> 0xa86dd8023f9dfc7full; final 0x6700d4ad6da7f598ull -> 0xbeb50a875a54f142ull
//   Random13/1/1: boundary 0x208d9b6671c1a676ull -> 0xdcc52e873a9194c5ull; rows 0xf16f31eb6a3e9cb5ull -> 0x9c210c2c89c8ff5bull
//   Random13/1/0: boundary 0x208d9b6671c1a676ull -> 0xdcc52e873a9194c5ull
//   Random15/0/1: boundary 0x5716fadc8c4b0d5bull -> 0x51567fd50a48fee9ull; rows 0x68620b60c6c34a86ull -> 0x058a59750f5c2edaull
//   Random15/0/0: boundary 0x5716fadc8c4b0d5bull -> 0x51567fd50a48fee9ull
//   Random15/1/1: boundary 0xb23d0b67238fd1e7ull -> 0xef7fdd2fa0e6a69cull; rows 0xcedba8eab255eaafull -> 0x1f7c484904cf76a3ull; final 0x368f2569225769bfull -> 0x2bfb1d08fb552fb9ull
//   Random15/1/0: boundary 0xb23d0b67238fd1e7ull -> 0xef7fdd2fa0e6a69cull; final 0x368f2569225769bfull -> 0x2bfb1d08fb552fb9ull
//   Random18/0/1: boundary 0xe23600f528bcdd7bull -> 0xf2e21fb21175ff16ull; rows 0xebd5ab75ab65addeull -> 0x8f91d573e0c6257cull; final 0xe325154611af7d9full -> 0x6da1d912cf25ecc7ull
//   Random18/0/0: boundary 0xe23600f528bcdd7bull -> 0xf2e21fb21175ff16ull; final 0xe325154611af7d9full -> 0x6da1d912cf25ecc7ull
//   Random20/0/1: boundary 0x240069001bcbada3ull -> 0x7129359fdae8391bull; rows 0x213f6b3cc7fb7912ull -> 0x8d4b2fa5d20fbc3eull; final 0x330197a267ccc229ull -> 0x6dcb4a3e435ba6e4ull
//   Random20/0/0: boundary 0x240069001bcbada3ull -> 0x7129359fdae8391bull; final 0x330197a267ccc229ull -> 0x6dcb4a3e435ba6e4ull
//   Random20/1/1: boundary 0xc31c3a969214a2c4ull -> 0xe2b19769791734a2ull; rows 0xd448e6522f1d5b65ull -> 0x9afc74209747e37bull
//   Random20/1/0: boundary 0xc31c3a969214a2c4ull -> 0xe2b19769791734a2ull
//   Random21/0/1: boundary 0x2650b37073d95820ull -> 0xafadb761afcc4cddull; rows 0x0910aa3a301640a8ull -> 0x81e0f928e01ffa95ull; final 0x473fbafaa3ef43e4ull -> 0x8b2e49ad4319212eull
//   Random21/0/0: boundary 0x2650b37073d95820ull -> 0xafadb761afcc4cddull; final 0x473fbafaa3ef43e4ull -> 0x8b2e49ad4319212eull
//   Random21/1/1: boundary 0x6f8f15c5a8b2e857ull -> 0x37d4b58e95ac4896ull; rows 0x0de50cd113ebf7abull -> 0xd17c057281827be5ull
//   Random21/1/0: boundary 0x6f8f15c5a8b2e857ull -> 0x37d4b58e95ac4896ull
//   Random22/0/1: boundary 0xe23be045f9a8abe6ull -> 0x5dfb44cfc3fa35a6ull; rows 0x8679c8d0972904a7ull -> 0x2198153265e4d6d8ull
//   Random22/0/0: boundary 0xe23be045f9a8abe6ull -> 0x5dfb44cfc3fa35a6ull
//   Random22/1/1: boundary 0x25eb6809c245abe1ull -> 0x26a6ded9a33ac9c7ull; rows 0xb4c0825d0260f461ull -> 0x32843b4165ed0d65ull; final 0x2e15840b8b10ee7eull -> 0xc108274da5752719ull
//   Random22/1/0: boundary 0x25eb6809c245abe1ull -> 0x26a6ded9a33ac9c7ull; final 0x2e15840b8b10ee7eull -> 0xc108274da5752719ull
//   Random24/0/1: boundary 0x10955313a67b52caull -> 0x547d2e8b53c15debull; rows 0x00617e954b379daeull -> 0xad45cc9db359421dull; final 0xcf3fa51fbfa4ea2dull -> 0x2d25268ecca7ff93ull
//   Random24/0/0: boundary 0x10955313a67b52caull -> 0x547d2e8b53c15debull; final 0xcf3fa51fbfa4ea2dull -> 0x2d25268ecca7ff93ull
//   Random24/1/1: boundary 0xe2c892bee5269be4ull -> 0x2a7ede136d4862d9ull; rows 0xf232e9ac090d2ed4ull -> 0xccdca4ae67d9fc98ull; final 0xda5a2e35ccc1717dull -> 0xd12b86e200f3afc2ull
//   Random24/1/0: boundary 0xe2c892bee5269be4ull -> 0x2a7ede136d4862d9ull; final 0xda5a2e35ccc1717dull -> 0xd12b86e200f3afc2ull
//   Random26/0/1: boundary 0xd811ace52ecfe7f9ull -> 0x059cd95973056346ull; rows 0xa0b443d355db01ecull -> 0x84bc9696eadb0bbfull; final 0x8ca71da42a210e76ull -> 0x980627b1675b1efcull
//   Random26/0/0: boundary 0xd811ace52ecfe7f9ull -> 0x059cd95973056346ull; final 0x8ca71da42a210e76ull -> 0x980627b1675b1efcull
//   Random26/1/1: boundary 0x3c8cda44b51df504ull -> 0x0255fa624dfed493ull; rows 0x0621b353b516fc75ull -> 0x4aa6b978b4bb9995ull; final 0xfc6f4792a0dbc9ddull -> 0xa41a189bbd93190bull
//   Random26/1/0: boundary 0x3c8cda44b51df504ull -> 0x0255fa624dfed493ull; final 0xfc6f4792a0dbc9ddull -> 0xa41a189bbd93190bull
//   Random27/0/1: boundary 0xd129482ad0f78521ull -> 0x7eb9ce984b56586cull; rows 0xd841a947af8d8c5bull -> 0x1769e70c36959215ull; final 0x5d34c684aa5301e2ull -> 0x11c88e4bb89b615full
//   Random27/0/0: boundary 0xd129482ad0f78521ull -> 0x7eb9ce984b56586cull; final 0x5d34c684aa5301e2ull -> 0x11c88e4bb89b615full
//   Random27/1/1: boundary 0xe654b5dc971ce9e4ull -> 0x4bc4e65f6de46606ull; rows 0xbdecb995e70b2952ull -> 0x0e9065156ad84593ull; final 0x0ea37f757d7a4501ull -> 0xa74668b564b145f4ull
//   Random27/1/0: boundary 0xe654b5dc971ce9e4ull -> 0x4bc4e65f6de46606ull; final 0x0ea37f757d7a4501ull -> 0xa74668b564b145f4ull
//   Random31/0/1: boundary 0x2f48a67f2d7f7aafull -> 0x1003f1ca9d62740full; rows 0xde560fce49effa1dull -> 0x5c48e4c1bfa57985ull; final 0xcebcd23dca304ebaull -> 0xc1eb7c94186e233bull
//   Random31/0/0: boundary 0x2f48a67f2d7f7aafull -> 0x1003f1ca9d62740full; final 0xcebcd23dca304ebaull -> 0xc1eb7c94186e233bull
//   Random31/1/1: boundary 0xd96c881a3927f935ull -> 0xe52214dfd2bcc0ccull; rows 0x5d858d37d39812bcull -> 0xbe7a3e7d201789eeull
//   Random31/1/0: boundary 0xd96c881a3927f935ull -> 0xe52214dfd2bcc0ccull
//   Random33/0/1: boundary 0x3800188d78bf38d1ull -> 0xa5d5f0be4a174ee0ull; rows 0xd9e3121713edfa7cull -> 0x946aee715f170460ull; final 0xdcdc0acfec99025eull -> 0x13b97ad10aa2a75full
//   Random33/0/0: boundary 0x3800188d78bf38d1ull -> 0xa5d5f0be4a174ee0ull; final 0xdcdc0acfec99025eull -> 0x13b97ad10aa2a75full
//   Random33/1/1: boundary 0x02f7237eeebc6e8eull -> 0x6d63842ccde05cf6ull; rows 0x127cd64c5eb937b5ull -> 0x538032f14405b847ull
//   Random33/1/0: boundary 0x02f7237eeebc6e8eull -> 0x6d63842ccde05cf6ull
//   Random35/0/1: boundary 0x7f66b48075be86e0ull -> 0x9cb5b0a5def223b9ull; rows 0x2b733b1f98259c66ull -> 0x33f31b56f2e5e7eeull; final 0x8aa2580a37361d78ull -> 0x7f45160be4b532e0ull
//   Random35/0/0: boundary 0x7f66b48075be86e0ull -> 0x9cb5b0a5def223b9ull; final 0x8aa2580a37361d78ull -> 0x7f45160be4b532e0ull
//   Random35/1/1: boundary 0xab532904d5ee771cull -> 0x3da4a7074c44ca84ull; rows 0x7c24fd8660f9e971ull -> 0x7c4219c074c408a9ull; final 0xe3b1d10f77652252ull -> 0xc172861c22c157e4ull
//   Random35/1/0: boundary 0xab532904d5ee771cull -> 0x3da4a7074c44ca84ull; final 0xe3b1d10f77652252ull -> 0xc172861c22c157e4ull
//   Random36/0/1: boundary 0xa88ba3e2cf6d5a61ull -> 0x31a11ede5003bbbbull; rows 0x5a53d43688153af9ull -> 0x9cd436ebeb263f3aull; final 0x3eff2b11a8feff91ull -> 0xa9546e23fecd6770ull
//   Random36/0/0: boundary 0xa88ba3e2cf6d5a61ull -> 0x31a11ede5003bbbbull; final 0x3eff2b11a8feff91ull -> 0xa9546e23fecd6770ull
//   Random36/1/1: boundary 0x0295357029345943ull -> 0xa1d744b27afae188ull; rows 0x75045cc6617d2c6full -> 0xde0e62f88b145437ull; final 0x72a0bcaba87a9a73ull -> 0xbe151812dcc138beull
//   Random36/1/0: boundary 0x0295357029345943ull -> 0xa1d744b27afae188ull; final 0x72a0bcaba87a9a73ull -> 0xbe151812dcc138beull
//   Random38/0/1: boundary 0xd77b31ee6a5f0d3dull -> 0x6d9025b5cc19eae3ull; rows 0x6bf2b9d6db115b6aull -> 0x8c8ed908f07f4a43ull; final 0x15199290c0825656ull -> 0x19d253ed4d87496cull
//   Random38/0/0: boundary 0xd77b31ee6a5f0d3dull -> 0x6d9025b5cc19eae3ull; final 0x15199290c0825656ull -> 0x19d253ed4d87496cull
//   Random38/1/1: boundary 0xd8d4999529bd4698ull -> 0x901689f14879fa92ull; rows 0xa7a929705335a6eaull -> 0xfd9d2b3590ae297bull; final 0x44d9b4717c2b74f9ull -> 0x2e7ba5256d8f738full
//   Random38/1/0: boundary 0xd8d4999529bd4698ull -> 0x901689f14879fa92ull; final 0x44d9b4717c2b74f9ull -> 0x2e7ba5256d8f738full
//   Random39/0/1: boundary 0x3c7109829604d779ull -> 0xc21be283ee23472cull; rows 0x6ca907bbf39fcc2eull -> 0xae74b0605e1e307bull
//   Random39/0/0: boundary 0x3c7109829604d779ull -> 0xc21be283ee23472cull
//   Random39/1/1: boundary 0x38efcf41a8bf31d7ull -> 0x70762b6b63161ef6ull; rows 0x5b21144661b2d54eull -> 0xb0c87605b9a7a140ull
//   Random39/1/0: boundary 0x38efcf41a8bf31d7ull -> 0x70762b6b63161ef6ull
//   Random42/0/1: boundary 0x3cb4ae7fd9b48f07ull -> 0x9173880eb691dc53ull; rows 0xd0b0aa6cde5345d3ull -> 0x43efd628f365cbadull; final 0x89aa51f9506b13c1ull -> 0xd0b08459abc51bf7ull
//   Random42/0/0: boundary 0x3cb4ae7fd9b48f07ull -> 0x9173880eb691dc53ull; final 0x89aa51f9506b13c1ull -> 0xd0b08459abc51bf7ull
//   Random42/1/1: boundary 0xbec67b6bcab320e8ull -> 0xbcddb4b358b50c4cull; rows 0xb82212e2d2983c2dull -> 0x413c691f4f4fd50full; final 0xfaa46e405b7f1967ull -> 0x66ef4c009ccb85f0ull
//   Random42/1/0: boundary 0xbec67b6bcab320e8ull -> 0xbcddb4b358b50c4cull; final 0xfaa46e405b7f1967ull -> 0x66ef4c009ccb85f0ull
//   Random43/0/1: boundary 0x0ef1f47045923638ull -> 0x2f3ce665257b740eull; rows 0xbfcd107676ac417aull -> 0x46e64aad86cdcfc5ull; final 0x7dc885057ff3ae16ull -> 0xa145a874e9f3b6cbull
//   Random43/0/0: boundary 0x0ef1f47045923638ull -> 0x2f3ce665257b740eull; final 0x7dc885057ff3ae16ull -> 0xa145a874e9f3b6cbull
//   Random43/1/1: boundary 0xfa436c851909ef62ull -> 0x1972980945f48fb2ull; rows 0xc52d7c033c01835bull -> 0x08e6fba957ca09e2ull; final 0xf0872b36f0c0478dull -> 0x22b7231fc68e7036ull
//   Random43/1/0: boundary 0xfa436c851909ef62ull -> 0x1972980945f48fb2ull; final 0xf0872b36f0c0478dull -> 0x22b7231fc68e7036ull
//   Random44/0/1: boundary 0x428e0523687f91bbull -> 0x26218306c50a6e9aull; rows 0xf093f4cb3b885975ull -> 0x00643aa994b8e3bdull; final 0x36a58af8445b0a33ull -> 0xca3da7b6fb9da968ull
//   Random44/0/0: boundary 0x428e0523687f91bbull -> 0x26218306c50a6e9aull; final 0x36a58af8445b0a33ull -> 0xca3da7b6fb9da968ull
//   Random44/1/1: boundary 0xfc8f7a8414d17cf7ull -> 0x22b7b15bf121d4a5ull; rows 0x0f9a6a6588e48ae1ull -> 0x0470d98b9dea358eull; final 0xb7c273b697699ff6ull -> 0x2a988cf4eece4123ull
//   Random44/1/0: boundary 0xfc8f7a8414d17cf7ull -> 0x22b7b15bf121d4a5ull; final 0xb7c273b697699ff6ull -> 0x2a988cf4eece4123ull
//   Random45/0/1: boundary 0xd320a226c9314fbeull -> 0xbcf622485b8343dfull; rows 0x18dea9ebb76ac827ull -> 0xba395bcb3696a8a7ull; final 0x39e9c0cee5d02cb2ull -> 0x7e8faadf85fe2805ull
//   Random45/0/0: boundary 0xd320a226c9314fbeull -> 0xbcf622485b8343dfull; final 0x39e9c0cee5d02cb2ull -> 0x7e8faadf85fe2805ull
//   Random45/1/1: boundary 0xba78b213363e7453ull -> 0xbe5fe3159d86e0cfull; rows 0x32c6f7cdce116f9dull -> 0x7fdcbc656d6928b0ull; final 0x5afa968b5efc152dull -> 0x2be21a27f036d41bull
//   Random45/1/0: boundary 0xba78b213363e7453ull -> 0xbe5fe3159d86e0cfull; final 0x5afa968b5efc152dull -> 0x2be21a27f036d41bull
//   Random48/0/1: boundary 0xa9cdc4fdbf2711f4ull -> 0x1d605fd446942e1eull; rows 0x626cd0f4b7c32e80ull -> 0xad2d1f5f1ec6eff2ull; final 0x6964de82cf42b764ull -> 0x78ecfaf2273e0f0dull
//   Random48/0/0: boundary 0xa9cdc4fdbf2711f4ull -> 0x1d605fd446942e1eull; final 0x6964de82cf42b764ull -> 0x78ecfaf2273e0f0dull
//   Random48/1/1: boundary 0x65ff21f3229cca95ull -> 0x8cfb96ec8a97eb1bull; rows 0x4e9b982f9ab665f5ull -> 0x151d1b2f00cbeffeull
//   Random48/1/0: boundary 0x65ff21f3229cca95ull -> 0x8cfb96ec8a97eb1bull
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
// R5 lane PAR-ORDERS-3 moves these runs' trade digests (INT27: the behaviour
// half of the lane's hash commit b65cee22, which says why each moves: finding
// 4, bc5f7a21, on the chart path; CalcOnFills keeps every trade and renumbers
// Trade::entry_incarnation). Harvested with PINEFORGE_QUIET_BAR_HARVEST on
// the integrated tree, where INT26's picks had moved the same runs' trade
// digests, so the digests are this tree's, not the lane's; the boundary counts
// equal the lane's. Their hash values move once, in the INT27 v19 hash re-pin:
//   CalcOnFills/0/1: trades 0x87698c8f4050ce14 -> 0x6f29557db540e116
//   CalcOnFills/0/0: trades 0x87698c8f4050ce14 -> 0x6f29557db540e116
//   Random10/0/1: trades 0xc9ae66b6356d2649 -> 0xaae6484f97956d07, boundary_count 69 -> 67
//   Random10/0/0: trades 0xc9ae66b6356d2649 -> 0xaae6484f97956d07, boundary_count 69 -> 67
//   Random20/0/1: trades 0x936a146fbbfb7d4b -> 0x4ae8ba38071c5c60, boundary_count 70 -> 69
//   Random20/0/0: trades 0x936a146fbbfb7d4b -> 0x4ae8ba38071c5c60, boundary_count 70 -> 69
// expectation corrected (INT27 v19 hash re-pin, 15 values over 6 rows), because the integrated tree's picks move v19 hash values inside the unreleased epoch: R5 lane PAR-ORDERS-3's recalculation rules (findings 1-2, an exit placed on a fill at a bar's extreme reads that fill; finding 4, after the matcher's own-level fill on a bar's first extreme the recalculation's orders act from that extreme); old values are INT26's pins, which every INT27 pick before PAR-ORDERS-3 kept (0631e045 reproduces each), harvested with this TU's own switch on the integrated tree and at every PAR-ORDERS-3 commit; this host projects one fixed execution hash, so the continuation does not enter; no trade, trade digest or count moves in this commit (the trade digests and boundary counts PAR-ORDERS-3 moves are pinned by its behaviour half above):
//   CalcOnFills/0/1: boundary 0xbd9feba11a355c84ull -> 0x109b8bbd2db3c39cull, rows 0xbd62a54cd2c27075ull -> 0x4990637dd008c142ull, final_hash 0x8e716ca55979f5e4ull -> 0x23caae4465e49b8dull [PAR-ORDERS-3 f4]
//   CalcOnFills/0/0: boundary 0xbd9feba11a355c84ull -> 0x109b8bbd2db3c39cull, final_hash 0x8e716ca55979f5e4ull -> 0x23caae4465e49b8dull [PAR-ORDERS-3 f4]
//   Random10/0/1: boundary 0x82933908492fe6aeull -> 0x53a92b8342582407ull, rows 0x839a10fa6478c692ull -> 0x337fd61947e125a0ull, final_hash 0x6907e89e2f502bd7ull -> 0xfb592dc1ad328c0bull [PAR-ORDERS-3 f4]
//   Random10/0/0: boundary 0x82933908492fe6aeull -> 0x53a92b8342582407ull, final_hash 0x6907e89e2f502bd7ull -> 0xfb592dc1ad328c0bull [PAR-ORDERS-3 f4]
//   Random20/0/1: boundary 0xad83f25bfdab1b50ull -> 0x6d39d123fcb64c3cull, rows 0x103b25042e1313deull -> 0x4350e4a911697ba6ull, final_hash 0xb27d00f43fad75a1ull -> 0x2fff8a1132834fe4ull [PAR-ORDERS-3 f4]
//   Random20/0/0: boundary 0xad83f25bfdab1b50ull -> 0x6d39d123fcb64c3cull, final_hash 0xb27d00f43fad75a1ull -> 0x2fff8a1132834fe4ull [PAR-ORDERS-3 f4]
// INT28 re-harvested it once on the integrated tree (main 962960b3 and the
// wave-J picks) with PINEFORGE_QUIET_BAR_HARVEST; the same harvest against
// main reproduces every old row. 220 of 276 runs move or are new (the four
// ThrottledStopEntry runs, the fixture lane W3B-ENG-GRID added so the probe's
// ThrottledReopens gate is still taken live), each marked with the pick(s) at
// whose boundary it moved (a harvest at every pick); an instrumented copy of
// the adapter (scratch only) shows the rule of each such pick firing in the
// run on the final tree, or, marked superseded, firing in it at the boundary
// before the named later pick and not after it. The rules are TradingView's
// (each lane's tests/fixtures tapes):
//   MarginCalls/0/1: boundary 0x9ffada7afff423a5ull -> 0x7685d0a40457d46full, rows 0xf1451bf046aea2aeull -> 0xaafecdc80e0c12a1ull [W3B F02 ledger booking]
//   MarginCalls/0/0: boundary 0x9ffada7afff423a5ull -> 0x7685d0a40457d46full [W3B F02 ledger booking]
//   MarginCallCycles/0/1: boundary 0x53630c222ae9b1a8ull -> 0x29243afd7d418ddbull, rows 0x26e80eef46f3b822ull -> 0xe34f9967f487081cull, final_hash 0x9dea3ea1d3cbc012ull -> 0x37573440557d0725ull [W3B void exit]
//   MarginCallCycles/0/0: boundary 0x53630c222ae9b1a8ull -> 0x29243afd7d418ddbull, final_hash 0x9dea3ea1d3cbc012ull -> 0x37573440557d0725ull [W3B void exit]
//   MarginCallCycles/1/1: boundary 0xd66af1875ed2e747ull -> 0xf4bc0848f6756193ull, rows 0xffa2470471097d1dull -> 0x533d78ca092362aaull [W3B F02 ledger booking]
//   MarginCallCycles/1/0: boundary 0xd66af1875ed2e747ull -> 0xf4bc0848f6756193ull [W3B F02 ledger booking]
//   ThrottledStopEntry/0/1: a new run [the ThrottledStopEntry fixture (W3B-ENG-GRID)]
//   ThrottledStopEntry/0/0: a new run [the ThrottledStopEntry fixture (W3B-ENG-GRID)]
//   ThrottledStopEntry/1/1: a new run [the ThrottledStopEntry fixture (W3B-ENG-GRID)]
//   ThrottledStopEntry/1/0: a new run [the ThrottledStopEntry fixture (W3B-ENG-GRID)]
//   CalcOnFills/0/1: boundary 0x109b8bbd2db3c39cull -> 0xfc8d0fb60bd5bfaaull, rows 0x4990637dd008c142ull -> 0x48055db8ef32071cull, final_hash 0x23caae4465e49b8dull -> 0xeef2208a06cbc6f1ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   CalcOnFills/0/0: boundary 0x109b8bbd2db3c39cull -> 0xfc8d0fb60bd5bfaaull, final_hash 0x23caae4465e49b8dull -> 0xeef2208a06cbc6f1ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   CalcOnFills/1/1: boundary 0x0e2860d938b1d36dull -> 0x64b0e03ae12cf797ull, rows 0xfe826221acbd42c3ull -> 0x087cb3f69b4b45b9ull, final_hash 0x1ae91d50d6762fd0ull -> 0x1e6e74d40df79e1eull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   CalcOnFills/1/0: boundary 0x0e2860d938b1d36dull -> 0x64b0e03ae12cf797ull, final_hash 0x1ae91d50d6762fd0ull -> 0x1e6e74d40df79e1eull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   CloseOrders/0/1: boundary 0xb27e9cfcbfee41d3ull -> 0x184e8d327c5e2707ull, rows 0x0033c4a6a644f0ddull -> 0xe6ad79a7e9fad579ull, trades 0xbc944a52841481b1ull -> 0x18d35429ed7abeedull, final_hash 0xbedbc1183b7526a1ull -> 0xf3223f1237a95489ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   CloseOrders/0/0: boundary 0xb27e9cfcbfee41d3ull -> 0x184e8d327c5e2707ull, trades 0xbc944a52841481b1ull -> 0x18d35429ed7abeedull, final_hash 0xbedbc1183b7526a1ull -> 0xf3223f1237a95489ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   CloseOrders/1/1: boundary 0xff96b9591f72f309ull -> 0xb4e85bb5577186c9ull, rows 0x5aaa3b2f562a501dull -> 0xc0c9c380b8d2c546ull, trades 0x4cdcb678c06d1a59ull -> 0x30b8367772531729ull, final_hash 0xc542c7fb8bb5559dull -> 0xb29e5a815acd8d0dull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   CloseOrders/1/0: boundary 0xff96b9591f72f309ull -> 0xb4e85bb5577186c9ull, trades 0x4cdcb678c06d1a59ull -> 0x30b8367772531729ull, final_hash 0xc542c7fb8bb5559dull -> 0xb29e5a815acd8d0dull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   CloseOrdersSlipped/0/1: rows 0xb1042b50a92de9a0ull -> 0xf5ebf3cacf1bb613ull [W3B F02 first id; W5 C1]
//   CloseOrdersSlipped/1/1: rows 0xa2710dbb1c8a57edull -> 0x5c384d163976870full [W3B F02 first id; W5 C1]
//   IntradayLoss/0/1: boundary 0x8a9bd5dd1429352eull -> 0x392fac4b53eaad92ull, rows 0x616b96966292e8adull -> 0xe02291acf9ac772dull, final_hash 0xc14ffe9c379fbee2ull -> 0x46bb7e611ca6e1c8ull [W8A R4]
//   IntradayLoss/0/0: boundary 0x8a9bd5dd1429352eull -> 0x392fac4b53eaad92ull, final_hash 0xc14ffe9c379fbee2ull -> 0x46bb7e611ca6e1c8ull [W8A R4]
//   IntradayLoss/1/1: boundary 0x1a92d1a03b233c6cull -> 0xeed99e6cc52c2efcull, rows 0x3382648892a694c4ull -> 0xafa99e76c624b49aull, final_hash 0x66f4f2fe8b455019ull -> 0x472c0f68bec82bf7ull [W8A R4]
//   IntradayLoss/1/0: boundary 0x1a92d1a03b233c6cull -> 0xeed99e6cc52c2efcull, final_hash 0x66f4f2fe8b455019ull -> 0x472c0f68bec82bf7ull [W8A R4]
//   SameBarBatch/0/1: boundary 0x85b57cf2c67fbdd6ull -> 0x1ee4d2724cccff28ull, rows 0xdf711eb77d489011ull -> 0xf20f7ef590b39dcdull [W3B F02 first id]
//   SameBarBatch/0/0: boundary 0x85b57cf2c67fbdd6ull -> 0x1ee4d2724cccff28ull [W3B F02 first id]
//   SameBarBatch/1/1: boundary 0x5d280dcf47a7af5bull -> 0xbd6e0d279e9729e6ull, rows 0x987f9a54f93d934eull -> 0xeb4f5e6198dd0178ull [W3B F02 first id]
//   SameBarBatch/1/0: boundary 0x5d280dcf47a7af5bull -> 0xbd6e0d279e9729e6ull [W3B F02 first id]
//   Random01/0/1: boundary 0xe7fdfddc42a2ae75ull -> 0xb7cb18e9c32443c1ull, rows 0x0b38785d82bc8fe6ull -> 0xf4ebe6782bc3ee5eull, trades 0xf2ae7ef9674d4425ull -> 0xe6d673a2f0d40429ull, final_hash 0x16fe00b5a88e8ff2ull -> 0x44b3af95af8e40ebull [W3B F02 ledger booking; W3B void exit]
//   Random01/0/0: boundary 0xe7fdfddc42a2ae75ull -> 0xb7cb18e9c32443c1ull, trades 0xf2ae7ef9674d4425ull -> 0xe6d673a2f0d40429ull, final_hash 0x16fe00b5a88e8ff2ull -> 0x44b3af95af8e40ebull [W3B F02 ledger booking; W3B void exit]
//   Random01/1/1: boundary 0x64e7087d99c04c46ull -> 0xd8ef670e43a3f4ffull, rows 0x1c0e119f02f64ddeull -> 0x6e941c1e9e09aa71ull, trades 0x64f480bfff90f76aull -> 0x8b2f1d5ce5ba6fb9ull, final_hash 0xb0114372fd93a01cull -> 0x517f790462c7efb9ull [W3B F02 ledger booking; W3B void exit]
//   Random01/1/0: boundary 0x64e7087d99c04c46ull -> 0xd8ef670e43a3f4ffull, trades 0x64f480bfff90f76aull -> 0x8b2f1d5ce5ba6fb9ull, final_hash 0xb0114372fd93a01cull -> 0x517f790462c7efb9ull [W3B F02 ledger booking; W3B void exit]
//   Random02/0/1: boundary 0x8a2b805556d22659ull -> 0x110e0c47b38884fdull, rows 0xce086fbf3b8296e5ull -> 0xedcdbd9e14af6a21ull, trades 0x41793244d920be3dull -> 0x7a3972561c477964ull, final_hash 0x5221fce916020c33ull -> 0x39b31da3cd2bb72cull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F19c]
//   Random02/0/0: boundary 0x8a2b805556d22659ull -> 0x110e0c47b38884fdull, trades 0x41793244d920be3dull -> 0x7a3972561c477964ull, final_hash 0x5221fce916020c33ull -> 0x39b31da3cd2bb72cull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F19c]
//   Random02/1/1: boundary 0x844819f3aaccd232ull -> 0xa45a30c7d9cff5b7ull, rows 0x74fc5a6e6a256e36ull -> 0x4fb7e76741a52631ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random02/1/0: boundary 0x844819f3aaccd232ull -> 0xa45a30c7d9cff5b7ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random03/0/1: boundary 0x2dad9d24e763c932ull -> 0xacc2755c7089fb70ull, rows 0xa3f6fa59cf96ba52ull -> 0x9bf3ecdb24c64985ull, trades 0x8bd01a3594893c29ull -> 0x6ed2b968ae7e07fcull, final_hash 0xeb202b23d902f8b8ull -> 0x16b4269636eeaf7aull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random03/0/0: boundary 0x2dad9d24e763c932ull -> 0xacc2755c7089fb70ull, trades 0x8bd01a3594893c29ull -> 0x6ed2b968ae7e07fcull, final_hash 0xeb202b23d902f8b8ull -> 0x16b4269636eeaf7aull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random03/1/1: boundary 0x7196371dde7bb130ull -> 0x1c932bfdca73b86bull, rows 0xbdf7be966e72b156ull -> 0x4cd3711ffc545640ull, trades 0xde85b8fe81a28e59ull -> 0x80ca2343fcd2c1d0ull, final_hash 0x92f95865ffb213eeull -> 0x5e606ff70d525f5bull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random03/1/0: boundary 0x7196371dde7bb130ull -> 0x1c932bfdca73b86bull, trades 0xde85b8fe81a28e59ull -> 0x80ca2343fcd2c1d0ull, final_hash 0x92f95865ffb213eeull -> 0x5e606ff70d525f5bull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random04/0/1: boundary 0x9d74676f534d8b76ull -> 0x3f2fc4283afc7cc4ull, rows 0xdb1c5eb0cb9f6c32ull -> 0xb7e20a93376bfe8bull, trades 0x38df0af477021418ull -> 0xcc6cf8406a1f71a0ull, final_hash 0x82a419ac37909af3ull -> 0xec2f788ae1bef8fcull, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Random04/0/0: boundary 0x9d74676f534d8b76ull -> 0x3f2fc4283afc7cc4ull, trades 0x38df0af477021418ull -> 0xcc6cf8406a1f71a0ull, final_hash 0x82a419ac37909af3ull -> 0xec2f788ae1bef8fcull, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Random04/1/1: boundary 0x19cdc09e2267df5aull -> 0xadf2b572cb770a1cull, rows 0x7eca887896ce161bull -> 0x4a54644e7d763961ull, trades 0x28a43578727c43b5ull -> 0x3659e955e5220f85ull, final_hash 0x350e10b2e19f0b44ull -> 0xa75420d0ae9c3b6eull, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Random04/1/0: boundary 0x19cdc09e2267df5aull -> 0xadf2b572cb770a1cull, trades 0x28a43578727c43b5ull -> 0x3659e955e5220f85ull, final_hash 0x350e10b2e19f0b44ull -> 0xa75420d0ae9c3b6eull, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; R1-CONSOLIDATE R1]
//   Random05/0/1: boundary 0xac397a37be0d4789ull -> 0x5288fe5363963744ull, rows 0x64b7bb57706476b6ull -> 0x83a7124d23410b32ull, final_hash 0x85274de8bea42d05ull -> 0xd3a0f35947f3607full [W3B F02 ledger booking; W8A R4]
//   Random05/0/0: boundary 0xac397a37be0d4789ull -> 0x5288fe5363963744ull, final_hash 0x85274de8bea42d05ull -> 0xd3a0f35947f3607full [W3B F02 ledger booking; W8A R4]
//   Random05/1/1: boundary 0xbd96a4fdd0383edbull -> 0x13a36719393c2baeull, rows 0xdcb784e78fba749eull -> 0xeed6589421e8cda7ull, final_hash 0x919116731d265d3dull -> 0x1c58bcc76863fb17ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8A R4]
//   Random05/1/0: boundary 0xbd96a4fdd0383edbull -> 0x13a36719393c2baeull, final_hash 0x919116731d265d3dull -> 0x1c58bcc76863fb17ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8A R4]
//   Random06/0/1: boundary 0x7deef79bd0217f64ull -> 0x163e7663afe28a2dull, rows 0xd166805cc595f38eull -> 0x7a488bb30d99363eull, trades 0x75cd705050ee6ca5ull -> 0x9703eeb61cd7387bull, final_hash 0x4c7c821f90e38cccull -> 0xa030d47050bdffdaull, trade_count 13 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R-B]
//   Random06/0/0: boundary 0x7deef79bd0217f64ull -> 0x163e7663afe28a2dull, trades 0x75cd705050ee6ca5ull -> 0x9703eeb61cd7387bull, final_hash 0x4c7c821f90e38cccull -> 0xa030d47050bdffdaull, trade_count 13 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R-B]
//   Random06/1/1: boundary 0x8a1bb21a5e64fed9ull -> 0x0a9324ccde3f4915ull, rows 0x7d881d1a93512848ull -> 0x6239cc3c60c5fd6eull, trades 0x5f84022dee0bd36full -> 0x11227961f96cbcc2ull, final_hash 0xaa8716ffbdfd11b2ull -> 0x4b67e46b5a3f02a4ull, trade_count 13 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R-B]
//   Random06/1/0: boundary 0x8a1bb21a5e64fed9ull -> 0x0a9324ccde3f4915ull, trades 0x5f84022dee0bd36full -> 0x11227961f96cbcc2ull, final_hash 0xaa8716ffbdfd11b2ull -> 0x4b67e46b5a3f02a4ull, trade_count 13 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R-B]
//   Random07/0/1: boundary 0xe6120913fe5da50dull -> 0x974115e531c0338full, rows 0xe90b62d77e4625b7ull -> 0x9d4ec9276f34e75dull, final_hash 0xbc6f67df5b0f0b2full -> 0x1a375024019a0553ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random07/0/0: boundary 0xe6120913fe5da50dull -> 0x974115e531c0338full, final_hash 0xbc6f67df5b0f0b2full -> 0x1a375024019a0553ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random07/1/1: boundary 0xe2c3492a3155bf7dull -> 0x9cd7a2146d1dc819ull, rows 0x0e5f1aa6cabc3a32ull -> 0x29d399651502d791ull, final_hash 0xbad1ae57e202b2d0ull -> 0xd9742bccbe934936ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random07/1/0: boundary 0xe2c3492a3155bf7dull -> 0x9cd7a2146d1dc819ull, final_hash 0xbad1ae57e202b2d0ull -> 0xd9742bccbe934936ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random08/0/1: boundary 0x0a3af182d33c479aull -> 0xd8361e46c395ccb5ull, rows 0xc91b237d10b90ecbull -> 0x5aa540a27984a70aull, trades 0x68ebab74d498abedull -> 0xe67dcbbac61f0c9full, final_hash 0x1930dc38f4f605fbull -> 0x818b37eba85784f1ull, boundary_count 93 -> 95, trade_count 24 -> 26 [W3B void exit; W4 F19c; W8A R-B]
//   Random08/0/0: boundary 0x0a3af182d33c479aull -> 0xd8361e46c395ccb5ull, trades 0x68ebab74d498abedull -> 0xe67dcbbac61f0c9full, final_hash 0x1930dc38f4f605fbull -> 0x818b37eba85784f1ull, boundary_count 93 -> 95, trade_count 24 -> 26 [W3B void exit; W4 F19c; W8A R-B]
//   Random08/1/1: boundary 0xc981400f75b65614ull -> 0x5c65b6eaad7034c5ull, rows 0xb37d8f41ed349758ull -> 0x51bf07a4a66e31a1ull, trades 0x84ec2499efb30a9eull -> 0x071e4e261711abeeull, final_hash 0xe48afc5e8a67e872ull -> 0x8eb670e9d6661682ull, boundary_count 93 -> 95, trade_count 24 -> 26 [W3B void exit; W8A R-B]
//   Random08/1/0: boundary 0xc981400f75b65614ull -> 0x5c65b6eaad7034c5ull, trades 0x84ec2499efb30a9eull -> 0x071e4e261711abeeull, final_hash 0xe48afc5e8a67e872ull -> 0x8eb670e9d6661682ull, boundary_count 93 -> 95, trade_count 24 -> 26 [W3B void exit; W8A R-B]
//   Random09/0/1: boundary 0xaaff8dcd840f16ceull -> 0x92e8370ab3e727c0ull, rows 0x9021538022cf9b9bull -> 0x672a4666a377cd09ull, final_hash 0xbbe3c07366b0cbeaull -> 0x306c858f362bd294ull [W3B void exit; W8A R4]
//   Random09/0/0: boundary 0xaaff8dcd840f16ceull -> 0x92e8370ab3e727c0ull, final_hash 0xbbe3c07366b0cbeaull -> 0x306c858f362bd294ull [W3B void exit; W8A R4]
//   Random09/1/1: boundary 0x026e829145cad6f5ull -> 0xf7f7c43e037a49b5ull, rows 0x32ca6bae685cea43ull -> 0x12ed3c4db0e659e5ull, final_hash 0xc55c597c6508d557ull -> 0x355ede665029e361ull [W3B void exit; W8A R4]
//   Random09/1/0: boundary 0x026e829145cad6f5ull -> 0xf7f7c43e037a49b5ull, final_hash 0xc55c597c6508d557ull -> 0x355ede665029e361ull [W3B void exit; W8A R4]
//   Random10/0/1: boundary 0x53a92b8342582407ull -> 0xaaa0d5096796399dull, rows 0x337fd61947e125a0ull -> 0x8951026f85a19c95ull, trades 0xaae6484f97956d07ull -> 0x4bf91f9abde25f6eull, final_hash 0xfb592dc1ad328c0bull -> 0xff504d50c65ba2feull, boundary_count 67 -> 62, trade_count 23 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random10/0/0: boundary 0x53a92b8342582407ull -> 0xaaa0d5096796399dull, trades 0xaae6484f97956d07ull -> 0x4bf91f9abde25f6eull, final_hash 0xfb592dc1ad328c0bull -> 0xff504d50c65ba2feull, boundary_count 67 -> 62, trade_count 23 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random10/1/1: boundary 0x3af283057bf553afull -> 0xb10de9f0194c4cd0ull, rows 0xd948d5022e691361ull -> 0x4f457d291e5d1b50ull, trades 0x724c585ad13c935dull -> 0x36fb31bb06edb88cull, final_hash 0x794b361143554e09ull -> 0x86f7243983d16b95ull, boundary_count 67 -> 62, trade_count 23 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random10/1/0: boundary 0x3af283057bf553afull -> 0xb10de9f0194c4cd0ull, trades 0x724c585ad13c935dull -> 0x36fb31bb06edb88cull, final_hash 0x794b361143554e09ull -> 0x86f7243983d16b95ull, boundary_count 67 -> 62, trade_count 23 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random11/0/1: boundary 0x9f99f3ad7d35ba23ull -> 0xc2d40e2309498204ull, rows 0x29a35fd6705cc48aull -> 0x47ab72d92cd1a804ull, final_hash 0x8235cad2a6dfb3f3ull -> 0xb89b1233521240a5ull [W3B F02 first id; W3B void exit; W8A R4]
//   Random11/0/0: boundary 0x9f99f3ad7d35ba23ull -> 0xc2d40e2309498204ull, final_hash 0x8235cad2a6dfb3f3ull -> 0xb89b1233521240a5ull [W3B void exit; W8A R4]
//   Random11/1/1: boundary 0xf6fb4588ed987e13ull -> 0x43ec3bbaca91488aull, rows 0x7dcd8b5314fb7562ull -> 0x8b43e1bf1c0c5ffbull, final_hash 0xb44ac10707d01de6ull -> 0x54c5d31ee5a16684ull [W3B F02 first id; W3B void exit; W5 C1; W8A R4]
//   Random11/1/0: boundary 0xf6fb4588ed987e13ull -> 0x43ec3bbaca91488aull, final_hash 0xb44ac10707d01de6ull -> 0x54c5d31ee5a16684ull [W3B F02 first id; W3B void exit; W8A R4]
//   Random12/0/1: boundary 0xc02247ff66b0dc87ull -> 0xbc3686bf9433a52eull, rows 0xf39477bfa32bb689ull -> 0xaec3d3079e6c1521ull, trades 0xf60b6b66546787a7ull -> 0xe801e0d8eaffcc6bull, final_hash 0x5f0b7200f4ecdfe1ull -> 0x347aaf5cd70466caull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random12/0/0: boundary 0xc02247ff66b0dc87ull -> 0xbc3686bf9433a52eull, trades 0xf60b6b66546787a7ull -> 0xe801e0d8eaffcc6bull, final_hash 0x5f0b7200f4ecdfe1ull -> 0x347aaf5cd70466caull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random12/1/1: boundary 0xfcd62d0327e0553aull -> 0x8647fe7278804323ull, rows 0xf36949e9fad408efull -> 0x3b87cd70a81d2133ull, trades 0xfc32c809c37c8290ull -> 0x16fca020c807a384ull, final_hash 0x84a69af72aae96d9ull -> 0x42a73d638c1997b3ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random12/1/0: boundary 0xfcd62d0327e0553aull -> 0x8647fe7278804323ull, trades 0xfc32c809c37c8290ull -> 0x16fca020c807a384ull, final_hash 0x84a69af72aae96d9ull -> 0x42a73d638c1997b3ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random13/0/1: boundary 0x53be6a77e760fe0bull -> 0x56b98418406a893aull, rows 0xe1b3dac90adb9f4bull -> 0xfbcef4d1953ddd45ull, trades 0x9b711a1ad7f0af2bull -> 0xcdf7b5ce1f4314d9ull, final_hash 0xafd98319ef581f61ull -> 0x4294104ba715c4e5ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random13/0/0: boundary 0x53be6a77e760fe0bull -> 0x56b98418406a893aull, trades 0x9b711a1ad7f0af2bull -> 0xcdf7b5ce1f4314d9ull, final_hash 0xafd98319ef581f61ull -> 0x4294104ba715c4e5ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random13/1/1: boundary 0xd238668589210027ull -> 0x145ca3678c5067c1ull, rows 0xc4b7dac2b95e01fbull -> 0x4f73ce76a795a693ull, trades 0x78cbd8f6435e4796ull -> 0xb3e3251fdef3358cull, final_hash 0xb4b9d6f51a69342full -> 0x81d5b4683e107c50ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random13/1/0: boundary 0xd238668589210027ull -> 0x145ca3678c5067c1ull, trades 0x78cbd8f6435e4796ull -> 0xb3e3251fdef3358cull, final_hash 0xb4b9d6f51a69342full -> 0x81d5b4683e107c50ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random14/0/1: boundary 0xd37abe66ab158e44ull -> 0x8102343dedc7f9baull, rows 0x38cb950f5ceb9d9cull -> 0x419caa2f1494ac0aull, final_hash 0x9d488869e9756d89ull -> 0xf4b4d3629853f062ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random14/0/0: boundary 0xd37abe66ab158e44ull -> 0x8102343dedc7f9baull, final_hash 0x9d488869e9756d89ull -> 0xf4b4d3629853f062ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random14/1/1: boundary 0xb8392469a2a0abfeull -> 0x295206578d5ff480ull, rows 0xfed8161637d6c938ull -> 0x281d0596aee9b7e0ull, final_hash 0x04b4096b966c585cull -> 0x441179a0f3e73edfull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random14/1/0: boundary 0xb8392469a2a0abfeull -> 0x295206578d5ff480ull, final_hash 0x04b4096b966c585cull -> 0x441179a0f3e73edfull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random15/0/1: boundary 0x1fc67d9462b82276ull -> 0xb3ea7ad42aa97779ull, rows 0x167381a97aa1c870ull -> 0xbe96e9e530c967c1ull, trades 0xc7b14196e7b743c6ull -> 0x7408d19137f1009aull, final_hash 0x69f92f0d300a126eull -> 0x33f7d111d486ddedull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random15/0/0: boundary 0x1fc67d9462b82276ull -> 0xb3ea7ad42aa97779ull, trades 0xc7b14196e7b743c6ull -> 0x7408d19137f1009aull, final_hash 0x69f92f0d300a126eull -> 0x33f7d111d486ddedull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random15/1/1: boundary 0xbbbd08da5449bc19ull -> 0x3835c1c95b71c190ull, rows 0x2a9a6d807f0676baull -> 0xc7dddf092d2d415bull, trades 0x9e6d92b9d40ff818ull -> 0x8c47d39a73e36388ull, final_hash 0xe8dd6d8df584c6d6ull -> 0xebe3ce184f432170ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random15/1/0: boundary 0xbbbd08da5449bc19ull -> 0x3835c1c95b71c190ull, trades 0x9e6d92b9d40ff818ull -> 0x8c47d39a73e36388ull, final_hash 0xe8dd6d8df584c6d6ull -> 0xebe3ce184f432170ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random16/0/1: boundary 0xe8851a2e0e645003ull -> 0x586e2a0af3b884eeull, rows 0xcfab7fb0e8c24b17ull -> 0x487f8658f4ce0da9ull, trades 0x6889115fd9a0fe41ull -> 0xbb86ab41adee8d23ull, final_hash 0x5639dcc11e103f25ull -> 0x3d540c1799b82d36ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random16/0/0: boundary 0xe8851a2e0e645003ull -> 0x586e2a0af3b884eeull, trades 0x6889115fd9a0fe41ull -> 0xbb86ab41adee8d23ull, final_hash 0x5639dcc11e103f25ull -> 0x3d540c1799b82d36ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random16/1/1: boundary 0x431440275998e000ull -> 0x09fefcd04a59b9ceull, rows 0x90d12ea4ed48dd05ull -> 0xbe70fa5a6c43d5cdull, trades 0x552b70400304fd25ull -> 0x5f87751701a631a7ull, final_hash 0xf7e82ef73f56a2eeull -> 0xa979bdea5746599bull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random16/1/0: boundary 0x431440275998e000ull -> 0x09fefcd04a59b9ceull, trades 0x552b70400304fd25ull -> 0x5f87751701a631a7ull, final_hash 0xf7e82ef73f56a2eeull -> 0xa979bdea5746599bull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random17/0/1: boundary 0xf4b303a1ef4696f7ull -> 0x4f041987fe01e1c2ull, rows 0x6417861b10d32340ull -> 0xc2f61e906254c6ceull, final_hash 0xa61332be53034674ull -> 0xe5b43a4ee15652b7ull [W3B void exit; W8A R4]
//   Random17/0/0: boundary 0xf4b303a1ef4696f7ull -> 0x4f041987fe01e1c2ull, final_hash 0xa61332be53034674ull -> 0xe5b43a4ee15652b7ull [W3B void exit; W8A R4]
//   Random17/1/1: boundary 0x4fbe2d70efbe24eaull -> 0x051ecf9eee42ea17ull, rows 0xaf770da711891f5aull -> 0x52f90b40b96ddaf8ull, final_hash 0x6665febf6fb7f354ull -> 0xb499c267440294d9ull [W3B void exit; W8A R4]
//   Random17/1/0: boundary 0x4fbe2d70efbe24eaull -> 0x051ecf9eee42ea17ull, final_hash 0x6665febf6fb7f354ull -> 0xb499c267440294d9ull [W3B void exit; W8A R4]
//   Random18/0/1: boundary 0xf434360cb53f77baull -> 0x46c44a4b3f43d1f1ull, rows 0xff5d4060a3fafb03ull -> 0x162fb95afde1b922ull, trades 0x5f03c99e6854a46aull -> 0xd8b1ce3cab1f5711ull, final_hash 0xe0f82ba48054cc9cull -> 0x65cc4963fe02e966ull, boundary_count 66 -> 75, trade_count 3 -> 9 [W3B void exit; W4 F19c; W8A R4; R1-CONSOLIDATE R1]
//   Random18/0/0: boundary 0xf434360cb53f77baull -> 0x46c44a4b3f43d1f1ull, trades 0x5f03c99e6854a46aull -> 0xd8b1ce3cab1f5711ull, final_hash 0xe0f82ba48054cc9cull -> 0x65cc4963fe02e966ull, boundary_count 66 -> 75, trade_count 3 -> 9 [W3B void exit; W4 F19c; W8A R4; R1-CONSOLIDATE R1]
//   Random18/1/1: boundary 0x7b6ad000002b1959ull -> 0x28d9f94548ee4134ull, rows 0x70472dcb4abe021full -> 0xf279e0c2d7ad4a08ull, final_hash 0x73a5afc061a888aaull -> 0xc8fcaa37ad2681ecull [W3B void exit; W8A R4]
//   Random18/1/0: boundary 0x7b6ad000002b1959ull -> 0x28d9f94548ee4134ull, final_hash 0x73a5afc061a888aaull -> 0xc8fcaa37ad2681ecull [W3B void exit; W8A R4]
//   Random19/0/1: boundary 0xa21f7b0b62d72170ull -> 0x2578d7b317dd9becull, rows 0xda14d42130450ee8ull -> 0x59557f1fefa231afull, trades 0x95d7b888983ad8e6ull -> 0x0713a3bde745d7edull, final_hash 0x0bcf8b1fde6775f8ull -> 0x7e530ea7e9bb00d6ull, trade_count 10 -> 11 [W3B F02 first id; W3B void exit; W4 F19a; W8A R-B]
//   Random19/0/0: boundary 0xa21f7b0b62d72170ull -> 0x2578d7b317dd9becull, trades 0x95d7b888983ad8e6ull -> 0x0713a3bde745d7edull, final_hash 0x0bcf8b1fde6775f8ull -> 0x7e530ea7e9bb00d6ull, trade_count 10 -> 11 [W3B F02 first id; W3B void exit; W4 F19a; W8A R-B]
//   Random19/1/1: boundary 0xceae18544190aae2ull -> 0x11fcb5008bfc8542ull, rows 0x7d8104e455ec01f8ull -> 0x181811064366b797ull, trades 0x011bec7774b82ec9ull -> 0xfc6798fec1de6897ull, final_hash 0x426c72d5bce82609ull -> 0xb5b2ab25557d9ce4ull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F19a; W8A R-B]
//   Random19/1/0: boundary 0xceae18544190aae2ull -> 0x11fcb5008bfc8542ull, trades 0x011bec7774b82ec9ull -> 0xfc6798fec1de6897ull, final_hash 0x426c72d5bce82609ull -> 0xb5b2ab25557d9ce4ull, trade_count 11 -> 12 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F19a; W8A R-B]
//   Random20/0/1: boundary 0x6d39d123fcb64c3cull -> 0x4795ce55e278f092ull, rows 0x4350e4a911697ba6ull -> 0x6739def4111d99a6ull, trades 0x4ae8ba38071c5c60ull -> 0x3d398549696d4c46ull, final_hash 0x2fff8a1132834fe4ull -> 0x29e3be3fcc918072ull, boundary_count 69 -> 67, trade_count 21 -> 20 [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random20/0/0: boundary 0x6d39d123fcb64c3cull -> 0x4795ce55e278f092ull, trades 0x4ae8ba38071c5c60ull -> 0x3d398549696d4c46ull, final_hash 0x2fff8a1132834fe4ull -> 0x29e3be3fcc918072ull, boundary_count 69 -> 67, trade_count 21 -> 20 [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random20/1/1: boundary 0x4b8bcf5ef1f015c5ull -> 0xa04d5df6a8012a95ull, rows 0x6e2d790aa8a7762bull -> 0xfb09750b63c74c91ull, trades 0x57a697ba56dd611eull -> 0xfccf9cfafc0d59f2ull, final_hash 0xd3e60d6fda04adbeull -> 0x84804b1c9915de38ull, boundary_count 72 -> 70, trade_count 21 -> 20 [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random20/1/0: boundary 0x4b8bcf5ef1f015c5ull -> 0xa04d5df6a8012a95ull, trades 0x57a697ba56dd611eull -> 0xfccf9cfafc0d59f2ull, final_hash 0xd3e60d6fda04adbeull -> 0x84804b1c9915de38ull, boundary_count 72 -> 70, trade_count 21 -> 20 [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random21/0/1: boundary 0xd7b52779988b5cfaull -> 0x26729ac375542b71ull, rows 0xcfddcd52fb629169ull -> 0x2ea4f6970b51e0c9ull, trades 0xbc89178a735678f6ull -> 0x44d2a25d5d3d9991ull, final_hash 0xfd9b7e4dad3197b5ull -> 0x274d05295edf06ecull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random21/0/0: boundary 0xd7b52779988b5cfaull -> 0x26729ac375542b71ull, trades 0xbc89178a735678f6ull -> 0x44d2a25d5d3d9991ull, final_hash 0xfd9b7e4dad3197b5ull -> 0x274d05295edf06ecull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random21/1/1: boundary 0x1c515b9e6cd109a3ull -> 0x51dde7af0f801ff0ull, rows 0xaa94d0f54845ee02ull -> 0xed6bda6ff2e91c25ull, trades 0x0cb28a314aa970aeull -> 0xacfc14dbfb99da34ull, final_hash 0x29fa4e20de356d85ull -> 0xd0b5041d0d30d22bull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random21/1/0: boundary 0x1c515b9e6cd109a3ull -> 0x51dde7af0f801ff0ull, trades 0x0cb28a314aa970aeull -> 0xacfc14dbfb99da34ull, final_hash 0x29fa4e20de356d85ull -> 0xd0b5041d0d30d22bull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random22/0/1: boundary 0x79adc16250cf8292ull -> 0x3c35d0fc3409545aull, rows 0x2512a0afd913906aull -> 0x289459a9d83dcf63ull, trades 0xedb01aeb6057c25bull -> 0x43bb057df8787cb2ull, final_hash 0x1d49ae94182c6dc3ull -> 0xc4d805069c4a36b4ull [W3B void exit]
//   Random22/0/0: boundary 0x79adc16250cf8292ull -> 0x3c35d0fc3409545aull, trades 0xedb01aeb6057c25bull -> 0x43bb057df8787cb2ull, final_hash 0x1d49ae94182c6dc3ull -> 0xc4d805069c4a36b4ull [W3B void exit]
//   Random22/1/1: boundary 0x301434ca9762fba0ull -> 0x46d42b9bf96f8dbfull, rows 0x9371e473c135020full -> 0xc61ebe11092e9bb7ull, trades 0x70ac53cf16690a86ull -> 0x19422223060a03fbull, final_hash 0xaadd4e3cb0a37251ull -> 0xaf77291e8769d31full [W3B void exit]
//   Random22/1/0: boundary 0x301434ca9762fba0ull -> 0x46d42b9bf96f8dbfull, trades 0x70ac53cf16690a86ull -> 0x19422223060a03fbull, final_hash 0xaadd4e3cb0a37251ull -> 0xaf77291e8769d31full [W3B void exit]
//   Random23/0/1: boundary 0xbf93319ca8e8ed61ull -> 0x8b54ce1fd52b1a9aull, rows 0x178099cac4dd9abfull -> 0x224cb5ae0f1ac93full, final_hash 0x160121fb8a6ad628ull -> 0x4133f7bb6954349aull [W8A R4]
//   Random23/0/0: boundary 0xbf93319ca8e8ed61ull -> 0x8b54ce1fd52b1a9aull, final_hash 0x160121fb8a6ad628ull -> 0x4133f7bb6954349aull [W8A R4]
//   Random23/1/1: boundary 0x03336650fccfc186ull -> 0x7b0357944761af1bull, rows 0x84fb2988132b6bdbull -> 0x176a0075bce64b8full, final_hash 0x54a3c7db6bc08316ull -> 0x8a0e000ca7692e34ull [W8A R4]
//   Random23/1/0: boundary 0x03336650fccfc186ull -> 0x7b0357944761af1bull, final_hash 0x54a3c7db6bc08316ull -> 0x8a0e000ca7692e34ull [W8A R4]
//   Random24/0/1: boundary 0x21714f0414285779ull -> 0xae7998ad09f09f7aull, rows 0x8e07ee97e3c18592ull -> 0x2e078b985c070260ull, final_hash 0x9b4982cc0c9d5836ull -> 0x1854a1f9f4a3d97bull [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Random24/0/0: boundary 0x21714f0414285779ull -> 0xae7998ad09f09f7aull, final_hash 0x9b4982cc0c9d5836ull -> 0x1854a1f9f4a3d97bull [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Random24/1/1: boundary 0x104927385caf1bafull -> 0x1defabc343733710ull, rows 0xb58d2d29b9934e0cull -> 0x5a31e5560954fd71ull, trades 0xe744bb83904b658aull -> 0x5971bdf0158339a3ull, final_hash 0x892eef5960b6186eull -> 0x29a17a681570c85bull [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Random24/1/0: boundary 0x104927385caf1bafull -> 0x1defabc343733710ull, trades 0xe744bb83904b658aull -> 0x5971bdf0158339a3ull, final_hash 0x892eef5960b6186eull -> 0x29a17a681570c85bull [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit]
//   Random25/0/1: boundary 0x7fe75851d29b5f6aull -> 0x871b48a97b54a1a7ull, rows 0xbd64625a7bcfb73cull -> 0xf85a29bfb38c03dfull, trades 0xf92cddf4fc024c49ull -> 0x38cd474fa60d28a1ull, final_hash 0xdbdc12e31962344aull -> 0x09b8a0954f3e27caull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random25/0/0: boundary 0x7fe75851d29b5f6aull -> 0x871b48a97b54a1a7ull, trades 0xf92cddf4fc024c49ull -> 0x38cd474fa60d28a1ull, final_hash 0xdbdc12e31962344aull -> 0x09b8a0954f3e27caull [W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random25/1/1: boundary 0x49f7049727341b3cull -> 0xa177541d9a0b1c6full, rows 0x4023f4c7efa8cdacull -> 0xc3a837662f5bbacdull, trades 0xf92cddf4fc024c49ull -> 0x38cd474fa60d28a1ull, final_hash 0x1e3a08e1516ae4beull -> 0x175ccb5de5ff9ff6ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08]
//   Random25/1/0: boundary 0x49f7049727341b3cull -> 0xa177541d9a0b1c6full, trades 0xf92cddf4fc024c49ull -> 0x38cd474fa60d28a1ull, final_hash 0x1e3a08e1516ae4beull -> 0x175ccb5de5ff9ff6ull [W3B F02 ledger booking; W3B void exit; W4 F08]
//   Random26/0/1: boundary 0xca8049a8b96e9724ull -> 0x745f6d660f0ff7c1ull, rows 0xc7e39193f4b07aa7ull -> 0x3fe4ca9a714df290ull, trades 0xd9522717c794d53bull -> 0x44110f44174fa225ull, final_hash 0xf4e31e8175400963ull -> 0x7400318cf813e724ull, trade_count 12 -> 11 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1]
//   Random26/0/0: boundary 0xca8049a8b96e9724ull -> 0x745f6d660f0ff7c1ull, trades 0xd9522717c794d53bull -> 0x44110f44174fa225ull, final_hash 0xf4e31e8175400963ull -> 0x7400318cf813e724ull, trade_count 12 -> 11 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random26/1/1: boundary 0x9c9ac48945130531ull -> 0x27527ee4f99b804dull, rows 0xeb4d985eb31f93e0ull -> 0xd7d832758f013ec4ull, trades 0xf7b8acd382701dd1ull -> 0x7762169e1cd29d9full, final_hash 0xecd913dc1c2bb6edull -> 0x89642d94f10f9107ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1]
//   Random26/1/0: boundary 0x9c9ac48945130531ull -> 0x27527ee4f99b804dull, trades 0xf7b8acd382701dd1ull -> 0x7762169e1cd29d9full, final_hash 0xecd913dc1c2bb6edull -> 0x89642d94f10f9107ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random27/0/1: boundary 0xa5ad84fe83acddfcull -> 0x9256b189ba68bea3ull, rows 0x4da695a0a63ae358ull -> 0x958d1671c170e18full [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random27/0/0: boundary 0xa5ad84fe83acddfcull -> 0x9256b189ba68bea3ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random27/1/1: boundary 0x67f5b41cf936747aull -> 0x505583c531027ba4ull, rows 0x06665f5393126836ull -> 0xb98058137450a938ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random27/1/0: boundary 0x67f5b41cf936747aull -> 0x505583c531027ba4ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random28/0/1: boundary 0x8b9cc3d4febe4c62ull -> 0x6a15e4675c7e99a1ull, rows 0x69471bc0f4097ac1ull -> 0x3530ef3b232b11e4ull, trades 0x7ee366b7feceed4eull -> 0xa35579679c597892ull, final_hash 0x373d1983261f87caull -> 0xfb480389d470203aull [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random28/0/0: boundary 0x8b9cc3d4febe4c62ull -> 0x6a15e4675c7e99a1ull, trades 0x7ee366b7feceed4eull -> 0xa35579679c597892ull, final_hash 0x373d1983261f87caull -> 0xfb480389d470203aull [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random28/1/1: boundary 0x57f3120e39e241ddull -> 0x7c85ce498057e507ull, rows 0x0a99bf96869493a4ull -> 0xd3dd35946efd7377ull, trades 0xee17522a5f3c15dfull -> 0xe8632e16032f1923ull, final_hash 0xe2efc2ce4ee88ff9ull -> 0x5f83107ff10203a9ull [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random28/1/0: boundary 0x57f3120e39e241ddull -> 0x7c85ce498057e507ull, trades 0xee17522a5f3c15dfull -> 0xe8632e16032f1923ull, final_hash 0xe2efc2ce4ee88ff9ull -> 0x5f83107ff10203a9ull [W3B void exit; W4 F08; R1-CONSOLIDATE R1]
//   Random29/0/1: boundary 0x89b631d5f5f01691ull -> 0xd6eaff478891b7daull, rows 0x167525b5e459f6a9ull -> 0xa9df1368e4aea283ull, final_hash 0xb42cb126538c6f0aull -> 0x38adbedb80d0ca20ull [W8A R4]
//   Random29/0/0: boundary 0x89b631d5f5f01691ull -> 0xd6eaff478891b7daull, final_hash 0xb42cb126538c6f0aull -> 0x38adbedb80d0ca20ull [W8A R4]
//   Random29/1/1: boundary 0x41cca15aeaf4b30dull -> 0x4d4b64bd0a7ede5cull, rows 0x5c0fe05b50ac7989ull -> 0xed3995b6f15ae5b5ull, final_hash 0xdaa6c53d3410e42full -> 0x5b7229f7447a7941ull [W8A R4]
//   Random29/1/0: boundary 0x41cca15aeaf4b30dull -> 0x4d4b64bd0a7ede5cull, final_hash 0xdaa6c53d3410e42full -> 0x5b7229f7447a7941ull [W8A R4]
//   Random30/0/1: boundary 0xe74142e41965c7f8ull -> 0xd66f88389fcd9547ull, rows 0x7d6bfad3ec602d1eull -> 0x43113d5b82059b43ull, final_hash 0x2ff813254130dde5ull -> 0x5aeb07c47b63d013ull [W8A R4]
//   Random30/0/0: boundary 0xe74142e41965c7f8ull -> 0xd66f88389fcd9547ull, final_hash 0x2ff813254130dde5ull -> 0x5aeb07c47b63d013ull [W8A R4]
//   Random30/1/1: boundary 0xffbc3c35359e842dull -> 0xe4fa3a6344099aa9ull, rows 0xa978037a0a737ed4ull -> 0xd45bcd6f2d572ec8ull, final_hash 0xb713984b2b953de4ull -> 0x8efe1379de796a86ull [W8A R4]
//   Random30/1/0: boundary 0xffbc3c35359e842dull -> 0xe4fa3a6344099aa9ull, final_hash 0xb713984b2b953de4ull -> 0x8efe1379de796a86ull [W8A R4]
//   Random31/0/1: boundary 0xc1d0985632aa85e5ull -> 0xae654f13b8fcd8e0ull, rows 0xb1ff4f1ff8ce7ff6ull -> 0xfc4e49dd5e112e39ull, trades 0xcad16841aad879ddull -> 0xaba92b6906e7cbd7ull, final_hash 0x0138efa3affc4452ull -> 0x76266a5b9ef30dd9ull, trade_count 9 -> 8 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random31/0/0: boundary 0xc1d0985632aa85e5ull -> 0xae654f13b8fcd8e0ull, trades 0xcad16841aad879ddull -> 0xaba92b6906e7cbd7ull, final_hash 0x0138efa3affc4452ull -> 0x76266a5b9ef30dd9ull, trade_count 9 -> 8 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random31/1/1: boundary 0x29c04e4a3f62fc66ull -> 0x20a319c3c35903f5ull, rows 0xd37bee2bcbd5b472ull -> 0x5b8d0644598201d6ull, trades 0xc42eba994cb3bc7full -> 0x70366bf507f34615ull, final_hash 0x3e283f49a532b649ull -> 0xcb15da47a5ac21e4ull, trade_count 9 -> 8 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random31/1/0: boundary 0x29c04e4a3f62fc66ull -> 0x20a319c3c35903f5ull, trades 0xc42eba994cb3bc7full -> 0x70366bf507f34615ull, final_hash 0x3e283f49a532b649ull -> 0xcb15da47a5ac21e4ull, trade_count 9 -> 8 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random32/0/1: boundary 0x69fb37996a1a6d8aull -> 0x7938e267e99e1ae2ull, rows 0x4eda1ad97257444eull -> 0x830f5d549f720bffull, trades 0xb5c0211fafc19569ull -> 0x28c568aab096b3f4ull, final_hash 0xf8f4a46581375bc8ull -> 0x52b7f53f1add68c3ull, trade_count 18 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random32/0/0: boundary 0x69fb37996a1a6d8aull -> 0x7938e267e99e1ae2ull, trades 0xb5c0211fafc19569ull -> 0x28c568aab096b3f4ull, final_hash 0xf8f4a46581375bc8ull -> 0x52b7f53f1add68c3ull, trade_count 18 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random32/1/1: boundary 0x6aa8d3b227f15c4cull -> 0x07895e91b01b07e9ull, rows 0xefb2e786712329f2ull -> 0x3e545e5e55b5b34aull, trades 0xb1a7a66b620c4fd5ull -> 0x31ca7f6e9e28a233ull, final_hash 0xe3b7a40e1641948dull -> 0x650e338da09e0df7ull, trade_count 19 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random32/1/0: boundary 0x6aa8d3b227f15c4cull -> 0x07895e91b01b07e9ull, trades 0xb1a7a66b620c4fd5ull -> 0x31ca7f6e9e28a233ull, final_hash 0xe3b7a40e1641948dull -> 0x650e338da09e0df7ull, trade_count 19 -> 17 [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random33/0/1: boundary 0x9651f8f876e48213ull -> 0xa6cc6efa0e6f3857ull, rows 0x70cd8d3b24d08640ull -> 0xfceb27f69369cac5ull, trades 0xb69ec34fc873583aull -> 0x011e89871f23d05bull, final_hash 0x495394a49a7d3245ull -> 0xb0153eac66d798a7ull, trade_count 13 -> 12 [W3B void exit]
//   Random33/0/0: boundary 0x9651f8f876e48213ull -> 0xa6cc6efa0e6f3857ull, trades 0xb69ec34fc873583aull -> 0x011e89871f23d05bull, final_hash 0x495394a49a7d3245ull -> 0xb0153eac66d798a7ull, trade_count 13 -> 12 [W3B void exit]
//   Random33/1/1: boundary 0x23686cd9a3fe0637ull -> 0xa8c495e9efbd8eedull, rows 0x17ecf363cd313d8aull -> 0xbfb3bb21f35e1383ull, trades 0xa51cad1078866f00ull -> 0xfde769901b66c55aull, final_hash 0xcefd187486fda5beull -> 0xd6051fe30b94c436ull, trade_count 13 -> 12 [W3B void exit]
//   Random33/1/0: boundary 0x23686cd9a3fe0637ull -> 0xa8c495e9efbd8eedull, trades 0xa51cad1078866f00ull -> 0xfde769901b66c55aull, final_hash 0xcefd187486fda5beull -> 0xd6051fe30b94c436ull, trade_count 13 -> 12 [W3B void exit]
//   Random34/0/1: boundary 0xb0d26d6ec62ee0b2ull -> 0x1e5d627e1af756e0ull, rows 0xe24b790bed6d8209ull -> 0x9f92abb6f41c803dull, final_hash 0xa8799fc8644a8be8ull -> 0x3d43f5ce9e1b1c4eull [W3B void exit; W8A R4]
//   Random34/0/0: boundary 0xb0d26d6ec62ee0b2ull -> 0x1e5d627e1af756e0ull, final_hash 0xa8799fc8644a8be8ull -> 0x3d43f5ce9e1b1c4eull [W3B void exit; W8A R4]
//   Random34/1/1: boundary 0x40ad548ed9e2d639ull -> 0x1702186e332219fdull, rows 0x2d022bb216743a64ull -> 0x9debd2a709932880ull, final_hash 0x0a4527b569ac8a10ull -> 0x83b6d02fc4870fc2ull [W3B void exit; W8A R4]
//   Random34/1/0: boundary 0x40ad548ed9e2d639ull -> 0x1702186e332219fdull, final_hash 0x0a4527b569ac8a10ull -> 0x83b6d02fc4870fc2ull [W3B void exit; W8A R4]
//   Random35/0/1: boundary 0xbdcab3fd94434657ull -> 0x93532c12dd05c8f8ull, rows 0x8f07aebde3f01772ull -> 0x470485b83257d3cbull, trades 0x834764512474d77cull -> 0x707dc61f9902c968ull, final_hash 0x4d118037ee1626adull -> 0x6aeef866f7922647ull, trade_count 24 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 M1; R1-CONSOLIDATE R1]
//   Random35/0/0: boundary 0xbdcab3fd94434657ull -> 0x93532c12dd05c8f8ull, trades 0x834764512474d77cull -> 0x707dc61f9902c968ull, final_hash 0x4d118037ee1626adull -> 0x6aeef866f7922647ull, trade_count 24 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 M1; R1-CONSOLIDATE R1]
//   Random35/1/1: boundary 0x9f158f1f03765f97ull -> 0x1f16fa70bb028179ull, rows 0x7ea6860a76f2a755ull -> 0xebced8f56faf89dcull, trades 0xce2ab725ccda6ebdull -> 0x7c4d53bac102a79eull, final_hash 0x07820fe3390f4450ull -> 0xa784e9e042779935ull, trade_count 24 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 M1; R1-CONSOLIDATE R1]
//   Random35/1/0: boundary 0x9f158f1f03765f97ull -> 0x1f16fa70bb028179ull, trades 0xce2ab725ccda6ebdull -> 0x7c4d53bac102a79eull, final_hash 0x07820fe3390f4450ull -> 0xa784e9e042779935ull, trade_count 24 -> 21 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 M1; R1-CONSOLIDATE R1]
//   Random36/0/1: boundary 0xc06367cbde7ecc9aull -> 0x5530362ed60c126dull, rows 0x9c13280fd6c4a76full -> 0x54bd05607173aa6bull, trades 0x71b837070cbf3386ull -> 0xf4c314289e107873ull, final_hash 0x4a73a58e1bf4c419ull -> 0x212db54b7b40d9baull, boundary_count 87 -> 84, trade_count 21 -> 20 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19c]
//   Random36/0/0: boundary 0xc06367cbde7ecc9aull -> 0x5530362ed60c126dull, trades 0x71b837070cbf3386ull -> 0xf4c314289e107873ull, final_hash 0x4a73a58e1bf4c419ull -> 0x212db54b7b40d9baull, boundary_count 87 -> 84, trade_count 21 -> 20 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19c]
//   Random36/1/1: boundary 0xf78394d86184426aull -> 0x10f19e1df0bfc765ull, rows 0xd52d837b120d0443ull -> 0xff9b419c51942aadull, trades 0xc91ddda9c3e46569ull -> 0x4957248d03a59eeaull, final_hash 0x1e90f705be3da39full -> 0x0ca6ed8a848b4f88ull, boundary_count 87 -> 84, trade_count 21 -> 20 [W3B F02 first id; W3B F02 ledger booking, superseded by W3B void exit; W3B void exit; W4 F08]
//   Random36/1/0: boundary 0xf78394d86184426aull -> 0x10f19e1df0bfc765ull, trades 0xc91ddda9c3e46569ull -> 0x4957248d03a59eeaull, final_hash 0x1e90f705be3da39full -> 0x0ca6ed8a848b4f88ull, boundary_count 87 -> 84, trade_count 21 -> 20 [W3B F02 first id; W3B F02 ledger booking, superseded by W3B void exit; W3B void exit; W4 F08]
//   Random37/0/1: boundary 0x8af4f686e8afd249ull -> 0x0e8497b06337b313ull, rows 0xb43a05dbbc636b7aull -> 0x1adac0628692b36cull, trades 0x52bf39ca2f2765c6ull -> 0x3698e45959c0e3faull, final_hash 0x8692adfc1cddb1c4ull -> 0x09ada14085aa6800ull, boundary_count 71 -> 68, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Random37/0/0: boundary 0x8af4f686e8afd249ull -> 0x0e8497b06337b313ull, trades 0x52bf39ca2f2765c6ull -> 0x3698e45959c0e3faull, final_hash 0x8692adfc1cddb1c4ull -> 0x09ada14085aa6800ull, boundary_count 71 -> 68, trade_count 18 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Random37/1/1: boundary 0xe82a3b0bd1861474ull -> 0x2cd9ee6e0acb218bull, rows 0x5ea014024a78fb97ull -> 0xe626fb41292b2064ull, trades 0x41de938f8a060981ull -> 0x6905921f420a2e09ull, final_hash 0x1ace37e580b56141ull -> 0xf428d94da051f1a3ull, boundary_count 70 -> 68, trade_count 17 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Random37/1/0: boundary 0xe82a3b0bd1861474ull -> 0x2cd9ee6e0acb218bull, trades 0x41de938f8a060981ull -> 0x6905921f420a2e09ull, final_hash 0x1ace37e580b56141ull -> 0xf428d94da051f1a3ull, boundary_count 70 -> 68, trade_count 17 -> 15 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; R1-CONSOLIDATE R1]
//   Random38/0/1: boundary 0xeaf05ca1aedc03d5ull -> 0x3ae43dc8cbf45c77ull, rows 0x80971876a1156ee8ull -> 0x74c616f05cd77691ull, trades 0xac12ee6aa53f424cull -> 0x54a52d324e3f9a97ull, final_hash 0x2d7719753c10f4bbull -> 0x36521d31d5e4e30aull, trade_count 25 -> 9 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W5 M2; W5 C2; W4 F08; W4 F14]
//   Random38/0/0: boundary 0xeaf05ca1aedc03d5ull -> 0x3ae43dc8cbf45c77ull, trades 0xac12ee6aa53f424cull -> 0x54a52d324e3f9a97ull, final_hash 0x2d7719753c10f4bbull -> 0x36521d31d5e4e30aull, trade_count 25 -> 9 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W5 M2; W5 C2; W4 F08; W4 F14]
//   Random38/1/1: boundary 0x8285a035265d771cull -> 0xb8987f947e11f828ull, rows 0x0d3b927724077789ull -> 0xd27af68d65e7f24aull, trades 0xc6a0970cf0716d7cull -> 0x1078dcc02e5a14b5ull, final_hash 0x09336a0556a2fbb7ull -> 0x64e1fcaebf205451ull, trade_count 25 -> 9 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W5 C2; W4 F08; W4 F14]
//   Random38/1/0: boundary 0x8285a035265d771cull -> 0xb8987f947e11f828ull, trades 0xc6a0970cf0716d7cull -> 0x1078dcc02e5a14b5ull, final_hash 0x09336a0556a2fbb7ull -> 0x64e1fcaebf205451ull, trade_count 25 -> 9 [TVDEF-DROPS R1; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W5 C2; W4 F08; W4 F14]
//   Random39/0/1: boundary 0xc989defa2d8cff00ull -> 0xe9c388052a13202aull, rows 0x31db41d49a36aaa6ull -> 0x1dbc13c6b0776cd5ull, final_hash 0xb8c88da9531702d9ull -> 0xf4d9f3205c30e58dull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random39/0/0: boundary 0xc989defa2d8cff00ull -> 0xe9c388052a13202aull, final_hash 0xb8c88da9531702d9ull -> 0xf4d9f3205c30e58dull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random39/1/1: boundary 0x588cc94ab848f7e6ull -> 0x16fc2000a3efe788ull, rows 0x3b1478373127afc0ull -> 0x5345722bb6f8f5c6ull, final_hash 0xbdab759ad2c5865cull -> 0x031d04dd65e46ae9ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random39/1/0: boundary 0x588cc94ab848f7e6ull -> 0x16fc2000a3efe788ull, final_hash 0xbdab759ad2c5865cull -> 0x031d04dd65e46ae9ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random40/0/1: boundary 0xc4f976129a95a285ull -> 0x630aca1d81d9a3d9ull, rows 0x389162a99c89a49cull -> 0x2772b62704839f14ull, trades 0x78c15a2f88a24807ull -> 0xfa904a6a264aca64ull, final_hash 0xcafbb488b92f9edaull -> 0xff8928292b458130ull [W3 F05 rule C; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random40/0/0: boundary 0xc4f976129a95a285ull -> 0x630aca1d81d9a3d9ull, trades 0x78c15a2f88a24807ull -> 0xfa904a6a264aca64ull, final_hash 0xcafbb488b92f9edaull -> 0xff8928292b458130ull [W3 F05 rule C; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random40/1/1: boundary 0x1cc6041a6a6fcdcdull -> 0x16f6d29d752b1b02ull, rows 0x33a043a5f0e45941ull -> 0x87f2102007dee2ffull, trades 0x41975079a7cc855bull -> 0x457989fe6088f21cull, final_hash 0x798fe8d7596aaa9cull -> 0xd740260ab955140cull [W3 F05 rule C; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random40/1/0: boundary 0x1cc6041a6a6fcdcdull -> 0x16f6d29d752b1b02ull, trades 0x41975079a7cc855bull -> 0x457989fe6088f21cull, final_hash 0x798fe8d7596aaa9cull -> 0xd740260ab955140cull [W3 F05 rule C; W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random41/0/1: boundary 0xb4b89e111611f4ceull -> 0x7fae0dae590a2a06ull, rows 0xea3d6d0c77bc7e76ull -> 0xeb3700f17478f99aull, trades 0xf6e95f1e1a0264bcull -> 0x9520c91295193892ull, final_hash 0x980f8a18b3d80a23ull -> 0xe1988c325c19350dull, boundary_count 51 -> 49, trade_count 6 -> 5 [W3B void exit]
//   Random41/0/0: boundary 0xb4b89e111611f4ceull -> 0x7fae0dae590a2a06ull, trades 0xf6e95f1e1a0264bcull -> 0x9520c91295193892ull, final_hash 0x980f8a18b3d80a23ull -> 0xe1988c325c19350dull, boundary_count 51 -> 49, trade_count 6 -> 5 [W3B void exit]
//   Random41/1/1: boundary 0xf15d27dad1575f3bull -> 0x7ef708ff93df2105ull, rows 0x9245b7b247af4cc1ull -> 0xca183a20221f587full, trades 0xf6e95f1e1a0264bcull -> 0x9520c91295193892ull, final_hash 0xc730d99532f512d8ull -> 0xdebdae0068274179ull, boundary_count 51 -> 49, trade_count 6 -> 5 [W3B void exit]
//   Random41/1/0: boundary 0xf15d27dad1575f3bull -> 0x7ef708ff93df2105ull, trades 0xf6e95f1e1a0264bcull -> 0x9520c91295193892ull, final_hash 0xc730d99532f512d8ull -> 0xdebdae0068274179ull, boundary_count 51 -> 49, trade_count 6 -> 5 [W3B void exit]
//   Random42/0/1: boundary 0x0da9b26d3847ab1full -> 0xd9c6b8d1a3e7f404ull, rows 0x9c9329bd9adc3676ull -> 0x7449fe1d24966dbbull, trades 0xd4d5dd88f1bc7129ull -> 0xd7697ec775faca38ull, final_hash 0x49a0634035589083ull -> 0x3938f0c23f35c481ull, trade_count 12 -> 13 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8E C1; W8A R-B]
//   Random42/0/0: boundary 0x0da9b26d3847ab1full -> 0xd9c6b8d1a3e7f404ull, trades 0xd4d5dd88f1bc7129ull -> 0xd7697ec775faca38ull, final_hash 0x49a0634035589083ull -> 0x3938f0c23f35c481ull, trade_count 12 -> 13 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8E C1; W8A R-B]
//   Random42/1/1: boundary 0xbd61639c42be9cb3ull -> 0x170c096ca2e1a5d8ull, rows 0x5d2fe8dd7d35b222ull -> 0x714318ce5a475b16ull, trades 0x292b9553961fa163ull -> 0x25bb0277fefbcdb0ull, final_hash 0xa80c78dbeddc4468ull -> 0xfe8452396cf5c313ull, trade_count 12 -> 13 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8E C1; W8A R-B]
//   Random42/1/0: boundary 0xbd61639c42be9cb3ull -> 0x170c096ca2e1a5d8ull, trades 0x292b9553961fa163ull -> 0x25bb0277fefbcdb0ull, final_hash 0xa80c78dbeddc4468ull -> 0xfe8452396cf5c313ull, trade_count 12 -> 13 [TVDEF-DROPS R2; W3B F02 first id; W3B F02 ledger booking; W3B void exit; W8E C1; W8A R-B]
//   Random43/0/1: boundary 0xc521512ba92a54c9ull -> 0x8e6bb457fef7a4c5ull, rows 0x5993bd8ceee91044ull -> 0x4274dbb74257f110ull, trades 0xbd8ba613082e0da5ull -> 0x8a290002ba9335b6ull, final_hash 0x5fbe995d73a8bff0ull -> 0x598141c24e351649ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random43/0/0: boundary 0xc521512ba92a54c9ull -> 0x8e6bb457fef7a4c5ull, trades 0xbd8ba613082e0da5ull -> 0x8a290002ba9335b6ull, final_hash 0x5fbe995d73a8bff0ull -> 0x598141c24e351649ull [W3B F02 first id; W3B F02 ledger booking; W3B void exit]
//   Random43/1/1: boundary 0xa9607e56c7e4b193ull -> 0x57615d26a78a4593ull, rows 0x941becccb8fe75ceull -> 0x951356055e6dd4d3ull, trades 0x572d3f9f9521e7caull -> 0x0c35e427d6c0e557ull, final_hash 0xfdc00d0d1ff0cbfbull -> 0x64eded952d140076ull [W3B F02 ledger booking; W3B void exit]
//   Random43/1/0: boundary 0xa9607e56c7e4b193ull -> 0x57615d26a78a4593ull, trades 0x572d3f9f9521e7caull -> 0x0c35e427d6c0e557ull, final_hash 0xfdc00d0d1ff0cbfbull -> 0x64eded952d140076ull [W3B F02 ledger booking; W3B void exit]
//   Random44/0/1: boundary 0x971e7c4a38e301ddull -> 0x3a7009cbf7950f96ull, rows 0x55249f0ae51722feull -> 0xcc18b619119c3134ull, trades 0x316199af1a335dbfull -> 0xd7933446c85fc9e2ull, final_hash 0x5a13570337a638b5ull -> 0xac6703f9b030be55ull, boundary_count 69 -> 65, trade_count 15 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19c]
//   Random44/0/0: boundary 0x971e7c4a38e301ddull -> 0x3a7009cbf7950f96ull, trades 0x316199af1a335dbfull -> 0xd7933446c85fc9e2ull, final_hash 0x5a13570337a638b5ull -> 0xac6703f9b030be55ull, boundary_count 69 -> 65, trade_count 15 -> 13 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19c]
//   Random44/1/1: boundary 0x3ee064c4f943e93aull -> 0xf26161d992b0b9d7ull, rows 0xbc62822019ded427ull -> 0x056fb98ed64a27afull, trades 0x9f0e22f8fb2632f6ull -> 0xd15149879bf25c79ull, final_hash 0xc18096028ba7be64ull -> 0x6dfe3e836bc8a2f5ull, boundary_count 69 -> 66 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random44/1/0: boundary 0x3ee064c4f943e93aull -> 0xf26161d992b0b9d7ull, trades 0x9f0e22f8fb2632f6ull -> 0xd15149879bf25c79ull, final_hash 0xc18096028ba7be64ull -> 0x6dfe3e836bc8a2f5ull, boundary_count 69 -> 66 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W4 F19a]
//   Random45/0/1: boundary 0xa155b80e37d41cf2ull -> 0xa9aede02d7738b3eull, rows 0xbacd32f5d8b94030ull -> 0xfc0f5911f30d9d12ull, trades 0xf7b0f07cda17c110ull -> 0x3b74877f071e9e8dull, final_hash 0x598c179a2d5c735cull -> 0x7929e5e00aefb02aull, boundary_count 99 -> 97, trade_count 25 -> 26 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8A R-B; R1-CONSOLIDATE R1]
//   Random45/0/0: boundary 0xa155b80e37d41cf2ull -> 0xa9aede02d7738b3eull, trades 0xf7b0f07cda17c110ull -> 0x3b74877f071e9e8dull, final_hash 0x598c179a2d5c735cull -> 0x7929e5e00aefb02aull, boundary_count 99 -> 97, trade_count 25 -> 26 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8A R-B; R1-CONSOLIDATE R1]
//   Random45/1/1: boundary 0x9644e4dd1f925cdfull -> 0xb01451bde1e6b5f2ull, rows 0x2cc38b54d184cf0aull -> 0xf4ece9e6145c366bull, trades 0x797aff1ec9239dffull -> 0x1b46de5d0b439457ull, final_hash 0x290739687f68f174ull -> 0xb59df0785bed3a18ull, boundary_count 99 -> 97, trade_count 24 -> 25 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8A R-B; R1-CONSOLIDATE R1]
//   Random45/1/0: boundary 0x9644e4dd1f925cdfull -> 0xb01451bde1e6b5f2ull, trades 0x797aff1ec9239dffull -> 0x1b46de5d0b439457ull, final_hash 0x290739687f68f174ull -> 0xb59df0785bed3a18ull, boundary_count 99 -> 97, trade_count 24 -> 25 [W3B F02 first id; W3B F02 ledger booking; W3B g1; W3B void exit; W8A R-B; R1-CONSOLIDATE R1]
//   Random46/0/1: boundary 0xa74f65268d41a1bbull -> 0x0db5fb4f84dc8c3aull, rows 0xfc4e6864d98d62c0ull -> 0x639c991bae48e61cull, final_hash 0x7097cd184da8a4dcull -> 0x2f60e2405e5c723full [W3B void exit; W8A R4]
//   Random46/0/0: boundary 0xa74f65268d41a1bbull -> 0x0db5fb4f84dc8c3aull, final_hash 0x7097cd184da8a4dcull -> 0x2f60e2405e5c723full [W3B void exit; W8A R4]
//   Random46/1/1: boundary 0xb1406e1f21710782ull -> 0xc0572ab1c2b3b3ddull, rows 0xe0b910f752cbc3adull -> 0x9ebe80a74159857cull, final_hash 0x2539296694529005ull -> 0x671d14ca701f5df6ull [W3B void exit; W8A R4]
//   Random46/1/0: boundary 0xb1406e1f21710782ull -> 0xc0572ab1c2b3b3ddull, final_hash 0x2539296694529005ull -> 0x671d14ca701f5df6ull [W3B void exit; W8A R4]
//   Random47/0/1: boundary 0x214dc1308457a01aull -> 0x23bb0900769c951dull, rows 0xe293c7196f5c9f95ull -> 0x1c9ae7de7a2c8332ull, trades 0x072cedccf12d6a31ull -> 0x522497c980811fd9ull, final_hash 0x8176d644a621a19eull -> 0x959c68a6579a5c69ull [W3B void exit]
//   Random47/0/0: boundary 0x214dc1308457a01aull -> 0x23bb0900769c951dull, trades 0x072cedccf12d6a31ull -> 0x522497c980811fd9ull, final_hash 0x8176d644a621a19eull -> 0x959c68a6579a5c69ull [W3B void exit]
//   Random47/1/1: boundary 0xfe949f5ba003397dull -> 0xa981a443d153dba3ull, rows 0xe112a9940c547399ull -> 0xc1d3e14bba2f6ca2ull, trades 0x33811b67f07874f3ull -> 0xd71728e8746a5845ull, final_hash 0x9ee620e1bf252639ull -> 0x559e4d89c592a7d1ull [W3B void exit]
//   Random47/1/0: boundary 0xfe949f5ba003397dull -> 0xa981a443d153dba3ull, trades 0x33811b67f07874f3ull -> 0xd71728e8746a5845ull, final_hash 0x9ee620e1bf252639ull -> 0x559e4d89c592a7d1ull [W3B void exit]
//   Random48/0/1: boundary 0x21c0bf9ae910d3acull -> 0x61058fc7c0ec1a28ull, rows 0xe2d98f0ffb5c1b96ull -> 0xa10d595a24d1c087ull, trades 0xda74ceb1a9e7d904ull -> 0x935393ec134d3933ull, final_hash 0x1e560afd06471537ull -> 0x41e9f2a144887b14ull, trade_count 8 -> 7 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R2]
//   Random48/0/0: boundary 0x21c0bf9ae910d3acull -> 0x61058fc7c0ec1a28ull, trades 0xda74ceb1a9e7d904ull -> 0x935393ec134d3933ull, final_hash 0x1e560afd06471537ull -> 0x41e9f2a144887b14ull, trade_count 8 -> 7 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W8A R2]
//   Random48/1/1: boundary 0x4fd90c8ca1025428ull -> 0x1495a6b529cfcd63ull, rows 0x7706d5e9ebfd9226ull -> 0x54746877dcc8bea2ull, trades 0x0876fbb01d747e2dull -> 0x5386898595fcb912ull, final_hash 0xee01efa6d7b40515ull -> 0x09e8b8606f92c5b5ull, trade_count 8 -> 7 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W5 C1; W4 F08; W8A R2]
//   Random48/1/0: boundary 0x4fd90c8ca1025428ull -> 0x1495a6b529cfcd63ull, trades 0x0876fbb01d747e2dull -> 0x5386898595fcb912ull, final_hash 0xee01efa6d7b40515ull -> 0x09e8b8606f92c5b5ull, trade_count 8 -> 7 [W3B F02 first id; W3B F02 ledger booking; W3B void exit; W4 F08; W8A R2]
// INT28-FIX re-harvested it once on the integrated tree (INT28 4efcb8c5 and
// INT28-FIX's ten commits) with PINEFORGE_QUIET_BAR_HARVEST; the same harvest against
// INT28 reproduces every old row. 26 of 276 runs move, each marked with the
// commit(s) at whose boundary it moved (a harvest at every commit): only the
// picked W5B-ENG-MARGIN-RESIDUAL rules SB and PC move any; CA, PA, SZ, FU,
// MS, SS, OU and OF move none. The rules are TradingView's (their lanes'
// tests/fixtures tapes):
//   CloseOrders/0/1: rows 0xe6ad79a7e9fad579ull -> 0x6afa6d0d92dbbd5bull [W5B PC]
//   CloseOrders/1/1: rows 0xc0c9c380b8d2c546ull -> 0xe8a3b469eb54c2c5ull [W5B PC]
//   Random08/1/1: boundary 0x5c65b6eaad7034c5ull -> 0x3c4f2aa9834d7c3dull, rows 0x51bf07a4a66e31a1ull -> 0xa77ade66b2b9b612ull, trades 0x071e4e261711abeeull -> 0x9318fb890727b7eaull, final_hash 0x8eb670e9d6661682ull -> 0x7d3886c233269af9ull, boundary_count 95 -> 94, trade_count 26 -> 25 [W5B SB]
//   Random08/1/0: boundary 0x5c65b6eaad7034c5ull -> 0x3c4f2aa9834d7c3dull, trades 0x071e4e261711abeeull -> 0x9318fb890727b7eaull, final_hash 0x8eb670e9d6661682ull -> 0x7d3886c233269af9ull, boundary_count 95 -> 94, trade_count 26 -> 25 [W5B SB]
//   Random14/0/1: rows 0x419caa2f1494ac0aull -> 0x180ca42c3e610d01ull [W5B PC]
//   Random14/1/1: rows 0x281d0596aee9b7e0ull -> 0xfd29f8252edce4edull [W5B PC]
//   Random16/0/1: boundary 0x586e2a0af3b884eeull -> 0x9443463c4ce11014ull, rows 0x487f8658f4ce0da9ull -> 0xf8517013485b3f4dull, trades 0xbb86ab41adee8d23ull -> 0x894ec2a3cafc14c8ull, final_hash 0x3d540c1799b82d36ull -> 0xa632bad9177f6447ull, trade_count 15 -> 14 [W5B SB; W5B PC]
//   Random16/0/0: boundary 0x586e2a0af3b884eeull -> 0x9443463c4ce11014ull, trades 0xbb86ab41adee8d23ull -> 0x894ec2a3cafc14c8ull, final_hash 0x3d540c1799b82d36ull -> 0xa632bad9177f6447ull, trade_count 15 -> 14 [W5B SB]
//   Random16/1/1: boundary 0x09fefcd04a59b9ceull -> 0xfbc1982865dc4bf2ull, rows 0xbe70fa5a6c43d5cdull -> 0x69ddeb53e5249c41ull, trades 0x5f87751701a631a7ull -> 0xe0b0e7cbf08a5214ull, final_hash 0xa979bdea5746599bull -> 0x3bb26d3efd6a0234ull, trade_count 15 -> 14 [W5B SB; W5B PC]
//   Random16/1/0: boundary 0x09fefcd04a59b9ceull -> 0xfbc1982865dc4bf2ull, trades 0x5f87751701a631a7ull -> 0xe0b0e7cbf08a5214ull, final_hash 0xa979bdea5746599bull -> 0x3bb26d3efd6a0234ull, trade_count 15 -> 14 [W5B SB]
//   Random26/0/1: boundary 0x745f6d660f0ff7c1ull -> 0x8add97bc79f4eed8ull, rows 0x3fe4ca9a714df290ull -> 0x76089d2a60f2b03bull, trades 0x44110f44174fa225ull -> 0x5a3b59068266bdaeull, final_hash 0x7400318cf813e724ull -> 0xe60d1a08ab2e3e6full, trade_count 11 -> 10 [W5B SB]
//   Random26/0/0: boundary 0x745f6d660f0ff7c1ull -> 0x8add97bc79f4eed8ull, trades 0x44110f44174fa225ull -> 0x5a3b59068266bdaeull, final_hash 0x7400318cf813e724ull -> 0xe60d1a08ab2e3e6full, trade_count 11 -> 10 [W5B SB]
//   Random26/1/1: boundary 0x27527ee4f99b804dull -> 0xcc3bc197c790f005ull, rows 0xd7d832758f013ec4ull -> 0x33cb5420c38eaa7dull, trades 0x7762169e1cd29d9full -> 0xe765763e4be6d1b7ull, final_hash 0x89642d94f10f9107ull -> 0xddd2f53991df857aull, trade_count 12 -> 11 [W5B SB]
//   Random26/1/0: boundary 0x27527ee4f99b804dull -> 0xcc3bc197c790f005ull, trades 0x7762169e1cd29d9full -> 0xe765763e4be6d1b7ull, final_hash 0x89642d94f10f9107ull -> 0xddd2f53991df857aull, trade_count 12 -> 11 [W5B SB]
//   Random28/0/1: boundary 0x6a15e4675c7e99a1ull -> 0x51b4ca4263f3d628ull, rows 0x3530ef3b232b11e4ull -> 0xf6099194c8e66357ull [W5B PC]
//   Random28/0/0: boundary 0x6a15e4675c7e99a1ull -> 0x51b4ca4263f3d628ull [W5B PC]
//   Random28/1/1: boundary 0x7c85ce498057e507ull -> 0xc43bc41443863b0dull, rows 0xd3dd35946efd7377ull -> 0xe0dbf72894e9d6adull [W5B PC]
//   Random28/1/0: boundary 0x7c85ce498057e507ull -> 0xc43bc41443863b0dull [W5B PC]
//   Random31/0/1: boundary 0xae654f13b8fcd8e0ull -> 0xa5c29650fe4d117full, rows 0xfc4e49dd5e112e39ull -> 0xb4e024403850088cull, trades 0xaba92b6906e7cbd7ull -> 0x101e40b9d91b7277ull, final_hash 0x76266a5b9ef30dd9ull -> 0x262f8705e1e7372eull, trade_count 8 -> 7 [W5B SB]
//   Random31/0/0: boundary 0xae654f13b8fcd8e0ull -> 0xa5c29650fe4d117full, trades 0xaba92b6906e7cbd7ull -> 0x101e40b9d91b7277ull, final_hash 0x76266a5b9ef30dd9ull -> 0x262f8705e1e7372eull, trade_count 8 -> 7 [W5B SB]
//   Random31/1/1: boundary 0x20a319c3c35903f5ull -> 0x7e35382dbbbad5f5ull, rows 0x5b8d0644598201d6ull -> 0x2c33aa8444715ca0ull, trades 0x70366bf507f34615ull -> 0x2a8f80e16d64b4eeull, final_hash 0xcb15da47a5ac21e4ull -> 0x3c874ea38a77d698ull, trade_count 8 -> 7 [W5B SB]
//   Random31/1/0: boundary 0x20a319c3c35903f5ull -> 0x7e35382dbbbad5f5ull, trades 0x70366bf507f34615ull -> 0x2a8f80e16d64b4eeull, final_hash 0xcb15da47a5ac21e4ull -> 0x3c874ea38a77d698ull, trade_count 8 -> 7 [W5B SB]
//   Random44/0/1: boundary 0x3a7009cbf7950f96ull -> 0x732debc0dd6f7cd7ull, rows 0xcc18b619119c3134ull -> 0xd287096c7a061725ull, trades 0xd7933446c85fc9e2ull -> 0x01cadf1a08d256deull, final_hash 0xac6703f9b030be55ull -> 0xc64966b83e7cef78ull [W5B SB]
//   Random44/0/0: boundary 0x3a7009cbf7950f96ull -> 0x732debc0dd6f7cd7ull, trades 0xd7933446c85fc9e2ull -> 0x01cadf1a08d256deull, final_hash 0xac6703f9b030be55ull -> 0xc64966b83e7cef78ull [W5B SB]
//   Random44/1/1: boundary 0xf26161d992b0b9d7ull -> 0xe4b01bfef621cc40ull, rows 0x056fb98ed64a27afull -> 0x12aff34f2be5d18cull, trades 0xd15149879bf25c79ull -> 0x346e57041dd6b6cdull, final_hash 0x6dfe3e836bc8a2f5ull -> 0xc8470526b19e84d5ull, boundary_count 66 -> 65, trade_count 15 -> 14 [W5B SB]
//   Random44/1/0: boundary 0xf26161d992b0b9d7ull -> 0xe4b01bfef621cc40ull, trades 0xd15149879bf25c79ull -> 0x346e57041dd6b6cdull, final_hash 0x6dfe3e836bc8a2f5ull -> 0xc8470526b19e84d5ull, boundary_count 66 -> 65, trade_count 15 -> 14 [W5B SB]
// INT29 re-harvested it once on the integrated tree (main 74491fa1, lane
// W10-DIAG-UNKNOWN's ten picks, lane W11-ENG-TIME-COLOR's eleven, INT29's own
// three commits, lane W12-ENG-TIME's thirteen and lane W13-ENG-MARGIN-OPP's
// four) with PINEFORGE_QUIET_BAR_HARVEST; the same harvest against
// main reproduces every old row. 4 of 276 runs move, and a harvest at each side
// of the W10 CAP-ON pick places all four there: the tree just before it
// reproduces main's rows, the tree at it already has the head's. They are the one
// scenario that sets a positive max_intraday_filled_orders, whose source
// statement now turns on the Pine cap switches it does not declare
// (tests/fixtures/intraday_cap_tv); the lane's own harvest on its tree gave
// the same values:
//   IntradayCapDeferred/0/1: boundary 0xde19edf4087b930dull -> 0x56c52ed78e3bf64eull, rows 0x560de539b138ea41ull -> 0xccd84788a40fdeaaull, final_hash 0x7730e7fe703663edull -> 0x30cddb814e876d43ull [W10 CAP-ON]
//   IntradayCapDeferred/0/0: boundary 0xde19edf4087b930dull -> 0x56c52ed78e3bf64eull, final_hash 0x7730e7fe703663edull -> 0x30cddb814e876d43ull [W10 CAP-ON]
//   IntradayCapDeferred/1/1: boundary 0x293bdcc491ae22d7ull -> 0x425990cfcb5d5162ull, rows 0xd165d92830916a1cull -> 0x35e7cf4c3a06c2cfull, final_hash 0x7ba4fe8defa15d4aull -> 0x782c119eeff543e4ull [W10 CAP-ON]
//   IntradayCapDeferred/1/0: boundary 0x293bdcc491ae22d7ull -> 0x425990cfcb5d5162ull, final_hash 0x7ba4fe8defa15d4aull -> 0x782c119eeff543e4ull [W10 CAP-ON]
// Lane TAIL-D re-harvested it once on its tree (8988faff and the lane's ten
// rule commits) with PINEFORGE_QUIET_BAR_HARVEST; the same harvest against
// 8988faff reproduces every old row. 40 of 276 runs move, and a harvest after
// each rule commit places each move at the commits named in brackets: FP17
// (an exit limit the recalculation of an entry's open fill places at or
// through the open print fills there, tests/fixtures/coof_open_limit) and X1
// (a strategy.exit call that names a level withdraws the price leg it leaves
// na, tests/fixtures/exit_reissue_na). No other rule moves a run:
//   Random02/0/1: boundary 0x110e0c47b38884fdull -> 0xbc3d35821c7f0340ull, rows 0xedcdbd9e14af6a21ull -> 0x6de37f7191594538ull, trades 0x7a3972561c477964ull -> 0x81aa2a130803e2efull, final_hash 0x39b31da3cd2bb72cull -> 0x55cb3a23ce562e01ull [TAIL-D FP17]
//   Random02/0/0: boundary 0x110e0c47b38884fdull -> 0xbc3d35821c7f0340ull, trades 0x7a3972561c477964ull -> 0x81aa2a130803e2efull, final_hash 0x39b31da3cd2bb72cull -> 0x55cb3a23ce562e01ull [TAIL-D FP17]
//   Random08/0/1: boundary 0xd8361e46c395ccb5ull -> 0xd37ef6cf9b7af14full, rows 0x5aa540a27984a70aull -> 0x42be52c508c5526bull, trades 0xe67dcbbac61f0c9full -> 0x2b85135e59cd2071ull, final_hash 0x818b37eba85784f1ull -> 0x8a24a317e5cc18bdull, boundary_count 95 -> 94 [TAIL-D FP17]
//   Random08/0/0: boundary 0xd8361e46c395ccb5ull -> 0xd37ef6cf9b7af14full, trades 0xe67dcbbac61f0c9full -> 0x2b85135e59cd2071ull, final_hash 0x818b37eba85784f1ull -> 0x8a24a317e5cc18bdull, boundary_count 95 -> 94 [TAIL-D FP17]
//   Random18/0/1: boundary 0x46c44a4b3f43d1f1ull -> 0x5fc781164fdbdbf7ull, rows 0x162fb95afde1b922ull -> 0xdffa80df9044f33bull, final_hash 0x65cc4963fe02e966ull -> 0xfe3755cafa818de6ull [TAIL-D FP17]
//   Random18/0/0: boundary 0x46c44a4b3f43d1f1ull -> 0x5fc781164fdbdbf7ull, final_hash 0x65cc4963fe02e966ull -> 0xfe3755cafa818de6ull [TAIL-D FP17]
//   Random25/0/1: boundary 0x871b48a97b54a1a7ull -> 0x07e6b9cb40382412ull, rows 0xf85a29bfb38c03dfull -> 0x67ef493ae25a3a47ull [TAIL-D X1]
//   Random25/0/0: boundary 0x871b48a97b54a1a7ull -> 0x07e6b9cb40382412ull [TAIL-D X1]
//   Random25/1/1: boundary 0xa177541d9a0b1c6full -> 0x23144355d1df6f60ull, rows 0xc3a837662f5bbacdull -> 0xb2174560120a3ec8ull [TAIL-D X1]
//   Random25/1/0: boundary 0xa177541d9a0b1c6full -> 0x23144355d1df6f60ull [TAIL-D X1]
//   Random31/0/1: boundary 0xa5c29650fe4d117full -> 0x94c6de025e8104bbull, rows 0xb4e024403850088cull -> 0x5990d1ebd754a80full, final_hash 0x262f8705e1e7372eull -> 0x12810545107c706bull [TAIL-D X1]
//   Random31/0/0: boundary 0xa5c29650fe4d117full -> 0x94c6de025e8104bbull, final_hash 0x262f8705e1e7372eull -> 0x12810545107c706bull [TAIL-D X1]
//   Random31/1/1: boundary 0x7e35382dbbbad5f5ull -> 0x17ef40f8ddd917b7ull, rows 0x2c33aa8444715ca0ull -> 0xe21f59451f2cf451ull, final_hash 0x3c874ea38a77d698ull -> 0x2c1e6a83a85b8cdbull [TAIL-D X1]
//   Random31/1/0: boundary 0x7e35382dbbbad5f5ull -> 0x17ef40f8ddd917b7ull, final_hash 0x3c874ea38a77d698ull -> 0x2c1e6a83a85b8cdbull [TAIL-D X1]
//   Random33/0/1: boundary 0xa6cc6efa0e6f3857ull -> 0xe3c91c1fe2e77f66ull, rows 0xfceb27f69369cac5ull -> 0x8ed11649e8ee34b0ull, final_hash 0xb0153eac66d798a7ull -> 0x58a3655fa601d4a7ull [TAIL-D X1]
//   Random33/0/0: boundary 0xa6cc6efa0e6f3857ull -> 0xe3c91c1fe2e77f66ull, final_hash 0xb0153eac66d798a7ull -> 0x58a3655fa601d4a7ull [TAIL-D X1]
//   Random33/1/1: boundary 0xa8c495e9efbd8eedull -> 0x3ae199c5f1e09198ull, rows 0xbfb3bb21f35e1383ull -> 0xeb34af0b85087c6full, final_hash 0xd6051fe30b94c436ull -> 0x5537a57c513e7f17ull [TAIL-D X1]
//   Random33/1/0: boundary 0xa8c495e9efbd8eedull -> 0x3ae199c5f1e09198ull, final_hash 0xd6051fe30b94c436ull -> 0x5537a57c513e7f17ull [TAIL-D X1]
//   Random38/0/1: boundary 0x3ae43dc8cbf45c77ull -> 0x7a72e075d0bcf84full, rows 0x74c616f05cd77691ull -> 0x8fb7a63eda294548ull, final_hash 0x36521d31d5e4e30aull -> 0x536763f10d3d8e4bull [TAIL-D X1]
//   Random38/0/0: boundary 0x3ae43dc8cbf45c77ull -> 0x7a72e075d0bcf84full, final_hash 0x36521d31d5e4e30aull -> 0x536763f10d3d8e4bull [TAIL-D X1]
//   Random38/1/1: boundary 0xb8987f947e11f828ull -> 0xeb586814ea3635e5ull, rows 0xd27af68d65e7f24aull -> 0x7cf6f74496bcc8cfull, final_hash 0x64e1fcaebf205451ull -> 0x09474c55e153dae8ull [TAIL-D X1]
//   Random38/1/0: boundary 0xb8987f947e11f828ull -> 0xeb586814ea3635e5ull, final_hash 0x64e1fcaebf205451ull -> 0x09474c55e153dae8ull [TAIL-D X1]
//   Random40/0/1: boundary 0x630aca1d81d9a3d9ull -> 0x82056ea3cd14b091ull, rows 0x2772b62704839f14ull -> 0xac6675ee324c1a9dull [TAIL-D X1]
//   Random40/0/0: boundary 0x630aca1d81d9a3d9ull -> 0x82056ea3cd14b091ull [TAIL-D X1]
//   Random40/1/1: boundary 0x16f6d29d752b1b02ull -> 0xea2aea67bd36b8feull, rows 0x87f2102007dee2ffull -> 0x583d751d33738ce3ull [TAIL-D X1]
//   Random40/1/0: boundary 0x16f6d29d752b1b02ull -> 0xea2aea67bd36b8feull [TAIL-D X1]
//   Random42/0/1: boundary 0xd9c6b8d1a3e7f404ull -> 0x1c36c6f9257072d0ull, rows 0x7449fe1d24966dbbull -> 0xccc61f07165cc2bbull [TAIL-D X1]
//   Random42/0/0: boundary 0xd9c6b8d1a3e7f404ull -> 0x1c36c6f9257072d0ull [TAIL-D X1]
//   Random42/1/1: boundary 0x170c096ca2e1a5d8ull -> 0xcc5a06dab05745c3ull, rows 0x714318ce5a475b16ull -> 0x279b780c0333235dull [TAIL-D X1]
//   Random42/1/0: boundary 0x170c096ca2e1a5d8ull -> 0xcc5a06dab05745c3ull [TAIL-D X1]
//   Random43/0/1: boundary 0x8e6bb457fef7a4c5ull -> 0x3230c4ca9a85bc81ull, rows 0x4274dbb74257f110ull -> 0x2c12e4a2a8a73638ull [TAIL-D X1]
//   Random43/0/0: boundary 0x8e6bb457fef7a4c5ull -> 0x3230c4ca9a85bc81ull [TAIL-D X1]
//   Random43/1/1: boundary 0x57615d26a78a4593ull -> 0xea6a2ed66ea0e0adull, rows 0x951356055e6dd4d3ull -> 0xa419d78c390bc110ull [TAIL-D X1]
//   Random43/1/0: boundary 0x57615d26a78a4593ull -> 0xea6a2ed66ea0e0adull [TAIL-D X1]
//   Random44/0/1: boundary 0x732debc0dd6f7cd7ull -> 0xb735db6aedc64d97ull, rows 0xd287096c7a061725ull -> 0x8e28c384e22ee775ull, trades 0x01cadf1a08d256deull -> 0x4398ebf224294454ull, final_hash 0xc64966b83e7cef78ull -> 0xdafe4531c6bb91c6ull [TAIL-D FP17]
//   Random44/0/0: boundary 0x732debc0dd6f7cd7ull -> 0xb735db6aedc64d97ull, trades 0x01cadf1a08d256deull -> 0x4398ebf224294454ull, final_hash 0xc64966b83e7cef78ull -> 0xdafe4531c6bb91c6ull [TAIL-D FP17]
//   Random45/0/1: boundary 0xa9aede02d7738b3eull -> 0x539e45fa8be150efull, rows 0xfc0f5911f30d9d12ull -> 0x877dfd1bbe564c03ull, final_hash 0x7929e5e00aefb02aull -> 0xec3d2f5ee99b238full [TAIL-D FP17, X1]
//   Random45/0/0: boundary 0xa9aede02d7738b3eull -> 0x539e45fa8be150efull, final_hash 0x7929e5e00aefb02aull -> 0xec3d2f5ee99b238full [TAIL-D FP17, X1]
//   Random45/1/1: boundary 0xb01451bde1e6b5f2ull -> 0xe76d1aa822a400efull, rows 0xf4ece9e6145c366bull -> 0x13464fddaa283992ull, trades 0x1b46de5d0b439457ull -> 0x4b078e8cd5d0038bull, final_hash 0xb59df0785bed3a18ull -> 0x793b98d2fd6175daull [TAIL-D X1]
//   Random45/1/0: boundary 0xb01451bde1e6b5f2ull -> 0xe76d1aa822a400efull, trades 0x1b46de5d0b439457ull -> 0x4b078e8cd5d0038bull, final_hash 0xb59df0785bed3a18ull -> 0x793b98d2fd6175daull [TAIL-D X1]
// Lane TAIL-H re-harvested it once on its tree (engine 9e6196ca and the
// lane's commits) with PINEFORGE_QUIET_BAR_HARVEST: the harvest on the lane's rule-PS commit
// reproduces every old value and the one on its rule-PK commit every new
// one, so each move is rule PK's (a margin call hands the position it leaves
// to the exits in their queue order again, tests/fixtures/exit_reservation);
// the trade digests, finals and counts do not move:
//   MarginCalls/0/1: boundary 0x7685d0a40457d46full -> 0x005d6993f2b1c2deull, rows 0xaafecdc80e0c12a1ull -> 0x3210e1484b83ce34ull [TAIL-H PK]
//   MarginCalls/0/0: boundary 0x7685d0a40457d46full -> 0x005d6993f2b1c2deull [TAIL-H PK]
// R5 lane TAIL-C re-harvested it once on its tree (r5/tail-c: lane W8E-EXITS's
// rule A picked onto INT29 8988faff, then TAIL-C's six rules) with
// PINEFORGE_QUIET_BAR_HARVEST; the same harvest against 8988faff reproduces
// every old row. 8 of 276 runs move, each marked with the commit at whose
// boundary it moved (a harvest at every commit; W8E's rule A and TAIL-C's
// magnifier, admission and infinite-quantity rules move none; W2 = the refill
// after a fill forced onto a leg's end, OPEN = the exit stop born through by a
// later open fill, POOC = the close pass's priced adds):
//   CloseOrders/0/1: boundary 0x184e8d327c5e2707ull -> 0xd105d5bf8915b2ceull, rows 0x6afa6d0d92dbbd5bull -> 0x31daa635496f2629ull, trades 0x18d35429ed7abeedull -> 0x61426583c5cf1a23ull, final_hash 0xf3223f1237a95489ull -> 0xcd9095a392afa563ull [TAIL-C POOC]
//   CloseOrders/0/0: boundary 0x184e8d327c5e2707ull -> 0xd105d5bf8915b2ceull, trades 0x18d35429ed7abeedull -> 0x61426583c5cf1a23ull, final_hash 0xf3223f1237a95489ull -> 0xcd9095a392afa563ull [TAIL-C POOC]
//   CloseOrders/1/1: boundary 0xb4e85bb5577186c9ull -> 0xfa10291c03058ad9ull, rows 0xe8a3b469eb54c2c5ull -> 0x44a8a845fb940d81ull, trades 0x30b8367772531729ull -> 0x30d51e9c8069ca8full, final_hash 0xb29e5a815acd8d0dull -> 0xaee588c13e83753bull [TAIL-C POOC]
//   CloseOrders/1/0: boundary 0xb4e85bb5577186c9ull -> 0xfa10291c03058ad9ull, trades 0x30b8367772531729ull -> 0x30d51e9c8069ca8full, final_hash 0xb29e5a815acd8d0dull -> 0xaee588c13e83753bull [TAIL-C POOC]
//   Random44/0/1: boundary 0x732debc0dd6f7cd7ull -> 0xa4d4997f7052067dull, rows 0xd287096c7a061725ull -> 0x87496f5cfd981becull, final_hash 0xc64966b83e7cef78ull -> 0x154f1fc08ce219c7ull [TAIL-C OPEN]
//   Random44/0/0: boundary 0x732debc0dd6f7cd7ull -> 0xa4d4997f7052067dull, final_hash 0xc64966b83e7cef78ull -> 0x154f1fc08ce219c7ull [TAIL-C OPEN]
//   Random45/0/1: boundary 0xa9aede02d7738b3eull -> 0x3c00afd78b2f850cull, rows 0xfc0f5911f30d9d12ull -> 0x622fccbecc7365a7ull, trades 0x3b74877f071e9e8dull -> 0x9e927212c83cadc5ull, final_hash 0x7929e5e00aefb02aull -> 0x805d6b921dffe691ull, boundary_count 97 -> 95, trade_count 26 -> 25 [TAIL-C OPEN]
//   Random45/0/0: boundary 0xa9aede02d7738b3eull -> 0x3c00afd78b2f850cull, trades 0x3b74877f071e9e8dull -> 0x9e927212c83cadc5ull, final_hash 0x7929e5e00aefb02aull -> 0x805d6b921dffe691ull, boundary_count 97 -> 95, trade_count 26 -> 25 [TAIL-C OPEN]
// INT30 harvested it once more on the integrated tree (engine main 7239ab37
// with lanes TAIL-A, TAIL-B, TAIL-G, TAIL-D, TAIL-H, TAIL-E and TAIL-C picked,
// in that order) and at each lane boundary of that history. main reproduces
// every old row; each row above moves at its own lane's picks and equals
// that lane's value (40 at TAIL-D's, 2 at TAIL-H's rule PK, 8 at TAIL-C's);
// TAIL-A, TAIL-B, TAIL-G, TAIL-H's rule PS, TAIL-E and TAIL-C's
// infinite-quantity rule move none. Four runs move at both TAIL-D's and
// TAIL-C's picks; with both lanes' rules in one run each is neither lane's
// value (main -> the integrated tree):
//   Random44/0/1: boundary 0x732debc0dd6f7cd7ull -> 0x6c0411f1f58edd3dull, rows 0xd287096c7a061725ull -> 0x2d81b13530f2e468ull, trades 0x01cadf1a08d256deull -> 0x4398ebf224294454ull, final_hash 0xc64966b83e7cef78ull -> 0x58aa329b6e21b01aull [TAIL-D then TAIL-C]
//   Random44/0/0: boundary 0x732debc0dd6f7cd7ull -> 0x6c0411f1f58edd3dull, trades 0x01cadf1a08d256deull -> 0x4398ebf224294454ull, final_hash 0xc64966b83e7cef78ull -> 0x58aa329b6e21b01aull [TAIL-D then TAIL-C]
//   Random45/0/1: boundary 0xa9aede02d7738b3eull -> 0x2bcba0ebccc11c6full, rows 0xfc0f5911f30d9d12ull -> 0x2b431fb4e22ef313ull, trades 0x3b74877f071e9e8dull -> 0x9e927212c83cadc5ull, final_hash 0x7929e5e00aefb02aull -> 0x1acb90ed825ee455ull, bar count 97 -> 95, boundary count 26 -> 25 [TAIL-D then TAIL-C]
//   Random45/0/0: boundary 0xa9aede02d7738b3eull -> 0x2bcba0ebccc11c6full, trades 0x3b74877f071e9e8dull -> 0x9e927212c83cadc5ull, final_hash 0x7929e5e00aefb02aull -> 0x1acb90ed825ee455ull, bar count 97 -> 95, boundary count 26 -> 25 [TAIL-D then TAIL-C]
// expectation corrected Random37/0/1: boundary 0x0e8497b06337b313 -> 0x30f7b45c06e02747,
// rows 0x1adac0628692b36c -> 0xf8824766258a7c0a, trades 0x3698e45959c0e3fa -> 0xd8069e61ccd6cf22,
// final_hash 0x09ada14085aa6800 -> 0x42a2b41cd76e3600; Random37/0/0 changes the same
// boundary, trades and final_hash, because the opening callback close uses the raw
// opening print before its own slippage, not the slipped entry execution price.
// TradingView's coof_close_slippage tape books the short's buy-close at 5912.50
// rather than 5912.25; coof_open_limit/td-fp17-coof-open proves the same projection
// for default non-immediate single-cohort closes, LC 499555 and SD 503600 ticks.
// coof_close_slippage/partial independently proves the 50% non-immediate close:
// TradingView books 24977.80 from raw open 24977.85, not slipped entry 24977.90.
// Random45/0/1 and Random45/0/0 retain the original baseline pins: the corrected
// non-immediate projection is not applied across distinct entry cohorts.
// Opening-stop shape audit, both recording modes on the ordinary path:
// Random08/0/{1,0}: short, slippage 0; stop 92, callback 93.75, limit 98.
// Random18/0/{1,0}: short, slippage 0; stop 98, callback 98.5, limit 101.5.
// Random44/0/{1,0}: short, slippage 0; stop/callback 91.75/94.5 and 80.75/82.25.
// These margin-backed bracket shapes keep the baseline digits. The independent
// coof_marketable_stop and carried tapes cover margin-free, unslipped shorts;
// the new activation and Market conversion are narrowed to that taped scope.
// Random08/1/{1,0} and Random44/1/{1,0} also retain baseline digits: first_open_fill
// requires the same ordinary (!spec || spec->intrabar.is_none()) path as conversion.
// expectation corrected (global exit children, 186 of 276 runs), because the source adapter's state now folds TradingView's entry-id table once it holds two entry ids or its peak passes 13 keys (TradingView's global exit children, pineforge-source-adapter/v4). Only Random28
// with the magnifier off moves its trades: its one same-bar trade A->X, a named
// stop already through when its limit entry fills, now fills at that entry's
// fill (105.5) instead of the stop level the bar crossed before the entry
// (102.25), as the pin's eth-member and eth-fresh tapes show
// (tests/fixtures/global_exit_children); no other run's trades move. Harvested
// with -DPINEFORGE_QUIET_BAR_HARVEST against main 700c5d24 (every old row
// reproduced) and this tree:
//   MarginCalls/0/1: boundary 0x005d6993f2b1c2deull -> 0x4bcea4f978d972aaull, rows 0x3210e1484b83ce34ull -> 0x9d9041a1674393c2ull, final_hash 0xd58658d95c54acbeull -> 0x717e3f9c839a052bull
//   MarginCalls/0/0: boundary 0x005d6993f2b1c2deull -> 0x4bcea4f978d972aaull, final_hash 0xd58658d95c54acbeull -> 0x717e3f9c839a052bull
//   MarginCalls/1/1: boundary 0x548dd443b38f74b0ull -> 0x3df3de80db85db62ull, rows 0xfdd852b37b07d408ull -> 0xf8701e62a6ccc2abull, final_hash 0x76f99b20ecd7e68cull -> 0x8ff634900f7f4491ull
//   MarginCalls/1/0: boundary 0x548dd443b38f74b0ull -> 0x3df3de80db85db62ull, final_hash 0x76f99b20ecd7e68cull -> 0x8ff634900f7f4491ull
//   CalcOnFills/0/1: boundary 0xfc8d0fb60bd5bfaaull -> 0xd72b0332fd230d12ull, rows 0x48055db8ef32071cull -> 0xccaa3a371697d096ull, final_hash 0xeef2208a06cbc6f1ull -> 0x782b53f3b9208eadull
//   CalcOnFills/0/0: boundary 0xfc8d0fb60bd5bfaaull -> 0xd72b0332fd230d12ull, final_hash 0xeef2208a06cbc6f1ull -> 0x782b53f3b9208eadull
//   CalcOnFills/1/1: boundary 0x64b0e03ae12cf797ull -> 0x4c36d59260cff1e8ull, rows 0x087cb3f69b4b45b9ull -> 0xc4db4870b394f199ull, final_hash 0x1e6e74d40df79e1eull -> 0x6372d23bba8999b2ull
//   CalcOnFills/1/0: boundary 0x64b0e03ae12cf797ull -> 0x4c36d59260cff1e8ull, final_hash 0x1e6e74d40df79e1eull -> 0x6372d23bba8999b2ull
//   CloseOrders/0/1: boundary 0xd105d5bf8915b2ceull -> 0x5c37531627d539efull, rows 0x31daa635496f2629ull -> 0xbe9bef110e3c1373ull, final_hash 0xcd9095a392afa563ull -> 0xe9218c7da1f48eabull
//   CloseOrders/0/0: boundary 0xd105d5bf8915b2ceull -> 0x5c37531627d539efull, final_hash 0xcd9095a392afa563ull -> 0xe9218c7da1f48eabull
//   CloseOrders/1/1: boundary 0xfa10291c03058ad9ull -> 0x359923313e616f54ull, rows 0x44a8a845fb940d81ull -> 0xb5c62af9112eea41ull, final_hash 0xaee588c13e83753bull -> 0x5493cdaf6cb43957ull
//   CloseOrders/1/0: boundary 0xfa10291c03058ad9ull -> 0x359923313e616f54ull, final_hash 0xaee588c13e83753bull -> 0x5493cdaf6cb43957ull
//   CloseOrdersSlipped/0/1: boundary 0xc2834ef9bd74b55full -> 0x40c5a78c03effcb4ull, rows 0xf5ebf3cacf1bb613ull -> 0x86f13e8920863005ull, final_hash 0x6c12422a13b3f077ull -> 0xd4b29cd71c1bf95full
//   CloseOrdersSlipped/0/0: boundary 0xc2834ef9bd74b55full -> 0x40c5a78c03effcb4ull, final_hash 0x6c12422a13b3f077ull -> 0xd4b29cd71c1bf95full
//   CloseOrdersSlipped/1/1: boundary 0x71906fcd647a169full -> 0x807012cdd2283145ull, rows 0x5c384d163976870full -> 0xe11e1b898ec834e8ull, final_hash 0x0330185649c756c2ull -> 0x021b24196a0982faull
//   CloseOrdersSlipped/1/0: boundary 0x71906fcd647a169full -> 0x807012cdd2283145ull, final_hash 0x0330185649c756c2ull -> 0x021b24196a0982faull
//   SameBarBatch/0/1: boundary 0x1ee4d2724cccff28ull -> 0x8a164a059578ab8dull, rows 0xf20f7ef590b39dcdull -> 0x195a3c8fb495f3f2ull, final_hash 0x77819f662ca36048ull -> 0x8e9cd7ece0b10463ull
//   SameBarBatch/0/0: boundary 0x1ee4d2724cccff28ull -> 0x8a164a059578ab8dull, final_hash 0x77819f662ca36048ull -> 0x8e9cd7ece0b10463ull
//   SameBarBatch/1/1: boundary 0xbd6e0d279e9729e6ull -> 0x3847edf901bf0336ull, rows 0xeb4f5e6198dd0178ull -> 0x759f8cd1b83c8355ull, final_hash 0x6a0ac704a36bef57ull -> 0xffcc8118cbe22f80ull
//   SameBarBatch/1/0: boundary 0xbd6e0d279e9729e6ull -> 0x3847edf901bf0336ull, final_hash 0x6a0ac704a36bef57ull -> 0xffcc8118cbe22f80ull
//   Random01/0/1: boundary 0xb7cb18e9c32443c1ull -> 0xf735a0cf0ce983f5ull, rows 0xf4ebe6782bc3ee5eull -> 0xc10eacaa7ca851b7ull, final_hash 0x44b3af95af8e40ebull -> 0x267be998bdc0fe3aull
//   Random01/0/0: boundary 0xb7cb18e9c32443c1ull -> 0xf735a0cf0ce983f5ull, final_hash 0x44b3af95af8e40ebull -> 0x267be998bdc0fe3aull
//   Random01/1/1: boundary 0xd8ef670e43a3f4ffull -> 0x4918ffb4dda67e41ull, rows 0x6e941c1e9e09aa71ull -> 0xe31aa22d21c7f1deull, final_hash 0x517f790462c7efb9ull -> 0x451001de8a9a3634ull
//   Random01/1/0: boundary 0xd8ef670e43a3f4ffull -> 0x4918ffb4dda67e41ull, final_hash 0x517f790462c7efb9ull -> 0x451001de8a9a3634ull
//   Random02/0/1: boundary 0xbc3d35821c7f0340ull -> 0x23c5ef1dd3dd7befull, rows 0x6de37f7191594538ull -> 0x9d6061d013de4f2dull, final_hash 0x55cb3a23ce562e01ull -> 0x465671bfd71f1b49ull
//   Random02/0/0: boundary 0xbc3d35821c7f0340ull -> 0x23c5ef1dd3dd7befull, final_hash 0x55cb3a23ce562e01ull -> 0x465671bfd71f1b49ull
//   Random02/1/1: boundary 0xa45a30c7d9cff5b7ull -> 0xeda8dd9b04dd5171ull, rows 0x4fb7e76741a52631ull -> 0x92dae6f76960708cull, final_hash 0x089086fe02b53049ull -> 0xf0d8e6f8c66ba0edull
//   Random02/1/0: boundary 0xa45a30c7d9cff5b7ull -> 0xeda8dd9b04dd5171ull, final_hash 0x089086fe02b53049ull -> 0xf0d8e6f8c66ba0edull
//   Random03/0/1: boundary 0xacc2755c7089fb70ull -> 0x8efdccf7ffce778full, rows 0x9bf3ecdb24c64985ull -> 0x4e448bd28567a84eull, final_hash 0x16b4269636eeaf7aull -> 0xf89c3da9a246c56aull
//   Random03/0/0: boundary 0xacc2755c7089fb70ull -> 0x8efdccf7ffce778full, final_hash 0x16b4269636eeaf7aull -> 0xf89c3da9a246c56aull
//   Random03/1/1: boundary 0x1c932bfdca73b86bull -> 0xa984da78281a6f67ull, rows 0x4cd3711ffc545640ull -> 0x23192b37dc9451faull, final_hash 0x5e606ff70d525f5bull -> 0x58cc7ecc091d34d7ull
//   Random03/1/0: boundary 0x1c932bfdca73b86bull -> 0xa984da78281a6f67ull, final_hash 0x5e606ff70d525f5bull -> 0x58cc7ecc091d34d7ull
//   Random04/0/1: boundary 0x3f2fc4283afc7cc4ull -> 0xd280c250b0a5c7e1ull, rows 0xb7e20a93376bfe8bull -> 0x3f346416821514d1ull, final_hash 0xec2f788ae1bef8fcull -> 0x4baafe4f5ce072deull
//   Random04/0/0: boundary 0x3f2fc4283afc7cc4ull -> 0xd280c250b0a5c7e1ull, final_hash 0xec2f788ae1bef8fcull -> 0x4baafe4f5ce072deull
//   Random04/1/1: boundary 0xadf2b572cb770a1cull -> 0xb7bd621548544961ull, rows 0x4a54644e7d763961ull -> 0x44175452c39d8931ull, final_hash 0xa75420d0ae9c3b6eull -> 0x8a1e40bc7b77aca8ull
//   Random04/1/0: boundary 0xadf2b572cb770a1cull -> 0xb7bd621548544961ull, final_hash 0xa75420d0ae9c3b6eull -> 0x8a1e40bc7b77aca8ull
//   Random05/1/1: boundary 0x13a36719393c2baeull -> 0xa01aa2d37370ef27ull, rows 0xeed6589421e8cda7ull -> 0x575d95342fa85cf5ull, final_hash 0x1c58bcc76863fb17ull -> 0x5fb131e7a5d7a095ull
//   Random05/1/0: boundary 0x13a36719393c2baeull -> 0xa01aa2d37370ef27ull, final_hash 0x1c58bcc76863fb17ull -> 0x5fb131e7a5d7a095ull
//   Random06/0/1: boundary 0x163e7663afe28a2dull -> 0x3f691ea6c5b82a94ull, rows 0x7a488bb30d99363eull -> 0x95903f8eb6032786ull, final_hash 0xa030d47050bdffdaull -> 0x07dd12270224ae8aull
//   Random06/0/0: boundary 0x163e7663afe28a2dull -> 0x3f691ea6c5b82a94ull, final_hash 0xa030d47050bdffdaull -> 0x07dd12270224ae8aull
//   Random06/1/1: boundary 0x0a9324ccde3f4915ull -> 0xed98de32895886baull, rows 0x6239cc3c60c5fd6eull -> 0xc10b04b6ed43f4eaull, final_hash 0x4b67e46b5a3f02a4ull -> 0x9cbe3e2d9e23fb08ull
//   Random06/1/0: boundary 0x0a9324ccde3f4915ull -> 0xed98de32895886baull, final_hash 0x4b67e46b5a3f02a4ull -> 0x9cbe3e2d9e23fb08ull
//   Random07/0/1: boundary 0x974115e531c0338full -> 0x38a6916856b137f8ull, rows 0x9d4ec9276f34e75dull -> 0x3aa6e1f6474d849bull, final_hash 0x1a375024019a0553ull -> 0xa05d024de2de4af1ull
//   Random07/0/0: boundary 0x974115e531c0338full -> 0x38a6916856b137f8ull, final_hash 0x1a375024019a0553ull -> 0xa05d024de2de4af1ull
//   Random07/1/1: boundary 0x9cd7a2146d1dc819ull -> 0x9bff9a3da19955ccull, rows 0x29d399651502d791ull -> 0xf21f6b0c16a83c13ull, final_hash 0xd9742bccbe934936ull -> 0x4c9a943bb069621cull
//   Random07/1/0: boundary 0x9cd7a2146d1dc819ull -> 0x9bff9a3da19955ccull, final_hash 0xd9742bccbe934936ull -> 0x4c9a943bb069621cull
//   Random08/0/1: boundary 0xd37ef6cf9b7af14full -> 0xeea57cda5350bb04ull, rows 0x42be52c508c5526bull -> 0x2f1e61ea38fb2675ull, final_hash 0x8a24a317e5cc18bdull -> 0x88f868b2637805ecull
//   Random08/0/0: boundary 0xd37ef6cf9b7af14full -> 0xeea57cda5350bb04ull, final_hash 0x8a24a317e5cc18bdull -> 0x88f868b2637805ecull
//   Random08/1/1: boundary 0x3c4f2aa9834d7c3dull -> 0xd797b030f1d2f01eull, rows 0xa77ade66b2b9b612ull -> 0x41b80b51926bd33eull, final_hash 0x7d3886c233269af9ull -> 0xd850846761b87002ull
//   Random08/1/0: boundary 0x3c4f2aa9834d7c3dull -> 0xd797b030f1d2f01eull, final_hash 0x7d3886c233269af9ull -> 0xd850846761b87002ull
//   Random09/0/1: boundary 0x92e8370ab3e727c0ull -> 0x0c68e77a3f7b0760ull, rows 0x672a4666a377cd09ull -> 0xff0fbcea65f2d984ull, final_hash 0x306c858f362bd294ull -> 0x8efe821a3ef46d45ull
//   Random09/0/0: boundary 0x92e8370ab3e727c0ull -> 0x0c68e77a3f7b0760ull, final_hash 0x306c858f362bd294ull -> 0x8efe821a3ef46d45ull
//   Random09/1/1: boundary 0xf7f7c43e037a49b5ull -> 0xbdad3983bf4de13eull, rows 0x12ed3c4db0e659e5ull -> 0xf7e0f637f127fad5ull, final_hash 0x355ede665029e361ull -> 0xd5e3868266cf224cull
//   Random09/1/0: boundary 0xf7f7c43e037a49b5ull -> 0xbdad3983bf4de13eull, final_hash 0x355ede665029e361ull -> 0xd5e3868266cf224cull
//   Random10/0/1: boundary 0xaaa0d5096796399dull -> 0xb564a90c13cde858ull, rows 0x8951026f85a19c95ull -> 0x0b6dc08fd115fe0aull, final_hash 0xff504d50c65ba2feull -> 0xb08edbbce30e6189ull
//   Random10/0/0: boundary 0xaaa0d5096796399dull -> 0xb564a90c13cde858ull, final_hash 0xff504d50c65ba2feull -> 0xb08edbbce30e6189ull
//   Random10/1/1: boundary 0xb10de9f0194c4cd0ull -> 0xfe6e546d4d6d57a3ull, rows 0x4f457d291e5d1b50ull -> 0x3e2f50c0d5e7339full, final_hash 0x86f7243983d16b95ull -> 0x71c635959f1365eeull
//   Random10/1/0: boundary 0xb10de9f0194c4cd0ull -> 0xfe6e546d4d6d57a3ull, final_hash 0x86f7243983d16b95ull -> 0x71c635959f1365eeull
//   Random11/0/1: boundary 0xc2d40e2309498204ull -> 0xd329adc42a76a850ull, rows 0x47ab72d92cd1a804ull -> 0xe3aa05fd54484a81ull, final_hash 0xb89b1233521240a5ull -> 0x65c76d0ec8c574fbull
//   Random11/0/0: boundary 0xc2d40e2309498204ull -> 0xd329adc42a76a850ull, final_hash 0xb89b1233521240a5ull -> 0x65c76d0ec8c574fbull
//   Random11/1/1: boundary 0x43ec3bbaca91488aull -> 0x3812719fa8195320ull, rows 0x8b43e1bf1c0c5ffbull -> 0xeac4605f7e20f62aull, final_hash 0x54c5d31ee5a16684ull -> 0x2e6025586772f3c4ull
//   Random11/1/0: boundary 0x43ec3bbaca91488aull -> 0x3812719fa8195320ull, final_hash 0x54c5d31ee5a16684ull -> 0x2e6025586772f3c4ull
//   Random12/0/1: boundary 0xbc3686bf9433a52eull -> 0x07b5b233ea6672bbull, rows 0xaec3d3079e6c1521ull -> 0xd5968b91bb5fbee1ull, final_hash 0x347aaf5cd70466caull -> 0x70c9cc50965fe672ull
//   Random12/0/0: boundary 0xbc3686bf9433a52eull -> 0x07b5b233ea6672bbull, final_hash 0x347aaf5cd70466caull -> 0x70c9cc50965fe672ull
//   Random12/1/1: boundary 0x8647fe7278804323ull -> 0x635610ddcf1e886dull, rows 0x3b87cd70a81d2133ull -> 0xad7f7455ade3d98cull, final_hash 0x42a73d638c1997b3ull -> 0x9eda6cdd6ea9c8c7ull
//   Random12/1/0: boundary 0x8647fe7278804323ull -> 0x635610ddcf1e886dull, final_hash 0x42a73d638c1997b3ull -> 0x9eda6cdd6ea9c8c7ull
//   Random13/0/1: boundary 0x56b98418406a893aull -> 0x925f08aa970fd4d1ull, rows 0xfbcef4d1953ddd45ull -> 0xb5423ffc76e2442aull, final_hash 0x4294104ba715c4e5ull -> 0x6700d4ad6da7f598ull
//   Random13/0/0: boundary 0x56b98418406a893aull -> 0x925f08aa970fd4d1ull, final_hash 0x4294104ba715c4e5ull -> 0x6700d4ad6da7f598ull
//   Random13/1/1: boundary 0x145ca3678c5067c1ull -> 0x208d9b6671c1a676ull, rows 0x4f73ce76a795a693ull -> 0xf16f31eb6a3e9cb5ull, final_hash 0x81d5b4683e107c50ull -> 0x6c24feeadc377b01ull
//   Random13/1/0: boundary 0x145ca3678c5067c1ull -> 0x208d9b6671c1a676ull, final_hash 0x81d5b4683e107c50ull -> 0x6c24feeadc377b01ull
//   Random14/0/1: boundary 0x8102343dedc7f9baull -> 0x317b50a90a8f7e46ull, rows 0x180ca42c3e610d01ull -> 0x34e2cd5d23f7320eull, final_hash 0xf4b4d3629853f062ull -> 0x7e2d50f568de6f0full
//   Random14/0/0: boundary 0x8102343dedc7f9baull -> 0x317b50a90a8f7e46ull, final_hash 0xf4b4d3629853f062ull -> 0x7e2d50f568de6f0full
//   Random14/1/1: boundary 0x295206578d5ff480ull -> 0xfb8e43a765208859ull, rows 0xfd29f8252edce4edull -> 0x5ad0b9e4ad562ef7ull, final_hash 0x441179a0f3e73edfull -> 0x1246d6915dd5918aull
//   Random14/1/0: boundary 0x295206578d5ff480ull -> 0xfb8e43a765208859ull, final_hash 0x441179a0f3e73edfull -> 0x1246d6915dd5918aull
//   Random15/0/1: boundary 0xb3ea7ad42aa97779ull -> 0x5716fadc8c4b0d5bull, rows 0xbe96e9e530c967c1ull -> 0x68620b60c6c34a86ull, final_hash 0x33f7d111d486ddedull -> 0x997aa1f5f5f7b8a6ull
//   Random15/0/0: boundary 0xb3ea7ad42aa97779ull -> 0x5716fadc8c4b0d5bull, final_hash 0x33f7d111d486ddedull -> 0x997aa1f5f5f7b8a6ull
//   Random15/1/1: boundary 0x3835c1c95b71c190ull -> 0xb23d0b67238fd1e7ull, rows 0xc7dddf092d2d415bull -> 0xcedba8eab255eaafull, final_hash 0xebe3ce184f432170ull -> 0x368f2569225769bfull
//   Random15/1/0: boundary 0x3835c1c95b71c190ull -> 0xb23d0b67238fd1e7ull, final_hash 0xebe3ce184f432170ull -> 0x368f2569225769bfull
//   Random16/0/1: boundary 0x9443463c4ce11014ull -> 0x3f9429a53301ca85ull, rows 0xf8517013485b3f4dull -> 0x1888a9b7da1cc3c2ull, final_hash 0xa632bad9177f6447ull -> 0x226dae53316a3b2aull
//   Random16/0/0: boundary 0x9443463c4ce11014ull -> 0x3f9429a53301ca85ull, final_hash 0xa632bad9177f6447ull -> 0x226dae53316a3b2aull
//   Random16/1/1: boundary 0xfbc1982865dc4bf2ull -> 0xbf02f3f6d610391eull, rows 0x69ddeb53e5249c41ull -> 0x497053e8dff2c3e0ull, final_hash 0x3bb26d3efd6a0234ull -> 0x71c34f49d37990fdull
//   Random16/1/0: boundary 0xfbc1982865dc4bf2ull -> 0xbf02f3f6d610391eull, final_hash 0x3bb26d3efd6a0234ull -> 0x71c34f49d37990fdull
//   Random18/0/1: boundary 0x5fc781164fdbdbf7ull -> 0xe23600f528bcdd7bull, rows 0xdffa80df9044f33bull -> 0xebd5ab75ab65addeull, final_hash 0xfe3755cafa818de6ull -> 0xe325154611af7d9full
//   Random18/0/0: boundary 0x5fc781164fdbdbf7ull -> 0xe23600f528bcdd7bull, final_hash 0xfe3755cafa818de6ull -> 0xe325154611af7d9full
//   Random18/1/1: boundary 0x28d9f94548ee4134ull -> 0xa117a73200c4d0d8ull, rows 0xf279e0c2d7ad4a08ull -> 0xf2697328ab5e1a72ull, final_hash 0xc8fcaa37ad2681ecull -> 0x04934ec1a8184ee4ull
//   Random18/1/0: boundary 0x28d9f94548ee4134ull -> 0xa117a73200c4d0d8ull, final_hash 0xc8fcaa37ad2681ecull -> 0x04934ec1a8184ee4ull
//   Random19/0/1: boundary 0x2578d7b317dd9becull -> 0x3dd599a76dbbba23ull, rows 0x59557f1fefa231afull -> 0x74aca03056ff324eull, final_hash 0x7e530ea7e9bb00d6ull -> 0xc5b88ba55d4a4996ull
//   Random19/0/0: boundary 0x2578d7b317dd9becull -> 0x3dd599a76dbbba23ull, final_hash 0x7e530ea7e9bb00d6ull -> 0xc5b88ba55d4a4996ull
//   Random19/1/1: boundary 0x11fcb5008bfc8542ull -> 0x17c3fa52056cbb12ull, rows 0x181811064366b797ull -> 0x662eac99acc2d7d5ull, final_hash 0xb5b2ab25557d9ce4ull -> 0x7a856764b0dc8cb8ull
//   Random19/1/0: boundary 0x11fcb5008bfc8542ull -> 0x17c3fa52056cbb12ull, final_hash 0xb5b2ab25557d9ce4ull -> 0x7a856764b0dc8cb8ull
//   Random20/0/1: boundary 0x4795ce55e278f092ull -> 0x240069001bcbada3ull, rows 0x6739def4111d99a6ull -> 0x213f6b3cc7fb7912ull, final_hash 0x29e3be3fcc918072ull -> 0x330197a267ccc229ull
//   Random20/0/0: boundary 0x4795ce55e278f092ull -> 0x240069001bcbada3ull, final_hash 0x29e3be3fcc918072ull -> 0x330197a267ccc229ull
//   Random20/1/1: boundary 0xa04d5df6a8012a95ull -> 0xc31c3a969214a2c4ull, rows 0xfb09750b63c74c91ull -> 0xd448e6522f1d5b65ull, final_hash 0x84804b1c9915de38ull -> 0x5f606de061bd1774ull
//   Random20/1/0: boundary 0xa04d5df6a8012a95ull -> 0xc31c3a969214a2c4ull, final_hash 0x84804b1c9915de38ull -> 0x5f606de061bd1774ull
//   Random21/0/1: boundary 0x26729ac375542b71ull -> 0x2650b37073d95820ull, rows 0x2ea4f6970b51e0c9ull -> 0x0910aa3a301640a8ull, final_hash 0x274d05295edf06ecull -> 0x473fbafaa3ef43e4ull
//   Random21/0/0: boundary 0x26729ac375542b71ull -> 0x2650b37073d95820ull, final_hash 0x274d05295edf06ecull -> 0x473fbafaa3ef43e4ull
//   Random21/1/1: boundary 0x51dde7af0f801ff0ull -> 0x6f8f15c5a8b2e857ull, rows 0xed6bda6ff2e91c25ull -> 0x0de50cd113ebf7abull, final_hash 0xd0b5041d0d30d22bull -> 0x22055d2921e009d3ull
//   Random21/1/0: boundary 0x51dde7af0f801ff0ull -> 0x6f8f15c5a8b2e857ull, final_hash 0xd0b5041d0d30d22bull -> 0x22055d2921e009d3ull
//   Random22/0/1: boundary 0x3c35d0fc3409545aull -> 0xe23be045f9a8abe6ull, rows 0x289459a9d83dcf63ull -> 0x8679c8d0972904a7ull, final_hash 0xc4d805069c4a36b4ull -> 0x46cc3da0b9089ac1ull
//   Random22/0/0: boundary 0x3c35d0fc3409545aull -> 0xe23be045f9a8abe6ull, final_hash 0xc4d805069c4a36b4ull -> 0x46cc3da0b9089ac1ull
//   Random22/1/1: boundary 0x46d42b9bf96f8dbfull -> 0x25eb6809c245abe1ull, rows 0xc61ebe11092e9bb7ull -> 0xb4c0825d0260f461ull, final_hash 0xaf77291e8769d31full -> 0x2e15840b8b10ee7eull
//   Random22/1/0: boundary 0x46d42b9bf96f8dbfull -> 0x25eb6809c245abe1ull, final_hash 0xaf77291e8769d31full -> 0x2e15840b8b10ee7eull
//   Random24/0/1: boundary 0xae7998ad09f09f7aull -> 0x10955313a67b52caull, rows 0x2e078b985c070260ull -> 0x00617e954b379daeull, final_hash 0x1854a1f9f4a3d97bull -> 0xcf3fa51fbfa4ea2dull
//   Random24/0/0: boundary 0xae7998ad09f09f7aull -> 0x10955313a67b52caull, final_hash 0x1854a1f9f4a3d97bull -> 0xcf3fa51fbfa4ea2dull
//   Random24/1/1: boundary 0x1defabc343733710ull -> 0xe2c892bee5269be4ull, rows 0x5a31e5560954fd71ull -> 0xf232e9ac090d2ed4ull, final_hash 0x29a17a681570c85bull -> 0xda5a2e35ccc1717dull
//   Random24/1/0: boundary 0x1defabc343733710ull -> 0xe2c892bee5269be4ull, final_hash 0x29a17a681570c85bull -> 0xda5a2e35ccc1717dull
//   Random25/0/1: boundary 0x07e6b9cb40382412ull -> 0xccc46bf223ebc080ull, rows 0x67ef493ae25a3a47ull -> 0xcfac6f28c2ed1802ull, final_hash 0x09b8a0954f3e27caull -> 0xf8ee1e7aa8df4c01ull
//   Random25/0/0: boundary 0x07e6b9cb40382412ull -> 0xccc46bf223ebc080ull, final_hash 0x09b8a0954f3e27caull -> 0xf8ee1e7aa8df4c01ull
//   Random25/1/1: boundary 0x23144355d1df6f60ull -> 0xc50df9991d2566fcull, rows 0xb2174560120a3ec8ull -> 0x4b7f805f16c35dfdull, final_hash 0x175ccb5de5ff9ff6ull -> 0x21bcd6104e8bd365ull
//   Random25/1/0: boundary 0x23144355d1df6f60ull -> 0xc50df9991d2566fcull, final_hash 0x175ccb5de5ff9ff6ull -> 0x21bcd6104e8bd365ull
//   Random26/0/1: boundary 0x8add97bc79f4eed8ull -> 0xd811ace52ecfe7f9ull, rows 0x76089d2a60f2b03bull -> 0xa0b443d355db01ecull, final_hash 0xe60d1a08ab2e3e6full -> 0x8ca71da42a210e76ull
//   Random26/0/0: boundary 0x8add97bc79f4eed8ull -> 0xd811ace52ecfe7f9ull, final_hash 0xe60d1a08ab2e3e6full -> 0x8ca71da42a210e76ull
//   Random26/1/1: boundary 0xcc3bc197c790f005ull -> 0x3c8cda44b51df504ull, rows 0x33cb5420c38eaa7dull -> 0x0621b353b516fc75ull, final_hash 0xddd2f53991df857aull -> 0xfc6f4792a0dbc9ddull
//   Random26/1/0: boundary 0xcc3bc197c790f005ull -> 0x3c8cda44b51df504ull, final_hash 0xddd2f53991df857aull -> 0xfc6f4792a0dbc9ddull
//   Random27/0/1: boundary 0x9256b189ba68bea3ull -> 0xd129482ad0f78521ull, rows 0x958d1671c170e18full -> 0xd841a947af8d8c5bull, final_hash 0x70f7b127c522e392ull -> 0x5d34c684aa5301e2ull
//   Random27/0/0: boundary 0x9256b189ba68bea3ull -> 0xd129482ad0f78521ull, final_hash 0x70f7b127c522e392ull -> 0x5d34c684aa5301e2ull
//   Random27/1/1: boundary 0x505583c531027ba4ull -> 0xe654b5dc971ce9e4ull, rows 0xb98058137450a938ull -> 0xbdecb995e70b2952ull, final_hash 0x94de1904cefde26dull -> 0x0ea37f757d7a4501ull
//   Random27/1/0: boundary 0x505583c531027ba4ull -> 0xe654b5dc971ce9e4ull, final_hash 0x94de1904cefde26dull -> 0x0ea37f757d7a4501ull
//   Random28/0/1: boundary 0x51b4ca4263f3d628ull -> 0xc96c1554502037ebull, rows 0xf6099194c8e66357ull -> 0x22e21a642387a4d3ull, trades 0xa35579679c597892ull -> 0xb315918107aeb676ull, final_hash 0xfb480389d470203aull -> 0xfa5b17cc77508cf0ull
//   Random28/0/0: boundary 0x51b4ca4263f3d628ull -> 0xc96c1554502037ebull, trades 0xa35579679c597892ull -> 0xb315918107aeb676ull, final_hash 0xfb480389d470203aull -> 0xfa5b17cc77508cf0ull
//   Random28/1/1: boundary 0xc43bc41443863b0dull -> 0xc11f413da0c9110eull, rows 0xe0dbf72894e9d6adull -> 0xe4fe4a1ed4077ba0ull, final_hash 0x5f83107ff10203a9ull -> 0x3c7b82504f9168b5ull
//   Random28/1/0: boundary 0xc43bc41443863b0dull -> 0xc11f413da0c9110eull, final_hash 0x5f83107ff10203a9ull -> 0x3c7b82504f9168b5ull
//   Random31/0/1: boundary 0x94c6de025e8104bbull -> 0x2f48a67f2d7f7aafull, rows 0x5990d1ebd754a80full -> 0xde560fce49effa1dull, final_hash 0x12810545107c706bull -> 0xcebcd23dca304ebaull
//   Random31/0/0: boundary 0x94c6de025e8104bbull -> 0x2f48a67f2d7f7aafull, final_hash 0x12810545107c706bull -> 0xcebcd23dca304ebaull
//   Random31/1/1: boundary 0x17ef40f8ddd917b7ull -> 0xd96c881a3927f935ull, rows 0xe21f59451f2cf451ull -> 0x5d858d37d39812bcull, final_hash 0x2c1e6a83a85b8cdbull -> 0xa1ff686cd364bf6aull
//   Random31/1/0: boundary 0x17ef40f8ddd917b7ull -> 0xd96c881a3927f935ull, final_hash 0x2c1e6a83a85b8cdbull -> 0xa1ff686cd364bf6aull
//   Random32/0/1: boundary 0x7938e267e99e1ae2ull -> 0x401cac1e4097a091ull, rows 0x830f5d549f720bffull -> 0x4b0794b5b0901ff0ull, final_hash 0x52b7f53f1add68c3ull -> 0x755402423c549e1full
//   Random32/0/0: boundary 0x7938e267e99e1ae2ull -> 0x401cac1e4097a091ull, final_hash 0x52b7f53f1add68c3ull -> 0x755402423c549e1full
//   Random32/1/1: boundary 0x07895e91b01b07e9ull -> 0x4586c43cea4202e0ull, rows 0x3e545e5e55b5b34aull -> 0x0519cfb78b41cc3eull, final_hash 0x650e338da09e0df7ull -> 0x5a042649fd309da3ull
//   Random32/1/0: boundary 0x07895e91b01b07e9ull -> 0x4586c43cea4202e0ull, final_hash 0x650e338da09e0df7ull -> 0x5a042649fd309da3ull
//   Random33/0/1: boundary 0xe3c91c1fe2e77f66ull -> 0x3800188d78bf38d1ull, rows 0x8ed11649e8ee34b0ull -> 0xd9e3121713edfa7cull, final_hash 0x58a3655fa601d4a7ull -> 0xdcdc0acfec99025eull
//   Random33/0/0: boundary 0xe3c91c1fe2e77f66ull -> 0x3800188d78bf38d1ull, final_hash 0x58a3655fa601d4a7ull -> 0xdcdc0acfec99025eull
//   Random33/1/1: boundary 0x3ae199c5f1e09198ull -> 0x02f7237eeebc6e8eull, rows 0xeb34af0b85087c6full -> 0x127cd64c5eb937b5ull, final_hash 0x5537a57c513e7f17ull -> 0x0234ec1608d9420eull
//   Random33/1/0: boundary 0x3ae199c5f1e09198ull -> 0x02f7237eeebc6e8eull, final_hash 0x5537a57c513e7f17ull -> 0x0234ec1608d9420eull
//   Random35/0/1: boundary 0x93532c12dd05c8f8ull -> 0x7f66b48075be86e0ull, rows 0x470485b83257d3cbull -> 0x2b733b1f98259c66ull, final_hash 0x6aeef866f7922647ull -> 0x8aa2580a37361d78ull
//   Random35/0/0: boundary 0x93532c12dd05c8f8ull -> 0x7f66b48075be86e0ull, final_hash 0x6aeef866f7922647ull -> 0x8aa2580a37361d78ull
//   Random35/1/1: boundary 0x1f16fa70bb028179ull -> 0xab532904d5ee771cull, rows 0xebced8f56faf89dcull -> 0x7c24fd8660f9e971ull, final_hash 0xa784e9e042779935ull -> 0xe3b1d10f77652252ull
//   Random35/1/0: boundary 0x1f16fa70bb028179ull -> 0xab532904d5ee771cull, final_hash 0xa784e9e042779935ull -> 0xe3b1d10f77652252ull
//   Random36/0/1: boundary 0x5530362ed60c126dull -> 0xa88ba3e2cf6d5a61ull, rows 0x54bd05607173aa6bull -> 0x5a53d43688153af9ull, final_hash 0x212db54b7b40d9baull -> 0x3eff2b11a8feff91ull
//   Random36/0/0: boundary 0x5530362ed60c126dull -> 0xa88ba3e2cf6d5a61ull, final_hash 0x212db54b7b40d9baull -> 0x3eff2b11a8feff91ull
//   Random36/1/1: boundary 0x10f19e1df0bfc765ull -> 0x0295357029345943ull, rows 0xff9b419c51942aadull -> 0x75045cc6617d2c6full, final_hash 0x0ca6ed8a848b4f88ull -> 0x72a0bcaba87a9a73ull
//   Random36/1/0: boundary 0x10f19e1df0bfc765ull -> 0x0295357029345943ull, final_hash 0x0ca6ed8a848b4f88ull -> 0x72a0bcaba87a9a73ull
//   Random37/0/1: boundary 0x30f7b45c06e02747ull -> 0x430fcc4706e62293ull, rows 0xf8824766258a7c0aull -> 0x1b407691e1df9efeull, final_hash 0x42a2b41cd76e3600ull -> 0x0cfda5999200e705ull
//   Random37/0/0: boundary 0x30f7b45c06e02747ull -> 0x430fcc4706e62293ull, final_hash 0x42a2b41cd76e3600ull -> 0x0cfda5999200e705ull
//   Random37/1/1: boundary 0x2cd9ee6e0acb218bull -> 0x37d4cbfc33b5719eull, rows 0xe626fb41292b2064ull -> 0x97e1b58cefd475f3ull, final_hash 0xf428d94da051f1a3ull -> 0x58ba78481e2c9f5eull
//   Random37/1/0: boundary 0x2cd9ee6e0acb218bull -> 0x37d4cbfc33b5719eull, final_hash 0xf428d94da051f1a3ull -> 0x58ba78481e2c9f5eull
//   Random38/0/1: boundary 0x7a72e075d0bcf84full -> 0xd77b31ee6a5f0d3dull, rows 0x8fb7a63eda294548ull -> 0x6bf2b9d6db115b6aull, final_hash 0x536763f10d3d8e4bull -> 0x15199290c0825656ull
//   Random38/0/0: boundary 0x7a72e075d0bcf84full -> 0xd77b31ee6a5f0d3dull, final_hash 0x536763f10d3d8e4bull -> 0x15199290c0825656ull
//   Random38/1/1: boundary 0xeb586814ea3635e5ull -> 0xd8d4999529bd4698ull, rows 0x7cf6f74496bcc8cfull -> 0xa7a929705335a6eaull, final_hash 0x09474c55e153dae8ull -> 0x44d9b4717c2b74f9ull
//   Random38/1/0: boundary 0xeb586814ea3635e5ull -> 0xd8d4999529bd4698ull, final_hash 0x09474c55e153dae8ull -> 0x44d9b4717c2b74f9ull
//   Random39/0/1: boundary 0xe9c388052a13202aull -> 0x3c7109829604d779ull, rows 0x1dbc13c6b0776cd5ull -> 0x6ca907bbf39fcc2eull, final_hash 0xf4d9f3205c30e58dull -> 0xb9633bc0e7068d48ull
//   Random39/0/0: boundary 0xe9c388052a13202aull -> 0x3c7109829604d779ull, final_hash 0xf4d9f3205c30e58dull -> 0xb9633bc0e7068d48ull
//   Random39/1/1: boundary 0x16fc2000a3efe788ull -> 0x38efcf41a8bf31d7ull, rows 0x5345722bb6f8f5c6ull -> 0x5b21144661b2d54eull, final_hash 0x031d04dd65e46ae9ull -> 0xdd9439083abdc078ull
//   Random39/1/0: boundary 0x16fc2000a3efe788ull -> 0x38efcf41a8bf31d7ull, final_hash 0x031d04dd65e46ae9ull -> 0xdd9439083abdc078ull
//   Random40/0/1: boundary 0x82056ea3cd14b091ull -> 0x92d066f10ca532c8ull, rows 0xac6675ee324c1a9dull -> 0xcaf2d4a713cf53a0ull, final_hash 0xff8928292b458130ull -> 0xc754f4aa4221e904ull
//   Random40/0/0: boundary 0x82056ea3cd14b091ull -> 0x92d066f10ca532c8ull, final_hash 0xff8928292b458130ull -> 0xc754f4aa4221e904ull
//   Random40/1/1: boundary 0xea2aea67bd36b8feull -> 0xedda3193d426477bull, rows 0x583d751d33738ce3ull -> 0x8e666c24d7b5b245ull, final_hash 0xd740260ab955140cull -> 0xa37ad00af21451b1ull
//   Random40/1/0: boundary 0xea2aea67bd36b8feull -> 0xedda3193d426477bull, final_hash 0xd740260ab955140cull -> 0xa37ad00af21451b1ull
//   Random41/0/1: boundary 0x7fae0dae590a2a06ull -> 0x1fda069b99f5c640ull, rows 0xeb3700f17478f99aull -> 0x533c23b35aa4cec8ull, final_hash 0xe1988c325c19350dull -> 0x515f754e33e48e55ull
//   Random41/0/0: boundary 0x7fae0dae590a2a06ull -> 0x1fda069b99f5c640ull, final_hash 0xe1988c325c19350dull -> 0x515f754e33e48e55ull
//   Random41/1/1: boundary 0x7ef708ff93df2105ull -> 0x514020d9dcd73266ull, rows 0xca183a20221f587full -> 0xeb281d4380bbbe21ull, final_hash 0xdebdae0068274179ull -> 0x9f12cd2301b8da41ull
//   Random41/1/0: boundary 0x7ef708ff93df2105ull -> 0x514020d9dcd73266ull, final_hash 0xdebdae0068274179ull -> 0x9f12cd2301b8da41ull
//   Random42/0/1: boundary 0x1c36c6f9257072d0ull -> 0x3cb4ae7fd9b48f07ull, rows 0xccc61f07165cc2bbull -> 0xd0b0aa6cde5345d3ull, final_hash 0x3938f0c23f35c481ull -> 0x89aa51f9506b13c1ull
//   Random42/0/0: boundary 0x1c36c6f9257072d0ull -> 0x3cb4ae7fd9b48f07ull, final_hash 0x3938f0c23f35c481ull -> 0x89aa51f9506b13c1ull
//   Random42/1/1: boundary 0xcc5a06dab05745c3ull -> 0xbec67b6bcab320e8ull, rows 0x279b780c0333235dull -> 0xb82212e2d2983c2dull, final_hash 0xfe8452396cf5c313ull -> 0xfaa46e405b7f1967ull
//   Random42/1/0: boundary 0xcc5a06dab05745c3ull -> 0xbec67b6bcab320e8ull, final_hash 0xfe8452396cf5c313ull -> 0xfaa46e405b7f1967ull
//   Random43/0/1: boundary 0x3230c4ca9a85bc81ull -> 0x0ef1f47045923638ull, rows 0x2c12e4a2a8a73638ull -> 0xbfcd107676ac417aull, final_hash 0x598141c24e351649ull -> 0x7dc885057ff3ae16ull
//   Random43/0/0: boundary 0x3230c4ca9a85bc81ull -> 0x0ef1f47045923638ull, final_hash 0x598141c24e351649ull -> 0x7dc885057ff3ae16ull
//   Random43/1/1: boundary 0xea6a2ed66ea0e0adull -> 0xfa436c851909ef62ull, rows 0xa419d78c390bc110ull -> 0xc52d7c033c01835bull, final_hash 0x64eded952d140076ull -> 0xf0872b36f0c0478dull
//   Random43/1/0: boundary 0xea6a2ed66ea0e0adull -> 0xfa436c851909ef62ull, final_hash 0x64eded952d140076ull -> 0xf0872b36f0c0478dull
//   Random44/0/1: boundary 0x696e38d779f969b3ull -> 0x428e0523687f91bbull, rows 0xecd4b36a793678a2ull -> 0xf093f4cb3b885975ull, final_hash 0xa790713cfd61703aull -> 0x36a58af8445b0a33ull
//   Random44/0/0: boundary 0x696e38d779f969b3ull -> 0x428e0523687f91bbull, final_hash 0xa790713cfd61703aull -> 0x36a58af8445b0a33ull
//   Random44/1/1: boundary 0xe4b01bfef621cc40ull -> 0xfc8f7a8414d17cf7ull, rows 0x12aff34f2be5d18cull -> 0x0f9a6a6588e48ae1ull, final_hash 0xc8470526b19e84d5ull -> 0xb7c273b697699ff6ull
//   Random44/1/0: boundary 0xe4b01bfef621cc40ull -> 0xfc8f7a8414d17cf7ull, final_hash 0xc8470526b19e84d5ull -> 0xb7c273b697699ff6ull
//   Random45/0/1: boundary 0x2bcba0ebccc11c6full -> 0xd320a226c9314fbeull, rows 0x2b431fb4e22ef313ull -> 0x18dea9ebb76ac827ull, final_hash 0x1acb90ed825ee455ull -> 0x39e9c0cee5d02cb2ull
//   Random45/0/0: boundary 0x2bcba0ebccc11c6full -> 0xd320a226c9314fbeull, final_hash 0x1acb90ed825ee455ull -> 0x39e9c0cee5d02cb2ull
//   Random45/1/1: boundary 0xe76d1aa822a400efull -> 0xba78b213363e7453ull, rows 0x13464fddaa283992ull -> 0x32c6f7cdce116f9dull, final_hash 0x793b98d2fd6175daull -> 0x5afa968b5efc152dull
//   Random45/1/0: boundary 0xe76d1aa822a400efull -> 0xba78b213363e7453ull, final_hash 0x793b98d2fd6175daull -> 0x5afa968b5efc152dull
//   Random47/0/1: boundary 0x23bb0900769c951dull -> 0x8b415587165d6e23ull, rows 0x1c9ae7de7a2c8332ull -> 0x872dabc3c6837c49ull, final_hash 0x959c68a6579a5c69ull -> 0x3fae3318d3df3437ull
//   Random47/0/0: boundary 0x23bb0900769c951dull -> 0x8b415587165d6e23ull, final_hash 0x959c68a6579a5c69ull -> 0x3fae3318d3df3437ull
//   Random47/1/1: boundary 0xa981a443d153dba3ull -> 0x7fba22d58cbd5742ull, rows 0xc1d3e14bba2f6ca2ull -> 0x952c13f4b0367030ull, final_hash 0x559e4d89c592a7d1ull -> 0x67a015b13a126f57ull
//   Random47/1/0: boundary 0xa981a443d153dba3ull -> 0x7fba22d58cbd5742ull, final_hash 0x559e4d89c592a7d1ull -> 0x67a015b13a126f57ull
//   Random48/0/1: boundary 0x61058fc7c0ec1a28ull -> 0xa9cdc4fdbf2711f4ull, rows 0xa10d595a24d1c087ull -> 0x626cd0f4b7c32e80ull, final_hash 0x41e9f2a144887b14ull -> 0x6964de82cf42b764ull
//   Random48/0/0: boundary 0x61058fc7c0ec1a28ull -> 0xa9cdc4fdbf2711f4ull, final_hash 0x41e9f2a144887b14ull -> 0x6964de82cf42b764ull
//   Random48/1/1: boundary 0x1495a6b529cfcd63ull -> 0x65ff21f3229cca95ull, rows 0x54746877dcc8bea2ull -> 0x4e9b982f9ab665f5ull, final_hash 0x09e8b8606f92c5b5ull -> 0x65feef2927240d81ull
//   Random48/1/0: boundary 0x1495a6b529cfcd63ull -> 0x65ff21f3229cca95ull, final_hash 0x09e8b8606f92c5b5ull -> 0x65feef2927240d81ull
// Re-harvested once more with -DPINEFORGE_QUIET_BAR_HARVEST after the global exit children change was
// rebased onto main 5089d44d, whose confirmed-bar stream change re-pinned rows of this table: 16 runs
// move from the merged pins, in hash fields only; the same harvest against main reproduces every main row,
// and the trade digests differ from main only in Random28 with the magnifier off (rule 4, above):
//   Random01/0/1: boundary 0xb7cb18e9c32443c1ull -> 0xf735a0cf0ce983f5ull, rows 0xf4ebe6782bc3ee5eull -> 0xc10eacaa7ca851b7ull, final_hash 0x44b3af95af8e40ebull -> 0x267be998bdc0fe3aull
//   Random01/0/0: boundary 0xb7cb18e9c32443c1ull -> 0xf735a0cf0ce983f5ull, final_hash 0x44b3af95af8e40ebull -> 0x267be998bdc0fe3aull
//   Random01/1/1: boundary 0xd8ef670e43a3f4ffull -> 0x4918ffb4dda67e41ull, rows 0x6e941c1e9e09aa71ull -> 0xe31aa22d21c7f1deull, final_hash 0x517f790462c7efb9ull -> 0x451001de8a9a3634ull
//   Random01/1/0: boundary 0xd8ef670e43a3f4ffull -> 0x4918ffb4dda67e41ull, final_hash 0x517f790462c7efb9ull -> 0x451001de8a9a3634ull
//   Random02/0/1: boundary 0xbc3d35821c7f0340ull -> 0x23c5ef1dd3dd7befull, rows 0x6de37f7191594538ull -> 0x9d6061d013de4f2dull, final_hash 0x55cb3a23ce562e01ull -> 0x465671bfd71f1b49ull
//   Random02/0/0: boundary 0xbc3d35821c7f0340ull -> 0x23c5ef1dd3dd7befull, final_hash 0x55cb3a23ce562e01ull -> 0x465671bfd71f1b49ull
//   Random02/1/1: boundary 0xa45a30c7d9cff5b7ull -> 0xeda8dd9b04dd5171ull, rows 0x4fb7e76741a52631ull -> 0x92dae6f76960708cull, final_hash 0x089086fe02b53049ull -> 0xf0d8e6f8c66ba0edull
//   Random02/1/0: boundary 0xa45a30c7d9cff5b7ull -> 0xeda8dd9b04dd5171ull, final_hash 0x089086fe02b53049ull -> 0xf0d8e6f8c66ba0edull
//   Random03/0/1: boundary 0xacc2755c7089fb70ull -> 0x8efdccf7ffce778full, rows 0x9bf3ecdb24c64985ull -> 0x4e448bd28567a84eull, final_hash 0x16b4269636eeaf7aull -> 0xf89c3da9a246c56aull
//   Random03/0/0: boundary 0xacc2755c7089fb70ull -> 0x8efdccf7ffce778full, final_hash 0x16b4269636eeaf7aull -> 0xf89c3da9a246c56aull
//   Random03/1/1: boundary 0x1c932bfdca73b86bull -> 0xa984da78281a6f67ull, rows 0x4cd3711ffc545640ull -> 0x23192b37dc9451faull, final_hash 0x5e606ff70d525f5bull -> 0x58cc7ecc091d34d7ull
//   Random03/1/0: boundary 0x1c932bfdca73b86bull -> 0xa984da78281a6f67ull, final_hash 0x5e606ff70d525f5bull -> 0x58cc7ecc091d34d7ull
//   Random04/0/1: boundary 0x3f2fc4283afc7cc4ull -> 0xd280c250b0a5c7e1ull, rows 0xb7e20a93376bfe8bull -> 0x3f346416821514d1ull, final_hash 0xec2f788ae1bef8fcull -> 0x4baafe4f5ce072deull
//   Random04/0/0: boundary 0x3f2fc4283afc7cc4ull -> 0xd280c250b0a5c7e1ull, final_hash 0xec2f788ae1bef8fcull -> 0x4baafe4f5ce072deull
//   Random04/1/1: boundary 0xadf2b572cb770a1cull -> 0xb7bd621548544961ull, rows 0x4a54644e7d763961ull -> 0x44175452c39d8931ull, final_hash 0xa75420d0ae9c3b6eull -> 0x8a1e40bc7b77aca8ull
//   Random04/1/0: boundary 0xadf2b572cb770a1cull -> 0xb7bd621548544961ull, final_hash 0xa75420d0ae9c3b6eull -> 0x8a1e40bc7b77aca8ull
// expectation corrected (admission and book rules fold, 86 of 276 rows), because the source layer's fold (pineforge-source-adapter/v4) now folds the admission and book rules' new state -- the close-first fact, a refused reversal's mark, the pyramiding records and the exit tombstones, each only while set; every closed trade is unchanged in every Trade field against the engine before this change (the per-trade %.17g dump of every run-pass and the no-hash digests), as are the counts; harvested with this TU's own switch:
//   SameBarBatch/0/1: boundary 0x8a164a059578ab8dull -> 0x4272086ca6ca1522ull; rows 0x195a3c8fb495f3f2ull -> 0x9cce9b3b467cbc06ull
//   SameBarBatch/0/0: boundary 0x8a164a059578ab8dull -> 0x4272086ca6ca1522ull
//   SameBarBatch/1/1: boundary 0x3847edf901bf0336ull -> 0x91becb3314b8352aull; rows 0x759f8cd1b83c8355ull -> 0xc14a15121fc4dafeull
//   SameBarBatch/1/0: boundary 0x3847edf901bf0336ull -> 0x91becb3314b8352aull
//   Random01/0/1: boundary 0xf735a0cf0ce983f5ull -> 0x5df08d6cf796c5d3ull; rows 0xc10eacaa7ca851b7ull -> 0x4dba08a92c4965caull; final 0x267be998bdc0fe3aull -> 0x230bd57fac6b2343ull
//   Random01/0/0: boundary 0xf735a0cf0ce983f5ull -> 0x5df08d6cf796c5d3ull; final 0x267be998bdc0fe3aull -> 0x230bd57fac6b2343ull
//   Random01/1/1: boundary 0x4918ffb4dda67e41ull -> 0x11d5580c3ad2c150ull; rows 0xe31aa22d21c7f1deull -> 0x6dd6f70026c3b02dull; final 0x451001de8a9a3634ull -> 0xcdb5e514bc30f399ull
//   Random01/1/0: boundary 0x4918ffb4dda67e41ull -> 0x11d5580c3ad2c150ull; final 0x451001de8a9a3634ull -> 0xcdb5e514bc30f399ull
//   Random02/0/1: boundary 0x0b6509e69384691aull -> 0x85a9793e7de8f83cull; rows 0x2446e9ce790e8950ull -> 0x102dfa9a83aa052full
//   Random02/0/0: boundary 0x0b6509e69384691aull -> 0x85a9793e7de8f83cull
//   Random02/1/1: boundary 0xd9b975ecbba66cc1ull -> 0x2a001ce40849c83dull; rows 0x04099bcd77418920ull -> 0x65150034e328c19aull
//   Random02/1/0: boundary 0xd9b975ecbba66cc1ull -> 0x2a001ce40849c83dull
//   Random05/1/1: boundary 0xdf482d00a8b5e374ull -> 0xaba4719aa5e8f252ull
//   Random05/1/0: boundary 0xdf482d00a8b5e374ull -> 0xaba4719aa5e8f252ull
//   Random06/0/1: boundary 0x93905ddd8a40a8e9ull -> 0x882130a3d4a37a45ull; rows 0xab6c94fe61d304bcull -> 0x9d48633130e6d805ull
//   Random06/0/0: boundary 0x93905ddd8a40a8e9ull -> 0x882130a3d4a37a45ull
//   Random06/1/1: boundary 0x09d7fe0f1aa61b3full -> 0xaf4ecf82144b1ac8ull; rows 0x676cd5f3f8ed4300ull -> 0xcc93645f8f449125ull
//   Random06/1/0: boundary 0x09d7fe0f1aa61b3full -> 0xaf4ecf82144b1ac8ull
//   Random08/0/1: boundary 0x940251d9fc1847a8ull -> 0xb7867ffc6f92d5b6ull
//   Random08/0/0: boundary 0x940251d9fc1847a8ull -> 0xb7867ffc6f92d5b6ull
//   Random08/1/1: boundary 0x8f792d51d286e4caull -> 0x01b83e2b5cc16f88ull
//   Random08/1/0: boundary 0x8f792d51d286e4caull -> 0x01b83e2b5cc16f88ull
//   Random12/0/1: boundary 0x07b5b233ea6672bbull -> 0xc9b1cbbfa70ad0a3ull; rows 0xd5968b91bb5fbee1ull -> 0x375237f7cb52e5dfull
//   Random12/0/0: boundary 0x07b5b233ea6672bbull -> 0xc9b1cbbfa70ad0a3ull
//   Random12/1/1: boundary 0x635610ddcf1e886dull -> 0xb5036003166df176ull; rows 0xad7f7455ade3d98cull -> 0x15c9d5c766ba819cull
//   Random12/1/0: boundary 0x635610ddcf1e886dull -> 0xb5036003166df176ull
//   Random13/0/1: boundary 0xa86dd8023f9dfc7full -> 0xb66e2e84b38eb678ull; rows 0x39207687f52009f5ull -> 0x5f65b3d7b36ea29full
//   Random13/0/0: boundary 0xa86dd8023f9dfc7full -> 0xb66e2e84b38eb678ull
//   Random13/1/1: boundary 0xdcc52e873a9194c5ull -> 0xb09d4fd7dceabe48ull; rows 0x9c210c2c89c8ff5bull -> 0xd8164625abea467aull
//   Random13/1/0: boundary 0xdcc52e873a9194c5ull -> 0xb09d4fd7dceabe48ull
//   Random14/0/1: boundary 0x317b50a90a8f7e46ull -> 0xd5e2120688f6c7c4ull; rows 0x34e2cd5d23f7320eull -> 0xb74eb65ef72609efull
//   Random14/0/0: boundary 0x317b50a90a8f7e46ull -> 0xd5e2120688f6c7c4ull
//   Random14/1/1: boundary 0xfb8e43a765208859ull -> 0x4dba47723bc1da30ull; rows 0x5ad0b9e4ad562ef7ull -> 0xecf56aeb677c9941ull
//   Random14/1/0: boundary 0xfb8e43a765208859ull -> 0x4dba47723bc1da30ull
//   Random16/0/1: boundary 0x3f9429a53301ca85ull -> 0xb620ea52d6dd3932ull; rows 0x1888a9b7da1cc3c2ull -> 0xf1c68dfa75d41802ull
//   Random16/0/0: boundary 0x3f9429a53301ca85ull -> 0xb620ea52d6dd3932ull
//   Random16/1/1: boundary 0xbf02f3f6d610391eull -> 0x4951556cd1f8d3eaull; rows 0x497053e8dff2c3e0ull -> 0x5067bcffb88ad75dull
//   Random16/1/0: boundary 0xbf02f3f6d610391eull -> 0x4951556cd1f8d3eaull
//   Random26/0/1: boundary 0x059cd95973056346ull -> 0xe59742c6d53a59e9ull; rows 0x84bc9696eadb0bbfull -> 0xf695db5cf061acc2ull
//   Random26/0/0: boundary 0x059cd95973056346ull -> 0xe59742c6d53a59e9ull
//   Random26/1/1: boundary 0x0255fa624dfed493ull -> 0xd3eba03324d377beull; rows 0x4aa6b978b4bb9995ull -> 0xd428da4ca60dbc80ull
//   Random26/1/0: boundary 0x0255fa624dfed493ull -> 0xd3eba03324d377beull
//   Random27/0/1: boundary 0x7eb9ce984b56586cull -> 0x922525ed0e0ac261ull; rows 0x1769e70c36959215ull -> 0xbb91aa8b02f75e5dull; final 0x11c88e4bb89b615full -> 0x44b2b289f46ef934ull
//   Random27/0/0: boundary 0x7eb9ce984b56586cull -> 0x922525ed0e0ac261ull; final 0x11c88e4bb89b615full -> 0x44b2b289f46ef934ull
//   Random27/1/1: boundary 0x4bc4e65f6de46606ull -> 0x05e1e2c7849e1a23ull; rows 0x0e9065156ad84593ull -> 0x609c20acd1c81b42ull; final 0xa74668b564b145f4ull -> 0x47be5c4fe4e9f14bull
//   Random27/1/0: boundary 0x4bc4e65f6de46606ull -> 0x05e1e2c7849e1a23ull; final 0xa74668b564b145f4ull -> 0x47be5c4fe4e9f14bull
//   Random28/0/1: boundary 0xc96c1554502037ebull -> 0xbb9929fd869d83ecull; rows 0x22e21a642387a4d3ull -> 0xaf3b8ed5ced5375bull
//   Random28/0/0: boundary 0xc96c1554502037ebull -> 0xbb9929fd869d83ecull
//   Random28/1/1: boundary 0xc11f413da0c9110eull -> 0xf8578fbbeaa81481ull; rows 0xe4fe4a1ed4077ba0ull -> 0x3e0acad585be86d9ull
//   Random28/1/0: boundary 0xc11f413da0c9110eull -> 0xf8578fbbeaa81481ull
//   Random32/0/1: boundary 0x401cac1e4097a091ull -> 0xb60b6c948bed15c6ull; rows 0x4b0794b5b0901ff0ull -> 0xb9418d6c8a8019adull
//   Random32/0/0: boundary 0x401cac1e4097a091ull -> 0xb60b6c948bed15c6ull
//   Random32/1/1: boundary 0x4586c43cea4202e0ull -> 0x2bfd687e5d081a07ull; rows 0x0519cfb78b41cc3eull -> 0xb78e2496d66141b5ull
//   Random32/1/0: boundary 0x4586c43cea4202e0ull -> 0x2bfd687e5d081a07ull
//   Random33/0/1: boundary 0xa5d5f0be4a174ee0ull -> 0x721216edad992e1dull; rows 0x946aee715f170460ull -> 0x467303755631d3f7ull
//   Random33/0/0: boundary 0xa5d5f0be4a174ee0ull -> 0x721216edad992e1dull
//   Random33/1/1: boundary 0x6d63842ccde05cf6ull -> 0xc58b299eef4ec679ull; rows 0x538032f14405b847ull -> 0x098db146c52cdba4ull
//   Random33/1/0: boundary 0x6d63842ccde05cf6ull -> 0xc58b299eef4ec679ull
//   Random35/0/1: boundary 0x9cb5b0a5def223b9ull -> 0xcb20cc7879d5dd93ull; rows 0x33f31b56f2e5e7eeull -> 0x9035c0800d822d83ull
//   Random35/0/0: boundary 0x9cb5b0a5def223b9ull -> 0xcb20cc7879d5dd93ull
//   Random35/1/1: boundary 0x3da4a7074c44ca84ull -> 0x6c694165432ec0aeull; rows 0x7c4219c074c408a9ull -> 0xb52f088513e02e11ull
//   Random35/1/0: boundary 0x3da4a7074c44ca84ull -> 0x6c694165432ec0aeull
//   Random36/0/1: boundary 0x31a11ede5003bbbbull -> 0x4c539c1e1279ea93ull
//   Random36/0/0: boundary 0x31a11ede5003bbbbull -> 0x4c539c1e1279ea93ull
//   Random36/1/1: boundary 0xa1d744b27afae188ull -> 0x3f4f63cc5f1e5e4full
//   Random36/1/0: boundary 0xa1d744b27afae188ull -> 0x3f4f63cc5f1e5e4full
//   Random37/0/1: boundary 0x430fcc4706e62293ull -> 0xc6cb57f1ce8eefbbull; rows 0x1b407691e1df9efeull -> 0x4860a22a5843d96aull
//   Random37/0/0: boundary 0x430fcc4706e62293ull -> 0xc6cb57f1ce8eefbbull
//   Random37/1/1: boundary 0x37d4cbfc33b5719eull -> 0x01d411837a3a9d62ull; rows 0x97e1b58cefd475f3ull -> 0x7a591ccd47474fc2ull
//   Random37/1/0: boundary 0x37d4cbfc33b5719eull -> 0x01d411837a3a9d62ull
//   Random38/0/1: boundary 0x6d9025b5cc19eae3ull -> 0x2f6d38ecf3660bbdull; rows 0x8c8ed908f07f4a43ull -> 0xdde4a1d753baf311ull
//   Random38/0/0: boundary 0x6d9025b5cc19eae3ull -> 0x2f6d38ecf3660bbdull
//   Random38/1/1: boundary 0x901689f14879fa92ull -> 0x704e2efa24abca57ull; rows 0xfd9d2b3590ae297bull -> 0x83619f47c2632492ull
//   Random38/1/0: boundary 0x901689f14879fa92ull -> 0x704e2efa24abca57ull
//   Random40/0/1: boundary 0x92d066f10ca532c8ull -> 0xdc43658be7d34abfull
//   Random40/0/0: boundary 0x92d066f10ca532c8ull -> 0xdc43658be7d34abfull
//   Random40/1/1: boundary 0xedda3193d426477bull -> 0xc8903bbbff4a0a85ull
//   Random40/1/0: boundary 0xedda3193d426477bull -> 0xc8903bbbff4a0a85ull
//   Random41/0/1: boundary 0x1fda069b99f5c640ull -> 0x6060dfbdc91677f4ull; rows 0x533c23b35aa4cec8ull -> 0x6be88d98095a127aull
//   Random41/0/0: boundary 0x1fda069b99f5c640ull -> 0x6060dfbdc91677f4ull
//   Random41/1/1: boundary 0x514020d9dcd73266ull -> 0x9655672bb08846d8ull; rows 0xeb281d4380bbbe21ull -> 0x65f1a64a95568f3bull
//   Random41/1/0: boundary 0x514020d9dcd73266ull -> 0x9655672bb08846d8ull
//   Random45/0/1: boundary 0xbcf622485b8343dfull -> 0xbf7d21e38106b05dull; rows 0xba395bcb3696a8a7ull -> 0xe5487eb42c395b04ull
//   Random45/0/0: boundary 0xbcf622485b8343dfull -> 0xbf7d21e38106b05dull
//   Random45/1/1: boundary 0xbe5fe3159d86e0cfull -> 0x2909929a3dbedb12ull; rows 0x7fdcbc656d6928b0ull -> 0xaad64569b7a940b5ull
//   Random45/1/0: boundary 0xbe5fe3159d86e0cfull -> 0x2909929a3dbedb12ull
// expectation corrected (resting-limit tick reach, 14 of 276 rows), because a calc_on_order_fills limit exit that closes the position held when it is placed, on the chart path and not yet reached where it is placed, rests at its tick-built threshold (RestingLimitTickReach, src/compat/pine/callback_lifecycle_rules.hpp): the request's trigger price moves in the source folds, the recorded rows and the final hash. Every closed trade keeps its fill (side, quantity, entry and exit time and price) and every money field; only CalcOnFills' trade digest moves, on entry_incarnation alone: on bar 4 its long's 102.75 exit limit is matched where its threshold is crossed on the way up, before the buy stop the bar's high fills, instead of at the high beside that stop, and booked at 102.75 either way; at that shared point main submits the stop entry the fill's recalculation delays one callback earlier, under an incarnation the next re-issue replaces, so here every later incarnation is one lower. Every count is unchanged; harvested with this TU's harvest switch against main 44eab7b1 (every old row reproduced) and this tree:
//   CalcOnFills/0/1: boundary 0xd72b0332fd230d12ull -> 0xb0b39b5f6f0a6911ull; rows 0xccaa3a371697d096ull -> 0xc084142a8d1a22caull; trades 0x6f29557db540e116ull -> 0xddcf84c49adba0d9ull; final 0x782b53f3b9208eadull -> 0x1ef9b2973605fbf9ull
//   CalcOnFills/0/0: boundary 0xd72b0332fd230d12ull -> 0xb0b39b5f6f0a6911ull; trades 0x6f29557db540e116ull -> 0xddcf84c49adba0d9ull; final 0x782b53f3b9208eadull -> 0x1ef9b2973605fbf9ull
//   Random08/0/1: boundary 0xb7867ffc6f92d5b6ull -> 0xeda2306e3464ed04ull
//   Random08/0/0: boundary 0xb7867ffc6f92d5b6ull -> 0xeda2306e3464ed04ull
//   Random10/0/1: boundary 0x43507c7025af0a6full -> 0xa9bd9cb86a3f80e4ull; rows 0xe11899f309400695ull -> 0x5c107f2327471892ull
//   Random10/0/0: boundary 0x43507c7025af0a6full -> 0xa9bd9cb86a3f80e4ull
//   Random20/0/1: boundary 0x7129359fdae8391bull -> 0xb3082baa28ce121eull
//   Random20/0/0: boundary 0x7129359fdae8391bull -> 0xb3082baa28ce121eull
//   Random22/0/1: boundary 0x5dfb44cfc3fa35a6ull -> 0xdee5a34184585dabull; rows 0x2198153265e4d6d8ull -> 0x433eed1e41804028ull
//   Random22/0/0: boundary 0x5dfb44cfc3fa35a6ull -> 0xdee5a34184585dabull
//   Random40/0/1: boundary 0xdc43658be7d34abfull -> 0xe779b6dee0e07501ull; rows 0xcaf2d4a713cf53a0ull -> 0x2e9b4f64942449ffull
//   Random40/0/0: boundary 0xdc43658be7d34abfull -> 0xe779b6dee0e07501ull
//   Random45/0/1: boundary 0xbf7d21e38106b05dull -> 0x5e7de0663f133e9eull
//   Random45/0/0: boundary 0xbf7d21e38106b05dull -> 0x5e7de0663f133e9eull
constexpr Pinned kPinned[] = {
    {"Quiet", 0, 1, 0xcd8d3ecb4d2c740aull, 0xe6da4342a82c460cull, 0x47fe0d7eaf8e51e3ull, 0x458f1ecf00ef1d5cull, 40, 40, 0},
    {"Quiet", 0, 0, 0xcd8d3ecb4d2c740aull, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0x458f1ecf00ef1d5cull, 40, 0, 0},
    {"Quiet", 1, 1, 0x2a102759d268d84dull, 0x923a578d640d7fdbull, 0x47fe0d7eaf8e51e3ull, 0x6ca064a309be6bfaull, 40, 40, 0},
    {"Quiet", 1, 0, 0x2a102759d268d84dull, 0x14650fb0739d0383ull, 0x47fe0d7eaf8e51e3ull, 0x6ca064a309be6bfaull, 40, 0, 0},
    {"HoldLongFull", 0, 1, 0x2be6111eb08aff44ull, 0x37135fdf24860586ull, 0xa32454de68248651ull, 0x6caa6506b43c5228ull, 40, 40, 1},
    {"HoldLongFull", 0, 0, 0x2be6111eb08aff44ull, 0x14650fb0739d0383ull, 0xa32454de68248651ull, 0x6caa6506b43c5228ull, 40, 0, 1},
    {"HoldLongFull", 1, 1, 0x6da42e25a0445e57ull, 0xa90a9669249de5f2ull, 0xa32454de68248651ull, 0x1fdb374b13f0dd9cull, 40, 40, 1},
    {"HoldLongFull", 1, 0, 0x6da42e25a0445e57ull, 0x14650fb0739d0383ull, 0xa32454de68248651ull, 0x1fdb374b13f0dd9cull, 40, 0, 1},
    {"MarginCalls", 0, 1, 0x4bcea4f978d972aaull, 0x9d9041a1674393c2ull, 0x259f52c0c76aafe9ull, 0x717e3f9c839a052bull, 36, 36, 3},
    {"MarginCalls", 0, 0, 0x4bcea4f978d972aaull, 0x14650fb0739d0383ull, 0x259f52c0c76aafe9ull, 0x717e3f9c839a052bull, 36, 0, 3},
    {"MarginCalls", 1, 1, 0x3df3de80db85db62ull, 0xf8701e62a6ccc2abull, 0x92cc9a49e414ed25ull, 0x8ff634900f7f4491ull, 36, 36, 2},
    {"MarginCalls", 1, 0, 0x3df3de80db85db62ull, 0x14650fb0739d0383ull, 0x92cc9a49e414ed25ull, 0x8ff634900f7f4491ull, 36, 0, 2},
    {"MarginCallCycles", 0, 1, 0x29243afd7d418ddbull, 0xe34f9967f487081cull, 0x657a702082e43f34ull, 0x37573440557d0725ull, 64, 64, 1},
    {"MarginCallCycles", 0, 0, 0x29243afd7d418ddbull, 0x14650fb0739d0383ull, 0x657a702082e43f34ull, 0x37573440557d0725ull, 64, 0, 1},
    {"MarginCallCycles", 1, 1, 0xf4bc0848f6756193ull, 0x533d78ca092362aaull, 0x9b23f7fef31dec0cull, 0x22ec1fab2742fa75ull, 64, 64, 3},
    {"MarginCallCycles", 1, 0, 0xf4bc0848f6756193ull, 0x14650fb0739d0383ull, 0x9b23f7fef31dec0cull, 0x22ec1fab2742fa75ull, 64, 0, 3},
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
    {"ThrottledStopEntry", 0, 1, 0x9858ef53016219b5ull, 0x7a9bf56c68507443ull, 0x0736976e175fe1b0ull, 0xff021a844c07b66aull, 40, 40, 1},
    {"ThrottledStopEntry", 0, 0, 0x9858ef53016219b5ull, 0x14650fb0739d0383ull, 0x0736976e175fe1b0ull, 0xff021a844c07b66aull, 40, 0, 1},
    {"ThrottledStopEntry", 1, 1, 0xfe8fa02d55c4672cull, 0x683824cc9c1fee22ull, 0x1a2ebc02c3e477f6ull, 0x9818e3412783937full, 40, 40, 1},
    {"ThrottledStopEntry", 1, 0, 0xfe8fa02d55c4672cull, 0x14650fb0739d0383ull, 0x1a2ebc02c3e477f6ull, 0x9818e3412783937full, 40, 0, 1},
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
    {"CalcOnFills", 0, 1, 0xb0b39b5f6f0a6911ull, 0xc084142a8d1a22caull, 0xddcf84c49adba0d9ull, 0x1ef9b2973605fbf9ull, 55, 40, 8},
    {"CalcOnFills", 0, 0, 0xb0b39b5f6f0a6911ull, 0x14650fb0739d0383ull, 0xddcf84c49adba0d9ull, 0x1ef9b2973605fbf9ull, 55, 0, 8},
    {"CalcOnFills", 1, 1, 0x4c36d59260cff1e8ull, 0xc4db4870b394f199ull, 0xa16d17de4933efdaull, 0x6372d23bba8999b2ull, 52, 40, 6},
    {"CalcOnFills", 1, 0, 0x4c36d59260cff1e8ull, 0x14650fb0739d0383ull, 0xa16d17de4933efdaull, 0x6372d23bba8999b2ull, 52, 0, 6},
    {"CloseOrders", 0, 1, 0x5c37531627d539efull, 0xbe9bef110e3c1373ull, 0x61426583c5cf1a23ull, 0xe9218c7da1f48eabull, 48, 48, 16},
    {"CloseOrders", 0, 0, 0x5c37531627d539efull, 0x14650fb0739d0383ull, 0x61426583c5cf1a23ull, 0xe9218c7da1f48eabull, 48, 0, 16},
    {"CloseOrders", 1, 1, 0x359923313e616f54ull, 0xb5c62af9112eea41ull, 0x30d51e9c8069ca8full, 0x5493cdaf6cb43957ull, 48, 48, 16},
    {"CloseOrders", 1, 0, 0x359923313e616f54ull, 0x14650fb0739d0383ull, 0x30d51e9c8069ca8full, 0x5493cdaf6cb43957ull, 48, 0, 16},
    {"CloseOrdersSlipped", 0, 1, 0x40c5a78c03effcb4ull, 0x86f13e8920863005ull, 0x0f201395d3fe211full, 0xd4b29cd71c1bf95full, 36, 36, 2},
    {"CloseOrdersSlipped", 0, 0, 0x40c5a78c03effcb4ull, 0x14650fb0739d0383ull, 0x0f201395d3fe211full, 0xd4b29cd71c1bf95full, 36, 0, 2},
    {"CloseOrdersSlipped", 1, 1, 0x807012cdd2283145ull, 0xe11e1b898ec834e8ull, 0x0f201395d3fe211full, 0x021b24196a0982faull, 36, 36, 2},
    {"CloseOrdersSlipped", 1, 0, 0x807012cdd2283145ull, 0x14650fb0739d0383ull, 0x0f201395d3fe211full, 0x021b24196a0982faull, 36, 0, 2},
    {"IntradayLoss", 0, 1, 0x392fac4b53eaad92ull, 0xe02291acf9ac772dull, 0x60c9f061d60a69edull, 0x46bb7e611ca6e1c8ull, 40, 40, 1},
    {"IntradayLoss", 0, 0, 0x392fac4b53eaad92ull, 0x14650fb0739d0383ull, 0x60c9f061d60a69edull, 0x46bb7e611ca6e1c8ull, 40, 0, 1},
    {"IntradayLoss", 1, 1, 0xeed99e6cc52c2efcull, 0xafa99e76c624b49aull, 0x60c9f061d60a69edull, 0x472c0f68bec82bf7ull, 40, 40, 1},
    {"IntradayLoss", 1, 0, 0xeed99e6cc52c2efcull, 0x14650fb0739d0383ull, 0x60c9f061d60a69edull, 0x472c0f68bec82bf7ull, 40, 0, 1},
    {"IntradayCapDeferred", 0, 1, 0x56c52ed78e3bf64eull, 0xccd84788a40fdeaaull, 0x2fa61401cccb3845ull, 0x30cddb814e876d43ull, 24, 24, 1},
    {"IntradayCapDeferred", 0, 0, 0x56c52ed78e3bf64eull, 0x14650fb0739d0383ull, 0x2fa61401cccb3845ull, 0x30cddb814e876d43ull, 24, 0, 1},
    {"IntradayCapDeferred", 1, 1, 0x425990cfcb5d5162ull, 0x35e7cf4c3a06c2cfull, 0x6517fe8ea7437b74ull, 0x782c119eeff543e4ull, 24, 24, 1},
    {"IntradayCapDeferred", 1, 0, 0x425990cfcb5d5162ull, 0x14650fb0739d0383ull, 0x6517fe8ea7437b74ull, 0x782c119eeff543e4ull, 24, 0, 1},
    {"PreopenSlice", 0, 1, 0xcbc8fb1ba9d83481ull, 0xbe57d9275ae9e2bbull, 0x96bb4615ab0ad33bull, 0x62f97adf60eb373full, 36, 36, 1},
    {"PreopenSlice", 0, 0, 0xcbc8fb1ba9d83481ull, 0x14650fb0739d0383ull, 0x96bb4615ab0ad33bull, 0x62f97adf60eb373full, 36, 0, 1},
    {"PreopenSlice", 1, 1, 0xadcfe58d853cedfcull, 0xecc971145edced41ull, 0x96bb4615ab0ad33bull, 0x94fff9b38c7f1bd5ull, 36, 36, 1},
    {"PreopenSlice", 1, 0, 0xadcfe58d853cedfcull, 0x14650fb0739d0383ull, 0x96bb4615ab0ad33bull, 0x94fff9b38c7f1bd5ull, 36, 0, 1},
    {"SameBarBatch", 0, 1, 0x4272086ca6ca1522ull, 0x9cce9b3b467cbc06ull, 0xa1c83f47f2e97c0dull, 0x8e9cd7ece0b10463ull, 36, 36, 8},
    {"SameBarBatch", 0, 0, 0x4272086ca6ca1522ull, 0x14650fb0739d0383ull, 0xa1c83f47f2e97c0dull, 0x8e9cd7ece0b10463ull, 36, 0, 8},
    {"SameBarBatch", 1, 1, 0x91becb3314b8352aull, 0xc14a15121fc4dafeull, 0xa1c83f47f2e97c0dull, 0xffcc8118cbe22f80ull, 36, 36, 8},
    {"SameBarBatch", 1, 0, 0x91becb3314b8352aull, 0x14650fb0739d0383ull, 0xa1c83f47f2e97c0dull, 0xffcc8118cbe22f80ull, 36, 0, 8},
    {"CommissionedShort", 0, 1, 0xc73e33b30bf09c29ull, 0x26a3e6d04263ad27ull, 0x35b634c9ced2d3caull, 0xeb0d9ea9f0bdfd4bull, 32, 32, 1},
    {"CommissionedShort", 0, 0, 0xc73e33b30bf09c29ull, 0x14650fb0739d0383ull, 0x35b634c9ced2d3caull, 0xeb0d9ea9f0bdfd4bull, 32, 0, 1},
    {"CommissionedShort", 1, 1, 0x350b092698bdf8fdull, 0x3fe25a6001c6c4feull, 0x35b634c9ced2d3caull, 0xd73d8e053dbf5cd2ull, 32, 32, 1},
    {"CommissionedShort", 1, 0, 0x350b092698bdf8fdull, 0x14650fb0739d0383ull, 0x35b634c9ced2d3caull, 0xd73d8e053dbf5cd2ull, 32, 0, 1},
    {"AccountFx", 0, 1, 0xd27340484425ff52ull, 0x4e596d61c552597eull, 0x3b853d0c008c3078ull, 0xea884b09a27ea73bull, 48, 48, 1},
    {"AccountFx", 0, 0, 0xd27340484425ff52ull, 0x14650fb0739d0383ull, 0x3b853d0c008c3078ull, 0xea884b09a27ea73bull, 48, 0, 1},
    {"Stream", 0, 1, 0x6d08c5f40ecf69e9ull, 0xba7d73957a71f419ull, 0xbe44c66b6a388e22ull, 0x8e1bef7a1d679259ull, 40, 40, 5},
    {"Stream", 0, 0, 0x6d08c5f40ecf69e9ull, 0x14650fb0739d0383ull, 0xbe44c66b6a388e22ull, 0x8e1bef7a1d679259ull, 40, 0, 5},
    {"Random01", 0, 1, 0x5df08d6cf796c5d3ull, 0x4dba08a92c4965caull, 0xe6d673a2f0d40429ull, 0x230bd57fac6b2343ull, 55, 55, 15},
    {"Random01", 0, 0, 0x5df08d6cf796c5d3ull, 0x14650fb0739d0383ull, 0xe6d673a2f0d40429ull, 0x230bd57fac6b2343ull, 55, 0, 15},
    {"Random01", 1, 1, 0x11d5580c3ad2c150ull, 0x6dd6f70026c3b02dull, 0x8b2f1d5ce5ba6fb9ull, 0xcdb5e514bc30f399ull, 55, 55, 15},
    {"Random01", 1, 0, 0x11d5580c3ad2c150ull, 0x14650fb0739d0383ull, 0x8b2f1d5ce5ba6fb9ull, 0xcdb5e514bc30f399ull, 55, 0, 15},
    {"Random02", 0, 1, 0x85a9793e7de8f83cull, 0x102dfa9a83aa052full, 0x81aa2a130803e2efull, 0x465671bfd71f1b49ull, 58, 40, 12},
    {"Random02", 0, 0, 0x85a9793e7de8f83cull, 0x14650fb0739d0383ull, 0x81aa2a130803e2efull, 0x465671bfd71f1b49ull, 58, 0, 12},
    {"Random02", 1, 1, 0x2a001ce40849c83dull, 0x65150034e328c19aull, 0x31c2efb8bb736f56ull, 0xf0d8e6f8c66ba0edull, 58, 40, 12},
    {"Random02", 1, 0, 0x2a001ce40849c83dull, 0x14650fb0739d0383ull, 0x31c2efb8bb736f56ull, 0xf0d8e6f8c66ba0edull, 58, 0, 12},
    {"Random03", 0, 1, 0x55f5fbb1ad46af78ull, 0x91c200dd67f2c6d3ull, 0x6ed2b968ae7e07fcull, 0xae6a17ed7812bfd1ull, 44, 44, 12},
    {"Random03", 0, 0, 0x55f5fbb1ad46af78ull, 0x14650fb0739d0383ull, 0x6ed2b968ae7e07fcull, 0xae6a17ed7812bfd1ull, 44, 0, 12},
    {"Random03", 1, 1, 0xb587dcd78562fc33ull, 0xefbf3dbd57d0d024ull, 0x80ca2343fcd2c1d0ull, 0x35ee7a28303aa6c5ull, 44, 44, 12},
    {"Random03", 1, 0, 0xb587dcd78562fc33ull, 0x14650fb0739d0383ull, 0x80ca2343fcd2c1d0ull, 0x35ee7a28303aa6c5ull, 44, 0, 12},
    {"Random04", 0, 1, 0x1ac71820aa7472f6ull, 0xc41eab81ee457dd2ull, 0xcc6cf8406a1f71a0ull, 0x697574dbf62c8ea2ull, 44, 44, 15},
    {"Random04", 0, 0, 0x1ac71820aa7472f6ull, 0x14650fb0739d0383ull, 0xcc6cf8406a1f71a0ull, 0x697574dbf62c8ea2ull, 44, 0, 15},
    {"Random04", 1, 1, 0x6cd2901c7ff5ce8eull, 0xa5a45a69ac5a66ecull, 0x3659e955e5220f85ull, 0x65a20dcd9f1980a1ull, 44, 44, 15},
    {"Random04", 1, 0, 0x6cd2901c7ff5ce8eull, 0x14650fb0739d0383ull, 0x3659e955e5220f85ull, 0x65a20dcd9f1980a1ull, 44, 0, 15},
    {"Random05", 0, 1, 0x5288fe5363963744ull, 0x83a7124d23410b32ull, 0xf91857e72781f5aeull, 0xd3a0f35947f3607full, 58, 53, 3},
    {"Random05", 0, 0, 0x5288fe5363963744ull, 0x14650fb0739d0383ull, 0xf91857e72781f5aeull, 0xd3a0f35947f3607full, 58, 0, 3},
    {"Random05", 1, 1, 0xaba4719aa5e8f252ull, 0x575d95342fa85cf5ull, 0xc83a1288cfd7ee4dull, 0x5fb131e7a5d7a095ull, 66, 53, 10},
    {"Random05", 1, 0, 0xaba4719aa5e8f252ull, 0x14650fb0739d0383ull, 0xc83a1288cfd7ee4dull, 0x5fb131e7a5d7a095ull, 66, 0, 10},
    {"Random06", 0, 1, 0x882130a3d4a37a45ull, 0x9d48633130e6d805ull, 0x9703eeb61cd7387bull, 0xdbab6a336c90c65eull, 46, 46, 17},
    {"Random06", 0, 0, 0x882130a3d4a37a45ull, 0x14650fb0739d0383ull, 0x9703eeb61cd7387bull, 0xdbab6a336c90c65eull, 46, 0, 17},
    {"Random06", 1, 1, 0xaf4ecf82144b1ac8ull, 0xcc93645f8f449125ull, 0x11227961f96cbcc2ull, 0x1347015357d2d8a6ull, 46, 46, 17},
    {"Random06", 1, 0, 0xaf4ecf82144b1ac8ull, 0x14650fb0739d0383ull, 0x11227961f96cbcc2ull, 0x1347015357d2d8a6ull, 46, 0, 17},
    {"Random07", 0, 1, 0xdf808db11dde4c8aull, 0xbf2e34a504ca5333ull, 0x67ce7c56c16f12a7ull, 0xa05d024de2de4af1ull, 46, 46, 12},
    {"Random07", 0, 0, 0xdf808db11dde4c8aull, 0x14650fb0739d0383ull, 0x67ce7c56c16f12a7ull, 0xa05d024de2de4af1ull, 46, 0, 12},
    {"Random07", 1, 1, 0x08510ec5518f91a6ull, 0x6082d5a6943988d9ull, 0x9cbc1f07c0648d08ull, 0x4c9a943bb069621cull, 46, 46, 12},
    {"Random07", 1, 0, 0x08510ec5518f91a6ull, 0x14650fb0739d0383ull, 0x9cbc1f07c0648d08ull, 0x4c9a943bb069621cull, 46, 0, 12},
    {"Random08", 0, 1, 0xeda2306e3464ed04ull, 0x05329eca8b44a2ecull, 0x2b85135e59cd2071ull, 0x58a481d75ef5c17cull, 94, 59, 26},
    {"Random08", 0, 0, 0xeda2306e3464ed04ull, 0x14650fb0739d0383ull, 0x2b85135e59cd2071ull, 0x58a481d75ef5c17cull, 94, 0, 26},
    {"Random08", 1, 1, 0x01b83e2b5cc16f88ull, 0xe4faba20ecef9864ull, 0x9318fb890727b7eaull, 0xd850846761b87002ull, 94, 59, 25},
    {"Random08", 1, 0, 0x01b83e2b5cc16f88ull, 0x14650fb0739d0383ull, 0x9318fb890727b7eaull, 0xd850846761b87002ull, 94, 0, 25},
    {"Random09", 0, 1, 0x0c68e77a3f7b0760ull, 0xff0fbcea65f2d984ull, 0xcda6a598aa82b5f0ull, 0x8efe821a3ef46d45ull, 58, 58, 2},
    {"Random09", 0, 0, 0x0c68e77a3f7b0760ull, 0x14650fb0739d0383ull, 0xcda6a598aa82b5f0ull, 0x8efe821a3ef46d45ull, 58, 0, 2},
    {"Random09", 1, 1, 0xbdad3983bf4de13eull, 0xf7e0f637f127fad5ull, 0x86b2bbc1465372d7ull, 0xd5e3868266cf224cull, 58, 58, 2},
    {"Random09", 1, 0, 0xbdad3983bf4de13eull, 0x14650fb0739d0383ull, 0x86b2bbc1465372d7ull, 0xd5e3868266cf224cull, 58, 0, 2},
    {"Random10", 0, 1, 0xa9bd9cb86a3f80e4ull, 0x5c107f2327471892ull, 0x4bf91f9abde25f6eull, 0x05050a7f6f43f8deull, 62, 51, 21},
    {"Random10", 0, 0, 0xa9bd9cb86a3f80e4ull, 0x14650fb0739d0383ull, 0x4bf91f9abde25f6eull, 0x05050a7f6f43f8deull, 62, 0, 21},
    {"Random10", 1, 1, 0x078823e295aa1e5aull, 0x074d774449e4001eull, 0x36fb31bb06edb88cull, 0x14c1017cb42c82adull, 62, 51, 21},
    {"Random10", 1, 0, 0x078823e295aa1e5aull, 0x14650fb0739d0383ull, 0x36fb31bb06edb88cull, 0x14c1017cb42c82adull, 62, 0, 21},
    {"Random11", 0, 1, 0xd329adc42a76a850ull, 0xe3aa05fd54484a81ull, 0x2f92b1af8d2cfafcull, 0x65c76d0ec8c574fbull, 60, 60, 3},
    {"Random11", 0, 0, 0xd329adc42a76a850ull, 0x14650fb0739d0383ull, 0x2f92b1af8d2cfafcull, 0x65c76d0ec8c574fbull, 60, 0, 3},
    {"Random11", 1, 1, 0x8e46e1ef7751abeeull, 0x7edce871e6834062ull, 0xad6d6d2577cca5c9ull, 0xfc8d53a8d4020ceeull, 60, 60, 8},
    {"Random11", 1, 0, 0x8e46e1ef7751abeeull, 0x14650fb0739d0383ull, 0xad6d6d2577cca5c9ull, 0xfc8d53a8d4020ceeull, 60, 0, 8},
    {"Random12", 0, 1, 0xc9b1cbbfa70ad0a3ull, 0x375237f7cb52e5dfull, 0xe801e0d8eaffcc6bull, 0x70c9cc50965fe672ull, 92, 62, 22},
    {"Random12", 0, 0, 0xc9b1cbbfa70ad0a3ull, 0x14650fb0739d0383ull, 0xe801e0d8eaffcc6bull, 0x70c9cc50965fe672ull, 92, 0, 22},
    {"Random12", 1, 1, 0xb5036003166df176ull, 0x15c9d5c766ba819cull, 0x16fca020c807a384ull, 0x9eda6cdd6ea9c8c7ull, 93, 62, 22},
    {"Random12", 1, 0, 0xb5036003166df176ull, 0x14650fb0739d0383ull, 0x16fca020c807a384ull, 0x9eda6cdd6ea9c8c7ull, 93, 0, 22},
    {"Random13", 0, 1, 0xb66e2e84b38eb678ull, 0x5f65b3d7b36ea29full, 0xcdf7b5ce1f4314d9ull, 0xbeb50a875a54f142ull, 60, 60, 16},
    {"Random13", 0, 0, 0xb66e2e84b38eb678ull, 0x14650fb0739d0383ull, 0xcdf7b5ce1f4314d9ull, 0xbeb50a875a54f142ull, 60, 0, 16},
    {"Random13", 1, 1, 0xb09d4fd7dceabe48ull, 0xd8164625abea467aull, 0xb3e3251fdef3358cull, 0x6c24feeadc377b01ull, 60, 60, 16},
    {"Random13", 1, 0, 0xb09d4fd7dceabe48ull, 0x14650fb0739d0383ull, 0xb3e3251fdef3358cull, 0x6c24feeadc377b01ull, 60, 0, 16},
    {"Random14", 0, 1, 0xd5e2120688f6c7c4ull, 0xb74eb65ef72609efull, 0x4e8d14373274d558ull, 0x7e2d50f568de6f0full, 49, 49, 16},
    {"Random14", 0, 0, 0xd5e2120688f6c7c4ull, 0x14650fb0739d0383ull, 0x4e8d14373274d558ull, 0x7e2d50f568de6f0full, 49, 0, 16},
    {"Random14", 1, 1, 0x4dba47723bc1da30ull, 0xecf56aeb677c9941ull, 0xbec287230f684e1dull, 0x1246d6915dd5918aull, 49, 49, 16},
    {"Random14", 1, 0, 0x4dba47723bc1da30ull, 0x14650fb0739d0383ull, 0xbec287230f684e1dull, 0x1246d6915dd5918aull, 49, 0, 16},
    {"Random15", 0, 1, 0x51567fd50a48fee9ull, 0x058a59750f5c2edaull, 0x7408d19137f1009aull, 0x997aa1f5f5f7b8a6ull, 42, 42, 12},
    {"Random15", 0, 0, 0x51567fd50a48fee9ull, 0x14650fb0739d0383ull, 0x7408d19137f1009aull, 0x997aa1f5f5f7b8a6ull, 42, 0, 12},
    {"Random15", 1, 1, 0xef7fdd2fa0e6a69cull, 0x1f7c484904cf76a3ull, 0x8c47d39a73e36388ull, 0x2bfb1d08fb552fb9ull, 42, 42, 12},
    {"Random15", 1, 0, 0xef7fdd2fa0e6a69cull, 0x14650fb0739d0383ull, 0x8c47d39a73e36388ull, 0x2bfb1d08fb552fb9ull, 42, 0, 12},
    {"Random16", 0, 1, 0xb620ea52d6dd3932ull, 0xf1c68dfa75d41802ull, 0x894ec2a3cafc14c8ull, 0x226dae53316a3b2aull, 54, 54, 14},
    {"Random16", 0, 0, 0xb620ea52d6dd3932ull, 0x14650fb0739d0383ull, 0x894ec2a3cafc14c8ull, 0x226dae53316a3b2aull, 54, 0, 14},
    {"Random16", 1, 1, 0x4951556cd1f8d3eaull, 0x5067bcffb88ad75dull, 0xe0b0e7cbf08a5214ull, 0x71c34f49d37990fdull, 54, 54, 14},
    {"Random16", 1, 0, 0x4951556cd1f8d3eaull, 0x14650fb0739d0383ull, 0xe0b0e7cbf08a5214ull, 0x71c34f49d37990fdull, 54, 0, 14},
    {"Random17", 0, 1, 0x4f041987fe01e1c2ull, 0xc2f61e906254c6ceull, 0xf3e62779123d7565ull, 0xe5b43a4ee15652b7ull, 49, 47, 1},
    {"Random17", 0, 0, 0x4f041987fe01e1c2ull, 0x14650fb0739d0383ull, 0xf3e62779123d7565ull, 0xe5b43a4ee15652b7ull, 49, 0, 1},
    {"Random17", 1, 1, 0x051ecf9eee42ea17ull, 0x52f90b40b96ddaf8ull, 0x5814b347681cd974ull, 0xb499c267440294d9ull, 50, 47, 2},
    {"Random17", 1, 0, 0x051ecf9eee42ea17ull, 0x14650fb0739d0383ull, 0x5814b347681cd974ull, 0xb499c267440294d9ull, 50, 0, 2},
    {"Random18", 0, 1, 0xf2e21fb21175ff16ull, 0x8f91d573e0c6257cull, 0xd8b1ce3cab1f5711ull, 0x6da1d912cf25ecc7ull, 75, 62, 9},
    {"Random18", 0, 0, 0xf2e21fb21175ff16ull, 0x14650fb0739d0383ull, 0xd8b1ce3cab1f5711ull, 0x6da1d912cf25ecc7ull, 75, 0, 9},
    {"Random18", 1, 1, 0xa117a73200c4d0d8ull, 0xf2697328ab5e1a72ull, 0x5f03c99e6854a46aull, 0x04934ec1a8184ee4ull, 66, 62, 3},
    {"Random18", 1, 0, 0xa117a73200c4d0d8ull, 0x14650fb0739d0383ull, 0x5f03c99e6854a46aull, 0x04934ec1a8184ee4ull, 66, 0, 3},
    {"Random19", 0, 1, 0x3dd599a76dbbba23ull, 0x74aca03056ff324eull, 0x0713a3bde745d7edull, 0xc5b88ba55d4a4996ull, 40, 40, 11},
    {"Random19", 0, 0, 0x3dd599a76dbbba23ull, 0x14650fb0739d0383ull, 0x0713a3bde745d7edull, 0xc5b88ba55d4a4996ull, 40, 0, 11},
    {"Random19", 1, 1, 0x17c3fa52056cbb12ull, 0x662eac99acc2d7d5ull, 0xfc6798fec1de6897ull, 0x7a856764b0dc8cb8ull, 40, 40, 12},
    {"Random19", 1, 0, 0x17c3fa52056cbb12ull, 0x14650fb0739d0383ull, 0xfc6798fec1de6897ull, 0x7a856764b0dc8cb8ull, 40, 0, 12},
    {"Random20", 0, 1, 0xb3082baa28ce121eull, 0x8d4b2fa5d20fbc3eull, 0x3d398549696d4c46ull, 0x6dcb4a3e435ba6e4ull, 67, 57, 20},
    {"Random20", 0, 0, 0xb3082baa28ce121eull, 0x14650fb0739d0383ull, 0x3d398549696d4c46ull, 0x6dcb4a3e435ba6e4ull, 67, 0, 20},
    {"Random20", 1, 1, 0xe2b19769791734a2ull, 0x9afc74209747e37bull, 0xfccf9cfafc0d59f2ull, 0x5f606de061bd1774ull, 70, 57, 20},
    {"Random20", 1, 0, 0xe2b19769791734a2ull, 0x14650fb0739d0383ull, 0xfccf9cfafc0d59f2ull, 0x5f606de061bd1774ull, 70, 0, 20},
    {"Random21", 0, 1, 0xafadb761afcc4cddull, 0x81e0f928e01ffa95ull, 0x44d2a25d5d3d9991ull, 0x8b2e49ad4319212eull, 54, 54, 18},
    {"Random21", 0, 0, 0xafadb761afcc4cddull, 0x14650fb0739d0383ull, 0x44d2a25d5d3d9991ull, 0x8b2e49ad4319212eull, 54, 0, 18},
    {"Random21", 1, 1, 0x37d4b58e95ac4896ull, 0xd17c057281827be5ull, 0xacfc14dbfb99da34ull, 0x22055d2921e009d3ull, 54, 54, 18},
    {"Random21", 1, 0, 0x37d4b58e95ac4896ull, 0x14650fb0739d0383ull, 0xacfc14dbfb99da34ull, 0x22055d2921e009d3ull, 54, 0, 18},
    {"Random22", 0, 1, 0xdee5a34184585dabull, 0x433eed1e41804028ull, 0x43bb057df8787cb2ull, 0x46cc3da0b9089ac1ull, 68, 49, 13},
    {"Random22", 0, 0, 0xdee5a34184585dabull, 0x14650fb0739d0383ull, 0x43bb057df8787cb2ull, 0x46cc3da0b9089ac1ull, 68, 0, 13},
    {"Random22", 1, 1, 0x26a6ded9a33ac9c7ull, 0x32843b4165ed0d65ull, 0x19422223060a03fbull, 0xc108274da5752719ull, 68, 49, 13},
    {"Random22", 1, 0, 0x26a6ded9a33ac9c7ull, 0x14650fb0739d0383ull, 0x19422223060a03fbull, 0xc108274da5752719ull, 68, 0, 13},
    {"Random23", 0, 1, 0x8b54ce1fd52b1a9aull, 0x224cb5ae0f1ac93full, 0xf0e62f804df9e022ull, 0x4133f7bb6954349aull, 56, 54, 1},
    {"Random23", 0, 0, 0x8b54ce1fd52b1a9aull, 0x14650fb0739d0383ull, 0xf0e62f804df9e022ull, 0x4133f7bb6954349aull, 56, 0, 1},
    {"Random23", 1, 1, 0x7b0357944761af1bull, 0x176a0075bce64b8full, 0xf0e62f804df9e022ull, 0x8a0e000ca7692e34ull, 56, 54, 1},
    {"Random23", 1, 0, 0x7b0357944761af1bull, 0x14650fb0739d0383ull, 0xf0e62f804df9e022ull, 0x8a0e000ca7692e34ull, 56, 0, 1},
    {"Random24", 0, 1, 0x547d2e8b53c15debull, 0xad45cc9db359421dull, 0xcdafd09a1c076093ull, 0x2d25268ecca7ff93ull, 60, 60, 16},
    {"Random24", 0, 0, 0x547d2e8b53c15debull, 0x14650fb0739d0383ull, 0xcdafd09a1c076093ull, 0x2d25268ecca7ff93ull, 60, 0, 16},
    {"Random24", 1, 1, 0x2a7ede136d4862d9ull, 0xccdca4ae67d9fc98ull, 0x5971bdf0158339a3ull, 0xd12b86e200f3afc2ull, 60, 60, 16},
    {"Random24", 1, 0, 0x2a7ede136d4862d9ull, 0x14650fb0739d0383ull, 0x5971bdf0158339a3ull, 0xd12b86e200f3afc2ull, 60, 0, 16},
    {"Random25", 0, 1, 0xccc46bf223ebc080ull, 0xcfac6f28c2ed1802ull, 0x38cd474fa60d28a1ull, 0xf8ee1e7aa8df4c01ull, 44, 44, 8},
    {"Random25", 0, 0, 0xccc46bf223ebc080ull, 0x14650fb0739d0383ull, 0x38cd474fa60d28a1ull, 0xf8ee1e7aa8df4c01ull, 44, 0, 8},
    {"Random25", 1, 1, 0xc50df9991d2566fcull, 0x4b7f805f16c35dfdull, 0x38cd474fa60d28a1ull, 0x21bcd6104e8bd365ull, 44, 44, 8},
    {"Random25", 1, 0, 0xc50df9991d2566fcull, 0x14650fb0739d0383ull, 0x38cd474fa60d28a1ull, 0x21bcd6104e8bd365ull, 44, 0, 8},
    {"Random26", 0, 1, 0xe59742c6d53a59e9ull, 0xf695db5cf061acc2ull, 0x5a3b59068266bdaeull, 0x980627b1675b1efcull, 42, 42, 10},
    {"Random26", 0, 0, 0xe59742c6d53a59e9ull, 0x14650fb0739d0383ull, 0x5a3b59068266bdaeull, 0x980627b1675b1efcull, 42, 0, 10},
    {"Random26", 1, 1, 0xd3eba03324d377beull, 0xd428da4ca60dbc80ull, 0xe765763e4be6d1b7ull, 0xa41a189bbd93190bull, 42, 42, 11},
    {"Random26", 1, 0, 0xd3eba03324d377beull, 0x14650fb0739d0383ull, 0xe765763e4be6d1b7ull, 0xa41a189bbd93190bull, 42, 0, 11},
    {"Random27", 0, 1, 0x922525ed0e0ac261ull, 0xbb91aa8b02f75e5dull, 0xbfe2f70bda5f466dull, 0x44b2b289f46ef934ull, 50, 50, 22},
    {"Random27", 0, 0, 0x922525ed0e0ac261ull, 0x14650fb0739d0383ull, 0xbfe2f70bda5f466dull, 0x44b2b289f46ef934ull, 50, 0, 22},
    {"Random27", 1, 1, 0x05e1e2c7849e1a23ull, 0x609c20acd1c81b42ull, 0x0bb88d26b3dc8cc1ull, 0x47be5c4fe4e9f14bull, 50, 50, 21},
    {"Random27", 1, 0, 0x05e1e2c7849e1a23ull, 0x14650fb0739d0383ull, 0x0bb88d26b3dc8cc1ull, 0x47be5c4fe4e9f14bull, 50, 0, 21},
    {"Random28", 0, 1, 0xbb9929fd869d83ecull, 0xaf3b8ed5ced5375bull, 0xb315918107aeb676ull, 0xfa5b17cc77508cf0ull, 54, 54, 19},
    {"Random28", 0, 0, 0xbb9929fd869d83ecull, 0x14650fb0739d0383ull, 0xb315918107aeb676ull, 0xfa5b17cc77508cf0ull, 54, 0, 19},
    {"Random28", 1, 1, 0xf8578fbbeaa81481ull, 0x3e0acad585be86d9ull, 0xe8632e16032f1923ull, 0x3c7b82504f9168b5ull, 54, 54, 19},
    {"Random28", 1, 0, 0xf8578fbbeaa81481ull, 0x14650fb0739d0383ull, 0xe8632e16032f1923ull, 0x3c7b82504f9168b5ull, 54, 0, 19},
    {"Random29", 0, 1, 0xd6eaff478891b7daull, 0xa9df1368e4aea283ull, 0xe6aec4ce1ae7ccd1ull, 0x38adbedb80d0ca20ull, 57, 57, 1},
    {"Random29", 0, 0, 0xd6eaff478891b7daull, 0x14650fb0739d0383ull, 0xe6aec4ce1ae7ccd1ull, 0x38adbedb80d0ca20ull, 57, 0, 1},
    {"Random29", 1, 1, 0x4d4b64bd0a7ede5cull, 0xed3995b6f15ae5b5ull, 0xe6aec4ce1ae7ccd1ull, 0x5b7229f7447a7941ull, 57, 57, 1},
    {"Random29", 1, 0, 0x4d4b64bd0a7ede5cull, 0x14650fb0739d0383ull, 0xe6aec4ce1ae7ccd1ull, 0x5b7229f7447a7941ull, 57, 0, 1},
    {"Random30", 0, 1, 0xd66f88389fcd9547ull, 0x43113d5b82059b43ull, 0x260809f2d3657490ull, 0x5aeb07c47b63d013ull, 61, 61, 1},
    {"Random30", 0, 0, 0xd66f88389fcd9547ull, 0x14650fb0739d0383ull, 0x260809f2d3657490ull, 0x5aeb07c47b63d013ull, 61, 0, 1},
    {"Random30", 1, 1, 0xe4fa3a6344099aa9ull, 0xd45bcd6f2d572ec8ull, 0x260809f2d3657490ull, 0x8efe1379de796a86ull, 61, 61, 1},
    {"Random30", 1, 0, 0xe4fa3a6344099aa9ull, 0x14650fb0739d0383ull, 0x260809f2d3657490ull, 0x8efe1379de796a86ull, 61, 0, 1},
    {"Random31", 0, 1, 0x1003f1ca9d62740full, 0x5c48e4c1bfa57985ull, 0x101e40b9d91b7277ull, 0xc1eb7c94186e233bull, 49, 49, 7},
    {"Random31", 0, 0, 0x1003f1ca9d62740full, 0x14650fb0739d0383ull, 0x101e40b9d91b7277ull, 0xc1eb7c94186e233bull, 49, 0, 7},
    {"Random31", 1, 1, 0xe52214dfd2bcc0ccull, 0xbe7a3e7d201789eeull, 0x2a8f80e16d64b4eeull, 0xa1ff686cd364bf6aull, 49, 49, 7},
    {"Random31", 1, 0, 0xe52214dfd2bcc0ccull, 0x14650fb0739d0383ull, 0x2a8f80e16d64b4eeull, 0xa1ff686cd364bf6aull, 49, 0, 7},
    {"Random32", 0, 1, 0xb60b6c948bed15c6ull, 0xb9418d6c8a8019adull, 0x28c568aab096b3f4ull, 0x755402423c549e1full, 62, 62, 17},
    {"Random32", 0, 0, 0xb60b6c948bed15c6ull, 0x14650fb0739d0383ull, 0x28c568aab096b3f4ull, 0x755402423c549e1full, 62, 0, 17},
    {"Random32", 1, 1, 0x2bfd687e5d081a07ull, 0xb78e2496d66141b5ull, 0x31ca7f6e9e28a233ull, 0x5a042649fd309da3ull, 62, 62, 17},
    {"Random32", 1, 0, 0x2bfd687e5d081a07ull, 0x14650fb0739d0383ull, 0x31ca7f6e9e28a233ull, 0x5a042649fd309da3ull, 62, 0, 17},
    {"Random33", 0, 1, 0x721216edad992e1dull, 0x467303755631d3f7ull, 0x011e89871f23d05bull, 0x13b97ad10aa2a75full, 51, 51, 12},
    {"Random33", 0, 0, 0x721216edad992e1dull, 0x14650fb0739d0383ull, 0x011e89871f23d05bull, 0x13b97ad10aa2a75full, 51, 0, 12},
    {"Random33", 1, 1, 0xc58b299eef4ec679ull, 0x098db146c52cdba4ull, 0xfde769901b66c55aull, 0x0234ec1608d9420eull, 51, 51, 12},
    {"Random33", 1, 0, 0xc58b299eef4ec679ull, 0x14650fb0739d0383ull, 0xfde769901b66c55aull, 0x0234ec1608d9420eull, 51, 0, 12},
    {"Random34", 0, 1, 0x1e5d627e1af756e0ull, 0x9f92abb6f41c803dull, 0x81535eb933e496e7ull, 0x3d43f5ce9e1b1c4eull, 51, 51, 1},
    {"Random34", 0, 0, 0x1e5d627e1af756e0ull, 0x14650fb0739d0383ull, 0x81535eb933e496e7ull, 0x3d43f5ce9e1b1c4eull, 51, 0, 1},
    {"Random34", 1, 1, 0x1702186e332219fdull, 0x9debd2a709932880ull, 0xa788aa75c6b931e3ull, 0x83b6d02fc4870fc2ull, 51, 51, 2},
    {"Random34", 1, 0, 0x1702186e332219fdull, 0x14650fb0739d0383ull, 0xa788aa75c6b931e3ull, 0x83b6d02fc4870fc2ull, 51, 0, 2},
    {"Random35", 0, 1, 0xcb20cc7879d5dd93ull, 0x9035c0800d822d83ull, 0x707dc61f9902c968ull, 0x7f45160be4b532e0ull, 63, 63, 21},
    {"Random35", 0, 0, 0xcb20cc7879d5dd93ull, 0x14650fb0739d0383ull, 0x707dc61f9902c968ull, 0x7f45160be4b532e0ull, 63, 0, 21},
    {"Random35", 1, 1, 0x6c694165432ec0aeull, 0xb52f088513e02e11ull, 0x7c4d53bac102a79eull, 0xc172861c22c157e4ull, 63, 63, 21},
    {"Random35", 1, 0, 0x6c694165432ec0aeull, 0x14650fb0739d0383ull, 0x7c4d53bac102a79eull, 0xc172861c22c157e4ull, 63, 0, 21},
    {"Random36", 0, 1, 0x4c539c1e1279ea93ull, 0x9cd436ebeb263f3aull, 0xf4c314289e107873ull, 0xa9546e23fecd6770ull, 84, 60, 20},
    {"Random36", 0, 0, 0x4c539c1e1279ea93ull, 0x14650fb0739d0383ull, 0xf4c314289e107873ull, 0xa9546e23fecd6770ull, 84, 0, 20},
    {"Random36", 1, 1, 0x3f4f63cc5f1e5e4full, 0xde0e62f88b145437ull, 0x4957248d03a59eeaull, 0xbe151812dcc138beull, 84, 60, 20},
    {"Random36", 1, 0, 0x3f4f63cc5f1e5e4full, 0x14650fb0739d0383ull, 0x4957248d03a59eeaull, 0xbe151812dcc138beull, 84, 0, 20},
    {"Random37", 0, 1, 0xc6cb57f1ce8eefbbull, 0x4860a22a5843d96aull, 0xd8069e61ccd6cf22ull, 0x0cfda5999200e705ull, 68, 45, 15},
    {"Random37", 0, 0, 0xc6cb57f1ce8eefbbull, 0x14650fb0739d0383ull, 0xd8069e61ccd6cf22ull, 0x0cfda5999200e705ull, 68, 0, 15},
    {"Random37", 1, 1, 0x01d411837a3a9d62ull, 0x7a591ccd47474fc2ull, 0x6905921f420a2e09ull, 0x58ba78481e2c9f5eull, 68, 45, 15},
    {"Random37", 1, 0, 0x01d411837a3a9d62ull, 0x14650fb0739d0383ull, 0x6905921f420a2e09ull, 0x58ba78481e2c9f5eull, 68, 0, 15},
    {"Random38", 0, 1, 0x2f6d38ecf3660bbdull, 0xdde4a1d753baf311ull, 0x54a52d324e3f9a97ull, 0x19d253ed4d87496cull, 54, 54, 9},
    {"Random38", 0, 0, 0x2f6d38ecf3660bbdull, 0x14650fb0739d0383ull, 0x54a52d324e3f9a97ull, 0x19d253ed4d87496cull, 54, 0, 9},
    {"Random38", 1, 1, 0x704e2efa24abca57ull, 0x83619f47c2632492ull, 0x1078dcc02e5a14b5ull, 0x2e7ba5256d8f738full, 54, 54, 9},
    {"Random38", 1, 0, 0x704e2efa24abca57ull, 0x14650fb0739d0383ull, 0x1078dcc02e5a14b5ull, 0x2e7ba5256d8f738full, 54, 0, 9},
    {"Random39", 0, 1, 0xc21be283ee23472cull, 0xae74b0605e1e307bull, 0xc193386eb35a8e65ull, 0xb9633bc0e7068d48ull, 44, 44, 19},
    {"Random39", 0, 0, 0xc21be283ee23472cull, 0x14650fb0739d0383ull, 0xc193386eb35a8e65ull, 0xb9633bc0e7068d48ull, 44, 0, 19},
    {"Random39", 1, 1, 0x70762b6b63161ef6ull, 0xb0c87605b9a7a140ull, 0xeea051f0749b0428ull, 0xdd9439083abdc078ull, 44, 44, 19},
    {"Random39", 1, 0, 0x70762b6b63161ef6ull, 0x14650fb0739d0383ull, 0xeea051f0749b0428ull, 0xdd9439083abdc078ull, 44, 0, 19},
    {"Random40", 0, 1, 0xe779b6dee0e07501ull, 0x2e9b4f64942449ffull, 0xfa904a6a264aca64ull, 0xc754f4aa4221e904ull, 77, 53, 18},
    {"Random40", 0, 0, 0xe779b6dee0e07501ull, 0x14650fb0739d0383ull, 0xfa904a6a264aca64ull, 0xc754f4aa4221e904ull, 77, 0, 18},
    {"Random40", 1, 1, 0xc8903bbbff4a0a85ull, 0x8e666c24d7b5b245ull, 0x457989fe6088f21cull, 0xa37ad00af21451b1ull, 78, 53, 19},
    {"Random40", 1, 0, 0xc8903bbbff4a0a85ull, 0x14650fb0739d0383ull, 0x457989fe6088f21cull, 0xa37ad00af21451b1ull, 78, 0, 19},
    {"Random41", 0, 1, 0x6060dfbdc91677f4ull, 0x6be88d98095a127aull, 0x9520c91295193892ull, 0x515f754e33e48e55ull, 49, 40, 5},
    {"Random41", 0, 0, 0x6060dfbdc91677f4ull, 0x14650fb0739d0383ull, 0x9520c91295193892ull, 0x515f754e33e48e55ull, 49, 0, 5},
    {"Random41", 1, 1, 0x9655672bb08846d8ull, 0x65f1a64a95568f3bull, 0x9520c91295193892ull, 0x9f12cd2301b8da41ull, 49, 40, 5},
    {"Random41", 1, 0, 0x9655672bb08846d8ull, 0x14650fb0739d0383ull, 0x9520c91295193892ull, 0x9f12cd2301b8da41ull, 49, 0, 5},
    {"Random42", 0, 1, 0x9173880eb691dc53ull, 0x43efd628f365cbadull, 0xd7697ec775faca38ull, 0xd0b08459abc51bf7ull, 60, 60, 13},
    {"Random42", 0, 0, 0x9173880eb691dc53ull, 0x14650fb0739d0383ull, 0xd7697ec775faca38ull, 0xd0b08459abc51bf7ull, 60, 0, 13},
    {"Random42", 1, 1, 0xbcddb4b358b50c4cull, 0x413c691f4f4fd50full, 0x25bb0277fefbcdb0ull, 0x66ef4c009ccb85f0ull, 60, 60, 13},
    {"Random42", 1, 0, 0xbcddb4b358b50c4cull, 0x14650fb0739d0383ull, 0x25bb0277fefbcdb0ull, 0x66ef4c009ccb85f0ull, 60, 0, 13},
    {"Random43", 0, 1, 0x2f3ce665257b740eull, 0x46e64aad86cdcfc5ull, 0x8a290002ba9335b6ull, 0xa145a874e9f3b6cbull, 63, 63, 14},
    {"Random43", 0, 0, 0x2f3ce665257b740eull, 0x14650fb0739d0383ull, 0x8a290002ba9335b6ull, 0xa145a874e9f3b6cbull, 63, 0, 14},
    {"Random43", 1, 1, 0x1972980945f48fb2ull, 0x08e6fba957ca09e2ull, 0x0c35e427d6c0e557ull, 0x22b7231fc68e7036ull, 63, 63, 14},
    {"Random43", 1, 0, 0x1972980945f48fb2ull, 0x14650fb0739d0383ull, 0x0c35e427d6c0e557ull, 0x22b7231fc68e7036ull, 63, 0, 14},
    {"Random44", 0, 1, 0x26218306c50a6e9aull, 0x00643aa994b8e3bdull, 0x4398ebf224294454ull, 0xca3da7b6fb9da968ull, 65, 47, 13},
    {"Random44", 0, 0, 0x26218306c50a6e9aull, 0x14650fb0739d0383ull, 0x4398ebf224294454ull, 0xca3da7b6fb9da968ull, 65, 0, 13},
    {"Random44", 1, 1, 0x22b7b15bf121d4a5ull, 0x0470d98b9dea358eull, 0x346e57041dd6b6cdull, 0x2a988cf4eece4123ull, 65, 47, 14},
    {"Random44", 1, 0, 0x22b7b15bf121d4a5ull, 0x14650fb0739d0383ull, 0x346e57041dd6b6cdull, 0x2a988cf4eece4123ull, 65, 0, 14},
    {"Random45", 0, 1, 0x5e7de0663f133e9eull, 0xe5487eb42c395b04ull, 0x9e927212c83cadc5ull, 0x7e8faadf85fe2805ull, 95, 63, 25},
    {"Random45", 0, 0, 0x5e7de0663f133e9eull, 0x14650fb0739d0383ull, 0x9e927212c83cadc5ull, 0x7e8faadf85fe2805ull, 95, 0, 25},
    {"Random45", 1, 1, 0x2909929a3dbedb12ull, 0xaad64569b7a940b5ull, 0x4b078e8cd5d0038bull, 0x2be21a27f036d41bull, 97, 63, 25},
    {"Random45", 1, 0, 0x2909929a3dbedb12ull, 0x14650fb0739d0383ull, 0x4b078e8cd5d0038bull, 0x2be21a27f036d41bull, 97, 0, 25},
    {"Random46", 0, 1, 0x0db5fb4f84dc8c3aull, 0x639c991bae48e61cull, 0xfaba8060a07d20fdull, 0x2f60e2405e5c723full, 52, 52, 1},
    {"Random46", 0, 0, 0x0db5fb4f84dc8c3aull, 0x14650fb0739d0383ull, 0xfaba8060a07d20fdull, 0x2f60e2405e5c723full, 52, 0, 1},
    {"Random46", 1, 1, 0xc0572ab1c2b3b3ddull, 0x9ebe80a74159857cull, 0xfaba8060a07d20fdull, 0x671d14ca701f5df6ull, 52, 52, 1},
    {"Random46", 1, 0, 0xc0572ab1c2b3b3ddull, 0x14650fb0739d0383ull, 0xfaba8060a07d20fdull, 0x671d14ca701f5df6ull, 52, 0, 1},
    {"Random47", 0, 1, 0x8b415587165d6e23ull, 0x872dabc3c6837c49ull, 0x522497c980811fd9ull, 0x3fae3318d3df3437ull, 57, 57, 13},
    {"Random47", 0, 0, 0x8b415587165d6e23ull, 0x14650fb0739d0383ull, 0x522497c980811fd9ull, 0x3fae3318d3df3437ull, 57, 0, 13},
    {"Random47", 1, 1, 0x7fba22d58cbd5742ull, 0x952c13f4b0367030ull, 0xd71728e8746a5845ull, 0x67a015b13a126f57ull, 57, 57, 13},
    {"Random47", 1, 0, 0x7fba22d58cbd5742ull, 0x14650fb0739d0383ull, 0xd71728e8746a5845ull, 0x67a015b13a126f57ull, 57, 0, 13},
    {"Random48", 0, 1, 0x1d605fd446942e1eull, 0xad2d1f5f1ec6eff2ull, 0x935393ec134d3933ull, 0x78ecfaf2273e0f0dull, 48, 48, 7},
    {"Random48", 0, 0, 0x1d605fd446942e1eull, 0x14650fb0739d0383ull, 0x935393ec134d3933ull, 0x78ecfaf2273e0f0dull, 48, 0, 7},
    {"Random48", 1, 1, 0x8cfb96ec8a97eb1bull, 0x151d1b2f00cbeffeull, 0x5386898595fcb912ull, 0x65feef2927240d81ull, 48, 48, 7},
    {"Random48", 1, 0, 0x8cfb96ec8a97eb1bull, 0x14650fb0739d0383ull, 0x5386898595fcb912ull, 0x65feef2927240d81ull, 48, 0, 7},
};
