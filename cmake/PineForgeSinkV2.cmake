# Standalone stage C: no v2 objects or feature definitions enter either engine archive.
option(PINEFORGE_WITH_ZSTD "Build the standalone sink-v2 transcoder with pinned zstd" ON)
find_package(Threads REQUIRED)
include(GNUInstallDirs)
if(PINEFORGE_WITH_ZSTD)
    if(PINEFORGE_ZSTD_SYSTEM)
        message(FATAL_ERROR "Stage C requires the pinned offline zstd build; system zstd is not admitted")
    endif()
    set(_pf_zstd_root "${PROJECT_SOURCE_DIR}/third_party/zstd/lib")
    file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/zstd/MANIFEST.sha256" _pf_zstd_manifest)
    if(NOT _pf_zstd_manifest STREQUAL "ae3ceb18f192311c14a9cb0d64d1f3ecfa4b8a8b4db3185c262549fff352d57c")
        message(FATAL_ERROR "Pinned zstd manifest differs")
    endif()
    file(STRINGS "${PROJECT_SOURCE_DIR}/third_party/zstd/MANIFEST.sha256" _pf_zstd_files)
    foreach(_pf_zstd_entry IN LISTS _pf_zstd_files)
        string(SUBSTRING "${_pf_zstd_entry}" 0 64 _pf_zstd_expected)
        string(SUBSTRING "${_pf_zstd_entry}" 66 -1 _pf_zstd_path)
        file(SHA256 "${PROJECT_SOURCE_DIR}/third_party/zstd/${_pf_zstd_path}" _pf_zstd_actual)
        if(NOT _pf_zstd_actual STREQUAL _pf_zstd_expected)
            message(FATAL_ERROR "Pinned zstd source differs: ${_pf_zstd_path}")
        endif()
    endforeach()
    add_library(pineforge_sink_zstd OBJECT
        ${_pf_zstd_root}/common/debug.c
        ${_pf_zstd_root}/common/entropy_common.c
        ${_pf_zstd_root}/common/error_private.c
        ${_pf_zstd_root}/common/fse_decompress.c
        ${_pf_zstd_root}/common/xxhash.c
        ${_pf_zstd_root}/common/zstd_common.c
        ${_pf_zstd_root}/compress/fse_compress.c
        ${_pf_zstd_root}/compress/hist.c
        ${_pf_zstd_root}/compress/huf_compress.c
        ${_pf_zstd_root}/compress/zstd_compress.c
        ${_pf_zstd_root}/compress/zstd_compress_literals.c
        ${_pf_zstd_root}/compress/zstd_compress_sequences.c
        ${_pf_zstd_root}/compress/zstd_compress_superblock.c
        ${_pf_zstd_root}/compress/zstd_double_fast.c
        ${_pf_zstd_root}/compress/zstd_fast.c
        ${_pf_zstd_root}/compress/zstd_lazy.c
        ${_pf_zstd_root}/compress/zstd_ldm.c
        ${_pf_zstd_root}/compress/zstd_opt.c
        ${_pf_zstd_root}/compress/zstd_preSplit.c
        # The writer's final E-only pass and test decoders need decompression.
        ${_pf_zstd_root}/decompress/huf_decompress.c
        ${_pf_zstd_root}/decompress/zstd_ddict.c
        ${_pf_zstd_root}/decompress/zstd_decompress.c
        ${_pf_zstd_root}/decompress/zstd_decompress_block.c
    )
    set_target_properties(pineforge_sink_zstd PROPERTIES
        POSITION_INDEPENDENT_CODE ON C_VISIBILITY_PRESET hidden)
    target_include_directories(pineforge_sink_zstd PRIVATE "${_pf_zstd_root}")
    target_compile_definitions(pineforge_sink_zstd PRIVATE
        ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0
        "ZSTDLIB_VISIBLE=__attribute__((visibility(\"hidden\")))")
    # ZSTD_MULTITHREAD is deliberately undefined. No pool/threading,
    # zstdmt_compress, legacy decoder or dictionary-builder units are built.
endif()

add_library(pineforge_sink_v2 STATIC
    tools/sink_v2/report_sink_v2_support.cpp
    tools/sink_v2/report_sink_v2_codec.cpp
    tools/sink_v2/report_sink_v2_writer.cpp
    tools/sink_v2/report_sink_v2_index.cpp
    tools/sink_v2/report_sink_v2_bounds.cpp
    tools/sink_v2/report_sink_reader.cpp
    tools/sink_v2/report_sink_transcode_v2.cpp)
