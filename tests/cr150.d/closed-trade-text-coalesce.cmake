if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND PINEFORGE_BUILD_SOURCE_LAYER)
    add_executable(test_closed_trade_text_coalesce test_closed_trade_text_coalesce.cpp)
    target_link_libraries(test_closed_trade_text_coalesce PRIVATE pineforge Threads::Threads)
    target_compile_options(test_closed_trade_text_coalesce PRIVATE -ffp-contract=off -UNDEBUG)
    target_link_options(test_closed_trade_text_coalesce PRIVATE
        "-Wl,--wrap=pread" "-Wl,--wrap=pwrite" "-Wl,--wrap=ftruncate"
        "-Wl,--wrap=read" "-Wl,--wrap=write"
        "-Wl,--wrap=readv" "-Wl,--wrap=writev"
        "-Wl,--wrap=preadv" "-Wl,--wrap=pwritev" "-Wl,--wrap=mmap")
    add_test(NAME test_closed_trade_text_coalesce COMMAND test_closed_trade_text_coalesce)
endif()
