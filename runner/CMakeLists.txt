# Dependencies belong only to the optional executable, not PineForge::pineforge.
find_package(SQLite3 REQUIRED)
find_package(CURL 7.86 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)

add_library(pineforge_live_support STATIC store.cpp transport.cpp)
target_compile_features(pineforge_live_support PUBLIC cxx_std_17)
target_include_directories(pineforge_live_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(TARGET SQLite3::SQLite3)
    set(_pf_sqlite_target SQLite3::SQLite3)
else()
    set(_pf_sqlite_target SQLite::SQLite3)
endif()
target_link_libraries(pineforge_live_support PUBLIC ${_pf_sqlite_target} CURL::libcurl OpenSSL::Crypto Threads::Threads)
target_compile_options(pineforge_live_support PRIVATE -Wall -Wextra -Wpedantic)

add_library(pineforge_live_startup STATIC native_startup.cpp)
target_compile_features(pineforge_live_startup PUBLIC cxx_std_17)
target_include_directories(pineforge_live_startup PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(pineforge_live_startup PUBLIC pineforge_live_support PineForge::pineforge)
target_compile_options(pineforge_live_startup PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)

add_executable(pineforge-live main.cpp parser.cpp)
target_include_directories(pineforge-live PRIVATE
    ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
target_link_libraries(pineforge-live PRIVATE pineforge_live_startup ${CMAKE_DL_LIBS})
target_compile_options(pineforge-live PRIVATE -Wall -Wextra -Wpedantic -ffp-contract=off)
install(TARGETS pineforge-live RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# examples/strategy.cpp derives from pineforge::source::PineStrategyHost, so
# it is the one example that a kernel-only build cannot compile.
if(PINEFORGE_BUILD_SOURCE_LAYER)
    add_library(native_live_example MODULE examples/strategy.cpp)
    target_link_libraries(native_live_example PRIVATE PineForge::pineforge)
    target_compile_options(native_live_example PRIVATE -fvisibility=hidden -ffp-contract=off)
    set_target_properties(native_live_example PROPERTIES PREFIX "" OUTPUT_NAME "native-live-example")
else()
    message(STATUS
        "Live runner example strategy.cpp skipped: it is Pine-bound and "
        "PINEFORGE_BUILD_SOURCE_LAYER is OFF")
endif()

add_library(native_market_example MODULE
    ${PROJECT_SOURCE_DIR}/examples/native/native_market_strategy.cpp)
target_link_libraries(native_market_example PRIVATE PineForge::pineforge)
target_compile_options(native_market_example PRIVATE -fvisibility=hidden -ffp-contract=off)
set_target_properties(native_market_example PROPERTIES PREFIX "" OUTPUT_NAME "native-market-example")

add_library(native_selected_example MODULE
    ${PROJECT_SOURCE_DIR}/examples/native/native_selected_strategy.cpp)
target_link_libraries(native_selected_example PRIVATE PineForge::pineforge)
target_compile_options(native_selected_example PRIVATE -fvisibility=hidden -ffp-contract=off)
set_target_properties(native_selected_example PROPERTIES PREFIX "" OUTPUT_NAME "native-selected-example")

# Both modules are examples/native sources, which check their own results with
# assert(); test_native_example_batch and test_native_example_selected drive
# them through the C ABI. Keep assert() live here too, as
# examples/native/CMakeLists.txt does for their example_* executables: strip
# NDEBUG after the build-type flags, so it wins over Release's -DNDEBUG.
# scripts/ci_verify.py's examples-assert-live stage checks every compile of an
# examples/native source for it.
foreach(_pf_example_module native_market_example native_selected_example)
    target_compile_options(${_pf_example_module} PRIVATE -UNDEBUG)
endforeach()

add_library(native_live_parser_example MODULE examples/demo_parser.cpp)
target_include_directories(native_live_parser_example PRIVATE ${PROJECT_SOURCE_DIR}/include)
set_target_properties(native_live_parser_example PROPERTIES PREFIX "" OUTPUT_NAME "native-live-parser-example")

if(PINEFORGE_BUILD_TESTS)
    add_executable(test_live_json ${PROJECT_SOURCE_DIR}/tests/test_live_json.cpp)
    target_include_directories(test_live_json PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(test_live_json PRIVATE -UNDEBUG)
    add_test(NAME test_live_json COMMAND test_live_json)
    add_executable(test_native_live_startup ${PROJECT_SOURCE_DIR}/tests/test_native_live_startup.cpp)
    target_link_libraries(test_native_live_startup PRIVATE pineforge_live_startup)
    target_compile_options(test_native_live_startup PRIVATE -UNDEBUG)
    add_test(NAME test_native_live_startup COMMAND test_native_live_startup)
    add_test(NAME native_live_help COMMAND pineforge-live --help)
    add_executable(test_live_parser ${PROJECT_SOURCE_DIR}/tests/test_live_parser.cpp parser.cpp)
    target_include_directories(test_live_parser PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_options(test_live_parser PRIVATE -UNDEBUG)
    target_link_libraries(test_live_parser PRIVATE ${CMAKE_DL_LIBS})
    foreach(parser_kind fixture wrong_abi)
        add_library(live_parser_${parser_kind} MODULE ${PROJECT_SOURCE_DIR}/tests/test_live_parser.cpp)
        target_include_directories(live_parser_${parser_kind} PRIVATE ${PROJECT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(live_parser_${parser_kind} PRIVATE PF_LIVE_PARSER_TEST_PLUGIN=1)
    endforeach()
    target_compile_definitions(live_parser_wrong_abi PRIVATE PF_LIVE_PARSER_TEST_ABI=2)
    add_test(NAME test_live_parser COMMAND test_live_parser $<TARGET_FILE:native_live_parser_example>
        $<TARGET_FILE:live_parser_fixture> $<TARGET_FILE:live_parser_wrong_abi>)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_test(NAME native_live_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example> $<TARGET_FILE:native_live_parser_example>
            $<TARGET_FILE:live_parser_fixture>)
        set_tests_properties(native_live_e2e PROPERTIES TIMEOUT 120)
    endif()
    foreach(contract_kind absent unknown native_no_export native_stub)
        add_library(live_contract_${contract_kind} MODULE
            ${PROJECT_SOURCE_DIR}/tests/test_live_contract_plugins.cpp)
        target_include_directories(live_contract_${contract_kind} PRIVATE
            ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
        target_compile_options(live_contract_${contract_kind} PRIVATE -fvisibility=hidden)
        set_target_properties(live_contract_${contract_kind} PROPERTIES PREFIX "")
    endforeach()
    target_compile_definitions(live_contract_absent PRIVATE PF_LIVE_CONTRACT_ABSENT=1)
    target_compile_definitions(live_contract_unknown PRIVATE PF_LIVE_CONTRACT_UNKNOWN=1)
    target_compile_definitions(live_contract_native_no_export PRIVATE PF_LIVE_CONTRACT_NATIVE_NO_EXPORT=1)
    target_compile_definitions(live_contract_native_stub PRIVATE PF_LIVE_CONTRACT_NATIVE_STUB=1)
    if(PINEFORGE_BUILD_SOURCE_LAYER)
        add_test(NAME native_live_startup_e2e COMMAND ${Python3_EXECUTABLE}
            ${PROJECT_SOURCE_DIR}/tests/native_live_startup_e2e.py $<TARGET_FILE:pineforge-live>
            $<TARGET_FILE:native_live_example> $<TARGET_FILE:native_market_example>
            $<TARGET_FILE:live_contract_absent> $<TARGET_FILE:live_contract_unknown>
            $<TARGET_FILE:live_contract_native_no_export> $<TARGET_FILE:live_contract_native_stub>)
        set_tests_properties(native_live_startup_e2e PROPERTIES TIMEOUT 120)
    endif()
    # Exercise the real MODULE through its C ABI, without linking a second engine.
    add_executable(test_native_example_batch
        ${PROJECT_SOURCE_DIR}/tests/test_native_example_batch.cpp)
    target_include_directories(test_native_example_batch PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
    target_compile_options(test_native_example_batch PRIVATE -UNDEBUG -ffp-contract=off)
    target_link_libraries(test_native_example_batch PRIVATE ${CMAKE_DL_LIBS})
    if(PINEFORGE_ENABLE_SANITIZERS)
        target_compile_options(test_native_example_batch PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_native_example_batch PRIVATE -fsanitize=address,undefined)
    endif()
    add_dependencies(test_native_example_batch native_market_example)
    add_test(NAME test_native_example_batch
        COMMAND test_native_example_batch $<TARGET_FILE:native_market_example>)
    set_tests_properties(test_native_example_batch PROPERTIES TIMEOUT 60)
    # Exercise the second native MODULE through the same C ABI boundary.
    add_executable(test_native_example_selected
        ${PROJECT_SOURCE_DIR}/tests/test_native_example_selected.cpp)
    target_include_directories(test_native_example_selected PRIVATE
        ${PROJECT_SOURCE_DIR}/include ${PROJECT_BINARY_DIR}/include)
    target_compile_options(test_native_example_selected PRIVATE -UNDEBUG -ffp-contract=off)
    target_link_libraries(test_native_example_selected PRIVATE ${CMAKE_DL_LIBS})
    if(PINEFORGE_ENABLE_SANITIZERS)
        target_compile_options(test_native_example_selected PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(test_native_example_selected PRIVATE -fsanitize=address,undefined)
    endif()
    add_dependencies(test_native_example_selected native_selected_example)
    add_test(NAME test_native_example_selected
        COMMAND test_native_example_selected $<TARGET_FILE:native_selected_example>)
    set_tests_properties(test_native_example_selected PROPERTIES TIMEOUT 60)
    foreach(native_test test_native_live_store test_native_live_websocket)
        add_executable(${native_test} ${PROJECT_SOURCE_DIR}/tests/${native_test}.cpp)
        target_link_libraries(${native_test} PRIVATE pineforge_live_support)
        target_compile_options(${native_test} PRIVATE -UNDEBUG)
        add_test(NAME ${native_test} COMMAND ${native_test})
        set_tests_properties(${native_test} PROPERTIES TIMEOUT 120)
    endforeach()
    set_tests_properties(test_native_live_websocket PROPERTIES SKIP_RETURN_CODE 77)
endif()