target_link_libraries(pineforge_sink_v2 PUBLIC pineforge_kernel Threads::Threads)
target_include_directories(pineforge_sink_v2 PUBLIC "${PROJECT_SOURCE_DIR}/tools/sink_v2"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_options(pineforge_sink_v2 PRIVATE -fno-fast-math -ffp-contract=off -Wall -Wextra -Wpedantic)
if(PINEFORGE_WITH_ZSTD)
    target_sources(pineforge_sink_v2 PRIVATE $<TARGET_OBJECTS:pineforge_sink_zstd>)
    target_include_directories(pineforge_sink_v2 PUBLIC "$<BUILD_INTERFACE:${_pf_zstd_root}>")
    target_compile_definitions(pineforge_sink_v2 PUBLIC PINEFORGE_WITH_ZSTD=1)
    string(SHA256 _pf_zstd_build_id
        "${_pf_zstd_manifest};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION};${CMAKE_BUILD_TYPE};${CMAKE_C_FLAGS};${CMAKE_C_FLAGS_RELEASE};${CMAKE_C_FLAGS_DEBUG};single-thread;no-asm;no-legacy")
    set(_pf_zstd_identity "zstd-1.5.7:${_pf_zstd_manifest}:build-${_pf_zstd_build_id}")
    target_compile_definitions(pineforge_sink_v2 PRIVATE PINEFORGE_READER_WITH_ZSTD=1
        "PINEFORGE_SINK_ZSTD_BUILD=\"${_pf_zstd_identity}\"")
    # Python conformance needs the SAME pinned decoder, never find_library().
    # ZSTDLIB_VISIBLE is hidden in the tool object set, so the reference library
    # uses a separately compiled copy of the identical, verified source closure.
    get_target_property(_pf_zstd_sources pineforge_sink_zstd SOURCES)
    add_library(pineforge_sink_zstd_reference SHARED
        tools/sink_v2/report_sink_zstd_reference.c ${_pf_zstd_sources})
    target_include_directories(pineforge_sink_zstd_reference PRIVATE "${_pf_zstd_root}")
    target_compile_definitions(pineforge_sink_zstd_reference PRIVATE
        ZSTD_DISABLE_ASM=1 ZSTD_LEGACY_SUPPORT=0
        "PINEFORGE_SINK_ZSTD_BUILD=\"${_pf_zstd_identity}\"")
endif()
if(PINEFORGE_ENABLE_COVERAGE)
    target_compile_options(pineforge_sink_v2 PRIVATE ${_pf_cov_flags})
endif()
add_executable(pf_sink_transcode_v2 tools/pf_sink_transcode_v2.cpp)
target_link_libraries(pf_sink_transcode_v2 PRIVATE pineforge_sink_v2)
target_compile_options(pf_sink_transcode_v2 PRIVATE -fno-fast-math -ffp-contract=off -Wall -Wextra -Wpedantic)
install(TARGETS pf_sink_transcode_v2 RUNTIME DESTINATION bin)
if(PINEFORGE_BUILD_TESTS AND PINEFORGE_WITH_ZSTD AND UNIX)
    add_executable(sink_v2_incremental_probe tests/sink_v2_incremental_probe.cpp)
    target_link_libraries(sink_v2_incremental_probe PRIVATE pineforge_sink_v2)
    target_include_directories(sink_v2_incremental_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(sink_v2_incremental_probe PRIVATE -fno-fast-math -ffp-contract=off -Wall -Wextra -Wpedantic)
    foreach(_pf_probe IN ITEMS sink_v2_structure_fault sink_v2_structure_control)
        add_executable(${_pf_probe} tests/sink_v2_incremental_probe.cpp)
        target_link_libraries(${_pf_probe} PRIVATE pineforge_sink_v2)
        target_include_directories(${_pf_probe} PRIVATE "${PROJECT_SOURCE_DIR}/src")
        target_compile_options(${_pf_probe} PRIVATE -fno-fast-math -ffp-contract=off -Wall -Wextra -Wpedantic)
        target_compile_definitions(${_pf_probe} PRIVATE PINEFORGE_SINK_V2_PROBE_TEE=1)
    endforeach()
    target_compile_definitions(sink_v2_structure_fault PRIVATE PINEFORGE_SINK_V2_PROBE_CORRUPT=1)
    add_executable(sink_v2_late_probe tests/sink_v2_late_probe.cpp)
    target_link_libraries(sink_v2_late_probe PRIVATE pineforge_sink_v2)
    target_include_directories(sink_v2_late_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_compile_options(sink_v2_late_probe PRIVATE -fno-fast-math -ffp-contract=off -Wall -Wextra -Wpedantic)
endif()
if(UNIX)
    add_executable(pf_sink_tee tools/sink_v2/report_sink_tee.cpp)
    target_compile_features(pf_sink_tee PRIVATE cxx_std_17)
    target_compile_options(pf_sink_tee PRIVATE -Wall -Wextra -Wpedantic)
    install(TARGETS pf_sink_tee RUNTIME DESTINATION bin)
endif()
if(PINEFORGE_WITH_ZSTD)
    install(FILES third_party/zstd/LICENSE
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pineforge/licenses RENAME zstd-LICENSE)
endif()
