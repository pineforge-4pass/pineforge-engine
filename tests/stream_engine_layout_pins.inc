// Measured pins of the engine layout that PF_STREAM_ENGINE_LAYOUT_VERSION
// stands for: the size of BacktestEngine and of NativeExecutionConsumer, and
// the offset and size of each member the live runner's reader touches
// (tests/test_stream_engine_layout.cpp names them and compares them).
//
// Row forms, one per line:
//   PF_STREAM_LAYOUT_STAMP(<stamp>u)
//   PF_STREAM_LAYOUT_SIZE(<class>, <bytes>)
//   PF_STREAM_LAYOUT_MEMBER(<class>, <member path>, <offset>, <bytes>)
//
// No layout has been measured for any stamp yet, so this file holds no row.
