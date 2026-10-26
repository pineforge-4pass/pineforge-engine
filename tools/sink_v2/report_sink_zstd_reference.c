/* Identify the verified source/build closure shared with the stage-C tool. */
#ifndef PINEFORGE_SINK_ZSTD_BUILD
#error "The conformance codec must carry the stage-C zstd build identity"
#endif

const char* pineforge_sink_zstd_reference_build(void) {
    return PINEFORGE_SINK_ZSTD_BUILD;
}
